#include "panelvcc.h"

namespace panelvcc {

bool Sequencer::onBurst(uint32_t) {
  if (!enabled_ || !vccOff_) return false;
  vccOff_ = false;
  pending_ = false;
  onCount_++;
  return true;
}

bool Sequencer::onSleep(uint32_t now_ms) {
  if (!enabled_ || vccOff_ || pending_) return false;  // already off or scheduled
  pending_ = true;
  deadline_ = now_ms + delay_;
  return false;
}

bool Sequencer::onWake(uint32_t) {
  pending_ = false;
  return false;
}

bool Sequencer::tick(uint32_t now_ms) {
  if (!enabled_ || !pending_) return false;
  if ((int32_t)(now_ms - deadline_) < 0) return false;
  pending_ = false;
  vccOff_ = true;
  offCount_++;
  return true;
}

}  // namespace panelvcc
