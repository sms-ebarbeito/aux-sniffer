// DisplayPort AUX decoder (Manchester-II, 1 Mbps) working on runs of
// (level, duration). No Arduino or ESP-IDF dependencies: it builds the same
// on the ESP32 and on the host (test/). Port of auxdec.py, extended to
// extract several messages per burst (request + reply).
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace auxdec {

// AUX: 4 header bytes + up to 16 data bytes.
constexpr size_t kMaxMsgBytes = 20;
// Half-bits expanded per burst. If it fills up, the rest is dropped
// (equivalent to a truncated burst).
constexpr size_t kMaxHalfbits = 4096;
// Bursts with fewer runs than this are dropped as noise (SPEC §3).
constexpr size_t kMinRuns = 20;
// Maximum messages per burst (normally 2: request and reply).
constexpr size_t kMaxMsgs = 8;
// Half-bits assigned to the last run when its duration is unknown
// (dur_ns == 0: the level stayed put until the end of the burst).
constexpr uint8_t kFinalRunUnits = 4;

struct Run {
  uint8_t level;    // 0 or 1
  uint32_t dur_ns;  // 0 = final run of unknown duration
};

struct Message {
  uint8_t bytes[kMaxMsgBytes];
  uint8_t nbytes;
  uint16_t nbits;  // bits read; if not a multiple of 8, nbits % 8 are left over
  bool stopped;    // true if it ended in a STOP; false if the burst was cut before
  bool inverted;   // inverted polarity (A/B swapped)
};

enum class Action : uint8_t { None, BacklightOn, BacklightOff };

// Duration -> half-bits, round-half-even like Python's round(), capped at 8.
// Returns 0 for pulses < 250 ns (glitch: the run is dropped).
uint8_t quantize(uint32_t dur_ns);

// Converts RMT symbols (rmt_symbol_word_t::val) to runs. Word layout:
// duration0 bits 0-14, level0 bit 15, duration1 bits 16-30, level1 bit 31. A
// duration of 0 is the RMT end-of-burst marker: that run is emitted with
// dur_ns = 0 and conversion stops. If it's missing (truncated buffer), every
// run has a real duration. Returns the number of runs.
size_t runsFromRmt(const uint32_t* words, size_t nwords, uint32_t tick_ns,
                   Run* out, size_t max_runs);

class Decoder {
 public:
  // Decodes every message in a burst. Tries both polarities and keeps the
  // one that yields more whole-byte messages (tie: normal polarity). Returns
  // the number of messages written to out (at most kMaxMsgs).
  size_t decode(const Run* runs, size_t nruns, Message* out, size_t max_msgs);

 private:
  size_t expand(const Run* runs, size_t nruns);
  size_t extract(bool inverted, Message* out, size_t max_msgs) const;
  bool syncAt(size_t i, uint8_t flip) const;

  uint8_t hb_[kMaxHalfbits];
  Message alt_[kMaxMsgs];
  size_t nhb_ = 0;
};

// Action for a message: only "80 06 00 00 XX" of exactly 40 bits.
// XX=01 -> ON, XX=02/05 -> OFF, anything else -> None.
Action classify(const Message& m);

// Human-readable text for debugging (in the style of auxdec.py). Returns buf.
const char* describeRequest(const Message& m, char* buf, size_t len);
const char* describeReply(const Message& m, char* buf, size_t len);

}  // namespace auxdec
