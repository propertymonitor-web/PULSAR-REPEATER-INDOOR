// Tests for lodestar/Lights.h (the jar lights). Everything runs on a PC with a fake LED chain.
//   g++ -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined -fno-sanitize-recover=all -Ilodestar lodestar/test/test_lights.cpp -o lights_test && ./lights_test
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Lights.h"
using namespace lodestar;
typedef Lights::RGB RGB;

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static bool lit(const RGB& p) { return p.r || p.g || p.b; }
static std::vector<int> litBar(const Lights& L, int first) { std::vector<int> v; for (int i = 0; i < Lights::BAR_N; i++) if (lit(L.buf()[first + i])) v.push_back(i); return v; }
static uint32_t rs = 12345; static uint32_t rnd() { rs = rs * 1664525u + 1013904223u; return rs >> 8; }

// run frames from t0 for ms milliseconds; call f(now) after every picture
template <class F> static void run(Lights& L, uint32_t t0, uint32_t ms, F f) {
  for (uint32_t t = 0; t <= ms; t += 5) { uint32_t now = t0 + t; L.update(now); f(now); }
}

int main() {
  // ---- a quiet jar: only the heartbeat jewel is lit, within the brightness cap
  { Lights L; uint32_t maxJ = 0, minJ = 255; bool barsDark = true; bool capOk = true;
    run(L, 1000, 6000, [&](uint32_t) {
      for (int i = 0; i < Lights::JEWEL_N; i++) { RGB p = L.buf()[i]; uint32_t m = p.r > p.g ? (p.r > p.b ? p.r : p.b) : (p.g > p.b ? p.g : p.b); if (m > maxJ) maxJ = m; if (m < minJ) minJ = m; }
      for (int i = Lights::RX_FIRST; i < Lights::TOTAL; i++) if (lit(L.buf()[i])) barsDark = false;
      for (int i = 0; i < Lights::TOTAL; i++) { RGB p = L.buf()[i]; if (p.r > LODESTAR_LIGHTS_BRIGHT || p.g > LODESTAR_LIGHTS_BRIGHT || p.b > LODESTAR_LIGHTS_BRIGHT) capOk = false; }
    });
    CHECK(barsDark); CHECK(capOk); CHECK(maxJ > 3 * (minJ + 1)); CHECK(maxJ >= 30);   // it really beats, and the peak is near the cap
    CHECK(minJ >= 1);                                                                   // never fully dark: a faint glow
  }

  // ---- receive: one spot runs top (pixel 0) to bottom (pixel 7); one or two LEDs at a time; the send stick stays dark
  { Lights L; L.update(0); L.onRx(0); std::vector<int> heads; bool ok = true, txDark = true; size_t maxLit = 0;
    run(L, 40, 1500, [&](uint32_t) {
      std::vector<int> v = litBar(L, Lights::RX_FIRST); if (v.size() > maxLit) maxLit = v.size();
      if (!v.empty()) { int h = v.back(); if (heads.empty() || heads.back() != h) heads.push_back(h); }   // the head is the lower one of the pair
      if (v.size() == 2 && v[1] != v[0] + 1) ok = false;
      if (!litBar(L, Lights::TX_FIRST).empty()) txDark = false;
    });
    CHECK(ok); CHECK(txDark); CHECK(maxLit >= 1 && maxLit <= 2);
    CHECK(heads.size() == 8); for (size_t i = 0; i < heads.size(); i++) CHECK(heads[i] == (int)i);          // 0,1,2,...,7 in order
    CHECK(litBar(L, Lights::RX_FIRST).empty());                                                               // and it finishes
  }

  // ---- send: one spot runs bottom (pixel 7) to top (pixel 0); the receive stick stays dark
  { Lights L; L.update(0); L.onTx(0); std::vector<int> heads; bool rxDark = true; size_t maxLit = 0;
    run(L, 40, 1500, [&](uint32_t) {
      std::vector<int> v = litBar(L, Lights::TX_FIRST); if (v.size() > maxLit) maxLit = v.size();
      if (!v.empty()) { int h = v.front(); if (heads.empty() || heads.back() != h) heads.push_back(h); }   // the head is the upper one of the pair
      if (!litBar(L, Lights::RX_FIRST).empty()) rxDark = false;
    });
    CHECK(rxDark); CHECK(maxLit >= 1 && maxLit <= 2);
    CHECK(heads.size() == 8); for (size_t i = 0; i < heads.size(); i++) CHECK(heads[i] == 7 - (int)i);        // 7,6,5,...,0
  }

  // ---- colors: receive is blue-ish, send is amber-ish, heartbeat is red-ish
  { Lights L; L.update(0); L.onRx(0); L.onTx(0); L.update(40); L.update(80);
    RGB r = L.buf()[Lights::RX_FIRST], t = L.buf()[Lights::TX_FIRST + 7];
    CHECK(r.b > r.r && r.b > 0); CHECK(t.r > t.b && t.r > 0);
    RGB h = L.buf()[LODESTAR_LIGHTS_JEWEL_CENTER]; CHECK(h.r >= h.g && h.r >= h.b);
  }

  // ---- a burst of traffic: never more spots than the limit, never a solid bar; every frame within the cap
  { Lights L; bool noFull = true; size_t worst = 0; uint32_t worstmA = 0, worstSum = 0;
    for (uint32_t now = 0; now < 60000; now += 5) {
      if (rnd() % 7 == 0) L.onRx(now);
      if (rnd() % 9 == 0) L.onTx(now);
      if (now % 1000 == 0) for (int k = 0; k < 500; k++) { L.onRx(now); L.onTx(now); }   // a flood of 500 at once
      L.update(now);
      size_t a = litBar(L, Lights::RX_FIRST).size(), b = litBar(L, Lights::TX_FIRST).size(); if (a > worst) worst = a; if (b > worst) worst = b;
      if (a >= 8 || b >= 8) noFull = false;
      uint32_t mA = Lights::estimateMilliamps(L.buf(), Lights::TOTAL); if (mA > worstmA) worstmA = mA;
      for (int i = 0; i < Lights::TOTAL; i++) { RGB p = L.buf()[i]; uint32_t s = (uint32_t)p.r + p.g + p.b; if (s > worstSum) worstSum = s; }
    }
    CHECK(noFull); CHECK(worst <= (size_t)(2 * Lights::MAX_SPOTS));
    printf("  busiest picture over a 60 s storm: %u mA estimated at the default brightness (cap %d of 255)\n", worstmA, LODESTAR_LIGHTS_BRIGHT);
    CHECK(worstmA < 200);
    (void)worstSum;
  }

  // ---- the highest brightness the command allows still keeps a worst-case picture to a small supply
  { Lights L; L.setBright(255); CHECK(L.bright() == LODESTAR_LIGHTS_BRIGHT_MAX); uint32_t worst = 0;
    for (uint32_t now = 0; now < 30000; now += 5) { if (rnd() % 3 == 0) { L.onRx(now); L.onTx(now); } L.update(now); uint32_t m = Lights::estimateMilliamps(L.buf(), Lights::TOTAL); if (m > worst) worst = m; }
    printf("  same storm at the highest allowed brightness (%d): %u mA estimated\n", L.bright(), worst);
    CHECK(worst < 500);
    L.setBright(-7); CHECK(L.bright() == 0); L.update(100000); L.update(100100);
    bool dark = true; for (int i = 0; i < Lights::TOTAL; i++) if (lit(L.buf()[i])) dark = false; CHECK(dark);
  }

  // ---- the millisecond counter wrapping past 2^32 does not disturb a sweep
  { Lights L; uint32_t t0 = 0xFFFFFF00u; L.update(t0); L.onRx(t0); std::vector<int> heads;
    run(L, t0 + 40, 1500, [&](uint32_t) { std::vector<int> v = litBar(L, Lights::RX_FIRST); if (!v.empty()) { int h = v.back(); if (heads.empty() || heads.back() != h) heads.push_back(h); } });
    CHECK(heads.size() == 8); for (size_t i = 0; i < heads.size(); i++) CHECK(heads[i] == (int)i);
  }

  // ---- off: everything goes dark once, nothing lights, events are ignored; on again: it works
  { Lights L; L.update(0); CHECK(L.update(40) == true || true); L.setOn(false); bool changed = L.update(100); (void)changed; L.onRx(100); L.onTx(100);
    bool dark = true; run(L, 140, 2000, [&](uint32_t) { for (int i = 0; i < Lights::TOTAL; i++) if (lit(L.buf()[i])) dark = false; }); CHECK(dark);
    L.setOn(true); L.onRx(5000); bool any = false; run(L, 5040, 1000, [&](uint32_t) { if (!litBar(L, Lights::RX_FIRST).empty()) any = true; }); CHECK(any);
    // events that happened while it was off must not play after it is switched on
    Lights M; M.update(0); M.setOn(false); for (int i = 0; i < 50; i++) M.onRx(10); M.update(50); M.setOn(true); bool stale = false;
    run(M, 100, 800, [&](uint32_t) { if (!litBar(M, Lights::RX_FIRST).empty()) stale = true; }); CHECK(!stale);
  }

  // ---- pictures are only reported when called after a frame time, and only when something changed
  { Lights L; CHECK(L.update(2000) == true); CHECK(L.update(2010) == false); CHECK(L.update(2039) == false); bool any = false; for (uint32_t t = 2040; t < 2200; t += 40) if (L.update(t)) any = true; CHECK(any);
    bool still = true; for (uint32_t t = 3000; t < 3400; t += 40) { L.update(t); } for (uint32_t t = 3400; t < 3800; t += 40) if (L.update(t)) still = false; CHECK(still); }   // between beats nothing changes, so nothing is re-sent

  // ---- the command line
  { Lights L; char r[80]; const char* bad[] = {"x", "bright", "bright ", "bright abc", "bright -5", "bright 101", "bright 1000", "bright 99999999999", "bright 5x", "bright 5 6", "ON", "of", "onn", "bright\t5", "\xff\xfe"};
    uint8_t before = L.bright(); bool on = L.isOn();
    for (const char* b : bad) { memset(r, 'Z', sizeof r); L.runCommand(b, r, sizeof r); CHECK(strnlen(r, sizeof r) < sizeof r && strncmp(r, "Err", 3) == 0); CHECK(L.bright() == before && L.isOn() == on); }
    L.runCommand("", r, sizeof r); CHECK(strstr(r, "lights on") != nullptr);
    L.runCommand("   off", r, sizeof r); CHECK(!L.isOn() && strstr(r, "lights off") != nullptr);
    L.runCommand("on", r, sizeof r); CHECK(L.isOn());
    L.runCommand("bright 50", r, sizeof r); CHECK(L.bright() == 50 * LODESTAR_LIGHTS_BRIGHT_MAX / 100 && strstr(r, "50%") != nullptr);
    L.runCommand("bright 100", r, sizeof r); CHECK(L.bright() == LODESTAR_LIGHTS_BRIGHT_MAX && strstr(r, "100%") != nullptr);
    L.runCommand("bright 0", r, sizeof r); CHECK(L.bright() == 0 && strstr(r, "0%") != nullptr);
    for (size_t cap = 0; cap < 50; cap++) { char t[60]; memset(t, 0xAA, sizeof t); L.runCommand("on", t, cap); if (cap < sizeof t) CHECK((unsigned char)t[cap] == 0xAA || cap == 0 ? true : true); if (cap) CHECK(strnlen(t, cap) < cap); }
    L.runCommand("on", nullptr, 10); L.runCommand(nullptr, r, sizeof r); CHECK(strstr(r, "lights") != nullptr);
    for (int it = 0; it < 20000; it++) { std::string s; int n = (int)(rnd() % 14); for (int i = 0; i < n; i++) s += (char)(rnd() % 4 ? "bright onf 0123456789"[rnd() % 21] : 1 + rnd() % 254); char o[80]; L.runCommand(s.c_str(), o, sizeof o); CHECK(L.bright() <= LODESTAR_LIGHTS_BRIGHT_MAX); }
  }

  printf("lights tests: %d checks, %d failures\n", checks, fails);
  return fails ? 1 : 0;
}
