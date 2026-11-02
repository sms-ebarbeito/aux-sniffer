# ESP32 DP-AUX sniffer → backlight control (firmware specification)

## 1. Goal

Passive USB-C/miniDP → eDP adapter with a 1920x1200 panel. The adapter **never turns the backlight off**: when the Mac puts the display to sleep or goes to sleep itself, the panel goes black but stays lit.

The ESP32 listens to the **DisplayPort AUX** channel (listen only, never transmits), detects the host's write to DPCD register **0x00600 (SET_POWER)** and controls the panel's **BL_EN** line:

| Detected write | Meaning | Action |
|---|---|---|
| `0x600 = 0x01` | D0, panel on | Backlight **ON** |
| `0x600 = 0x02` | D3, panel off | Backlight **OFF** |
| `0x600 = 0x05` | D3 with AUX active (DP 1.4; not observed, but treat the same) | Backlight **OFF** |

All of this is **verified with real captures** using a logic analyzer, a Mac mini and a MacBook (same macOS). See section 6.

## 2. Hardware (already built and tested)

```
AUX+ via (40-pin connector pin 15 / panel pin 9) ──1kΩ──||10nF──► A  ┐
AUX- via (40-pin connector pin 16 / panel pin 10) ──1kΩ──||10nF──► B  ├ MAX485 module (Arduino) at 3.3 V
                                                        │  R7 (120Ω), R5 and R6 (bias) REMOVED
                                     RE, DE ──► GND     │  RE and DE to GND = receiver only
                                     RO ─────────────────┴──► ESP32 GPIO_AUX_IN
Module VCC = ESP32 3V3 · GND common with the adapter
```

- RO is a 0 / 3.3 V digital signal: it goes **straight** to the GPIO, no divider.
- The AC coupling (10 nF) is essential. With DC coupling the receiver only produced glitches.

**Backlight output, final wiring (installed and tested with an ESP32-C3 SuperMini, `BL_MODE_DIRECT_OD` mode):**

```
                              BL_EN trace
 adapter (original) ──────────────╳──────────┬─────────────┬──── panel BL_EN
                                 cut         │             │
                                           10 kΩ         GPIO_BL (GPIO5)
                                  (external, panel side)   open-drain, no internal pulls
                                             │
                                     adapter 3.3 V

ESP32 (3V3 pin) and MAX485 powered from the same adapter 3.3 V · common GND
```

- The BL_EN trace is **cut**: the adapter no longer drives BL_EN. On the panel side, an **external 10 kΩ pull-up** to the adapter's 3.3 V holds it high, and the GPIO goes straight to that line.
- **GPIO_BL LOW** (open-drain output active): BL_EN goes to GND and the **backlight turns off**. The pin sinks about 0.33 mA.
- **GPIO_BL released** (open-drain high, high impedance, or pin unconfigured): the external pull-up takes BL_EN to 3.3 V and the **backlight stays on**. This is the safe state.
- **Why the pull-up is external rather than the ESP32's internal one:** the internal one only exists while the ESP32 runs the firmware and has configured it. During reset, the bootloader or flashing, after a hang, or with the chip held in reset, the pin is in its default state and BL_EN would float, leaving the backlight in an undefined state. The 10 kΩ, on the other hand, doesn't depend on the ESP32: as long as the adapter has 3.3 V, BL_EN is high unless the GPIO actively pulls it low. Also, the internal one is around 45 kΩ and more sensitive to noise. That's why the firmware configures the pin **with no internal pull-up or pull-down**: the line is defined only by the external resistor.
- **The ESP32 is powered from the same 3.3 V as the pull-up.** If the ESP32 lost power while the adapter stayed powered (for example, with the 3V3 wire disconnected), the GPIO protection diode would pull BL_EN to around 0.6 V through the 10 kΩ and the backlight would turn off. With a single rail, both power up and down together.

**MOSFET alternative (supported by the firmware as `BL_MODE_MOSFET`, not installed):**

```
GPIO_BL ──100Ω──┬── 2N7002 gate
               100kΩ
                │
               GND        drain ── BL_EN (with its pull-up)
                          source ── GND
```

- **GPIO_BL HIGH**: the MOSFET conducts, BL_EN goes to GND and the **backlight turns off**.
- **GPIO_BL LOW or unconfigured**: thanks to the 100 kΩ pull-down, the **backlight stays on**. Useful if the ESP32 has a separate power supply (with common GND).

**Suggested pins.** Strapping pins must be avoided. For AUX_IN, the pin must be routable to the RMT (any GPIO works).

| Board | GPIO_AUX_IN | GPIO_BL | Avoid |
|---|---|---|---|
| ESP32 (WROOM) | 4 | 25 | 0, 2, 5, 12, 15, 6–11 |
| ESP32-S3 | 4 | 5 | 0, 3, 45, 46, 19/20 (USB), 26–32 (flash/PSRAM) |
| ESP32-C3 | 4 | 5 | 2, 8, 9, 18/19 (USB) |

## 3. Signal characteristics on RO (measured)

- **Encoding:** Manchester-II at 1 Mbps. Bit = 1 µs and half-bit = 0.5 µs.
- **Asymmetry:** the halves aren't symmetric because of the receiver threshold. Pulses of **375–625 ns** were observed for a half-bit and **875–1125 ns** for a full bit. The decoder has to classify by ranges, not by exact value.
- **Undefined idle level:** with AC coupling, RO can stay high or low between frames. **Don't assume an idle level.**
- **Idle noise:** none observed. Still, it's worth dropping bursts of fewer than 20 edges.
- **Messages:** each one has this structure:
  - **Preamble:** ~16 Manchester zeros, i.e. alternating 0.5 µs pulses.
  - **SYNC:** 2 µs at one level and 2 µs at the other (4 half-bits + 4 half-bits).
  - **Data:** Manchester-encoded.
  - **STOP:** same pattern as the SYNC.
- **Request and reply in a single burst:** the panel's reply arrives **a few µs** after the request's STOP. With an idle threshold of ~6–10 µs, **request and reply end up in the same burst**, so the decoder must extract **several messages per burst**.
- **Polarity with the current wiring:**
  - SYNC = 4 half-bits **high** followed by 4 **low**.
  - Data bit = **level of the first half-bit** (high = 1).
  - If A and B are swapped, the whole signal comes out inverted. The decoder must also try the inverted SYNC pattern and, if it finds it, invert the levels.

## 4. Decoding algorithm

The working reference is `auxdec.py`, in this same folder. It runs on sigrok `.sr` captures and is the source of truth. The steps:

1. **Capture** a burst as a list of runs `(level, duration_ns)`.
2. **Quantize** each run to half-bits: `u = round(dur_ns / 500)`, clamped to 1..8. In practice:
   - `<750 ns` → 1
   - `750–1500 ns` → 2
   - `1500–2750 ns` → 4–5
   - longer → 6–8 (STOP + SYNC back to back).
3. **Expand** into an array of half-bits (`level` repeated `u` times).
4. **Find the SYNC:** the pattern `1111 0000` preceded by at least 8 preamble half-bits (or `0000 1111` if the polarity is inverted; in that case, invert everything).
5. **Read the data:** after the SYNC, take pairs of half-bits `(a, b)`.
   - If `a != b`, the bit is `a`.
   - If `a == b`, the data is over (it's the STOP).
6. **Assemble the bytes:** group 8 bits at a time, MSB first, and drop the remainder if the bit count isn't a multiple of 8.
7. **Keep searching** for another SYNC in the rest of the burst (the reply).

**Request format (source → panel):**

```
byte0 = CMD[3:0] << 4 | ADDR[19:16]
byte1 = ADDR[15:8]
byte2 = ADDR[7:0]
byte3 = LEN - 1
byte4.. = data (writes only)
CMD: 1000 = native write, 1001 = native read, 0xxx = I2C-over-AUX
```

**The frame to detect is exactly this one, 5 bytes long:**

```
80 06 00 00 XX     → native write, addr 0x00600, len 1, data XX
```

- `XX = 01` → backlight ON.
- `XX = 02` or `05` → backlight OFF.

Anything else is ignored. A reply never has 5 bytes starting with `80 06 00 00`: the reply to a write is a single `00` byte (ACK). So requiring exactly those 5 bytes is enough.

## 5. Firmware requirements

- **Framework:** Arduino-ESP32 **core 3.x** (ESP-IDF 5.x), using `driver/rmt_rx.h` directly, which is the new RMT driver.
- **RMT RX capture:**
  - **Resolution:** 10–20 MHz (50–100 ns per tick).
  - **`signal_range_min_ns`:** ~100–150 ns (glitch filter; the hardware maximum is around 3 µs).
  - **`signal_range_max_ns`:** ~10 000 ns (end of burst after 10 µs without edges).
  - **Buffer:** at least **256 symbols**. A write-plus-reply burst has about 195 runs, about 100 RMT symbols. EDID reads are longer: if they overflow, they're dropped without harm.
  - **RMT memory:** on the classic ESP32 each channel has 64 symbols per block, so several blocks must be allocated (`mem_block_symbols`). On the S3, DMA can be used.
  - **Re-arming:** call `rmt_receive()` again **right away**. Use double buffering and pass the burst through a queue (`xQueueSendFromISR` from the `on_recv_done` callback) to a task that decodes it. There are ≥400 µs between bursts, more than enough to re-arm.
- **Logic:**
  - **At boot:** backlight on as early as possible. In direct mode (the one installed), `GPIO_BL` open-drain, released and with no internal pulls. In MOSFET mode, `GPIO_BL` LOW.
  - **On detecting `80 06 00 00 02` or `05`:** backlight OFF. In direct mode, `GPIO_BL` LOW; in MOSFET mode, HIGH.
  - **On detecting `80 06 00 00 01`:** backlight ON. In direct mode, `GPIO_BL` released; in MOSFET mode, LOW.
  - No debouncing needed: each event is a single write.
- **Serial debugging (115200):** in debug mode, print every decoded request (address, command, data), never from the ISR or the callback. Show the state change with the board's LED.
- **Watchdog** enabled. If something fails, the reset releases the pin and the backlight stays on: thanks to the external 10 kΩ pull-up in direct mode, or to the gate pull-down in MOSFET mode.

## 6. Real sequences observed (macOS, Mac mini and MacBook)

**Display sleep** (`pmset displaysleepnow`). macOS waits **~10 s with the screen black** before sending D3:

```
 3.00 s   (screen black)
13.10 s   Native WRITE 0x00600 data=02        ← backlight off
35.04 s   Native READ  0x00600 len=1          ← on wake
35.04 s   Native WRITE 0x00600 data=01        ← backlight on
35.05 s   link training: WRITE 0x107=00, 0x100=0a, 0x101=82, 0x102=21/22/00, READ 0x202...
35.12 s   READ 0x6921D, 0x68028 (Apple-specific registers), WRITE 0x20F=07, 0x2011=07
```

**System sleep:** `WRITE 0x00600 data=02` at the moment it goes to sleep, then total silence.

**Cable connect / system wake:** DPCD capability read (`0x000` len 16, `0x400`, `0x500`...), EDID read over I2C-over-AUX (`@0x50`, 8 × 16 bytes), probe of `@0x52` (NACK), `WRITE 0x00600 data=01` and link training.

**Cable disconnect:** generates nothing on AUX. In practice, if the ESP32 is powered from the adapter, it powers down along with it.

## 7. Test vectors

The file `testvectors_0x600.json` holds the **real runs** (`[level, duration_ns]`, captured at 8 MHz, so the resolution is 125 ns) of three bursts:

| t (s) | Request bytes | Meaning |
|---|---|---|
| 13.100 | `80 06 00 00 02` | WRITE 0x600 = 02 (request + ACK reply in the same burst) |
| 35.044 | `90 06 00 00` | READ 0x600 (with its reply) |
| 35.044 | `80 06 00 00 01` | WRITE 0x600 = 01 (request + ACK reply) |

They're for testing the decoder in C without hardware: convert the runs into an array, pass them through the decoding function and check that exactly those bytes and the right action come out.

## 8. Phase 2: panel VCC cut

**Status:** verified on real hardware (2026-11-02): panel VCC is cut ~2 s after
display sleep and the panel comes back on wake. The long-idle flicker test
(panel asleep >20 min) is still pending, so the flicker fix is **to be confirmed**.

**Why:** on the LG SPB1 panel, leaving VCC on with no video for ~20 min makes
two corners flicker for several minutes after wake (probably DC/VCOM charge
build-up). Real laptops cut panel VCC after D3; this does the same.

```
 adapter 3.3 V ──┬── ESP32 / MAX485 / pull-ups
                 │
                 └── FDC606P source (pin 4)
                          drain (pins 1,2,5,6) ──── panel VCC (connector pins 18–21, panel 12–13)
                          gate  (pin 3) ──┬──── PIN_PANEL_VCC (GPIO10 on the C3)
                                         10 kΩ
                                          │
                                         GND
 (the original VCC trace is cut: adapter side -> source, panel side -> drain)
```

- **Switch:** FDC606P, P-channel MOSFET, SuperSOT-6, package marking **606G**,
  salvaged from a MacBook logic board 820-2877-B (reference designator Q7930).
  Pinout: **1, 2, 5, 6 = drain; 3 = gate; 4 = source.**
- **Source** goes to the adapter's 3.3 V (the rail before the cut, the same
  one feeding the ESP32, the MAX485 and the pull-ups). **Drain** goes to panel
  VCC.
- **Gate LOW = panel powered; gate HIGH = panel VCC off.**
- **10 kΩ pull-down on the gate:** with the ESP32 absent, in reset or still
  booting, the GPIO floats; the pull-down holds the gate LOW so the panel stays
  powered. Without it, a floating gate could leave the panel off.
- **HPD trace cut:** the source-side HPD is pulled up to 3.3 V through 10 kΩ and
  the panel-side HPD is left floating. The source then always sees a display
  and keeps talking AUX while the panel is unpowered. HPD is panel pin 17,
  i.e. pin 27 of the 40-pin connector.

Behavior:

1. A write of 02/05 to DPCD 0x600 turns the backlight off (as before). After
   `PANEL_VCC_OFF_DELAY_MS` (default 2000), `PIN_PANEL_VCC` goes HIGH, unless a
   D0 (0x600 = 01) arrived in the meantime.
2. While VCC is off, **any AUX burst** (the unpowered panel can't talk, so it is
   the source) sets `PIN_PANEL_VCC` LOW immediately. The backlight stays off
   until the usual 0x600 = 01; it is never turned on while VCC is off.
3. Each new 02 repeats the cycle.

`PIN_PANEL_VCC = -1` disables the feature (no GPIO touched); that is the
default on the S3 and WROOM builds. On the C3 it is GPIO10 (not a strapping or
USB pin). The sequencing logic is in `panelvcc.h/.cpp` (pure, tested in
`test/test_panelvcc.cpp`). In debug mode Serial prints `panel VCC off` /
`panel VCC on (AUX activity)`, and `stats` includes `vcc_off` / `vcc_on`
counters, to see whether macOS polls AUX during sleep (which would wake the
panel repeatedly).

## 9. Ideas for later (out of scope)

- **PWM brightness:** cut the 2 kHz PWM trace (it comes out of pin 5 of the 8-pin MCU sitting above the jog wheel) and have the ESP32 generate the PWM with LEDC, reading the wheel or the original PWM as the "requested brightness".
- **Control from the computer:** over WiFi (HTTP or MQTT), or by reading DDC/CI brightness commands (I2C-over-AUX to `@0x37`, VCP 0x10). The latter requires someone to answer with an ACK, otherwise macOS doesn't send them.
