// Passive DisplayPort AUX sniffer -> backlight control (see SPEC.md).
//
// Flow:
//   RMT RX (PIN_AUX_IN) --burst--> onRecvDone (ISR): re-arms the RMT with the
//   next buffer of the ring and queues the burst
//   --> decodeTask (high priority): symbols -> runs -> auxdec -> action on
//       PIN_BL and the LED; in debug mode, queues text for loop()
//   --> loop() (low priority): prints to Serial. Never delays the action.
//
// Configuration (board, pins, backlight mode): config.h.

#include <Arduino.h>

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "auxdec.h"
#include "config.h"
#include "panelvcc.h"

static_assert(sizeof(rmt_symbol_word_t) == sizeof(uint32_t), "auxdec::runsFromRmt expects 32-bit symbols");

// ---------------------------------------------------------------- backlight

static volatile bool g_blOff = false;

// Writes the backlight state. In DIRECT_OD, 1 leaves the pin in high
// impedance (the external 10 kΩ pull-up raises BL_EN); 0 pulls it low.
static void blWrite(bool off) {
#if BL_MODE == BL_MODE_DIRECT_OD
  gpio_set_level((gpio_num_t)PIN_BL, off ? 0 : 1);
#else
  gpio_set_level((gpio_num_t)PIN_BL, off ? 1 : 0);
#endif
}

// Backlight ON without a glitch: output level first, then direction.
// No internal pulls in either mode.
static void blInit() {
  blWrite(false);
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << PIN_BL;
#if BL_MODE == BL_MODE_DIRECT_OD
  io.mode = GPIO_MODE_INPUT_OUTPUT_OD;  // input enabled: lets us read the actual BL_EN level
#else
  io.mode = GPIO_MODE_OUTPUT;
#endif
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io);
  blWrite(false);
}

// On esp_restart() (not on a panic: there the chip reset already releases the pin).
static void blShutdown() { blWrite(false); }

// ---------------------------------------------------------------- panel VCC

static panelvcc::Sequencer g_vcc(PIN_PANEL_VCC >= 0, PANEL_VCC_OFF_DELAY_MS);

// Gate HIGH = panel VCC off. Level first, then direction (no glitch).
static void vccWrite(bool off) {
#if PIN_PANEL_VCC >= 0
  gpio_set_level((gpio_num_t)PIN_PANEL_VCC, off ? 1 : 0);
#else
  (void)off;
#endif
}

static void vccInit() {
#if PIN_PANEL_VCC >= 0
  vccWrite(false);
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << PIN_PANEL_VCC;
  io.mode = GPIO_MODE_OUTPUT;
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io);
  vccWrite(false);
#endif
}

// On esp_restart(): leave the panel powered.
static void vccShutdown() { vccWrite(false); }

static void ledWrite(bool on) {
#if PIN_LED >= 0
  digitalWrite(PIN_LED, (on ^ LED_ACTIVE_LOW) ? HIGH : LOW);
#else
  (void)on;
#endif
}

// As early as possible: initVariant() runs before setup() and before Serial.
void initVariant() {
  vccInit();
  blInit();
}

// ---------------------------------------------------------------- state

struct Burst {
  const rmt_symbol_word_t* sym;
  uint16_t nsym;
  int64_t t_us;
};

struct LogItem {
  enum Kind : uint8_t { Msg, Undecodable, State, Vcc } kind;
  uint8_t index;  // position of the message in the burst (0 = request)
  bool blOff;     // State: new state; Vcc: true = panel VCC off
  uint16_t nruns;
  int64_t t_us;
  auxdec::Message msg;
};

static struct {
  volatile uint32_t bursts, noise, undecodable, truncated, messages;
  volatile uint32_t dropped, logDropped, rearmFail;
} g_stats;

// Each counter has a single writer (the ISR or the task); loop() only reads.
static inline void IRAM_ATTR bump(volatile uint32_t& c) { c = c + 1; }

static rmt_symbol_word_t g_rxBuf[NUM_RX_BUFS][RX_BUF_SYMBOLS];
static rmt_channel_handle_t g_rx = nullptr;
static rmt_receive_config_t g_rxCfg = {};
static volatile uint8_t g_rxIdx = 0;  // buffer currently receiving
static volatile bool g_rxStopped = false;
static bool g_rxOk = false;
static QueueHandle_t g_burstQ, g_logQ;
static volatile bool g_debug = AUX_DEBUG;

// ---------------------------------------------------------------- RMT

// ISR. The ring has NUM_RX_BUFS buffers and the queue NUM_RX_BUFS - 2 slots:
// at most the queued ones + the one the task is processing + the one
// receiving are in use, so the next one in the ring is always free. If the
// queue is full, the burst is dropped and the same buffer is reused.
static bool IRAM_ATTR onRecvDone(rmt_channel_handle_t ch, const rmt_rx_done_event_data_t* ed, void*) {
  BaseType_t woken = pdFALSE;
  Burst b = {ed->received_symbols, (uint16_t)ed->num_symbols, esp_timer_get_time()};
  uint8_t next = g_rxIdx;
  if (xQueueSendFromISR(g_burstQ, &b, &woken) == pdTRUE)
    next = (next + 1) % NUM_RX_BUFS;
  else
    bump(g_stats.dropped);
  g_rxIdx = next;
  if (rmt_receive(ch, g_rxBuf[next], sizeof(g_rxBuf[next]), &g_rxCfg) != ESP_OK) g_rxStopped = true;
  return woken == pdTRUE;
}

static esp_err_t rxInit() {
  rmt_rx_channel_config_t cc = {};
  cc.gpio_num = (gpio_num_t)PIN_AUX_IN;
  cc.clk_src = RMT_CLK_SRC_DEFAULT;
  cc.resolution_hz = RMT_RESOLUTION_HZ;
  cc.mem_block_symbols = RMT_MEM_SYMBOLS;
  esp_err_t err = rmt_new_rx_channel(&cc, &g_rx);
  if (err != ESP_OK) return err;
  rmt_rx_event_callbacks_t cbs = {};
  cbs.on_recv_done = onRecvDone;
  if ((err = rmt_rx_register_event_callbacks(g_rx, &cbs, nullptr)) != ESP_OK) return err;
  if ((err = rmt_enable(g_rx)) != ESP_OK) return err;
  g_rxCfg.signal_range_min_ns = RMT_MIN_NS;
  g_rxCfg.signal_range_max_ns = RMT_MAX_NS;
  return rmt_receive(g_rx, g_rxBuf[0], sizeof(g_rxBuf[0]), &g_rxCfg);
}

// ---------------------------------------------------------------- decoding

static void logPush(const LogItem& it) {
  if (xQueueSend(g_logQ, &it, 0) != pdTRUE) bump(g_stats.logDropped);
}

// Applies a VCC state change reported by the sequencer.
static void vccApply(int64_t t_us) {
  bool off = g_vcc.vccOff();
  vccWrite(off);
  if (!g_debug) return;
  LogItem it = {};
  it.kind = LogItem::Vcc;
  it.blOff = off;
  it.t_us = t_us;
  logPush(it);
}

static void setBacklight(bool off, int64_t t_us) {
  if (!off && !g_vcc.backlightAllowed()) return;  // never backlight on with VCC off
  blWrite(off);
  ledWrite(off);
  if (off == g_blOff) return;
  g_blOff = off;
  LogItem it = {};
  it.kind = LogItem::State;
  it.blOff = off;
  it.t_us = t_us;
  logPush(it);
}

static void handleBurst(const Burst& b) {
  static auxdec::Decoder dec;
  static auxdec::Run runs[RX_BUF_SYMBOLS * 2];
  static auxdec::Message msgs[auxdec::kMaxMsgs];

  bump(g_stats.bursts);
  // Any burst while VCC is off can only come from the source: power the panel
  // before anything else, even if the burst turns out to be noise.
  if (g_vcc.onBurst((uint32_t)(b.t_us / 1000))) vccApply(b.t_us);
  size_t nr = auxdec::runsFromRmt(&b.sym->val, b.nsym, 1000000000UL / RMT_RESOLUTION_HZ, runs,
                                  sizeof(runs) / sizeof(runs[0]));
  if (nr < auxdec::kMinRuns) {
    bump(g_stats.noise);
    return;
  }
  if (runs[nr - 1].dur_ns != 0) bump(g_stats.truncated);  // no end marker: buffer full

  size_t nm = dec.decode(runs, nr, msgs, auxdec::kMaxMsgs);
  if (nm == 0) {
    bump(g_stats.undecodable);
    if (g_debug) {
      LogItem it = {};
      it.kind = LogItem::Undecodable;
      it.nruns = (uint16_t)nr;
      it.t_us = b.t_us;
      logPush(it);
    }
    return;
  }
  for (size_t i = 0; i < nm; i++) {
    bump(g_stats.messages);
    if (g_debug) {  // before the action, so the log comes out in order (logPush doesn't block)
      LogItem it = {};
      it.kind = LogItem::Msg;
      it.index = (uint8_t)i;
      it.nruns = (uint16_t)nr;
      it.t_us = b.t_us;
      it.msg = msgs[i];
      logPush(it);
    }
    auxdec::Action a = auxdec::classify(msgs[i]);
    if (a == auxdec::Action::BacklightOff) {
      setBacklight(true, b.t_us);
      g_vcc.onSleep((uint32_t)(b.t_us / 1000));
    } else if (a == auxdec::Action::BacklightOn) {
      g_vcc.onWake((uint32_t)(b.t_us / 1000));
      setBacklight(false, b.t_us);
    }
  }
}

static void decodeTask(void*) {
  esp_task_wdt_add(nullptr);
  for (;;) {
    Burst b;
    if (xQueueReceive(g_burstQ, &b, pdMS_TO_TICKS(50)) == pdTRUE) handleBurst(b);
    int64_t now_us = esp_timer_get_time();
    if (g_vcc.tick((uint32_t)(now_us / 1000))) vccApply(now_us);
    // If re-arming in the ISR failed, the channel is stopped and the ISR won't
    // run again: no race. g_rxIdx is free by the ring invariant.
    if (g_rxOk && g_rxStopped) {
      g_rxStopped = false;
      bump(g_stats.rearmFail);
      if (rmt_receive(g_rx, g_rxBuf[g_rxIdx], sizeof(g_rxBuf[0]), &g_rxCfg) != ESP_OK) g_rxStopped = true;
    }
    esp_task_wdt_reset();
  }
}

// ---------------------------------------------------------------- Serial

static void printTime(int64_t t_us) {
  Serial.printf("[%7lu.%03lu] ", (unsigned long)(t_us / 1000000), (unsigned long)(t_us / 1000 % 1000));
}

static void printItem(const LogItem& it) {
  char buf[128];
  printTime(it.t_us);
  switch (it.kind) {
    case LogItem::State:
      Serial.printf(">>> BACKLIGHT %s\n", it.blOff ? "OFF" : "ON");
      break;
    case LogItem::Vcc:
      Serial.println(it.blOff ? "panel VCC off" : "panel VCC on (AUX activity)");
      break;
    case LogItem::Undecodable:
      Serial.printf("?? burst without SYNC (%u runs)\n", it.nruns);
      break;
    case LogItem::Msg:
      if (it.index == 0)
        Serial.printf("REQ  %s\n", auxdec::describeRequest(it.msg, buf, sizeof buf));
      else
        Serial.printf("     %s\n", auxdec::describeReply(it.msg, buf, sizeof buf));
      break;
  }
}

static void printStats() {
  Serial.printf(
      "stats: bursts=%lu noise=%lu no_sync=%lu truncated=%lu messages=%lu dropped=%lu "
      "log_lost=%lu rearms=%lu | vcc_off=%lu vcc_on=%lu | backlight=%s panel_vcc=%s rmt=%s\n",
      (unsigned long)g_stats.bursts, (unsigned long)g_stats.noise, (unsigned long)g_stats.undecodable,
      (unsigned long)g_stats.truncated, (unsigned long)g_stats.messages, (unsigned long)g_stats.dropped,
      (unsigned long)g_stats.logDropped, (unsigned long)g_stats.rearmFail, (unsigned long)g_vcc.offCount(),
      (unsigned long)g_vcc.onCount(), g_blOff ? "OFF" : "ON",
      !g_vcc.enabled() ? "disabled" : g_vcc.vccOff() ? "OFF" : "ON", g_rxOk ? "ok" : "ERROR");
}

static void printBanner() {
  Serial.printf("\naux-sniffer | %s | AUX_IN=GPIO%d BL=GPIO%d (%s) LED=GPIO%d PANEL_VCC=GPIO%d (off after %d ms)\n",
                BOARD_NAME, PIN_AUX_IN, PIN_BL,
                BL_MODE == BL_MODE_DIRECT_OD ? "direct open-drain, LOW=OFF" : "MOSFET, HIGH=OFF", PIN_LED,
                PIN_PANEL_VCC, PANEL_VCC_OFF_DELAY_MS);
  Serial.printf("RMT %lu Hz, mem %d symbols, buffer %d x %d, filter %d ns, end of burst %d ns\n",
                (unsigned long)RMT_RESOLUTION_HZ, RMT_MEM_SYMBOLS, NUM_RX_BUFS, RX_BUF_SYMBOLS, RMT_MIN_NS,
                RMT_MAX_NS);
  Serial.printf("debug=%s  (keys: d=debug on/off, s=stats, h=help)\n", g_debug ? "on" : "off");
}

// ---------------------------------------------------------------- setup / loop

void setup() {
  vccInit();  // initVariant() already did it; repeating it is harmless
  blInit();
  esp_register_shutdown_handler(blShutdown);
  esp_register_shutdown_handler(vccShutdown);
#if PIN_LED >= 0
  pinMode(PIN_LED, OUTPUT);
  for (int i = 0; i < 3; i++) {  // sign of life
    ledWrite(true);
    delay(60);
    ledWrite(false);
    delay(60);
  }
#endif

  Serial.setTxBufferSize(4096);
  Serial.begin(SERIAL_BAUD);

  esp_task_wdt_config_t wdt = {};
  wdt.timeout_ms = WDT_TIMEOUT_MS;
  wdt.idle_core_mask = (1 << portNUM_PROCESSORS) - 1;
  wdt.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&wdt) == ESP_ERR_INVALID_STATE) esp_task_wdt_init(&wdt);
  esp_task_wdt_add(nullptr);  // loopTask

  g_burstQ = xQueueCreate(NUM_RX_BUFS - 2, sizeof(Burst));
  g_logQ = xQueueCreate(64, sizeof(LogItem));
  xTaskCreate(decodeTask, "auxdec", 4096, nullptr, 10, nullptr);

  esp_err_t err = rxInit();
  g_rxOk = err == ESP_OK;
  delay(500);  // give the host time to open the USB CDC port
  printBanner();
  if (!g_rxOk) Serial.printf("ERROR starting RMT: %s. The backlight stays on.\n", esp_err_to_name(err));
}

void loop() {
  static uint32_t lastStats = 0, lastErr = 0, lastBursts = 0;
  LogItem it;
  while (xQueueReceive(g_logQ, &it, pdMS_TO_TICKS(50)) == pdTRUE) {
    printItem(it);
    esp_task_wdt_reset();
  }

  while (Serial.available()) {
    switch (Serial.read()) {
      case 'd':
        g_debug = !g_debug;
        Serial.printf("debug=%s\n", g_debug ? "on" : "off");
        break;
      case 's':
        printStats();
        break;
      case 'h':
        printBanner();
        break;
    }
  }

  uint32_t now = millis();
  if (g_debug && now - lastStats > 10000 && g_stats.bursts != lastBursts) {
    lastStats = now;
    lastBursts = g_stats.bursts;
    printStats();
  }
  if (!g_rxOk && now - lastErr > 5000) {
    lastErr = now;
    Serial.println("ERROR: RMT not started, sniffer inactive (backlight on)");
  }
  esp_task_wdt_reset();
}
