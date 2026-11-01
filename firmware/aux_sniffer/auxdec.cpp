#include "auxdec.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace auxdec {

namespace {

constexpr size_t kPreambleMin = 8;  // minimum half-bits before the SYNC
constexpr uint8_t kSync[8] = {1, 1, 1, 1, 0, 0, 0, 0};

int good(const Message* m, size_t n) {
  int score = 0;
  for (size_t i = 0; i < n; i++)
    if (m[i].nbits >= 8 && m[i].nbits % 8 == 0) score++;
  return score;
}

// Cumulative snprintf that never overruns the buffer.
void append(char* buf, size_t len, size_t* pos, const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

void append(char* buf, size_t len, size_t* pos, const char* fmt, ...) {
  if (*pos >= len) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf + *pos, len - *pos, fmt, ap);
  va_end(ap);
  if (n > 0) *pos += (size_t)n;
}

void appendData(char* buf, size_t len, size_t* pos, const uint8_t* d, size_t n) {
  if (n == 0) return;
  append(buf, len, pos, " data=");
  for (size_t i = 0; i < n; i++) append(buf, len, pos, i ? " %02x" : "%02x", d[i]);
}

void appendTail(char* buf, size_t len, size_t* pos, const Message& m) {
  if (m.nbits % 8) append(buf, len, pos, "  [%u bits]", (unsigned)m.nbits);
  if (!m.stopped) append(buf, len, pos, "  [truncated]");
}

}  // namespace

uint8_t quantize(uint32_t dur_ns) {
  uint32_t q = dur_ns / 500, r = dur_ns % 500;
  if (r > 250 || (r == 250 && (q & 1))) q++;
  return q > 8 ? 8 : (uint8_t)q;
}

size_t runsFromRmt(const uint32_t* words, size_t nwords, uint32_t tick_ns,
                   Run* out, size_t max_runs) {
  size_t n = 0;
  for (size_t i = 0; i < nwords; i++) {
    for (int half = 0; half < 2; half++) {
      uint32_t v = half ? words[i] >> 16 : words[i] & 0xFFFF;
      uint32_t dur = v & 0x7FFF;
      if (n >= max_runs) return n;
      out[n].level = (uint8_t)(v >> 15);
      out[n].dur_ns = dur * tick_ns;
      n++;
      if (dur == 0) return n;
    }
  }
  return n;
}

size_t Decoder::expand(const Run* runs, size_t nruns) {
  nhb_ = 0;
  for (size_t i = 0; i < nruns; i++) {
    uint8_t u = runs[i].dur_ns ? quantize(runs[i].dur_ns) : kFinalRunUnits;
    for (uint8_t k = 0; k < u && nhb_ < kMaxHalfbits; k++) hb_[nhb_++] = runs[i].level;
  }
  return nhb_;
}

bool Decoder::syncAt(size_t i, uint8_t flip) const {
  for (int k = 0; k < 8; k++)
    if ((hb_[i + k] ^ flip) != kSync[k]) return false;
  return true;
}

size_t Decoder::extract(bool inverted, Message* out, size_t max_msgs) const {
  const uint8_t flip = inverted ? 1 : 0;
  size_t nmsg = 0, start = 0;
  while (nmsg < max_msgs) {
    // SYNC = 1111 0000 with at least kPreambleMin half-bits before it,
    // counted from the start of the burst or from the previous message's
    // STOP (so the STOP itself, which has the same pattern, doesn't count).
    size_t i = start + kPreambleMin;
    while (i + 8 <= nhb_ && !syncAt(i, flip)) i++;
    if (i + 8 > nhb_) break;
    i += 8;

    Message& m = out[nmsg];
    memset(&m, 0, sizeof(m));
    m.inverted = inverted;
    while (i + 1 < nhb_) {
      uint8_t a = hb_[i] ^ flip, b = hb_[i + 1] ^ flip;
      if (a == b) {
        m.stopped = true;
        break;
      }
      size_t byte = m.nbits / 8;
      if (byte < kMaxMsgBytes) m.bytes[byte] = (uint8_t)(m.bytes[byte] << 1 | a);
      if (m.nbits < 0xFFFF) m.nbits++;
      i += 2;
    }
    size_t full = m.nbits / 8;
    m.nbytes = (uint8_t)(full < kMaxMsgBytes ? full : kMaxMsgBytes);
    nmsg++;
    if (!m.stopped) break;
    start = i;
  }
  return nmsg;
}

size_t Decoder::decode(const Run* runs, size_t nruns, Message* out, size_t max_msgs) {
  if (nruns < kMinRuns || max_msgs == 0) return 0;
  expand(runs, nruns);
  if (max_msgs > kMaxMsgs) max_msgs = kMaxMsgs;
  size_t n = extract(false, out, max_msgs);
  size_t na = extract(true, alt_, max_msgs);
  if (good(alt_, na) > good(out, n)) {
    memcpy(out, alt_, na * sizeof(Message));
    return na;
  }
  return n;
}

Action classify(const Message& m) {
  if (m.nbits != 40) return Action::None;
  const uint8_t* b = m.bytes;
  if (b[0] != 0x80 || b[1] != 0x06 || b[2] != 0x00 || b[3] != 0x00) return Action::None;
  switch (b[4]) {
    case 0x01: return Action::BacklightOn;
    case 0x02:
    case 0x05: return Action::BacklightOff;
    default: return Action::None;
  }
}

const char* describeRequest(const Message& m, char* buf, size_t len) {
  size_t pos = 0;
  buf[0] = '\0';
  const uint8_t* by = m.bytes;
  if (m.nbytes < 3) {
    append(buf, len, &pos, "??");
    for (size_t i = 0; i < m.nbytes; i++) append(buf, len, &pos, " %02x", by[i]);
    appendTail(buf, len, &pos, m);
    return buf;
  }
  unsigned cmd = by[0] >> 4;
  unsigned long addr = ((unsigned long)(by[0] & 0xF) << 16) | (by[1] << 8) | by[2];
  bool write = (cmd & 3) == 0;
  const uint8_t* data = by + 4;
  size_t ndata = (write && m.nbytes > 4) ? m.nbytes - 4 : 0;
  if (cmd & 0x8) {
    const char* kind = write ? "Native WRITE" : (cmd & 3) == 1 ? "Native READ " : "Native cmd? ";
    append(buf, len, &pos, "%s 0x%05lX", kind, addr);
    if (m.nbytes > 3) append(buf, len, &pos, " len=%u", by[3] + 1u);
  } else {
    static const char* const ops[] = {"I2C WRITE   ", "I2C READ    ", "I2C WRSTAT  ", "I2C ?       "};
    append(buf, len, &pos, "%s @0x%02lX%s", ops[cmd & 3], addr & 0x7F, (cmd & 4) ? " MOT" : "");
    if (m.nbytes == 3)
      append(buf, len, &pos, " (addr-only)");
    else
      append(buf, len, &pos, " len=%u", by[3] + 1u);
  }
  appendData(buf, len, &pos, data, ndata);
  appendTail(buf, len, &pos, m);
  return buf;
}

const char* describeReply(const Message& m, char* buf, size_t len) {
  size_t pos = 0;
  buf[0] = '\0';
  if (m.nbytes == 0) {
    append(buf, len, &pos, "REPLY (empty)");
    appendTail(buf, len, &pos, m);
    return buf;
  }
  static const char* const st[] = {"ACK", "NACK", "DEFER", "?"};
  unsigned n = m.bytes[0] >> 4;
  if (n >> 2)
    append(buf, len, &pos, "REPLY %s/I2C_%s", st[n & 3], st[(n >> 2) & 3]);
  else
    append(buf, len, &pos, "REPLY %s", st[n & 3]);
  appendData(buf, len, &pos, m.bytes + 1, m.nbytes - 1u);
  appendTail(buf, len, &pos, m);
  return buf;
}

}  // namespace auxdec
