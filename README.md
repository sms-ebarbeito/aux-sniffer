# aux-sniffer

ESP32 firmware that passively listens to the DisplayPort AUX channel and
turns the panel backlight off or on according to the host's writes to the
DPCD register `0x600` (SET_POWER). The full specification is in
[SPEC.md](SPEC.md). The main board is the **ESP32-C3 SuperMini**; ESP32
(WROOM) and ESP32-S3 build too, but are secondary.

```
firmware/aux_sniffer/
  aux_sniffer.ino   RMT RX, tasks, backlight, Serial
  config.h          the ONLY file to touch: board, pins, backlight mode, RMT
  auxdec.h/.cpp     AUX decoder with no Arduino dependencies
  panelvcc.h/.cpp   panel VCC cut sequencing (pure, host-tested)
test/               host test (clang) with the real bursts
build.sh            builds for c3 / s3 / wroom
```

## Decoder test (on the Mac, no hardware)

```sh
make -C test
```

Runs the three real bursts from `testvectors_0x600.json` and checks the
request bytes, the reply (ACK) and the action. It also tests inverted
polarity, the full RMT-symbol path at 10 and 20 MHz, ±100 ns of jitter, two
transactions in the same burst, and random noise. On top of that, it **cuts
each burst at every possible point**, by run and by RMT symbol, and checks
that a wrong action never comes out and that, once the request is complete,
the right one always does:

```
t=13.100240s 80 06 00 00 02    correct action from 139/193 runs,  70/97 RMT symbols
t=35.044497s 80 06 00 00 01    correct action from 140/195 runs,  70/98 RMT symbols
```

## Building

```sh
./build.sh          # ESP32-C3 SuperMini (default)
./build.sh all      # c3, s3 and wroom
```

The script uses these FQBNs:

| Board | FQBN |
|---|---|
| C3 SuperMini | `esp32:esp32:esp32c3:CDCOnBoot=cdc` |
| S3 | `esp32:esp32:esp32s3:CDCOnBoot=cdc` |
| WROOM | `esp32:esp32:esp32` |

On the SuperMini, **`CDCOnBoot=cdc` is required**: the board has no
USB-serial chip and Serial goes out over native USB (GPIO18/19). The binary
ends up in `build/<board>/`.

To change the configuration without editing `config.h`, for example to use
MOSFET mode:

```sh
EXTRA="-DBL_MODE=BL_MODE_MOSFET" ./build.sh
```

> **Apple Silicon without Rosetta:** the `ctags` that arduino-cli installs is
> x86_64 and doesn't run. `build.sh` detects this and uses
> `tools/ctags-noop`, which does nothing. The sketch doesn't need the
> prototypes ctags generates. If Rosetta gets installed
> (`softwareupdate --install-rosetta`), the script goes back to the regular
> ctags on its own. To build by hand without the script, add
> `--build-property "tools.ctags.path=$PWD/tools/ctags-noop"`.

## Flashing

```sh
arduino-cli board list          # find the port, e.g. /dev/cu.usbmodem1101
./build.sh c3
arduino-cli upload -p /dev/cu.usbmodem1101 \
  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc --input-dir build/c3
arduino-cli monitor -p /dev/cu.usbmodem1101 -c baudrate=115200
```

If the SuperMini doesn't enter download mode, hold **BOOT** (GPIO9), tap
**RESET** and release BOOT. After flashing, a RESET boots it normally.

## Wiring (C3 SuperMini)

### AUX receiver (same as in the SPEC)

```
AUX+ (eDP pin 15) ──1kΩ──||10nF──► A ┐
AUX- (eDP pin 16) ──1kΩ──||10nF──► B ├ MAX485 at 3.3 V (R7, R5 and R6 removed)
                RE, DE ──► GND       │
                RO ──────────────────┴──► GPIO4 (PIN_AUX_IN)
MAX485 VCC = 3V3 · common GND
```

If A and B end up swapped, nothing breaks: the decoder detects the polarity
on every burst.

### Backlight: final wiring (direct mode, `BL_MODE_DIRECT_OD`, the default)

Installed and tested: the backlight turns off on display sleep and comes back
on wake.

```
                              BL_EN trace
 adapter (original) ──────────────╳──────────┬─────────────┬──── panel BL_EN
                                 cut         │             │
                                           10 kΩ         GPIO5 (PIN_BL)
                                  (external, panel side)   open-drain, no pulls
                                             │
                                     adapter 3.3 V

adapter 3.3 V ──► SuperMini 3V3 and MAX485 VCC
adapter GND ────► GND
```

- **The BL_EN trace is cut.** The adapter no longer drives BL_EN: it is set
  only by the external 10 kΩ pull-up, soldered on the panel side, and by
  GPIO5.
- **Backlight OFF:** the GPIO drives LOW and sinks about 0.33 mA through the
  10 kΩ.
- **Backlight ON:** the GPIO is released (open-drain, high impedance) and the
  pull-up takes BL_EN to 3.3 V.
- **Why the pull-up is external:** an internal ESP32 pull-up only exists
  while the firmware is running and has configured it. During reset, the
  bootloader or flashing, after a hang, or with the chip held in reset, the
  pin is in its default state and BL_EN would float. The 10 kΩ doesn't depend
  on the ESP32, so **in any of those cases the backlight stays on**. That's
  why the firmware configures the pin **with no internal pulls**, in
  `initVariant()`, before `setup()`. On a watchdog, a panic or an
  `esp_restart()`, the pin is released again.
- **The ESP32 must be powered from the same 3.3 V as the pull-up.** If the
  ESP32 lost power while the adapter stayed on (for example, with the 3V3
  wire disconnected), the GPIO protection diode would pull BL_EN down to about
  0.6 V through the 10 kΩ and the backlight would turn off. The SuperMini is
  powered through its **3V3** pin, bypassing its regulator.

### Backlight: MOSFET alternative (`BL_MODE_MOSFET`, not installed)

```
GPIO5 (PIN_BL) ──100Ω──┬── 2N7002 gate
                      100kΩ             drain ── BL_EN (with its pull-up)
                       │                source ── GND
                      GND
```

- **HIGH:** the MOSFET conducts and the **backlight turns off**.
- **LOW, or the pin unconfigured:** the 100 kΩ pull-down keeps the
  **backlight on**.

Useful if the ESP32 has its own power supply, with a common GND. Build it
with `EXTRA="-DBL_MODE=BL_MODE_MOSFET" ./build.sh`.

### Panel VCC cut (phase 2, `PIN_PANEL_VCC`)

**Why:** on the LG SPB1 panel, leaving VCC on with no video for ~20 min makes
two corners flicker for several minutes after wake (probably DC/VCOM charge
build-up). Real laptops cut panel VCC after D3; this does the same.

```
 adapter 3.3 V ──┬── ESP32 / MAX485 / pull-ups
                 │
                 └── FDC606P source (pin 4)
                          drain (pins 1,2,5,6) ──── panel VCC (eDP pins 12–13)
                          gate  (pin 3) ──┬──── PIN_PANEL_VCC (GPIO10 on the C3)
                                         10 kΩ
                                          │
                                         GND
 (the original VCC trace is cut: adapter side -> source, panel side -> drain)
```

- **Switch:** FDC606P, P-channel, SuperSOT-6, reused from a donor board.
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
  and keeps talking AUX while the panel is unpowered. Which connector pin is
  HPD (17 or 21) is **to be confirmed** (not yet measured).

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

### LED

The SuperMini's blue LED is on GPIO8, active low. **Lit means the ESP32 is
turning the backlight off.** It blinks 3 times at boot. GPIO8 is a strapping
pin: it's only used as an output after boot.

## What you see on Serial (115200, debug mode)

Debug output is on by default (`AUX_DEBUG 1` in `config.h`). Console
commands: **`d`** toggles debug, **`s`** prints statistics and **`h`** repeats
the header.

Example built from the sequences in SPEC §6 (not yet a real capture from the
ESP32):

```
aux-sniffer | ESP32-C3 SuperMini | AUX_IN=GPIO4 BL=GPIO5 (direct open-drain, LOW=OFF) LED=GPIO8
RMT 10000000 Hz, mem 96 symbols, buffer 4 x 512, filter 150 ns, end of burst 10000 ns
debug=on  (keys: d=debug on/off, s=stats, h=help)
[     13.100] REQ  Native WRITE 0x00600 len=1 data=02
[     13.100]      REPLY ACK
[     13.100] >>> BACKLIGHT OFF
[     35.043] REQ  Native READ  0x00600 len=1
[     35.043]      REPLY ACK data=02
[     35.044] REQ  Native WRITE 0x00600 len=1 data=01
[     35.044]      REPLY ACK
[     35.044] >>> BACKLIGHT ON
[     35.050] REQ  Native WRITE 0x00100 len=1 data=0a
...
stats: bursts=412 noise=0 no_sync=0 truncated=0 messages=824 dropped=0 log_lost=0 rearms=0 | backlight=ON rmt=ok
```

- **Time:** seconds since boot.
- **`REQ`:** the source's request. The next line is the panel's reply, which
  arrives in the same burst.
- **`>>> BACKLIGHT`:** always printed, even with debug off.
- **`?? burst without SYNC`:** a burst of 20 runs or more that couldn't be
  decoded.
- **`stats`:** printed every 10 s if there was traffic. The counters are:
  - `truncated`: bursts that didn't fit in the buffer.
  - `dropped`: bursts lost because the queue was full.
  - `log_lost`: debug lines that weren't printed. Never affects the action.
  - `rearms`: times the RMT had to be restarted.

Serial output is printed from `loop()`, which runs at low priority, through a
queue. If the port is slow or nobody is reading it, log lines are lost, but
backlight control is never delayed.

## How a burst fits in the C3 RMT

The C3 RMT has 2 RX channels with 48 symbols of memory each and **no DMA**. A
write with its ACK takes about 98 symbols, and the request alone, 70.

1. **Two chained blocks.** The RX channel uses `mem_block_symbols = 96`, i.e.
   its own block plus the other RX channel's, which is left unused.
2. **Driver ping-pong.** Core 3.3.10 ships ESP-IDF v5.5.4, which on targets
   with `SOC_RMT_SUPPORT_RX_PINGPONG` (C3 and S3) uses the RMT memory as a
   ping-pong buffer. On each half (48 symbols, about 50 to 100 µs of signal),
   an interrupt copies the data into the user buffer, which holds 512
   symbols. That way the whole burst fits, even a 16-byte read, which is
   around 210 symbols. This was checked in `soc_caps.h` and in the symbols of
   `libesp_driver_rmt.a` (`ping_pong_symbols`, `en_partial_rx`).
   `flags.en_partial_rx` (also available) isn't used: it's meant for when the
   user buffer is smaller than the burst, which isn't the case here.
3. **Tolerance to cuts.** If for any reason the burst gets cut, the decoder
   processes whatever arrived. For example, if ping-pong fell short and only
   the 96 symbols of RMT memory were left, that's still enough: the request
   is complete at symbol 70. The host test checks this by cutting at every
   symbol.

To receive bursts without losing any, the ISR (`on_recv_done`) re-arms the
RMT right away with the next buffer from a ring of 4 and hands the burst to a
high-priority task through a queue. That task decodes it and drives the pin.

## Hardware status

**Tested** with the ESP32-C3 SuperMini and the final wiring (direct mode with
an external 10 kΩ pull-up): the backlight turns off on display sleep and
comes back on wake.

**Not verified yet:**

- **System sleep.** SPEC §6 says it sends the same `WRITE 0x600 = 02`.
- **The `truncated` counter.** It would show whether the RMT ping-pong
  delivers complete bursts for long reads too. The `0x600` write already
  decodes correctly.
- **WROOM and S3.** They build but haven't been tested. On the WROOM, which
  has no ping-pong, bursts longer than 256 symbols (EDID reads) should get
  truncated without affecting `0x600`.

## License

[MIT](LICENSE). Copyright (c) 2026 Enrique Barbeito.
