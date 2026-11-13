// LODESTAR PULSAR "jar lights": an OPTIONAL add-on for a repeater built into a glass jar.
//
//   * 7-LED round jewel  = heartbeat (a soft "lub-dub" every two seconds, so you can see the repeater is alive)
//   * 8-LED stick "RX"   = a spot of light runs from the TOP of the jar to the BOTTOM each time a packet is received
//   * 8-LED stick "TX"   = a spot of light runs from the BOTTOM of the jar to the TOP each time a packet is sent
//
// All 23 LEDs are one chain on ONE data pin:  RX stick (8)  ->  jewel (7)  ->  TX stick (8).
// Mounting: the RX stick hangs with DIN at the TOP; the jewel sits at the bottom; the TX stick has its DIN at the BOTTOM
// (and its DOUT at the top). Each spot then travels the same way the data does.
// This file has no hardware calls, so every rule below is tested on a PC (lodestar/test/test_lights.cpp).
// The part that talks to the real LEDs is a few lines of glue added by tools/apply_lights.py.
// NOT TESTED ON REAL HARDWARE. See PULSAR-JAR-LIGHTS.txt.
//
// Imagined and engineered by:
// Aries Thorne & Todd Steinhauer
//
// Moral Support: Athena the K9
#pragma once
#include <stdint.h>
#include <string.h>

// ---- things you can change with -D flags at build time -------------------------------------------
#ifndef LODESTAR_LIGHTS_BRIGHT          // normal brightness, 0..255 (this is a cap; colors are scaled by it)
#define LODESTAR_LIGHTS_BRIGHT 40
#endif
#ifndef LODESTAR_LIGHTS_BRIGHT_MAX      // the highest brightness the `lights bright` command can ever set (protects the power supply)
#define LODESTAR_LIGHTS_BRIGHT_MAX 120
#endif
#ifndef LODESTAR_LIGHTS_GLOW            // how faintly the jewel glows between heartbeats: the final LED value, 0 (dark) to 20
#define LODESTAR_LIGHTS_GLOW 2
#endif
#ifndef LODESTAR_LIGHTS_JEWEL_CENTER    // which of the 7 jewel pixels is the middle one (0 = first in the chain)
#define LODESTAR_LIGHTS_JEWEL_CENTER 0
#endif
#ifndef LODESTAR_LIGHTS_TX_FLIPPED      // 1 (default) = the TX stick is mounted with its DIN at the BOTTOM of the jar; 0 = DIN at the top
#define LODESTAR_LIGHTS_TX_FLIPPED 1
#endif
#ifndef LODESTAR_LIGHTS_HEART_RGB       // heartbeat color
#define LODESTAR_LIGHTS_HEART_RGB 255, 40, 70
#endif
#ifndef LODESTAR_LIGHTS_RX_RGB          // receive color (blue)
#define LODESTAR_LIGHTS_RX_RGB 30, 110, 255
#endif
#ifndef LODESTAR_LIGHTS_TX_RGB          // send color (amber)
#define LODESTAR_LIGHTS_TX_RGB 255, 110, 0
#endif

namespace lodestar {

class Lights {
 public:
  static const int JEWEL_N = 7, BAR_N = 8;
  static const int RX_FIRST = 0, JEWEL_FIRST = 8, TX_FIRST = 15, TOTAL = 23;
  static const uint32_t FRAME_MS = 40;            // 25 pictures a second
  static const uint32_t STEP_MS = 70;             // the spot moves one LED every 70 ms (a sweep takes about 0.6 s)
  static const uint32_t SPACING_MS = 3 * STEP_MS; // spots on one stick start at least this far apart
  static const int MAX_SPOTS = 3, PEND_MAX = 3;   // a burst of traffic shows as a short stream, never a solid bar
  static const uint32_t BEAT_PERIOD_MS = 2000;

  struct RGB { uint8_t r, g, b; };

  Lights() { bright_ = LODESTAR_LIGHTS_BRIGHT; clampBright(); reset(); }

  // ---- events (called from the radio code; they only note that something happened) ----
  void onRx(uint32_t) { if (on_ && rx_.pend < PEND_MAX) rx_.pend++; }
  void onTx(uint32_t) { if (on_ && tx_.pend < PEND_MAX) tx_.pend++; }

  // ---- call as often as you like; returns true when buf() holds a NEW picture that should be sent to the LEDs ----
  bool update(uint32_t now) {
    if (started_ && (int32_t)(now - next_frame_) < 0) return false;
    started_ = true; next_frame_ = now + FRAME_MS;
    RGB cur[TOTAL]; memset(cur, 0, sizeof cur);
    if (on_) {
      spawn(rx_, now); spawn(tx_, now);
      heartbeat(cur, now);
      drawBar(cur, rx_, now, false);
      drawBar(cur, tx_, now, true);
    } else { rx_.pend = tx_.pend = 0; }
    bool changed = memcmp(cur, buf_, sizeof cur) != 0;
    memcpy(buf_, cur, sizeof cur);
    return changed;
  }
  const RGB* buf() const { return buf_; }

  // ---- settings (runtime only; they go back to the build defaults at reboot) ----
  bool isOn() const { return on_; }
  void setOn(bool v) { on_ = v; if (!v) { rx_ = Bar(); tx_ = Bar(); } }
  uint8_t bright() const { return bright_; }
  void setBright(int v) { bright_ = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); clampBright(); }

  // `args` = text after the word "lights" (may be empty). Writes a short reply. Always returns true (the word is ours).
  bool runCommand(const char* args, char* reply, size_t cap) {
    while (args && *args == ' ') args++;
    if (!args || !*args) { status(reply, cap); return true; }
    if (!strcmp(args, "on")) { setOn(true); status(reply, cap); return true; }
    if (!strcmp(args, "off")) { setOn(false); status(reply, cap); return true; }
    if (!strncmp(args, "bright ", 7)) {
      const char* p = args + 7; while (*p == ' ') p++;
      int v = 0, n = 0;
      while (*p >= '0' && *p <= '9' && n < 4) { v = v * 10 + (*p - '0'); p++; n++; }
      if (n == 0 || n > 3 || *p != 0 || v > 100) { put(reply, cap, "Err - lights bright 0-100 (percent)"); return true; }
      setBright(v * LODESTAR_LIGHTS_BRIGHT_MAX / 100); status(reply, cap); return true;
    }
    put(reply, cap, "Err - lights [on|off|bright 0-100]");
    return true;
  }

  // Rough supply current for a picture, in milliamps, from typical WS2812B datasheet figures
  // (about 20 mA per color at full level, about 1 mA per LED doing nothing). An ESTIMATE, not a measurement.
  static uint32_t estimateMilliamps(const RGB* p, int n) {
    uint32_t sum = 0;
    for (int i = 0; i < n; i++) sum += (uint32_t)p[i].r + p[i].g + p[i].b;
    return (sum * 20 + 127) / 255 + (uint32_t)n;
  }

 private:
  struct Bar {
    uint32_t t0[MAX_SPOTS]; bool act[MAX_SPOTS]; uint32_t last; bool have; uint8_t pend;
    Bar() { memset(t0, 0, sizeof t0); memset(act, 0, sizeof act); last = 0; have = false; pend = 0; }
  };
  RGB buf_[TOTAL];
  Bar rx_, tx_;
  uint8_t bright_;
  bool on_ = true, started_ = false;
  uint32_t next_frame_ = 0;

  void reset() { memset(buf_, 0, sizeof buf_); rx_ = Bar(); tx_ = Bar(); }
  void clampBright() { if (bright_ > LODESTAR_LIGHTS_BRIGHT_MAX) bright_ = LODESTAR_LIGHTS_BRIGHT_MAX; }

  static void put(char* r, size_t cap, const char* s) { if (!r || !cap) return; size_t n = strlen(s); if (n >= cap) n = cap - 1; memcpy(r, s, n); r[n] = 0; }
  void status(char* r, size_t cap) const {
    char t[48]; int pct = (int)(((uint32_t)bright_ * 100 + LODESTAR_LIGHTS_BRIGHT_MAX / 2) / LODESTAR_LIGHTS_BRIGHT_MAX);
    char* w = t; const char* a = on_ ? "lights on, brightness " : "lights off, brightness ";
    while (*a) *w++ = *a++;
    if (pct >= 100) { *w++ = '1'; *w++ = '0'; *w++ = '0'; } else { if (pct >= 10) *w++ = (char)('0' + pct / 10); *w++ = (char)('0' + pct % 10); }
    *w++ = '%'; *w = 0; put(r, cap, t);
  }

  // colour * level (0..255, gamma-shaped so dim looks dim) * overall brightness
  RGB shade(int r, int g, int b, uint32_t level) const {
    uint32_t l = level > 255 ? 255 : level; l = l * l / 255;
    RGB o; o.r = (uint8_t)((uint32_t)r * l / 255 * bright_ / 255); o.g = (uint8_t)((uint32_t)g * l / 255 * bright_ / 255); o.b = (uint8_t)((uint32_t)b * l / 255 * bright_ / 255);
    return o;
  }
  static void maxInto(RGB& d, const RGB& s) { if (s.r > d.r) d.r = s.r; if (s.g > d.g) d.g = s.g; if (s.b > d.b) d.b = s.b; }

  static uint32_t tri(int32_t dt, int32_t rise, int32_t fall) {   // a pulse that rises then falls, 0..255
    if (dt < 0) return 0;
    if (dt < rise) return (uint32_t)(255 * dt / rise);
    dt -= rise; if (dt < fall) return (uint32_t)(255 - 255 * dt / fall);
    return 0;
  }
  static uint32_t beat(int32_t t) {                                // "lub-dub"
    uint32_t a = tri(t, 90, 230), b = tri(t - 300, 80, 260) * 160 / 255;
    return a > b ? a : b;
  }
  void heartbeat(RGB* o, uint32_t now) const {
    int32_t t = (int32_t)(now % BEAT_PERIOD_MS);
    uint32_t centre = beat(t);
    uint32_t ring = beat(t - 70) * 180 / 255;                         // the ring follows a moment later and a little softer
    static const int hc[3] = {LODESTAR_LIGHTS_HEART_RGB};
    int hm = hc[0] > hc[1] ? (hc[0] > hc[2] ? hc[0] : hc[2]) : (hc[1] > hc[2] ? hc[1] : hc[2]);
    RGB glow; glow.r = glowCh(hc[0], hm); glow.g = glowCh(hc[1], hm); glow.b = glowCh(hc[2], hm);
    for (int i = 0; i < JEWEL_N; i++) {
      RGB c = shade(LODESTAR_LIGHTS_HEART_RGB, i == LODESTAR_LIGHTS_JEWEL_CENTER ? centre : ring);
      if (bright_) maxInto(c, glow);                                  // never fully dark between beats, so you can see it is on (brightness 0 = really dark)
      o[JEWEL_FIRST + i] = c;
    }
  }
  static uint8_t glowCh(int ch, int maxch) {                          // the glow has the heartbeat's colour, its brightest colour at LODESTAR_LIGHTS_GLOW
    if (maxch <= 0 || LODESTAR_LIGHTS_GLOW <= 0) return 0;
    int g = LODESTAR_LIGHTS_GLOW > 20 ? 20 : LODESTAR_LIGHTS_GLOW;
    return (uint8_t)((ch * g + maxch / 2) / maxch);
  }

  void spawn(Bar& b, uint32_t now) {
    for (int i = 0; i < MAX_SPOTS; i++)                           // a spot that has run off the end is finished
      if (b.act[i] && (now - b.t0[i]) / STEP_MS >= (uint32_t)BAR_N + 1) b.act[i] = false;
    if (!b.pend) return;
    if (b.have && (int32_t)(now - b.last) < (int32_t)SPACING_MS) return;
    for (int i = 0; i < MAX_SPOTS; i++) if (!b.act[i]) { b.act[i] = true; b.t0[i] = now; b.last = now; b.have = true; b.pend--; return; }
  }

  // rx: spot goes down (pixel 0 = top, DIN at the top); tx: spot goes up
  void drawBar(RGB* o, const Bar& b, uint32_t now, bool isTx) const {
    for (int i = 0; i < MAX_SPOTS; i++) {
      if (!b.act[i]) continue;
      int h = (int)((now - b.t0[i]) / STEP_MS);
      for (int k = 0; k < 2; k++) {                               // k = 0 the bright head, k = 1 the dimmer tail just behind it
        int step = h - k; if (step < 0 || step >= BAR_N) continue;
        int p = isTx ? (BAR_N - 1 - step) : step;                 // p = how far down from the top of the jar
        int idx = isTx ? TX_FIRST + (LODESTAR_LIGHTS_TX_FLIPPED ? BAR_N - 1 - p : p) : RX_FIRST + p;
        RGB c = isTx ? shade(LODESTAR_LIGHTS_TX_RGB, k ? 90 : 255) : shade(LODESTAR_LIGHTS_RX_RGB, k ? 90 : 255);
        maxInto(o[idx], c);
      }
    }
  }
};

}  // namespace lodestar
