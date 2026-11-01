// Host test for the AUX decoder: runs the real bursts from
// testvectors_0x600.json through auxdec and checks bytes, reply and action.
//   make -C test        (builds with clang++ and runs)
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "auxdec.h"

using auxdec::Action;
using auxdec::Message;
using auxdec::Run;

// ---------------------------------------------------------------- mini JSON

struct Json {
  enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::vector<std::pair<std::string, Json>> obj;

  const Json& operator[](const char* key) const {
    for (auto& kv : obj)
      if (kv.first == key) return kv.second;
    fprintf(stderr, "JSON: missing key '%s'\n", key);
    exit(2);
  }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& s) : s_(s) {}
  Json parse() {
    Json v = value();
    ws();
    if (p_ != s_.size()) fail("trailing garbage");
    return v;
  }

 private:
  [[noreturn]] void fail(const char* what) {
    fprintf(stderr, "JSON: %s at position %zu\n", what, p_);
    exit(2);
  }
  void ws() {
    while (p_ < s_.size() && isspace((unsigned char)s_[p_])) p_++;
  }
  bool eat(char c) {
    ws();
    if (p_ < s_.size() && s_[p_] == c) {
      p_++;
      return true;
    }
    return false;
  }
  void expect(char c) {
    if (!eat(c)) fail("unexpected character");
  }
  std::string string() {
    expect('"');
    std::string out;
    while (p_ < s_.size() && s_[p_] != '"') {
      if (s_[p_] == '\\') {
        p_++;
        if (p_ >= s_.size()) fail("incomplete escape");
        char e = s_[p_];
        out += e == 'n' ? '\n' : e == 't' ? '\t' : e;  // enough for this file
      } else {
        out += s_[p_];
      }
      p_++;
    }
    if (p_ >= s_.size()) fail("unterminated string");
    p_++;
    return out;
  }
  Json value() {
    ws();
    if (p_ >= s_.size()) fail("unexpected end");
    Json v;
    char c = s_[p_];
    if (c == '{') {
      p_++;
      v.type = Json::Obj;
      if (eat('}')) return v;
      do {
        ws();
        std::string k = string();
        expect(':');
        v.obj.emplace_back(k, value());
      } while (eat(','));
      expect('}');
    } else if (c == '[') {
      p_++;
      v.type = Json::Arr;
      if (eat(']')) return v;
      do v.arr.push_back(value());
      while (eat(','));
      expect(']');
    } else if (c == '"') {
      v.type = Json::Str;
      v.str = string();
    } else if (s_.compare(p_, 4, "true") == 0 || s_.compare(p_, 5, "false") == 0) {
      v.type = Json::Bool;
      v.num = s_[p_] == 't';
      p_ += s_[p_] == 't' ? 4 : 5;
    } else if (s_.compare(p_, 4, "null") == 0) {
      p_ += 4;
    } else {
      char* end;
      v.type = Json::Num;
      v.num = strtod(s_.c_str() + p_, &end);
      if (end == s_.c_str() + p_) fail("invalid value");
      p_ = end - s_.c_str();
    }
    return v;
  }

  const std::string& s_;
  size_t p_ = 0;
};

// ---------------------------------------------------------------- helpers

static int g_checks = 0, g_fail = 0;

#define CHECK(cond, ...)                                    \
  do {                                                      \
    g_checks++;                                             \
    if (!(cond)) {                                          \
      g_fail++;                                             \
      printf("  FAIL %s:%d: %s\n    ", __FILE__, __LINE__, #cond); \
      printf(__VA_ARGS__);                                  \
      printf("\n");                                         \
    }                                                       \
  } while (0)

static std::string hex(const Message& m) {
  std::string s;
  char b[4];
  for (int i = 0; i < m.nbytes; i++) {
    snprintf(b, sizeof b, i ? " %02x" : "%02x", m.bytes[i]);
    s += b;
  }
  return s;
}

static const char* actionName(Action a) {
  return a == Action::BacklightOn ? "ON" : a == Action::BacklightOff ? "OFF" : "-";
}

static auxdec::Decoder g_dec;  // 4 KB+: better off the stack

static std::vector<Message> decodeRuns(const std::vector<Run>& runs) {
  Message out[auxdec::kMaxMsgs];
  size_t n = g_dec.decode(runs.data(), runs.size(), out, auxdec::kMaxMsgs);
  return std::vector<Message>(out, out + n);
}

// Resulting action of a burst: that of the last message that asks for one.
static Action burstAction(const std::vector<Message>& msgs) {
  Action a = Action::None;
  for (auto& m : msgs)
    if (auxdec::classify(m) != Action::None) a = auxdec::classify(m);
  return a;
}

// Packs runs the way the RMT would at tick_ns per tick. If end_marker, the
// last run is replaced by the end marker (duration 0), just like when the RMT
// stops on signal_range_max_ns.
static std::vector<uint32_t> packRmt(const std::vector<Run>& runs, uint32_t tick_ns, bool end_marker) {
  std::vector<uint32_t> halves;
  for (size_t i = 0; i < runs.size(); i++) {
    bool last = i + 1 == runs.size();
    uint32_t t = (last && end_marker) ? 0 : (runs[i].dur_ns + tick_ns / 2) / tick_ns;
    if (t > 0x7FFF) t = 0x7FFF;
    if (t == 0 && !(last && end_marker)) continue;  // the RMT filter swallows it
    halves.push_back((uint32_t)runs[i].level << 15 | t);
  }
  if (halves.size() % 2) halves.push_back(0);  // padding: duration 0, level 0
  std::vector<uint32_t> words;
  for (size_t i = 0; i < halves.size(); i += 2) words.push_back(halves[i] | halves[i + 1] << 16);
  return words;
}

static std::vector<Run> unpackRmt(const std::vector<uint32_t>& words, size_t nwords, uint32_t tick_ns) {
  std::vector<Run> runs(nwords * 2);
  runs.resize(auxdec::runsFromRmt(words.data(), nwords, tick_ns, runs.data(), runs.size()));
  return runs;
}

static uint32_t g_rng = 12345;
static uint32_t rnd() {  // deterministic LCG: tests are reproducible
  g_rng = g_rng * 1103515245u + 12345u;
  return g_rng >> 8;
}

// ---------------------------------------------------------------- vectors

struct Vector {
  std::string name, request, reply, meaning;
  Action action;
  std::vector<Run> runs;
};

// What each real burst must decode to (SPEC §7). The reply to each request
// was checked against auxdec.py.
struct Expect {
  const char* request;
  const char* reply;
  Action action;
};
static const Expect kExpect[] = {
    {"80 06 00 00 02", "00", Action::BacklightOff},
    {"90 06 00 00", "00 02", Action::None},
    {"80 06 00 00 01", "00", Action::BacklightOn},
};

static std::vector<Vector> loadVectors(const char* path) {
  std::ifstream f(path);
  if (!f) {
    fprintf(stderr, "cannot open %s\n", path);
    exit(2);
  }
  std::stringstream ss;
  ss << f.rdbuf();
  std::string text = ss.str();
  Json root = JsonParser(text).parse();
  std::vector<Vector> out;
  for (auto& fr : root["frames"].arr) {
    Vector v;
    v.request = fr["decoded_hex"].str;
    v.meaning = fr["meaning"].str;
    char name[64];
    snprintf(name, sizeof name, "t=%.6fs %s", fr["t_s"].num, v.request.c_str());
    v.name = name;
    bool found = false;
    for (auto& e : kExpect)
      if (v.request == e.request) {
        v.reply = e.reply;
        v.action = e.action;
        found = true;
      }
    if (!found) {
      fprintf(stderr, "vector without an expectation: %s\n", v.request.c_str());
      exit(2);
    }
    for (auto& r : fr["runs"].arr) v.runs.push_back({(uint8_t)r.arr[0].num, (uint32_t)r.arr[1].num});
    out.push_back(v);
  }
  return out;
}

// Checks that the decoded burst has the expected request, reply and action.
static void checkBurst(const Vector& v, const std::vector<Message>& msgs, const char* ctx) {
  CHECK(msgs.size() == 2, "[%s] %s: %zu messages (expected request + reply)", ctx, v.name.c_str(), msgs.size());
  if (msgs.size() < 2) return;
  CHECK(hex(msgs[0]) == v.request, "[%s] %s: request '%s'", ctx, v.name.c_str(), hex(msgs[0]).c_str());
  CHECK(msgs[0].nbits % 8 == 0 && msgs[0].stopped, "[%s] %s: request %u bits stopped=%d", ctx, v.name.c_str(),
        msgs[0].nbits, msgs[0].stopped);
  CHECK(hex(msgs[1]) == v.reply, "[%s] %s: reply '%s'", ctx, v.name.c_str(), hex(msgs[1]).c_str());
  CHECK(auxdec::classify(msgs[1]) == Action::None, "[%s] %s: the reply must not trigger an action", ctx, v.name.c_str());
  CHECK(burstAction(msgs) == v.action, "[%s] %s: action %s", ctx, v.name.c_str(), actionName(burstAction(msgs)));
}

// ---------------------------------------------------------------- tests

static void testQuantize() {
  printf("quantize\n");
  const struct { uint32_t ns; uint8_t u; } cases[] = {
      {100, 0}, {249, 0}, {250, 0}, {251, 1}, {375, 1}, {500, 1}, {625, 1}, {750, 2},
      {875, 2}, {1000, 2}, {1125, 2}, {1250, 2}, {1251, 3}, {1750, 4}, {1875, 4}, {2250, 4},
      {2625, 5}, {2750, 6}, {3875, 8}, {10000, 8}, {1000000, 8},
  };
  for (auto& c : cases)
    CHECK(auxdec::quantize(c.ns) == c.u, "quantize(%u) = %u, expected %u", c.ns, auxdec::quantize(c.ns), c.u);
}

static void testClassify() {
  printf("classify\n");
  auto mk = [](std::initializer_list<int> b, int nbits) {
    Message m{};
    for (int x : b) m.bytes[m.nbytes++] = (uint8_t)x;
    m.nbits = (uint16_t)nbits;
    m.stopped = true;
    return m;
  };
  CHECK(auxdec::classify(mk({0x80, 0x06, 0, 0, 0x01}, 40)) == Action::BacklightOn, "01");
  CHECK(auxdec::classify(mk({0x80, 0x06, 0, 0, 0x02}, 40)) == Action::BacklightOff, "02");
  CHECK(auxdec::classify(mk({0x80, 0x06, 0, 0, 0x05}, 40)) == Action::BacklightOff, "05");
  CHECK(auxdec::classify(mk({0x80, 0x06, 0, 0, 0x03}, 40)) == Action::None, "03");
  CHECK(auxdec::classify(mk({0x80, 0x06, 0, 0, 0x00}, 40)) == Action::None, "00");
  CHECK(auxdec::classify(mk({0x80, 0x06, 0, 0x01, 0x02}, 40)) == Action::None, "len 2");
  CHECK(auxdec::classify(mk({0x80, 0x06, 0x01, 0, 0x02}, 40)) == Action::None, "addr 0x601");
  CHECK(auxdec::classify(mk({0x90, 0x06, 0, 0, 0x02}, 40)) == Action::None, "read");
  CHECK(auxdec::classify(mk({0x80, 0x06, 0, 0, 0x02}, 43)) == Action::None, "extra bits");
  CHECK(auxdec::classify(mk({0x80, 0x06, 0, 0, 0x02, 0x00}, 48)) == Action::None, "6 bytes");
  CHECK(auxdec::classify(mk({0x80, 0x06, 0, 0}, 32)) == Action::None, "4 bytes");
}

static void testVectors(const std::vector<Vector>& vs) {
  printf("real vectors (runs in ns, as in the JSON)\n");
  char buf[128];
  for (auto& v : vs) {
    auto msgs = decodeRuns(v.runs);
    checkBurst(v, msgs, "direct");
    if (!msgs.empty()) {
      auxdec::describeRequest(msgs[0], buf, sizeof buf);
      CHECK(v.meaning == buf, "describeRequest '%s' != '%s'", buf, v.meaning.c_str());
      printf("  %-30s REQ %-36s | ", v.name.c_str(), buf);
      if (msgs.size() > 1) printf("%s", auxdec::describeReply(msgs[1], buf, sizeof buf));
      printf("  -> %s\n", actionName(burstAction(msgs)));
    }
  }
}

static void testInverted(const std::vector<Vector>& vs) {
  printf("inverted polarity (A/B swapped)\n");
  for (auto& v : vs) {
    auto runs = v.runs;
    for (auto& r : runs) r.level ^= 1;
    auto msgs = decodeRuns(runs);
    checkBurst(v, msgs, "inverted");
    CHECK(!msgs.empty() && msgs[0].inverted, "%s: should detect inversion", v.name.c_str());
  }
}

static void testRmtPipeline(const std::vector<Vector>& vs) {
  printf("RMT pipeline (quantized to ticks, end marker, runsFromRmt)\n");
  for (uint32_t tick : {100u, 50u}) {
    for (auto& v : vs) {
      auto words = packRmt(v.runs, tick, true);
      auto runs = unpackRmt(words, words.size(), tick);
      CHECK(!runs.empty() && runs.back().dur_ns == 0, "%s: end marker missing", v.name.c_str());
      char ctx[32];
      snprintf(ctx, sizeof ctx, "rmt %u ns", tick);
      checkBurst(v, decodeRuns(runs), ctx);
    }
  }
}

static void testJitter(const std::vector<Vector>& vs) {
  const int kIter = 500;
  const int kJitter = 100;  // ±ns per run, on top of the real asymmetry
  printf("jitter ±%d ns per run, %d iterations per vector, 100 ns ticks\n", kJitter, kIter);
  for (auto& v : vs) {
    int bad = 0;
    for (int it = 0; it < kIter; it++) {
      auto runs = v.runs;
      for (auto& r : runs) r.dur_ns = (uint32_t)((int)r.dur_ns + (int)(rnd() % (2 * kJitter + 1)) - kJitter);
      auto words = packRmt(runs, 100, true);
      auto msgs = decodeRuns(unpackRmt(words, words.size(), 100));
      if (msgs.empty() || hex(msgs[0]) != v.request || burstAction(msgs) != v.action) bad++;
    }
    CHECK(bad == 0, "%s: %d/%d iterations failed", v.name.c_str(), bad, kIter);
  }
}

// Cuts the burst at every possible point. A wrong action must never come
// out; once the request is complete, the right one must always come out.
static void testTruncation(const std::vector<Vector>& vs) {
  printf("truncation (burst cut at every run / at every RMT symbol)\n");
  for (auto& v : vs) {
    // a) by runs, without end marker (that's how the RMT delivers a full buffer)
    size_t kmin = 0;
    int wrong = 0, late_miss = 0;
    for (size_t k = 0; k <= v.runs.size(); k++) {
      std::vector<Run> cut(v.runs.begin(), v.runs.begin() + k);
      Action a = burstAction(decodeRuns(cut));
      if (a != Action::None && a != v.action) wrong++;
      if (a == v.action && !kmin) kmin = k;
      if (kmin && a != v.action) late_miss++;
    }
    CHECK(wrong == 0, "%s: %d cuts gave a wrong action", v.name.c_str(), wrong);
    CHECK(late_miss == 0, "%s: %d cuts after the request lost the action", v.name.c_str(), late_miss);

    // b) by RMT symbols at 10 MHz
    auto words = packRmt(v.runs, 100, true);
    size_t smin = 0;
    wrong = late_miss = 0;
    for (size_t s = 0; s <= words.size(); s++) {
      Action a = burstAction(decodeRuns(unpackRmt(words, s, 100)));
      if (a != Action::None && a != v.action) wrong++;
      if (a == v.action && !smin) smin = s;
      if (smin && a != v.action) late_miss++;
    }
    CHECK(wrong == 0, "%s: %d RMT cuts gave a wrong action", v.name.c_str(), wrong);
    CHECK(late_miss == 0, "%s: %d RMT cuts after the request lost the action", v.name.c_str(), late_miss);

    if (v.action != Action::None) {
      // The request must fit comfortably in the C3 RMT memory (2 blocks of
      // 48 symbols), even if the driver's ping-pong failed.
      CHECK(smin > 0 && smin <= 96, "%s: the request needs %zu symbols", v.name.c_str(), smin);
      printf("  %-30s correct action from %3zu/%zu runs, %3zu/%zu RMT symbols\n", v.name.c_str(), kmin,
             v.runs.size(), smin, words.size());
    } else {
      printf("  %-30s no action at any cut (%zu runs, %zu symbols)\n", v.name.c_str(), v.runs.size(),
             words.size());
    }
  }
}

static void testConcatenated(const std::vector<Vector>& vs) {
  printf("two transactions in the same burst (READ + WRITE 01 at t=35.044)\n");
  const Vector* rd = nullptr;
  const Vector* wr = nullptr;
  for (auto& v : vs) {
    if (v.request == "90 06 00 00") rd = &v;
    if (v.request == "80 06 00 00 01") wr = &v;
  }
  CHECK(rd && wr, "missing vectors");
  if (!rd || !wr) return;
  auto runs = rd->runs;
  runs.back().dur_ns = 3000;  // short gap between transactions
  runs.insert(runs.end(), wr->runs.begin(), wr->runs.end());
  auto msgs = decodeRuns(runs);
  CHECK(msgs.size() == 4, "%zu messages, expected 4", msgs.size());
  if (msgs.size() == 4) {
    CHECK(hex(msgs[0]) == "90 06 00 00" && hex(msgs[1]) == "00 02" && hex(msgs[2]) == "80 06 00 00 01" &&
              hex(msgs[3]) == "00",
          "'%s' '%s' '%s' '%s'", hex(msgs[0]).c_str(), hex(msgs[1]).c_str(), hex(msgs[2]).c_str(),
          hex(msgs[3]).c_str());
  }
  CHECK(burstAction(msgs) == Action::BacklightOn, "action %s", actionName(burstAction(msgs)));
}

static void testNoise() {
  printf("noise\n");
  std::vector<Run> shortBurst;
  for (int i = 0; i < 19; i++) shortBurst.push_back({(uint8_t)(i & 1), 500});
  CHECK(decodeRuns(shortBurst).empty(), "a 19-run burst should be dropped");

  const int kIter = 20000;
  int actions = 0;
  for (int it = 0; it < kIter; it++) {
    std::vector<Run> runs;
    size_t n = 20 + rnd() % 400;
    uint8_t lvl = rnd() & 1;
    for (size_t i = 0; i < n; i++, lvl ^= 1) runs.push_back({lvl, 150 + rnd() % 4000});
    if (burstAction(decodeRuns(runs)) != Action::None) actions++;
  }
  CHECK(actions == 0, "%d of %d random bursts triggered the backlight", actions, kIter);
}

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "../testvectors_0x600.json";
  auto vs = loadVectors(path);
  printf("%zu vectors from %s\n\n", vs.size(), path);
  CHECK(vs.size() == 3, "expected 3 vectors");

  testQuantize();
  testClassify();
  testVectors(vs);
  testInverted(vs);
  testRmtPipeline(vs);
  testJitter(vs);
  testTruncation(vs);
  testConcatenated(vs);
  testNoise();

  printf("\n%d checks, %d failures -> %s\n", g_checks, g_fail, g_fail ? "FAIL" : "OK");
  return g_fail ? 1 : 0;
}
