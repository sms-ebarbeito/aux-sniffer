// Host test for the panel VCC sequencer.   make -C test
#include <cstdint>
#include <cstdio>

#include "panelvcc.h"

using panelvcc::Sequencer;

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, ...)                          \
  do {                                            \
    g_checks++;                                   \
    if (!(cond)) {                                \
      g_fail++;                                   \
      printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      printf(__VA_ARGS__);                        \
      printf("\n");                               \
    }                                             \
  } while (0)

static void testSleepCutsAfterDelay() {
  Sequencer s(true, 2000);
  CHECK(!s.onSleep(1000), "sleep alone must not cut");
  CHECK(s.cutPending(), "cut pending");
  CHECK(!s.tick(2999), "too early");
  CHECK(!s.vccOff(), "still on");
  CHECK(s.tick(3000), "cut at the deadline");
  CHECK(s.vccOff() && !s.backlightAllowed(), "off, backlight not allowed");
  CHECK(!s.tick(9000), "no repeated event");
  CHECK(s.offCount() == 1, "off count");
}

static void testWakeCancels() {
  Sequencer s(true, 2000);
  s.onSleep(0);
  s.onWake(1500);
  CHECK(!s.cutPending(), "cancelled");
  CHECK(!s.tick(5000) && !s.vccOff(), "no cut after D0");
  CHECK(s.offCount() == 0, "no off events");
}

static void testBurstWakes() {
  Sequencer s(true, 2000);
  s.onSleep(0);
  s.tick(2000);
  CHECK(s.vccOff(), "off");
  CHECK(s.onBurst(2500), "burst turns VCC on");
  CHECK(!s.vccOff() && s.backlightAllowed(), "on");
  CHECK(!s.onBurst(2600), "second burst: no change");
  CHECK(s.onCount() == 1, "on count");
}

static void testBurstWhileOnIsNoop() {
  Sequencer s(true, 2000);
  CHECK(!s.onBurst(10), "burst while on");
  s.onSleep(0);
  CHECK(!s.onBurst(500) && s.cutPending(), "burst during delay doesn't cancel the cut");
}

static void testBacklightNeverOnWhileOff() {
  Sequencer s(true, 100);
  s.onSleep(0);
  for (uint32_t t = 0; t < 1000; t += 10) {
    s.tick(t);
    if (s.vccOff()) CHECK(!s.backlightAllowed(), "backlight allowed with VCC off at %u", t);
  }
  CHECK(s.vccOff(), "went off");
  s.onBurst(1000);  // the burst carrying D0 wakes VCC first, then D0
  CHECK(s.backlightAllowed(), "allowed once VCC is on");
}

static void testDisabled() {
  Sequencer s(false, 0);
  CHECK(!s.onSleep(0) && !s.cutPending(), "sleep ignored");
  CHECK(!s.tick(100000) && !s.vccOff(), "never cuts");
  CHECK(!s.onBurst(0) && !s.onWake(0), "nothing");
  CHECK(s.offCount() == 0 && s.onCount() == 0 && !s.enabled(), "counters");
}

static void testRepeatedCycles() {
  Sequencer s(true, 2000);
  uint32_t t = 0;
  for (int i = 1; i <= 5; i++) {
    s.onSleep(t);
    t += 2000;
    CHECK(s.tick(t) && s.vccOff(), "cycle %d off", i);
    t += 5000;
    CHECK(s.onBurst(t) && !s.vccOff(), "cycle %d on", i);
    s.onWake(t);
    t += 1000;
  }
  CHECK(s.offCount() == 5 && s.onCount() == 5, "counts %u/%u", s.offCount(), s.onCount());
}

static void testSleepWhileOffAndWrap() {
  Sequencer s(true, 2000);
  s.onSleep(0);
  s.tick(2000);
  CHECK(!s.onSleep(3000) && !s.cutPending(), "sleep while off: nothing to schedule");
  s.onBurst(4000);
  // millisecond counter wrap-around
  Sequencer w(true, 2000);
  w.onSleep(0xFFFFFF00u);
  CHECK(!w.tick(0xFFFFFFFFu), "before the deadline across the wrap");
  CHECK(w.tick(0x000007FFu) , "after the deadline across the wrap");
}

int main() {
  testSleepCutsAfterDelay();
  testWakeCancels();
  testBurstWakes();
  testBurstWhileOnIsNoop();
  testBacklightNeverOnWhileOff();
  testDisabled();
  testRepeatedCycles();
  testSleepWhileOffAndWrap();
  printf("%d checks, %d failures -> %s\n", g_checks, g_fail, g_fail ? "FAIL" : "OK");
  return g_fail ? 1 : 0;
}
