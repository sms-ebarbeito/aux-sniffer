// Sniffer configuration: board, pins, backlight mode and RMT.
// This is the ONLY place to touch to adapt the firmware. The board is picked
// automatically from the FQBN it's built for; any value can be overridden
// with -D (for example EXTRA="-DPIN_AUX_IN=6" ./build.sh, which uses
// --build-property "compiler.cpp.extra_flags=...").
#pragma once

#include "sdkconfig.h"

// ---------------------------------------------------------------- backlight
// BL_MODE_DIRECT_OD (default, the one installed): BL_EN trace cut, the GPIO
//   goes straight to BL_EN on the panel side, with an external 10 kΩ pull-up
//   to the adapter's 3.3 V. Open-drain with no internal pulls: LOW = backlight
//   OFF; releasing the pin (high impedance) = ON, the external pull-up raises
//   it. It's external so BL_EN stays high even while the ESP32 is in reset or
//   has no firmware running. The ESP32 must be powered from the adapter's
//   same 3.3 V.
// BL_MODE_MOSFET (SPEC §2): GPIO -> 100 Ω -> 2N7002 gate (100 kΩ to GND).
//   HIGH = backlight OFF, LOW = ON.
// In both modes the boot, reset or error state is backlight ON.
#define BL_MODE_MOSFET 1
#define BL_MODE_DIRECT_OD 2
#ifndef BL_MODE
#define BL_MODE BL_MODE_DIRECT_OD
#endif

// ---------------------------------------------------------------- board
#if CONFIG_IDF_TARGET_ESP32C3
// ESP32-C3 SuperMini (main board). Avoid 2, 8, 9 (strapping) and 18/19
// (native USB: Serial goes out there with CDC On Boot).
#define BOARD_NAME "ESP32-C3 SuperMini"
#ifndef PIN_AUX_IN
#define PIN_AUX_IN 4
#endif
#ifndef PIN_BL
#define PIN_BL 5
#endif
#ifndef PIN_LED
#define PIN_LED 8  // blue LED; strapping pin, only used as an output after boot
#endif
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 1
#endif
// RMT memory for the RX channel: both 48-symbol RX blocks (the channel's own
// and the other RX channel's). The driver uses them as ping-pong and keeps
// copying into the user buffer, so a burst can be longer than this.
#ifndef RMT_MEM_SYMBOLS
#define RMT_MEM_SYMBOLS 96
#endif

#elif CONFIG_IDF_TARGET_ESP32S3
#define BOARD_NAME "ESP32-S3"
#ifndef PIN_AUX_IN
#define PIN_AUX_IN 4
#endif
#ifndef PIN_BL
#define PIN_BL 5
#endif
#ifndef PIN_LED
#define PIN_LED -1  // the S3 DevKit LED is RGB (WS2812): not used
#endif
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 0
#endif
#ifndef RMT_MEM_SYMBOLS
#define RMT_MEM_SYMBOLS 96  // 2 blocks of 48, ping-pong as on the C3
#endif

#elif CONFIG_IDF_TARGET_ESP32
#define BOARD_NAME "ESP32 (WROOM)"
#ifndef PIN_AUX_IN
#define PIN_AUX_IN 4
#endif
#ifndef PIN_BL
#define PIN_BL 25
#endif
#ifndef PIN_LED
#define PIN_LED 2  // LED on many DevKits; strapping pin, output only after boot
#endif
#ifndef LED_ACTIVE_LOW
#define LED_ACTIVE_LOW 0
#endif
// The classic ESP32 has no RX ping-pong: the whole burst has to fit in the
// channel memory. 4 blocks of 64 = 256 symbols.
#ifndef RMT_MEM_SYMBOLS
#define RMT_MEM_SYMBOLS 256
#endif

#else
#error "Unsupported board: add its pins in config.h"
#endif

// ---------------------------------------------------------------- RMT
#define RMT_RESOLUTION_HZ 10000000  // 100 ns per tick
#define RMT_MIN_NS 150              // shorter pulses = glitch
#define RMT_MAX_NS 10000            // 10 µs without edges = end of burst
// User buffer per burst. Write + ACK ≈ 100 symbols; the longest possible AUX
// burst (16 data bytes) is around 210.
#define RX_BUF_SYMBOLS 512
#define NUM_RX_BUFS 4  // ring of buffers between the callback and the task

// ---------------------------------------------------------------- misc
#ifndef AUX_DEBUG
#define AUX_DEBUG 1  // print every message to Serial (toggled with 'd')
#endif
#define SERIAL_BAUD 115200
#define WDT_TIMEOUT_MS 5000

// ---------------------------------------------------------------- checks
#if CONFIG_IDF_TARGET_ESP32C3
#define C3_FORBIDDEN(p) ((p) == 2 || (p) == 8 || (p) == 9 || (p) == 18 || (p) == 19)
static_assert(!C3_FORBIDDEN(PIN_AUX_IN), "PIN_AUX_IN: strapping or USB GPIO on the C3");
static_assert(!C3_FORBIDDEN(PIN_BL), "PIN_BL: strapping or USB GPIO on the C3");
#endif
static_assert(PIN_AUX_IN != PIN_BL, "PIN_AUX_IN and PIN_BL must be different");
static_assert(BL_MODE == BL_MODE_MOSFET || BL_MODE == BL_MODE_DIRECT_OD, "invalid BL_MODE");
