// Panel VCC sequencing (phase 2): pure state machine, no Arduino or ESP-IDF
// dependencies, time is injected. It builds the same on the ESP32 and on the
// host (test/).
//
// Rules:
//   - sleep (DPCD 0x600 = 02/05): after offDelayMs, panel VCC goes off, unless
//     a D0 (0x600 = 01) arrives first.
//   - while VCC is off, any AUX burst turns it back on immediately (the
//     unpowered panel cannot talk, so it can only be the source).
//   - the backlight must never be on while VCC is off.
//   - disabled (enabled = false): nothing ever happens.
#pragma once

#include <stdint.h>

namespace panelvcc {

class Sequencer {
 public:
  // enabled = false turns the whole feature into a no-op.
  Sequencer(bool enabled, uint32_t offDelayMs) : enabled_(enabled), delay_(offDelayMs) {}

  // All of these return true if the VCC state changed (the caller must then
  // drive the pin to vccOff()). now_ms is a free-running millisecond counter;
  // wrap-around is handled.

  // Any AUX burst. Turns VCC back on if it was off.
  bool onBurst(uint32_t now_ms);
  // DPCD 0x600 written with a sleep value (02/05): schedules the cut.
  bool onSleep(uint32_t now_ms);
  // DPCD 0x600 = 01 (D0): cancels a pending cut.
  bool onWake(uint32_t now_ms);
  // Periodic call: executes the cut once the delay has elapsed.
  bool tick(uint32_t now_ms);

  bool enabled() const { return enabled_; }
  bool vccOff() const { return vccOff_; }
  bool cutPending() const { return pending_; }
  // False while VCC is off: the backlight must stay off.
  bool backlightAllowed() const { return !vccOff_; }

  uint32_t offCount() const { return offCount_; }
  uint32_t onCount() const { return onCount_; }

 private:
  bool enabled_;
  uint32_t delay_;
  bool vccOff_ = false;
  bool pending_ = false;
  uint32_t deadline_ = 0;
  uint32_t offCount_ = 0, onCount_ = 0;
};

}  // namespace panelvcc
