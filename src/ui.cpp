#include "ui.h"
#include "state.h"
#include "ble_nus.h"
#include "sprites.h"
#include "rtc.h"
#include "xfer.h"
#include "pack.h"
#include "imu.h"
#include "menu.h"
#include <pgmspace.h>

// Read-only handle to the loop-rate counter published by main.cpp. Used
// by the SYSTEM diagnostic view to show the actual achieved loop Hz.
namespace diag { extern volatile uint32_t loop_hz; }

namespace {
U8G2 *u = nullptr;

// Rotated to U8G2_R1 => landscape 400 x 300.
constexpr int W = 400;
constexpr int H = 300;
constexpr int TOP_H = 22;
constexpr int BOT_H = 18;

// "Connected enough to drive animations" — true on real BLE link OR in demo
// mode. Used by mood/sprite logic so the showcase actually plays in demo.
// The top bar's BT indicator and bottom bar's "BLE: ..." string still use
// ble_nus::connected() directly so the real link state is never hidden.
static inline bool linkActive() {
  return ble_nus::connected() || g_state.demo_mode;
}

// Forward decls for helpers defined further down.
static void drawProgressBar(int x, int y, int w, int h, int pct);

// Map state to sprite ID.
SpriteId moodToSprite() {
  if (g_state.napping) return SPR_SLEEPING;
  // Shake-to-dizzy wins over the normal state mapping for the dizzy window.
  if (g_state.dizzy_until_ms && millis() < g_state.dizzy_until_ms) return SPR_ANNOYED;
  if (!linkActive()) return SPR_SLEEPING;
  if (g_state.prompt.active)  return SPR_NOTIFICATION;
  if (g_state.msg.length()) {
    String m = g_state.msg; m.toLowerCase();
    if (m.indexOf("error") >= 0 || m.indexOf("fail") >= 0) return SPR_ERROR;
    if (m.indexOf("compact") >= 0) return SPR_SWEEPING;
    if (m.indexOf("done") >= 0 || m.indexOf("success") >= 0) {
      if (g_state.running == 0) return SPR_HAPPY;
    }
  }
  // 2+ sessions running = juggling (matches state-mapping.md tiers)
  if (g_state.running >= 2) return SPR_JUGGLING;
  if (g_state.running > 0)  return SPR_BUILDING;
  if (g_state.waiting > 0)  return SPR_THINKING;
  return SPR_IDLE;
}

// When connected and genuinely idle (no sessions, no prompt) for a while,
// cycle through ALL animations on the main view so you can preview them.
SpriteId showcaseSprite() {
  // Lifecycle order: wake → think → build → ship → celebrate → rest.
  // All 16 sprites in the rotation; ~5 s each so the full cycle is ~80 s.
  static const SpriteId cycle[] = {
    SPR_IDLE,
    SPR_IDLE_READING,
    SPR_BUBBLE,
    SPR_THINKING,
    SPR_TYPING,
    SPR_BUILDING,
    SPR_CARRYING,
    SPR_JUGGLING,
    SPR_HEADPHONES,
    SPR_SWEEPING,
    SPR_NOTIFICATION,
    SPR_ANNOYED,
    SPR_HAPPY,
    SPR_DOUBLE_JUMP,
    SPR_ERROR,
    SPR_SLEEPING,
  };
  constexpr uint32_t per_sprite_ms = 5000;
  uint32_t now = millis();
  int idx = (now / per_sprite_ms) % (sizeof(cycle) / sizeof(cycle[0]));
  return cycle[idx];
}

bool inShowcase() {
  if (!linkActive()) return false;
  if (g_state.prompt.active) return false;
  if (g_state.running > 0 || g_state.waiting > 0) return false;
  if (g_state.total > 0 && g_state.msg.length()) return false; // recent activity
  return true;
}

const char *moodLabel(SpriteId s) {
  switch (s) {
    case SPR_SLEEPING:     return "OFFLINE";
    case SPR_IDLE:         return "READY";
    case SPR_IDLE_READING: return "READING";
    case SPR_BUBBLE:       return "MUSING";
    case SPR_BUILDING:     return "WORKING";
    case SPR_TYPING:       return "TYPING";
    case SPR_THINKING:     return "THINKING";
    case SPR_SWEEPING:     return "COMPACT";
    case SPR_JUGGLING:     return "JUGGLING";
    case SPR_CARRYING:     return "MOVING";
    case SPR_HEADPHONES:   return "GROOVING";
    case SPR_HAPPY:        return "DONE";
    case SPR_DOUBLE_JUMP:  return "CELEBRATE";
    case SPR_ANNOYED:      return "ANNOYED";
    case SPR_ERROR:        return "ERROR";
    case SPR_NOTIFICATION: return "APPROVE?";
    default:               return "";
  }
}

uint32_t lastHash = 0xFFFFFFFFu;
uint32_t lastDrawMs = 0;
uint32_t lastFrameMs = 0;

uint32_t hashState() {
  uint32_t h = 2166136261u;
  auto mix = [&](const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; }
  };
  mix(&g_state.total, sizeof(int));
  mix(&g_state.running, sizeof(int));
  mix(&g_state.waiting, sizeof(int));
  mix(&g_state.tokens, sizeof(uint32_t));
  mix(&g_state.tokens_today, sizeof(uint32_t));
  mix(&g_state.battery_pct, sizeof(int));
  mix(&g_state.temp_c, sizeof(float));
  mix(&g_state.humidity_pct, sizeof(float));
  mix(&g_state.view, 1);
  mix(g_state.msg.c_str(), g_state.msg.length());
  mix(g_state.prompt.tool.c_str(), g_state.prompt.tool.length());
  mix(g_state.prompt.hint.c_str(), g_state.prompt.hint.length());
  for (int i = 0; i < 8; i++) mix(g_state.entries[i].c_str(), g_state.entries[i].length());
  uint8_t conn = ble_nus::connected() ? 1 : 0;
  mix(&conn, 1);
  uint8_t hopen = g_state.history_open ? 1 : 0;
  mix(&hopen, 1);
  uint8_t dmode = g_state.demo_mode ? 1 : 0;
  mix(&dmode, 1);
  uint32_t af = g_state.anim_frame;
  mix(&af, sizeof(af));
  // Velocity ring buffer state — drives both the histogram and the mood
  // modifier in moodAdjective().
  mix(&g_state.velocity_count, 1);
  mix(&g_state.velocity_idx,   1);
  return h;
}

// --- Sprite blit ---
// Runtime pack override wins over the built-in PROGMEM sprite. If the
// active pack supplies frames for this slot, blit them directly from PSRAM
// (no copy needed). Otherwise stage the PROGMEM frame through a RAM
// scratch buffer and drawXBM that.
void drawSprite(int x, int y, SpriteId id) {
  if (pack::hasOverride(id)) {
    uint8_t fc = pack::overrideFrameCount(id);
    if (fc > 0) {
      uint8_t fi = g_state.anim_frame % fc;
      const uint8_t *p = pack::overrideFrame(id, fi);
      if (p) {
        u->drawXBM(x, y, SPRITE_W, SPRITE_H, (uint8_t *)p);
        return;
      }
    }
  }
  const SpriteInfo &info = SPRITES[id];
  if (info.frame_count == 0) return;
  uint8_t frame_idx = g_state.anim_frame % info.frame_count;
  static uint8_t buf[SPRITE_BYTES];
  memcpy_P(buf, info.frames[frame_idx], SPRITE_BYTES);
  u->drawXBM(x, y, SPRITE_W, SPRITE_H, buf);
}

// --- Top bar ---
// Layout left to right:
//   [name]                                       [BT] [WiFi] [Temp] [Hum] [time] [bat]
void drawTopBar() {
  u->setDrawColor(1);
  u->drawBox(0, 0, W, TOP_H);
  u->setDrawColor(0);

  u->setFont(u8g2_font_7x13B_tf);
  u->drawStr(6, 15, g_state.name.c_str());

  // Demo-mode chip immediately right of the name. The top bar is drawn with
  // the bar itself as ink (color 1) and text as cleared pixels (color 0), so
  // an attention chip needs the inverse: punch a blank pill into the bar and
  // draw the "DEMO" glyphs in ink inside it. Reads as a small inset label.
  if (g_state.demo_mode) {
    int name_w = u->getStrWidth(g_state.name.c_str());
    int cx = 6 + name_w + 6;
    const char *demo = "DEMO";
    u->setFont(u8g2_font_6x10_tf);
    int cw = u->getStrWidth(demo) + 6;
    u->setDrawColor(0);                 // erase a pill into the ink bar
    u->drawRBox(cx, 4, cw, 14, 2);
    u->setDrawColor(1);                 // outline + ink text inside
    u->drawRFrame(cx, 4, cw, 14, 2);
    u->drawStr(cx + 3, 14, demo);
    u->setDrawColor(0);                 // restore for the rest of the top bar
    u->setFont(u8g2_font_7x13B_tf);
  }

  // Compose right-side stats from rightmost first.
  int x = W - 6;

  // Battery icon + %
  {
    char buf[8] = "";
    if (g_state.battery_pct >= 0) snprintf(buf, sizeof(buf), "%d%%", g_state.battery_pct);
    int tw = u->getStrWidth(buf);
    x -= tw;
    if (buf[0]) u->drawStr(x, 15, buf);
    x -= 4;
    // battery icon to the LEFT of percent
    int bw = 18, bh = 10;
    x -= bw + 2;
    int bx = x, by = (TOP_H - bh) / 2;
    u->drawFrame(bx, by, bw, bh);
    u->drawBox(bx + bw, by + 2, 2, bh - 4);
    int fill = (g_state.battery_pct >= 0) ? (g_state.battery_pct * (bw - 2) / 100) : 0;
    if (fill > 0) u->drawBox(bx + 1, by + 1, fill, bh - 2);

    // Lightning bolt overlay when charging.
    // The top bar is drawn white-on-black-via-inverse, so this whole block
    // is running with setDrawColor(0). We must restore it after touching it.
    if (g_state.charging) {
      int cx = bx + bw / 2;
      int cy = by + bh / 2;
      // Erase the fill behind the bolt so it stays visible on filled batteries.
      u->setDrawColor(0);
      u->drawBox(cx - 3, by + 1, 6, bh - 2);
      u->setDrawColor(1);
      // Zig-zag lightning: top-right → middle-left → bottom-right
      u->drawLine(cx + 2, by + 1, cx - 1, cy);
      u->drawLine(cx - 1, cy,     cx + 1, cy);
      u->drawLine(cx + 1, cy,     cx - 2, by + bh - 2);
      u->setDrawColor(0);   // restore for the rest of the top bar
    }
    x -= 10;
  }

  // Clock HH:MM
  {
    char buf[8] = "--:--";
    if (g_state.time_sync_ms) {
      uint32_t elapsed = (millis() - g_state.time_sync_ms) / 1000U;
      uint32_t local = g_state.time_epoch + elapsed + g_state.time_offset_sec;
      int h = (local / 3600) % 24;
      int m = (local / 60) % 60;
      snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
    }
    int tw = u->getStrWidth(buf);
    x -= tw;
    u->drawStr(x, 15, buf);
    x -= 10;
  }

  // Humidity
  if (!isnan(g_state.humidity_pct)) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%d%%", (int)(g_state.humidity_pct + 0.5f));
    int tw = u->getStrWidth(buf);
    x -= tw;
    u->drawStr(x, 15, buf);
    x -= 8;
  }

  // Temperature (Latin-1 degree sign 0xB0)
  if (!isnan(g_state.temp_c)) {
    char buf[12];
    snprintf(buf, sizeof(buf), "%d\xB0""C", (int)(g_state.temp_c + 0.5f));
    int tw = u->getStrWidth(buf);
    x -= tw;
    u->drawStr(x, 15, buf);
    x -= 10;
  }

  // Helper: filled-square indicator + label, reading right-to-left.
  auto drawIndicator = [&](const char *label, bool on) {
    int lw = u->getStrWidth(label);
    x -= lw;
    u->drawStr(x, 15, label);
    x -= 4;
    int sq = 8;
    x -= sq;
    int sy = (TOP_H - sq) / 2;
    u->drawFrame(x, sy, sq, sq);
    if (on) u->drawBox(x + 2, sy + 2, sq - 4, sq - 4);
    x -= 8;
  };

  drawIndicator("WiFi", false);
  // Show "BT*" with a trailing asterisk when the link is encrypted, so a
  // glance at the top bar confirms transcript snippets aren't in clear text.
  drawIndicator(g_state.secure ? "BT*" : "BT", ble_nus::connected());

  u->setDrawColor(1);
}

// --- Bottom bar ---
void drawBottomBar() {
  u->setDrawColor(1);
  u->drawHLine(0, H - BOT_H, W);
  u->setFont(u8g2_font_6x10_tf);

  // The bottom bar is tight (400 px, 6x10 font => ~66 chars max). The right
  // side eats up ~16 chars for "DEMO heartbeat" / "BLE: advertising", so the
  // left string has to stay under ~48 chars or it overlaps. Keep it terse,
  // and put the "(N/3)" page index at the tail so the user always sees which
  // view they're on without us having to spell out MAIN/USAGE/SYSTEM.
  bool active_prompt = g_state.prompt.active && g_state.prompt.id.length();
  char left[96];
  char page[8] = "";
  if (!active_prompt) {
    snprintf(page, sizeof(page), " (%u/4)", (unsigned)(g_state.view + 1));
  }
  if (active_prompt) {
    snprintf(left, sizeof(left),
             "[KEY] APPROVE  [BOOT] DENY  longpress=history");
  } else if (g_state.view == 0) {
    snprintf(left, sizeof(left),
             "[KEY] next  [BOOT] prev  longpress=history%s", page);
  } else if (g_state.view == 1) {
    snprintf(left, sizeof(left),
             "[KEY] next  [BOOT] prev  longpress=menu%s", page);
  } else if (g_state.view == 2) {
    snprintf(left, sizeof(left),
             "[KEY] next  [BOOT] prev  longpress=%s%s",
             g_state.demo_mode ? "exit demo" : "demo", page);
  } else {
    // CLOCK view — long-press just cycles, nothing extra to hint at.
    snprintf(left, sizeof(left),
             "[KEY] next  [BOOT] prev%s", page);
  }
  u->drawStr(6, H - 5, left);

  // Right-aligned source-of-truth label. In demo mode it always reads
  // "DEMO heartbeat" regardless of BLE state — there's no chance of mistaking
  // the rotating scenes for live data.
  const char *r;
  if (g_state.demo_mode)              r = "DEMO heartbeat";
  else if (ble_nus::connected())      r = "BLE: connected";
  else                                r = "BLE: advertising";
  int rw = u->getStrWidth(r);
  u->drawStr(W - rw - 6, H - 5, r);
}

// --- UTF-8 / CJK text helpers ---
// Byte length of the UTF-8 character whose lead byte is c.
static int utf8CharLen(unsigned char c) {
  if ((c & 0x80) == 0)    return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;  // invalid lead — advance 1 byte to make progress
}

// Truncate a UTF-8 string to fit `max_w` px in the CURRENTLY-SET font,
// appending "~" if it was cut. Splits only on character boundaries so a
// multi-byte glyph is never sliced (which would render as mojibake). The
// caller must set the font first (we measure with getUTF8Width).
static String clipUTF8(const String &text, int max_w) {
  if (u->getUTF8Width(text.c_str()) <= max_w) return text;
  int tilde = u->getUTF8Width("~");
  int i = 0, n = text.length();
  String acc;
  while (i < n) {
    int clen = utf8CharLen((unsigned char)text[i]);
    if (i + clen > n) clen = n - i;
    String cand = acc + text.substring(i, i + clen);
    if (u->getUTF8Width(cand.c_str()) + tilde > max_w) break;
    acc = cand;
    i += clen;
  }
  return acc + "~";
}

// Word/character-wrap UTF-8 text with the wqy14 CJK font. Latin words wrap at
// spaces; CJK (no spaces) wraps at the character that would overflow max_w.
void drawWrappedText(int x, int y, int max_w, int line_h, const String &text, int max_lines) {
  if (text.length() == 0) return;
  u->setFont(u8g2_font_wqy14_t_gb2312);
  int i = 0, n = text.length(), line = 0;
  while (i < n && line < max_lines) {
    int line_start = i, last_space = -1;
    String acc;
    while (i < n) {
      int clen = utf8CharLen((unsigned char)text[i]);
      if (i + clen > n) clen = n - i;
      String cand = acc + text.substring(i, i + clen);
      if (u->getUTF8Width(cand.c_str()) > max_w) break;
      if (text[i] == ' ') last_space = i;
      acc = cand;
      i += clen;
    }
    int line_end = i;
    if (i < n && last_space > line_start) { line_end = last_space; i = last_space; }
    u->drawUTF8(x, y + line * line_h, text.substring(line_start, line_end).c_str());
    while (i < n && text[i] == ' ') i++;
    line++;
  }
}

// Compute the mean response time (seconds) across the populated slots of the
// velocity ring buffer. Returns 0 if no approvals have been recorded yet.
static uint16_t velocityMean() {
  if (g_state.velocity_count == 0) return 0;
  uint32_t sum = 0;
  for (int i = 0; i < g_state.velocity_count; i++) {
    sum += g_state.velocity[i];
  }
  return (uint16_t)(sum / g_state.velocity_count);
}

// Derive a single mood adjective from energy tier (0..5) + state.
// Velocity from the last few approvals nudges the perceived energy tier:
// fast (<5s avg) → +1, slow (>30s avg) → -1. Mirrors the M5 reference
// firmware's stats.velocity-driven mood logic.
static const char *moodAdjective() {
  int e = (int)g_state.energy_tier;
  bool running = g_state.running > 0;
  bool waiting = g_state.waiting > 0;
  bool offline = !linkActive();

  if (g_state.velocity_count > 0) {
    uint16_t avg = velocityMean();
    if (avg > 0 && avg < 5)        e++;
    else if (avg > 30)             e--;
    if (e < 0) e = 0;
    if (e > 5) e = 5;
  }

  if (offline)                  return "asleep";
  if (g_state.prompt.active)    return "alert";
  if (waiting)                  return "pensive";
  if (running && e >= 4)        return "focused";
  if (running && e >= 2)        return "steady";
  if (running)                  return "weary";
  if (e >= 5)                   return "spry";
  if (e >= 3)                   return "content";
  if (e >= 1)                   return "drowsy";
  return "tired";
}

// Format an unsigned int with thousand separator into a stack buffer.
static const char *fmtThousands(unsigned long n, char *buf, size_t n_buf) {
  if (n < 1000) snprintf(buf, n_buf, "%lu", n);
  else if (n < 1000000) snprintf(buf, n_buf, "%lu,%03lu", n / 1000, n % 1000);
  else snprintf(buf, n_buf, "%lu,%03lu,%03lu",
                n / 1000000, (n / 1000) % 1000, n % 1000);
  return buf;
}

// Like fmtThousands but caps at 5 chars by scaling to K / M units. Used in
// the dense KPI grid where each cell only has ~7 chars of horizontal room.
//   <10000          -> "9,999"   (5 chars, full precision)
//   10K..999K       -> "999K"    (4 chars)
//   1M..9.9M        -> "9.9M"    (4 chars)
//   >=10M           -> "12M"     (3 chars, truncate the tenths)
static const char *fmtCompactTokens(unsigned long n, char *buf, size_t n_buf) {
  if (n < 10000)        return fmtThousands(n, buf, n_buf);
  if (n < 1000000)      { snprintf(buf, n_buf, "%luK", n / 1000); return buf; }
  if (n < 10000000) {
    unsigned long whole = n / 1000000;
    unsigned long tenths = (n % 1000000) / 100000;
    snprintf(buf, n_buf, "%lu.%luM", whole, tenths);
    return buf;
  }
  snprintf(buf, n_buf, "%luM", n / 1000000);
  return buf;
}

// Verbose duration: "1h 02m 03s" — readable in the Now line.
static void fmtDuration(uint32_t secs, char *out, size_t n) {
  uint32_t h = secs / 3600, m = (secs / 60) % 60, s = secs % 60;
  if (h > 0) snprintf(out, n, "%luh %02lum %02lus", (unsigned long)h, (unsigned long)m, (unsigned long)s);
  else if (m > 0) snprintf(out, n, "%lum %02lus", (unsigned long)m, (unsigned long)s);
  else snprintf(out, n, "%lus", (unsigned long)s);
}

// Compact duration for the KPI grid: stays under 7 chars at any uptime.
//   <1m   -> "30s"
//   <1h   -> "30m"
//   <1d   -> "12h05m"
//   else  -> "5d12h"
static void fmtCompactDuration(uint32_t secs, char *out, size_t n) {
  if (secs < 60)        snprintf(out, n, "%lus", (unsigned long)secs);
  else if (secs < 3600) snprintf(out, n, "%lum", (unsigned long)(secs / 60));
  else if (secs < 86400) {
    snprintf(out, n, "%luh%02lum",
             (unsigned long)(secs / 3600),
             (unsigned long)((secs / 60) % 60));
  } else {
    snprintf(out, n, "%lud%luh",
             (unsigned long)(secs / 86400),
             (unsigned long)((secs / 3600) % 24));
  }
}

// Draw a row of N filled / empty pip discs.
static void drawPips(int x, int y, int n, int filled, int radius, int spacing) {
  for (int i = 0; i < n; i++) {
    int cx = x + i * spacing + radius;
    if (i < filled) u->drawDisc(cx, y, radius);
    else            u->drawCircle(cx, y, radius);
  }
}

// Heart bitmaps for the Mood row. 11 wide x 8 tall, two variants (filled
// and outline). Matches the heart-rating visual used by the official
// M5StickC reference firmware -- the user explicitly asked for it.
constexpr int HEART_W = 11;
constexpr int HEART_H = 8;
static const uint8_t HEART_FILLED_XBM[] PROGMEM = {
  0x6C, 0x00,   // ..##.##....
  0xFC, 0x01,   // ..#######..
  0xFC, 0x01,   // ..#######..
  0xFC, 0x01,   // ..#######..
  0xF8, 0x00,   // ...#####...
  0xF8, 0x00,   // ...#####...
  0x70, 0x00,   // ....###....
  0x20, 0x00,   // .....#.....
};
static const uint8_t HEART_OUTLINE_XBM[] PROGMEM = {
  0x6C, 0x00,   // ..##.##....
  0x94, 0x01,   // ..#.#..##..
  0x04, 0x01,   // ..#.....#..
  0x04, 0x01,   // ..#.....#..
  0x88, 0x00,   // ...#...#...
  0x88, 0x00,   // ...#...#...
  0x50, 0x00,   // ....#.#....
  0x20, 0x00,   // .....#.....
};
static void drawHeart(int x, int y, bool filled) {
  // drawXBM consumes a RAM pointer; stage from PROGMEM. The bitmap is tiny
  // (16 B) so the stack copy is cheap.
  static uint8_t buf[sizeof(HEART_FILLED_XBM)];
  memcpy_P(buf, filled ? HEART_FILLED_XBM : HEART_OUTLINE_XBM, sizeof(buf));
  u->drawXBM(x, y, HEART_W, HEART_H, buf);
}

// Mood -> hearts mapping. Same inputs as moodAdjective() (energy_tier +
// velocity + offline + prompt + workload), output is 0..5 hearts that
// matches the official M5 firmware's 5-heart affection scale.
static int computeMoodHearts() {
  if (g_state.napping)    return 0;          // asleep = no hearts
  if (!linkActive())      return 1;          // offline = one dim heart
  int e = (int)g_state.energy_tier;
  if (g_state.velocity_count > 0) {
    uint16_t avg = velocityMean();
    if (avg > 0 && avg < 5)     e++;
    else if (avg > 30)          e--;
  }
  if (g_state.prompt.active)    e--;         // alert / interrupted
  if (g_state.running > 2)      e--;         // overloaded
  if (e < 0) e = 0;
  if (e > 5) e = 5;
  return e;
}

// Battery-cell-style indicator: N rounded rectangles in a row, the first
// `filled` of them solid, the rest just outlined. Reads like a fuel gauge
// or a health bar — better than discs for "discrete tiers" semantics.
static void drawCellBar(int x, int y, int total_w, int h, int n, int filled) {
  if (n <= 0) return;
  int gap = 2;
  int cell_w = (total_w - gap * (n - 1)) / n;
  if (cell_w < 4) cell_w = 4;
  for (int i = 0; i < n; i++) {
    int cx = x + i * (cell_w + gap);
    if (i < filled) u->drawRBox(cx, y, cell_w, h, 2);
    else            u->drawRFrame(cx, y, cell_w, h, 2);
  }
}

// Continuous progress bar with N-1 subdivision tick marks knocked out in
// inverse colour so they're visible on both filled and unfilled regions.
// More polished than a row of pips for a continuous progress metric.
static void drawTickedBar(int x, int y, int w, int h, int pct, int ticks) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  u->drawRFrame(x, y, w, h, 2);
  int inner_x = x + 2;
  int inner_y = y + 1;
  int inner_w = w - 4;
  int inner_h = h - 2;
  if (inner_w <= 0 || inner_h <= 0) return;
  int fw = inner_w * pct / 100;
  if (fw > 0) u->drawBox(inner_x, inner_y, fw, inner_h);
  if (ticks > 1) {
    u->setDrawColor(0);
    for (int i = 1; i < ticks; i++) {
      int tx = inner_x + (inner_w * i) / ticks;
      u->drawVLine(tx, inner_y, inner_h);
    }
    u->setDrawColor(1);
  }
}

// Full-screen approval view used when prompt.active. The MAIN dashboard is
// replaced entirely so the prompt is impossible to miss. KEY approves,
// BOOT denies, either long-press escapes to view navigation.
static void drawApprovalView() {
  // Notification sprite on the left
  drawSprite(8, TOP_H + 4, SPR_NOTIFICATION);

  // Title pill below sprite
  int pill_y = TOP_H + 4 + SPRITE_H + 4;
  u->setDrawColor(1);
  u->drawRBox(8, pill_y, SPRITE_W, 22, 4);
  u->setDrawColor(0);
  u->setFont(u8g2_font_helvB14_tf);
  const char *label = "APPROVE?";
  int lw = u->getStrWidth(label);
  u->drawStr(8 + (SPRITE_W - lw) / 2, pill_y + 16, label);
  u->setDrawColor(1);

  // Right column: huge title + tool + hint + timer
  int rx = 8 + SPRITE_W + 14;
  int ry = TOP_H + 4;

  u->setFont(u8g2_font_logisoso28_tr);
  u->drawStr(rx, ry + 28, "APPROVE?");
  ry += 42;

  u->setFont(u8g2_font_wqy14_t_gb2312);
  String tool = "Tool: " + (g_state.prompt.tool.length() ? g_state.prompt.tool : String("(unknown)"));
  u->drawUTF8(rx, ry, clipUTF8(tool, W - rx - 8).c_str());
  ry += 18;

  // Hint, wrapped over multiple lines (drawWrappedText sets wqy14 itself).
  drawWrappedText(rx, ry, W - rx - 8, 14, g_state.prompt.hint, 5);
  ry += 14 * 5 + 6;

  // Timer (how long the prompt has been waiting)
  uint32_t secs = g_state.prompt_started_ms
                  ? (millis() - g_state.prompt_started_ms) / 1000
                  : 0;
  char timer[24];
  snprintf(timer, sizeof(timer), "waiting %lus", (unsigned long)secs);
  u->setFont(u8g2_font_helvB14_tf);
  u->drawStr(rx, ry, timer);

  // Big visual key hints centred along the bottom of the content area.
  int hy = H - BOT_H - 38;
  int half = (W - 24) / 2;

  // KEY = APPROVE — solid inverse box
  u->setDrawColor(1);
  u->drawRBox(8, hy, half, 28, 6);
  u->setDrawColor(0);
  u->setFont(u8g2_font_helvB14_tf);
  const char *a = "[KEY] APPROVE";
  int aw = u->getStrWidth(a);
  u->drawStr(8 + (half - aw) / 2, hy + 19, a);
  u->setDrawColor(1);

  // BOOT = DENY — outlined box
  u->drawRFrame(W - 8 - half, hy, half, 28, 6);
  const char *dn = "[BOOT] DENY";
  int dnw = u->getStrWidth(dn);
  u->drawStr(W - 8 - half + (half - dnw) / 2, hy + 19, dn);
}

// --- Main view: dashboard layout ---
void drawMainView() {
  // Active permission prompt fully overrides the dashboard so it can't be
  // missed. Long-press of either button still navigates views as an escape.
  if (g_state.prompt.active && g_state.prompt.id.length()) {
    drawApprovalView();
    return;
  }

  bool idle_carousel = inShowcase();
  SpriteId mood = idle_carousel ? showcaseSprite() : moodToSprite();

  // ===== Left column: sprite + state pill =====
  int spr_x = 8;
  int spr_y = TOP_H + 4;
  drawSprite(spr_x, spr_y, mood);

  // State pill below sprite
  int pill_y = spr_y + SPRITE_H + 4;
  u->setDrawColor(1);
  u->drawRBox(spr_x, pill_y, SPRITE_W, 22, 4);
  u->setDrawColor(0);
  u->setFont(u8g2_font_helvB14_tf);
  const char *label = moodLabel(mood);
  int lw = u->getStrWidth(label);
  u->drawStr(spr_x + (SPRITE_W - lw) / 2, pill_y + 16, label);
  u->setDrawColor(1);

  // ===== Right column: vital signs =====
  //
  // Design: label-on-left + visualisation-on-right, four stacked rows.
  //
  //   Mood    <adjective in helvB18>
  //   Energy  [▮▮▮▮▯]   4/5         (5 rounded cells, fuel-gauge style)
  //   Fed     [██████▒▒▒▒]  7/10    (continuous bar w/ 10 subdivisions)
  //   Level   96  -> L97 in 12K tok (medium number + countdown subtitle)
  //
  // The previous layout used logisoso24 (24 px) for both Mood and Level
  // and rendered Energy / Fed as rows of pip discs. The big logisoso for
  // Level was crashing into Fed's pips; the pips themselves looked toy-ish
  // for what is otherwise a fairly information-dense dashboard. This
  // redesign tightens vertical rhythm to ~22 px per row, swaps logisoso
  // for helvB18 (still bold and prominent, less cartoon), and replaces
  // pips with proper bar visualisations.
  int rx = spr_x + SPRITE_W + 12;
  int ry = TOP_H + 6;
  int label_col_w = 52;   // x-offset where the bar / number starts in each row
  char buf[64];

  // --- Mood: row of 5 hearts (filled = current mood level) ---
  // Was a helvB18 adjective ("focused" / "spry" / etc); the user asked for
  // the heart-rating visual from the official M5StickC reference firmware.
  // moodAdjective() and computeMoodHearts() share the same input logic so
  // SYSTEM-view consumers of moodAdjective() (if any) still work.
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(rx, ry + 14, "Mood");
  {
    int hearts = computeMoodHearts();
    int hx = rx + label_col_w;
    int hy = ry + 6;
    int spacing = HEART_W + 4;
    for (int i = 0; i < 5; i++) {
      drawHeart(hx + i * spacing, hy, i < hearts);
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%d/5", hearts);
    u->setFont(u8g2_font_6x10_tf);
    u->drawStr(hx + 5 * spacing + 4, ry + 14, buf);
  }
  ry += 20;

  // --- Energy: 5 rounded cells ---
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(rx, ry + 13, "Energy");
  {
    int bar_x = rx + label_col_w;
    int bar_w = 96;
    drawCellBar(bar_x, ry + 2, bar_w, 14, 5, g_state.energy_tier);
    snprintf(buf, sizeof(buf), "%d/5", g_state.energy_tier);
    u->setFont(u8g2_font_6x10_tf);
    u->drawStr(bar_x + bar_w + 6, ry + 13, buf);
  }
  ry += 20;

  // --- Fed: continuous bar with 10-segment subdivisions ---
  uint8_t fed = (uint8_t)((g_state.tokens % TOKENS_PER_LEVEL) / TOKENS_PER_FED_PIP);
  int fed_pct = (int)((g_state.tokens % TOKENS_PER_LEVEL) * 100UL / TOKENS_PER_LEVEL);
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(rx, ry + 12, "Fed");
  {
    int bar_x = rx + label_col_w;
    int bar_w = 128;
    drawTickedBar(bar_x, ry + 2, bar_w, 12, fed_pct, 10);
    snprintf(buf, sizeof(buf), "%u/10", (unsigned)fed);
    u->setFont(u8g2_font_6x10_tf);
    u->drawStr(bar_x + bar_w + 6, ry + 12, buf);
  }
  ry += 20;

  // --- Level: medium bold number + arrow countdown ---
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(rx, ry + 16, "Level");
  u->setFont(u8g2_font_helvB18_tf);
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)g_state.level);
  u->drawStr(rx + label_col_w, ry + 18, buf);
  int level_num_w = u->getStrWidth(buf);
  // Countdown tail: "-> L97 in 12K tok"
  uint32_t to_next = TOKENS_PER_LEVEL - (g_state.tokens % TOKENS_PER_LEVEL);
  char tail[40];
  if (to_next >= 1000) {
    snprintf(tail, sizeof(tail), "-> L%lu in %luK tok",
             (unsigned long)(g_state.level + 1), to_next / 1000);
  } else {
    snprintf(tail, sizeof(tail), "-> L%lu in %lu tok",
             (unsigned long)(g_state.level + 1), to_next);
  }
  u->setFont(u8g2_font_6x10_tf);
  u->drawStr(rx + label_col_w + level_num_w + 8, ry + 16, tail);
  ry += 24;

  // Divider then Now line
  u->drawHLine(rx, ry, W - rx - 6);
  ry += 12;
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(rx, ry, "Now");
  u->setFont(u8g2_font_7x13_tf);
  if (g_state.prompt.active) {
    u->setFont(u8g2_font_wqy14_t_gb2312);
    u->drawUTF8(rx + 44, ry, clipUTF8("approval: " + g_state.prompt.tool, W - rx - 50).c_str());
  } else if (g_state.running > 0) {
    char d[24];
    uint32_t secs = g_state.run_started_ms ? (millis() - g_state.run_started_ms) / 1000 : 0;
    fmtDuration(secs, d, sizeof(d));
    snprintf(buf, sizeof(buf), "%d session%s for %s",
             g_state.running, g_state.running == 1 ? "" : "s", d);
    u->drawStr(rx + 44, ry, buf);
  } else if (!linkActive()) {
    u->drawStr(rx + 44, ry, g_state.napping ? "offline, napping (energy refilling)" : "offline, will nap in <5 min");
  } else {
    u->drawStr(rx + 44, ry, g_state.total > 0 ? "idle (sessions parked)" : "all clear");
  }
  ry += 14;

  // 2x2 KPI grid (compact).
  //
  // Each cell is ~123 px wide on a 400 px display. Label is the 6x10 font;
  // value is 7x13B. All labels here are <= 4 chars so a 36-px column is
  // plenty; values are kept <= 6 chars via the compact formatters above.
  //
  // Labels were renamed in this revision to match what the heartbeat
  // actually carries (per REFERENCE.md):
  //   desk   = g_state.tokens       "output tokens since desktop app start"
  //   today  = g_state.tokens_today "output tokens since local midnight"
  //   rate   = g_state.tokens_1h / window_minutes   (window <= 60 min so
  //                                                  cold-boot rates aren't
  //                                                  divided by an empty hour)
  //   up     = firmware uptime
  struct Kpi { const char *label; const char *value; };
  char b_tok[16], b_today[16], b_rate[16], b_up[16];
  fmtCompactTokens(g_state.tokens, b_tok, sizeof(b_tok));
  fmtCompactTokens(g_state.tokens_today, b_today, sizeof(b_today));
  {
    // Dynamic window: divide by the actual sampled span, not the nominal
    // 60 minutes. Critical for the first hour of uptime, where dividing by
    // 60 systematically reports a rate ~60x too low.
    uint32_t uptime_min = millis() / 60000UL;
    uint32_t win_min = uptime_min > 60 ? 60 : (uptime_min < 1 ? 1 : uptime_min);
    unsigned long tpm = g_state.tokens_1h / win_min;
    snprintf(b_rate, sizeof(b_rate), "%lu/min", tpm);
  }
  fmtCompactDuration(millis() / 1000, b_up, sizeof(b_up));

  Kpi kpis[4] = {
    {"desk",  b_tok},
    {"today", b_today},
    {"rate",  b_rate},
    {"up",    b_up},
  };
  int col_w = (W - rx - 6) / 2;
  constexpr int VAL_OFFSET = 38;   // x-offset for value within each cell
  for (int i = 0; i < 4; i++) {
    int col = i % 2, row = i / 2;
    int x = rx + col * col_w;
    int y = ry + row * 14;
    u->setFont(u8g2_font_6x10_tf);
    u->drawStr(x, y, kpis[i].label);
    u->setFont(u8g2_font_7x13B_tf);
    u->drawStr(x + VAL_OFFSET, y, kpis[i].value);
  }
  ry += 14 * 2 + 2;

  // Honest footnote — neither tokens field counts input/prompt bytes.
  u->setFont(u8g2_font_5x7_tf);
  u->drawStr(rx, ry + 6, "* output tokens only (input/prompt not counted)");
  ry += 8;

  // ===== Bottom band: Recent activity =====
  int ey = pill_y + 28;
  if (ey < ry) ey = ry + 2;
  u->drawHLine(6, ey, W - 12);
  ey += 12;
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(6, ey, "Recent activity");
  // msg + transcript entries are received text that may contain Chinese —
  // render with the CJK font (drawUTF8) and clip on UTF-8 char boundaries so
  // multibyte glyphs aren't sliced into mojibake.
  if (g_state.msg.length()) {
    u->setFont(u8g2_font_wqy14_t_gb2312);
    String m = clipUTF8("msg: " + g_state.msg, 230);
    int mw = u->getUTF8Width(m.c_str());
    u->drawUTF8(W - mw - 6, ey, m.c_str());
  }
  ey += 4;

  int line_y = ey + 14;
  int rows = 0;
  for (int i = 0; i < 3; i++) {
    if (g_state.entries[i].length()) {
      u->setFont(u8g2_font_wqy14_t_gb2312);
      u->drawUTF8(8, line_y, clipUTF8("> " + g_state.entries[i], W - 14).c_str());
      rows++;
    }
    line_y += 14;
  }
  if (rows == 0) {
    // Official M5StickC behaviour: when `entries` is empty, fall back to
    // showing the `msg` field as the recent line.
    if (g_state.msg.length()) {
      u->setFont(u8g2_font_wqy14_t_gb2312);
      u->drawUTF8(8, ey + 14, clipUTF8("> " + g_state.msg, W - 14).c_str());
    } else {
      u->setFont(u8g2_font_6x10_tf);
      u->drawStr(8, ey + 14, linkActive()
                              ? "(no transcript yet — start a Claude session)"
                              : "(offline)");
    }
  }
}

// Format a token count with thousands separator.
static String fmtTokens(unsigned long n) {
  char buf[16];
  if (n < 1000) snprintf(buf, sizeof(buf), "%lu", n);
  else if (n < 1000000) snprintf(buf, sizeof(buf), "%lu,%03lu", n / 1000, n % 1000);
  else snprintf(buf, sizeof(buf), "%lu,%03lu,%03lu",
                n / 1000000, (n / 1000) % 1000, n % 1000);
  return String(buf);
}

// Format seconds until target as "Xh Ym" or "Ym".
static String fmtCountdown(int32_t secs) {
  if (secs <= 0) return String("--");
  int h = secs / 3600;
  int m = (secs / 60) % 60;
  char buf[16];
  if (h > 0) snprintf(buf, sizeof(buf), "%dh %02dm", h, m);
  else       snprintf(buf, sizeof(buf), "%dm", m);
  return String(buf);
}

// Compute seconds until next local midnight.
static int32_t secsToMidnight() {
  if (!g_state.time_sync_ms) return -1;
  uint32_t elapsed = (millis() - g_state.time_sync_ms) / 1000U;
  uint32_t local = g_state.time_epoch + elapsed + g_state.time_offset_sec;
  uint32_t day = local % 86400;
  return (int32_t)(86400 - day);
}

// Draw a horizontal progress bar.
static void drawProgressBar(int x, int y, int w, int h, int pct) {
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  u->drawFrame(x, y, w, h);
  int fill = (w - 4) * pct / 100;
  if (fill > 0) u->drawBox(x + 2, y + 2, fill, h - 4);
}

// Format a weekday + time as "Thu 5:00 AM" (used for reset hints).
// Without RTC and TZ math we just pick a hardcoded reset point for demo.
static String fmtWeeklyReset() {
  // Anthropic resets weekly on Thursday 5:00 AM in many regions.
  // We don't know the user's plan reset; just label it consistently.
  return String("Thu 5:00 AM");
}

// --- Usage view: claude.ai-style layout ---
//
// Each row has the title and "X% used" on one line, the reset hint on the
// next line, then the progress bar. The data-source caveat is in a single
// footer line, not per-row.
//
// IMPORTANT data-source note: REFERENCE.md (the protocol spec) exposes only
// `tokens` and `tokens_today` in the heartbeat snapshot. Anthropic's real
// 5-hour / weekly / per-model quota is *not* in the protocol — it lives in
// the user's claude.ai subscription record and is shown by /usage in Claude
// Code. We DO NOT fabricate those percentages here.
//
void drawUsageView() {
  // Header bar
  u->setDrawColor(1);
  u->drawBox(0, TOP_H, W, 24);
  u->setDrawColor(0);
  // Title is a variable-width font, so measure it and offset the plan tag
  // dynamically -- the old hard-coded x=146 was 7 px short of the actual
  // "Plan usage limits" width and the two strings overlapped.
  u->setFont(u8g2_font_helvB14_tf);
  const char *title = "Plan usage limits";
  u->drawStr(8, TOP_H + 17, title);
  int title_w = u->getStrWidth(title);
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(8 + title_w + 12, TOP_H + 17, "Max (5x)");
  if (g_state.time_sync_ms) {
    uint32_t elapsed = (millis() - g_state.time_sync_ms) / 1000U;
    uint32_t local = g_state.time_epoch + elapsed + g_state.time_offset_sec;
    int hh = (local / 3600) % 24;
    int mm = (local / 60) % 60;
    char b[8];
    snprintf(b, sizeof(b), "%02d:%02d", hh, mm);
    u->setFont(u8g2_font_helvB14_tf);
    int tw = u->getStrWidth(b);
    u->drawStr(W - tw - 8, TOP_H + 17, b);
  }
  u->setDrawColor(1);

  int y = TOP_H + 30;

  // Three-line row primitive matching the claude.ai layout:
  //   Line 1: <title>                                          <X% used>
  //   Line 2: <reset subtitle>                                  (small/grey)
  //   Line 3: [progress bar]
  auto row = [&](const char *title,
                 const char *subtitle,
                 int pct /* -1 => n/a */) {
    u->setFont(u8g2_font_helvB12_tf);
    u->drawStr(8, y + 11, title);

    char rbuf[16];
    if (pct < 0) strcpy(rbuf, "n/a");
    else         snprintf(rbuf, sizeof(rbuf), "%d%% used", pct);
    u->setFont(u8g2_font_6x13B_tf);
    int tw = u->getStrWidth(rbuf);
    u->drawStr(W - tw - 8, y + 11, rbuf);

    u->setFont(u8g2_font_6x10_tf);
    u->drawStr(8, y + 23, subtitle);

    int bx = 8, by = y + 28, bw = W - 16, bh = 5;
    u->drawFrame(bx, by, bw, bh);
    if (pct > 0) {
      int fw = (bw - 2) * pct / 100;
      if (fw > 0) u->drawBox(bx + 1, by + 1, fw, bh - 2);
    }
    y += 38;
  };

  // ---- Row 1: Current session (5-hour rolling) ----
  // Closest local proxy: tokens_5h vs an assumed Max-5x envelope of 500K.
  // Reset countdown = 5h - age of the oldest sample in our window.
  // Both the envelope and the rolling reset are LOCAL approximations, not
  // Anthropic's real 5h subscription window — but they're computed off real
  // sensor data (the heartbeat token stream), not made up.
  constexpr unsigned long BUDGET_5H = 500000UL;
  int pct_5h = (int)((uint64_t)g_state.tokens_5h * 100UL / BUDGET_5H);
  if (pct_5h > 100) pct_5h = 100;
  char sub_5h[40];
  snprintf(sub_5h, sizeof(sub_5h),
           "Resets in 5h (rolling window since boot)");
  row("Current session", sub_5h, pct_5h);

  // ---- Section: Weekly limits ----
  // helvB12_tf is variable-width; measure rather than hardcode so the
  // subtitle never collides with the section header.
  u->setFont(u8g2_font_helvB12_tf);
  const char *wk = "Weekly limits";
  u->drawStr(8, y + 10, wk);
  int wk_w = u->getStrWidth(wk);
  u->setFont(u8g2_font_6x10_tf);
  u->drawStr(8 + wk_w + 12, y + 10, "BLE protocol does not expose these");
  y += 14;

  // ---- Row 2: All models (n/a) ----
  row("All models",
      "Resets Thu 5:00 AM   (data not in protocol)",
      -1);

  // ---- Row 3: Sonnet only (n/a) ----
  row("Sonnet only",
      "Resets Thu 5:00 AM   (no per-model split in protocol)",
      -1);

  // ---- Authoritative footer (these ARE real) ----
  u->drawHLine(8, y, W - 16);
  y += 12;
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(8, y, "Authoritative (from BLE heartbeat)");
  y += 13;
  u->setFont(u8g2_font_6x10_tf);
  char b1[24], b2[24];
  fmtThousands(g_state.tokens, b1, sizeof(b1));
  fmtThousands(g_state.tokens_today, b2, sizeof(b2));
  int32_t mid = secsToMidnight();
  char buf[120];
  if (mid > 0) {
    snprintf(buf, sizeof(buf),
             "session  %s tok    today  %s tok    midnight in %dh %02dm",
             b1, b2, (int)(mid / 3600), (int)((mid / 60) % 60));
  } else {
    snprintf(buf, sizeof(buf), "session  %s tok    today  %s tok", b1, b2);
  }
  u->drawStr(8, y, buf);
  y += 11;
  u->drawStr(8, y, "For real subscription quota run /usage in Claude Code.");
}

// Format bytes with KB/MB unit.
static String fmtBytes(unsigned long n) {
  char buf[24];
  if (n >= 1024UL * 1024UL) snprintf(buf, sizeof(buf), "%.2f MB", n / (1024.0 * 1024.0));
  else if (n >= 1024UL)     snprintf(buf, sizeof(buf), "%.1f KB", n / 1024.0);
  else                      snprintf(buf, sizeof(buf), "%lu B", n);
  return String(buf);
}

// Draw a tiny mascot strip walking back and forth across the bottom.
// Nearest-neighbour downsample of a 128x128 sprite into an `avatar`-sized
// box at (x,y), OR-ing source pixels so thin features survive the shrink.
// Uses the current g_state.anim_frame for the sprite's animation phase.
static void blitScaledSprite(SpriteId id, int x, int y, int avatar) {
  const SpriteInfo &info = SPRITES[id];
  uint8_t frame_idx = g_state.anim_frame % info.frame_count;
  static uint8_t buf[SPRITE_BYTES];
  memcpy_P(buf, info.frames[frame_idx], SPRITE_BYTES);
  for (int oy = 0; oy < avatar; oy++) {
    int sy0 = oy * SPRITE_H / avatar;
    int sy1 = (oy + 1) * SPRITE_H / avatar;
    if (sy1 <= sy0) sy1 = sy0 + 1;
    for (int ox = 0; ox < avatar; ox++) {
      int sx0 = ox * SPRITE_W / avatar;
      int sx1 = (ox + 1) * SPRITE_W / avatar;
      if (sx1 <= sx0) sx1 = sx0 + 1;
      bool any = false;
      for (int sy = sy0; sy < sy1 && !any; sy++) {
        for (int sx = sx0; sx < sx1 && !any; sx++) {
          int bit = sy * SPRITE_W + sx;
          if (buf[bit >> 3] & (1 << (bit & 7))) any = true;
        }
      }
      if (any) u->drawPixel(x + ox, y + oy);
    }
  }
}

static void drawWalkingMascot(int strip_y, int strip_h) {
  constexpr uint32_t period_ms = 16000;
  uint32_t t = millis() % period_ms;
  float phase = (float)t / (float)period_ms;     // 0..1

  // x oscillates with a soft ease at the ends.
  const int avatar = 56;
  int x_min = 12, x_max = W - 12 - avatar;
  float u01 = phase < 0.5f ? (phase * 2.0f) : (1.0f - (phase - 0.5f) * 2.0f);
  // ease out at edges
  float eased = u01 * u01 * (3 - 2 * u01);
  int x = x_min + (int)((x_max - x_min) * eased);
  int y = strip_y + (strip_h - avatar) / 2;

  // pick a sprite based on phase
  SpriteId id;
  if (phase < 0.05f || phase > 0.95f)      id = SPR_SLEEPING;       // at left edge: sleep briefly
  else if (phase < 0.45f)                  id = SPR_BUILDING;       // walking right: building
  else if (phase < 0.55f)                  id = SPR_HAPPY;          // middle peak: happy
  else if (phase < 0.95f)                  id = SPR_THINKING;       // walking left: thinking
  else                                     id = SPR_SLEEPING;

  blitScaledSprite(id, x, y, avatar);

  // Floor line under the mascot.
  u->drawHLine(8, y + avatar + 1, W - 16);
}

// CLOCK-view mascot, repurposed as a DAY-PROGRESS cursor. Its horizontal
// position encodes how far through the local day we are: left edge = 00:00,
// right edge = 24:00. The step animation is preserved for liveness, so the
// pet is simultaneously decoration AND the day-progress readout — instead
// of a plain progress bar. The pose follows a working-day rhythm keyed to
// the actual local hour `hh`:
//   00:00–10:00  dozing  (SPR_SLEEPING)   — pre-work nap
//   10:00–18:00  working (SPR_BUILDING)   — the work block (TYPING reads
//                                           as mush on the RLCD at 40px)
//   18:00–22:00  happy   (SPR_HAPPY)      — clocking off
//   22:00–24:00  asleep  (SPR_SLEEPING)   — bedtime
static void drawDayProgressMascot(int strip_y, int day_pct, int hh) {
  if (day_pct < 0) day_pct = 0;
  if (day_pct > 100) day_pct = 100;

  const int avatar = 40;
  int x_min = 34, x_max = W - 34 - avatar;
  int x = x_min + (x_max - x_min) * day_pct / 100;
  int floor_y = strip_y + avatar + 2;
  int y = floor_y - avatar;   // mascot stands ON the floor line

  // The day's timeline: a floor line with start / midday / end ticks.
  int line_l = x_min, line_r = x_max + avatar;
  u->drawHLine(line_l, floor_y, line_r - line_l);
  u->drawVLine(line_l, floor_y - 4, 5);                       // 00:00
  u->drawVLine(line_r, floor_y - 4, 5);                       // 24:00
  u->drawVLine((line_l + line_r) / 2, floor_y - 3, 4);        // 12:00

  SpriteId id;
  if      (hh < 10) id = SPR_SLEEPING;   // pre-10am: dozing
  else if (hh < 18) id = SPR_BUILDING;   // 10:00-18:00: working
  else if (hh < 22) id = SPR_HAPPY;      // 18:00-22:00: clocking off
  else              id = SPR_SLEEPING;   // 22:00+: asleep
  blitScaledSprite(id, x, y, avatar);

  // Endpoint labels + live % readout under the floor line.
  u->setFont(u8g2_font_5x7_tf);
  u->drawStr(line_l - 2, floor_y + 9, "0:00");
  const char *end = "24:00";
  int ew = u->getStrWidth(end);
  u->drawStr(line_r - ew + 2, floor_y + 9, end);
  char pc[12];
  snprintf(pc, sizeof(pc), "day %d%%", day_pct);
  u->setFont(u8g2_font_6x10_tf);
  int pw = u->getStrWidth(pc);
  u->drawStr((W - pw) / 2, floor_y + 10, pc);
}

// --- Velocity histogram ---
// Renders the response-time ring buffer (last 8 approvals) as a bar chart.
// Position 0 (leftmost) = oldest, position 7 (rightmost) = newest. Slots not
// yet filled sit on the LEFT and just show a baseline tick.
//
// The buffer is populated by protocol::sendPermission and mirrors the M5
// reference firmware's stats.velocity[]. Each value is the response time in
// seconds, capped at uint16_t. We clamp the chart scale to 60s and draw a
// small overflow caret above any bar that exceeded the cap.
static void drawVelocityHistogram(int x, int y, int w, int h) {
  // Header
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(x, y + 10, "Velocity");

  if (g_state.velocity_count == 0) {
    u->setFont(u8g2_font_6x10_tf);
    u->drawStr(x + 72, y + 10, "(no approvals recorded yet)");
    return;
  }

  // Stats line on the same row as the header.
  uint16_t vmin = 65535, vmax = 0;
  uint32_t sum = 0;
  for (int i = 0; i < g_state.velocity_count; i++) {
    int idx = (g_state.velocity_idx - 1 - i + 16) % 8;
    uint16_t v = g_state.velocity[idx];
    if (v < vmin) vmin = v;
    if (v > vmax) vmax = v;
    sum += v;
  }
  uint16_t vavg = (uint16_t)(sum / g_state.velocity_count);
  char info[80];
  snprintf(info, sizeof(info),
           "avg %us  min %us  max %us  (n=%u/8, 60s scale)",
           (unsigned)vavg, (unsigned)vmin, (unsigned)vmax,
           (unsigned)g_state.velocity_count);
  u->setFont(u8g2_font_6x10_tf);
  u->drawStr(x + 72, y + 10, info);

  // Bar region
  const uint16_t SCALE_MAX = 60;
  int bar_top = y + 14;
  int axis_label_h = 8;
  int bar_h = h - 14 - axis_label_h;
  if (bar_h < 12) bar_h = 12;
  int bar_baseline = bar_top + bar_h;

  int n = 8;
  int slot_w = w / n;
  int bar_w = slot_w - 3;
  if (bar_w < 6) bar_w = 6;

  int slots_empty = 8 - g_state.velocity_count;
  for (int i = 0; i < n; i++) {
    int bx = x + i * slot_w + (slot_w - bar_w) / 2;
    if (i < slots_empty) {
      // Empty placeholder — small tick on the baseline.
      u->drawHLine(bx, bar_baseline, bar_w);
      continue;
    }
    int k = 7 - i;  // 0 = newest (rightmost)
    int idx = (g_state.velocity_idx - 1 - k + 16) % 8;
    uint16_t v = g_state.velocity[idx];
    uint16_t capped = v > SCALE_MAX ? SCALE_MAX : v;
    int bh = (int)((uint32_t)capped * bar_h / SCALE_MAX);
    if (bh < 1) bh = 1;
    u->drawBox(bx, bar_baseline - bh, bar_w, bh);

    // Numeric label above the bar if there's room.
    int label_y = bar_baseline - bh - 1;
    if (label_y - bar_top >= 8) {
      char b[6];
      snprintf(b, sizeof(b), "%u", (unsigned)v);
      u->setFont(u8g2_font_5x7_tf);
      int tw = u->getStrWidth(b);
      u->drawStr(bx + (bar_w - tw) / 2, label_y, b);
    }

    // Overflow caret if the value exceeded the chart scale.
    if (v > SCALE_MAX) {
      int cx = bx + bar_w / 2;
      u->drawTriangle(cx - 3, bar_top + 4,
                      cx + 3, bar_top + 4,
                      cx,     bar_top);
    }
  }

  // Baseline line beneath the bars.
  u->drawHLine(x, bar_baseline + 1, w);

  // Axis labels.
  u->setFont(u8g2_font_5x7_tf);
  u->drawStr(x, bar_baseline + 8, "oldest");
  const char *nw = "newest";
  int tw = u->getStrWidth(nw);
  u->drawStr(x + w - tw, bar_baseline + 8, nw);
}

// --- System view ---
// Layout: title strip, two-column info rows with memory bars, footer.
void drawSystemView() {
  // Title pill
  u->setDrawColor(1);
  u->drawBox(0, TOP_H, W, 30);
  u->setDrawColor(0);
  u->setFont(u8g2_font_logisoso24_tr);
  const char *sys_title = "SYSTEM";
  u->drawStr(12, TOP_H + 22, sys_title);
  int sys_w = u->getStrWidth(sys_title);
  u->setFont(u8g2_font_helvB14_tf);
  u->drawStr(12 + sys_w + 14, TOP_H + 22, "diagnostics");
  u->setDrawColor(1);

  int y = TOP_H + 44;
  const int row_h = 16;
  char buf[96];

  // Two-column info rows: left col = label, right col = value with extra fields.
  auto row = [&](const char *label, const char *value) {
    u->setFont(u8g2_font_6x13B_tf);
    u->drawStr(12, y, label);
    u->setFont(u8g2_font_7x13_tf);
    u->drawStr(96, y, value);
    y += row_h;
  };

  // Battery: filtered voltage is what the rest of the firmware trusts
  // (g_state.battery_v) — show raw alongside it so a load-sag spike or a
  // bad ADC reading is visible on-device. ADC pin mV is included as a
  // sanity check on the voltage divider (200K/100K -> ratio 3).
  snprintf(buf, sizeof(buf),
           "%.2fV ewma  raw %.2fV  SOC %d%%  adc %.0fmV",
           g_state.battery_v, g_state.battery_v_raw,
           g_state.battery_pct, g_state.battery_pin_mv);
  row("Battery", buf);

  // Charge: heuristic flag + which condition triggered + the 60-s delta
  // that the trending-up rule looks at. 'F' = near-full plateau,
  // 'U' = trending up, '-' = none. The SOC is FROZEN while charging so
  // the user sees what the gauge said just before charge began.
  {
    const char *cs = g_state.charging ? "yes" : "no";
    const char *why = (g_state.charging_reason == 'J') ? "raw jump >60mV"
                    : (g_state.charging_reason == 'F') ? "near-full >4.18V"
                    : (g_state.charging_reason == 'U') ? "trending up"
                    : (g_state.charging_reason == 'S') ? "sticky carry"
                                                       : "-";
    snprintf(buf, sizeof(buf),
             "%s  reason %s  d60s %+.0fmV  n %lu",
             cs, why,
             g_state.battery_dv_60s * 1000.0f,
             (unsigned long)g_state.battery_samples);
    row("Charge", buf);
  }

  if (!isnan(g_state.temp_c)) {
    snprintf(buf, sizeof(buf), "%.1f \xB0""C    Humidity %.0f%%   (SHTC3)",
             g_state.temp_c, g_state.humidity_pct);
  } else strcpy(buf, "SHTC3 not detected on I2C 0x70");
  row("Climate", buf);

  if (ble_nus::connected()) {
    uint32_t last = g_state.last_heartbeat_ms ? (millis() - g_state.last_heartbeat_ms) / 1000 : 0;
    snprintf(buf, sizeof(buf), "connected   hb %lus ago   NUS svc 6e400001",
             (unsigned long)last);
  } else {
    snprintf(buf, sizeof(buf), "advertising as %s", g_state.name.c_str());
  }
  row("BLE", buf);

  if (g_state.time_sync_ms) {
    uint32_t elapsed = (millis() - g_state.time_sync_ms) / 1000U;
    const char *rtc_tag = !rtc::isPresent()       ? " RTC:absent"
                        : rtc::hasValidTime()     ? " RTC:ok"
                                                  : " RTC:no-vbat";
    snprintf(buf, sizeof(buf), "synced %lus ago   tz UTC%+ld:%02ld%s",
             (unsigned long)elapsed,
             (long)(g_state.time_offset_sec / 3600),
             (long)((labs(g_state.time_offset_sec) / 60) % 60),
             rtc_tag);
  } else {
    snprintf(buf, sizeof(buf), "not synced (RTC %s)",
             rtc::isPresent() ? "present but never written"
                              : "absent");
  }
  row("Time", buf);

  uint32_t up = millis() / 1000;
  snprintf(buf, sizeof(buf), "%luh %02lum %02lus   turns %lu",
           up / 3600, (up / 60) % 60, up % 60, (unsigned long)g_state.turns_done);
  row("Uptime", buf);

  // Power row -- aggregated power-relevant knobs so we can verify
  // current-saving changes actually shipped. CPU should be 80 MHz idle,
  // loop should achieve ~50 Hz with a 20 ms delay, BLE advertising
  // 1000 ms when not connected.
  snprintf(buf, sizeof(buf),
           "cpu %u MHz  loop %lu Hz  BLE adv %u ms",
           (unsigned)getCpuFrequencyMhz(),
           (unsigned long)diag::loop_hz,
           (unsigned)ble_nus::advertisingIntervalMs());
  row("Power", buf);

  // IMU row -- raw accel + derived orientation. "face-down" matches the
  // nap-trigger condition in imu::loop.
  if (imu::isPresent()) {
    const char *orient = imu::lastAzG() < -0.7f ? "face-down"
                       : imu::lastAzG() >  0.7f ? "face-up"
                       : "tilted";
    bool dizzy = g_state.dizzy_until_ms && millis() < g_state.dizzy_until_ms;
    snprintf(buf, sizeof(buf), "a %+.2f %+.2f %+.2fg  %s%s",
             imu::lastAxG(), imu::lastAyG(), imu::lastAzG(),
             orient, dizzy ? "  [DIZZY]" : "");
  } else {
    strcpy(buf, "QMI8658 not detected on I2C 0x6A/0x6B");
  }
  row("IMU", buf);

  // Storage row — combines LittleFS usage with the active pack identity.
  // The old "Stats" row (energy/level/mood text) was dropped because it
  // duplicated the MAIN view dashboard.
  {
    unsigned long fs_tot  = xfer::fsTotalBytes();
    unsigned long fs_used = xfer::fsUsedBytes();
    String pack_label;
    if (pack::loadPending()) {
      pack_label = "decoding...";
    } else if (pack::activeName().length()) {
      pack_label = pack::activeName() + " (" + String(pack::overrideCount()) + "/16)";
    } else {
      pack_label = "(built-in)";
    }
    if (fs_tot > 0) {
      String used_s = fmtBytes(fs_used);
      String tot_s  = fmtBytes(fs_tot);
      snprintf(buf, sizeof(buf), "fs %s/%s  pack %s",
               used_s.c_str(), tot_s.c_str(), pack_label.c_str());
    } else {
      snprintf(buf, sizeof(buf), "LittleFS unavailable  pack %s",
               pack_label.c_str());
    }
    row("Storage", buf);
  }

  // ---- Memory section with bars ----
  int my = y + 6;
  u->drawHLine(8, my - 4, W - 16);

  auto memBar = [&](const char *label, unsigned long used, unsigned long total) {
    if (total == 0) {
      u->setFont(u8g2_font_6x13B_tf);
      u->drawStr(12, my + 10, label);
      u->setFont(u8g2_font_7x13_tf);
      u->drawStr(96, my + 10, "not available");
      my += 22;
      return;
    }
    int pct = (int)((uint64_t)used * 100UL / total);
    String used_s = fmtBytes(used);
    String tot_s  = fmtBytes(total);
    char b[64];
    snprintf(b, sizeof(b), "%s / %s  (%d%%)", used_s.c_str(), tot_s.c_str(), pct);

    // label + value text on a row
    u->setFont(u8g2_font_6x13B_tf);
    u->drawStr(12, my + 10, label);
    u->setFont(u8g2_font_6x10_tf);
    u->drawStr(96, my + 10, b);
    // bar BELOW the text row
    int bx = 12, by = my + 14;
    int bw = W - 24;
    drawProgressBar(bx, by, bw, 8, pct);
    my += 28;
  };

  unsigned long heap_tot  = ESP.getHeapSize();
  unsigned long heap_used = heap_tot - ESP.getFreeHeap();
  memBar("Heap",  heap_used, heap_tot);

  unsigned long psr_tot = ESP.getPsramSize();
  if (psr_tot > 0) {
    unsigned long psr_used = psr_tot - ESP.getFreePsram();
    memBar("PSRAM", psr_used, psr_tot);
  } else {
    memBar("PSRAM", 0, 0);
  }

  // ---- Bottom strip: velocity histogram OR walking mascot ----
  // Below memory section, above the firmware tag + bottom bar. When we have
  // at least one recorded approval, the histogram takes priority because
  // diagnostic data > decoration; otherwise we show the walking mascot.
  int strip_top = my + 4;
  int strip_h   = H - BOT_H - 14 - strip_top;
  if (strip_h > 30) {
    if (g_state.velocity_count > 0) {
      drawVelocityHistogram(12, strip_top, W - 24, strip_h);
    } else {
      drawWalkingMascot(strip_top, strip_h);
    }
  }

  // Build / firmware tag in bottom-left area above bottom bar.
  u->setFont(u8g2_font_6x10_tf);
  const char *build = "Claude-desktop-buddy   built " __DATE__ " " __TIME__;
  u->drawStr(12, H - BOT_H - 4, build);
}

// --- Clock view ---
// Dedicated big-clock screen. Hours/minutes in 50 px digits centred top,
// seconds and a colon that blinks once a second underneath. Date and
// day-of-week on a second line. Tiny status footer.
//
// Data source: g_state.time_epoch / time_offset_sec / time_sync_ms, which
// gets seeded from the PCF85063 on boot and refreshed by BLE `{"time":...}`.
// If neither source has fired, we show "--:--" and a "waiting for time sync"
// hint instead of fabricating.
// CLOCK view — redesigned as a glance-information dashboard rather than a
// bare wall clock. Four stacked zones separated by thin rules:
//
//   A. Big HH:MM (centred) + weekday tag (top-left) + 60-tick seconds row
//      that replaces the old blinking colon. The tick row shows elapsed
//      vs remaining seconds in the current minute as a single horizontal
//      timeline — gives the clock visual rhythm without flashing pixels.
//   B. Full date heading + calendar metadata (weekday, week #, day-of-
//      year). The old "Thu, Jan 15 2026" single line was information-
//      starved — this layout reads like a calendar app's header.
//   C. Three equal sensor cards (TEMP / HUMIDITY / BATTERY) using the
//      SHTC3 + battery data the device already collects. Card frames
//      are the structural element, not decoration.
//   D. Day-progress bar + footer meta (TZ / sync age / source).
//
// The walking mascot strip that used to fill the bottom third was dropped
// — it added no information and the new sensor cards are a better use of
// that space on a CLOCK view.
void drawClockView() {
  bool have_time = (g_state.time_sync_ms != 0);
  uint32_t local_sec = 0;
  if (have_time) {
    uint32_t elapsed = (millis() - g_state.time_sync_ms) / 1000U;
    local_sec = g_state.time_epoch + elapsed + g_state.time_offset_sec;
  }
  int hh = (local_sec / 3600) % 24;
  int mm = (local_sec / 60) % 60;
  int ss = (local_sec) % 60;
  uint32_t day_sec = local_sec % 86400UL;
  int day_pct = (int)((uint64_t)day_sec * 100UL / 86400UL);

  if (!have_time) {
    u->setFont(u8g2_font_wqy14_t_gb2312);
    const char *t = "等待时间同步";
    int tw = u->getUTF8Width(t);
    u->drawUTF8((W - tw) / 2, TOP_H + 120, t);
    const char *t2 = ble_nus::connected()
      ? "桌面端将很快推送当前时间"
      : "请在 Claude 桌面端打开 Hardware Buddy 并连接";
    int t2w = u->getUTF8Width(t2);
    u->drawUTF8((W - t2w) / 2, TOP_H + 146, t2);
    return;
  }

  // ---- Date math ---- (weekday index order starts Thursday: 1970-01-01)
  static const char *DOW_CN[] = {"星期四", "星期五", "星期六", "星期日",
                                 "星期一", "星期二", "星期三"};
  static const uint8_t MD[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  auto isLeap = [](int y) {
    return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  };
  uint32_t days_total = local_sec / 86400UL;
  int dow = days_total % 7;   // 1970-01-01 was a Thursday
  uint32_t days = days_total;
  int year = 1970;
  while (true) {
    uint32_t yd = isLeap(year) ? 366 : 365;
    if (days < yd) break;
    days -= yd;
    year++;
  }
  int year_days_total = isLeap(year) ? 366 : 365;
  int day_of_year = (int)days + 1;
  int mon = 1;
  for (int m = 0; m < 12; m++) {
    uint32_t dm = MD[m];
    if (m == 1 && isLeap(year)) dm = 29;
    if (days < dm) break;
    days -= dm;
    mon++;
  }
  int day = (int)days + 1;
  int week = (day_of_year + 6) / 7;   // simple week number, not ISO 8601

  // ============ Zone A: Big HH:MM:SS + weekday + seconds tick row ============
  u->setFont(u8g2_font_wqy14_t_gb2312);
  u->drawUTF8(20, TOP_H + 16, DOW_CN[dow]);

  // Big HH:MM with :SS appended on the SAME baseline (smaller font) so it
  // reads as one time, wristwatch-style — the whole HH:MM:SS block is
  // centred together. logisoso fonts are numerals-only ("tn"), so the
  // colon between HH and MM is drawn manually as two stacked filled boxes.
  char hh_buf[4], mm_buf[4], ss_buf[6];
  snprintf(hh_buf, sizeof(hh_buf), "%02d", hh);
  snprintf(mm_buf, sizeof(mm_buf), "%02d", mm);
  snprintf(ss_buf, sizeof(ss_buf), ":%02d", ss);

  u->setFont(u8g2_font_logisoso50_tn);
  int hh_w = u->getStrWidth(hh_buf);
  int mm_w = u->getStrWidth(mm_buf);
  u->setFont(u8g2_font_logisoso24_tn);
  int ss_w = u->getStrWidth(ss_buf);

  int colon_w = 14;
  int gap     = 6;
  int ss_gap  = 10;
  int hm_w    = hh_w + gap + colon_w + gap + mm_w;   // width of HH:MM
  int block_w = hm_w + ss_gap + ss_w;                // + the small :SS
  int block_x = (W - block_w) / 2;
  int big_y   = TOP_H + 62;                          // shared baseline

  u->setFont(u8g2_font_logisoso50_tn);
  u->drawStr(block_x, big_y, hh_buf);
  int colon_x = block_x + hh_w + gap + (colon_w - 6) / 2;
  u->drawBox(colon_x, big_y - 36, 6, 6);
  u->drawBox(colon_x, big_y - 12, 6, 6);
  u->drawStr(block_x + hh_w + gap + colon_w + gap, big_y, mm_buf);
  // :SS — same baseline (big_y), so it sits flush at the bottom of the
  // big digits like a subdial.
  u->setFont(u8g2_font_logisoso24_tn);
  u->drawStr(block_x + hm_w + ss_gap, big_y, ss_buf);

  // Seconds tick row — 60 vertical ticks across the full content width.
  // Past seconds = short ticks, current second = tall 3px cursor, future
  // seconds = minimal stubs. The bottom rule of this row IS the Zone A /
  // Zone B divider — we don't draw a second horizontal line right beneath
  // it (two rules that close together read as a crowded smudge, which is
  // what pushed the date heading up against the seconds line before).
  int sep1_y;
  {
    int tr_x = 24, tr_y = big_y + 10;
    int tr_w = W - 48;
    int tr_h = 12;
    sep1_y = tr_y + tr_h - 1;
    u->drawHLine(tr_x, sep1_y, tr_w);
    for (int s = 0; s < 60; s++) {
      int tx = tr_x + (int)((int32_t)s * tr_w / 60);
      if (s == ss)        u->drawBox(tx, tr_y + 1, 3, tr_h - 2);
      else if (s < ss)    u->drawVLine(tx, tr_y + tr_h - 5, 4);
      else                u->drawVLine(tx, tr_y + tr_h - 3, 2);
    }
  }

  // ============ Zone B: Date heading + calendar metadata (Chinese) ============
  // wqy14 is the only Chinese font included; it also carries ASCII/digits, so
  // a whole "YYYY年M月D日" line renders with one drawUTF8 call.
  u->setFont(u8g2_font_wqy14_t_gb2312);
  char date_buf[48];
  snprintf(date_buf, sizeof(date_buf), "%d年%d月%d日", year, mon, day);
  int date_y = sep1_y + 22;
  u->drawUTF8(20, date_y, date_buf);

  char meta_buf[64];
  snprintf(meta_buf, sizeof(meta_buf), "第 %d 周      全年第 %d / %d 天",
           week, day_of_year, year_days_total);
  u->drawUTF8(20, date_y + 18, meta_buf);

  // Zone B / Zone C separator.
  int sep2_y = date_y + 26;
  u->drawHLine(20, sep2_y, W - 40);

  // ============ Zone C: Three sensor cards ============
  int card_h = 40;
  {
    int cards_y  = sep2_y + 6;
    int margin   = 20;
    int gap_c    = 8;
    int card_w   = (W - 2 * margin - 2 * gap_c) / 3;

    // label is Chinese (drawUTF8/wqy14, white-on-black band); value is
    // numeric (helvB18). Band is 15 px to fit the 14 px Chinese glyphs.
    auto drawCard = [&](int idx, const char *label, const char *value) {
      int cx = margin + idx * (card_w + gap_c);
      u->drawFrame(cx, cards_y, card_w, card_h);
      u->drawBox(cx, cards_y, card_w, 15);
      u->setDrawColor(0);
      u->setFont(u8g2_font_wqy14_t_gb2312);
      int lw = u->getUTF8Width(label);
      u->drawUTF8(cx + (card_w - lw) / 2, cards_y + 12, label);
      u->setDrawColor(1);
      // Value centred in the ~25 px below the 15 px label band.
      u->setFont(u8g2_font_helvB18_tf);
      int vw = u->getStrWidth(value);
      u->drawStr(cx + (card_w - vw) / 2, cards_y + 33, value);
    };

    char b1[16], b2[16], b3[16];
    if (!isnan(g_state.temp_c))      snprintf(b1, sizeof(b1), "%.1f\xB0""C", g_state.temp_c);
    else                              strcpy(b1, "--");
    if (!isnan(g_state.humidity_pct)) snprintf(b2, sizeof(b2), "%.0f%%",      g_state.humidity_pct);
    else                              strcpy(b2, "--");
    if (g_state.battery_pct >= 0)     snprintf(b3, sizeof(b3), "%d%%%s",      g_state.battery_pct,
                                                                              g_state.charging ? "+" : "");
    else                              strcpy(b3, "--");

    drawCard(0, "温度", b1);
    drawCard(1, "湿度", b2);
    drawCard(2, "电量", b3);
  }

  // ============ Zone D: Day-progress mascot + footer meta ============
  // The walking mascot doubles as the day-progress indicator: its x
  // position = fraction of the local day elapsed (00:00 left .. 24:00
  // right). Replaces a plain progress bar — decoration and information
  // in one element.
  int cards_bottom = sep2_y + 6 + card_h;
  drawDayProgressMascot(cards_bottom + 4, day_pct, hh);

  // Footer meta: timezone \xB7 sync age \xB7 source.
  {
    long tz_h = g_state.time_offset_sec / 3600;
    long tz_m = (labs(g_state.time_offset_sec) / 60) % 60;
    uint32_t sync_age = (millis() - g_state.time_sync_ms) / 1000;
    const char *src = rtc::hasValidTime() ? "RTC" : "BLE";
    char foot[80];
    const char *unit; unsigned long n;
    if      (sync_age < 60)   { unit = "s"; n = sync_age; }
    else if (sync_age < 3600) { unit = "m"; n = sync_age / 60; }
    else                      { unit = "h"; n = sync_age / 3600; }
    snprintf(foot, sizeof(foot),
             "UTC%+ld:%02ld   \xB7   synced %lu%s ago via %s",
             tz_h, tz_m, n, unit, src);
    u->setFont(u8g2_font_6x10_tf);
    u->drawStr(20, H - BOT_H - 6, foot);
  }
}

void drawOfflineHint() {
  // Overlay when not connected — show a subtle hint band over main view.
  // Suppressed in demo mode (the user is watching the showcase, not waiting
  // on a real link).
  if (linkActive()) return;
  const int box_h = 18;
  int y = H - BOT_H - box_h - 2;
  u->setDrawColor(1);
  u->drawBox(0, y, W, box_h);
  u->setDrawColor(0);
  // 7x13 fixed-width: 49 chars * 7 = 343 px, fits inside W=400 with margin.
  // Wider variable fonts (helvB12) overflow the screen.
  u->setFont(u8g2_font_7x13_tf);
  const char *t = "Open Hardware Buddy in Claude desktop and connect";
  int tw = u->getStrWidth(t);
  int tx = (W - tw) / 2;
  if (tx < 4) tx = 4;
  u->drawStr(tx, y + 13, t);
  u->setDrawColor(1);
}
} // namespace

void ui::begin(U8G2 *uu) {
  u = uu;
}

// Full-screen transcript history overlay. Shows all 8 entries from the most
// recent heartbeat (vs. the MAIN view which only has space for the latest 3).
// Opened by long-press of KEY or BOOT on MAIN view; any subsequent button
// press closes it (handled by main.cpp's button router).
static void drawHistoryOverlay() {
  u->clearBuffer();
  u->setDrawColor(1);

  // Title bar
  u->drawBox(0, 0, W, TOP_H);
  u->setDrawColor(0);
  u->setFont(u8g2_font_helvB14_tf);
  u->drawStr(8, 16, "Transcript history");

  // Just the filled-slot count in the corner. (The msg used to be embedded
  // here but byte-truncating a Chinese msg into a fixed buffer produced
  // mojibake; msg is shown on the MAIN view anyway.)
  int filled = 0;
  for (int i = 0; i < 8; i++) if (g_state.entries[i].length()) filled++;
  char ctitle[16];
  snprintf(ctitle, sizeof(ctitle), "%d/8", filled);
  u->setFont(u8g2_font_6x13B_tf);
  int tw = u->getStrWidth(ctitle);
  u->drawStr(W - tw - 8, 16, ctitle);
  u->setDrawColor(1);

  // 8 rows × 26 px ≈ 208 px (TOP_H + 18 .. H - BOT_H - 10 spans ~242 px)
  int row_h = 26;
  int y = TOP_H + 10;

  u->setFont(u8g2_font_7x13_tf);
  for (int i = 0; i < 8; i++) {
    // Row index label
    char label[6];
    snprintf(label, sizeof(label), "%d.", i + 1);
    u->setFont(u8g2_font_6x13B_tf);
    u->drawStr(8, y + 14, label);

    if (g_state.entries[i].length()) {
      // CJK-capable, UTF-8-safe single-line truncation.
      u->setFont(u8g2_font_wqy14_t_gb2312);
      u->drawUTF8(34, y + 14, clipUTF8(g_state.entries[i], W - 40).c_str());
    } else {
      u->setFont(u8g2_font_6x10_tf);
      u->drawStr(34, y + 14, "(empty)");
    }

    // Faint separator
    if (i < 7) u->drawHLine(8, y + row_h - 4, W - 16);
    y += row_h;
  }

  // Footer hint
  u->drawBox(0, H - BOT_H, W, BOT_H);
  u->setDrawColor(0);
  u->setFont(u8g2_font_6x10_tf);
  const char *hint = "[any key] close history overlay";
  u->drawStr(8, H - 5, hint);
  // Right side: source = current heartbeat
  const char *src = ble_nus::connected() ? "live heartbeat" : "stale (BLE offline)";
  int sw = u->getStrWidth(src);
  u->drawStr(W - sw - 6, H - 5, src);
  u->setDrawColor(1);

  u->sendBuffer();
}

// Settings menu — full-screen list. Selected row inverted; destructive
// items get a small "!" marker. Bottom hint bar shows the active key map.
static void drawMenu() {
  u->clearBuffer();
  u->setDrawColor(1);

  // Title bar
  u->drawBox(0, 0, W, TOP_H);
  u->setDrawColor(0);
  u->setFont(u8g2_font_wqy14_t_gb2312);
  u->drawUTF8(8, 16, "设置");
  u->setFont(u8g2_font_6x10_tf);
  // Subtitle counter
  char sub[16];
  snprintf(sub, sizeof(sub), "%d/%d", g_state.menu_selected + 1, menu::itemCount());
  int sw = u->getStrWidth(sub);
  u->drawStr(W - sw - 8, 16, sub);
  u->setDrawColor(1);

  // Item list
  int row_h = 28;
  int y = TOP_H + 8;
  for (int i = 0; i < menu::itemCount(); i++) {
    bool sel = (i == g_state.menu_selected);
    if (sel) {
      u->setDrawColor(1);
      u->drawRBox(6, y, W - 12, row_h - 4, 4);
      u->setDrawColor(0);
    }
    // Marker for destructive items
    u->setFont(u8g2_font_helvB14_tf);
    if (menu::itemIsDestructive(i)) {
      u->drawStr(14, y + 18, "!");
    }
    // Label (Chinese -> drawUTF8 + wqy14)
    u->setFont(u8g2_font_wqy14_t_gb2312);
    u->drawUTF8(28, y + 18, menu::itemLabel(i));
    // Inline value (e.g. Sound 开/关), right-aligned
    const char *val = menu::itemValue(i);
    if (val && val[0]) {
      int vw = u->getUTF8Width(val);
      u->drawUTF8(W - vw - 16, y + 18, val);
    }
    u->setDrawColor(1);
    y += row_h;
  }

  // Bottom hint
  u->drawBox(0, H - BOT_H, W, BOT_H);
  u->setDrawColor(0);
  u->setFont(u8g2_font_6x10_tf);
  u->drawStr(8, H - 5,
             "[KEY] down  [BOOT] up  [KEY long] activate  [BOOT long] close");
  u->setDrawColor(1);

  u->sendBuffer();
}

// Full-screen confirm screen for destructive menu items. The first KEY-long
// from the menu surfaces this; a second KEY-long fires the action.
static void drawMenuConfirm() {
  u->clearBuffer();
  u->setDrawColor(1);

  // Title bar with attention chip
  u->drawBox(0, 0, W, TOP_H);
  u->setDrawColor(0);
  u->setFont(u8g2_font_helvB14_tf);
  u->drawStr(8, 16, "Confirm");
  u->setDrawColor(1);

  // Big "!" warning to the left
  int icon_x = 24;
  int icon_y = TOP_H + 50;
  u->setFont(u8g2_font_logisoso50_tn);
  // logisoso50 is digits-only so we can't render "!"; draw a manual
  // triangle warning instead.
  int tri_size = 60;
  int tri_cx = icon_x + tri_size / 2;
  int tri_top = icon_y - tri_size;
  u->drawTriangle(tri_cx, tri_top,
                  icon_x, icon_y,
                  icon_x + tri_size, icon_y);
  // Hollow inside
  u->setDrawColor(0);
  u->drawTriangle(tri_cx, tri_top + 8,
                  icon_x + 6, icon_y - 4,
                  icon_x + tri_size - 6, icon_y - 4);
  u->setDrawColor(1);
  // Exclamation mark inside the triangle
  int bang_x = tri_cx - 2;
  u->drawBox(bang_x, tri_top + 16, 4, 22);
  u->drawBox(bang_x, tri_top + 42, 4, 4);

  // Title + body to the right of the icon (Chinese -> drawUTF8 + wqy14).
  int tx = icon_x + tri_size + 18;
  u->setFont(u8g2_font_wqy14_t_gb2312);
  u->drawUTF8(tx, TOP_H + 22, menu::confirmTitle());

  // Body — split on '\n' manually (newline is ASCII, safe inside UTF-8).
  const char *body = menu::confirmBody();
  int line_y = TOP_H + 48;
  const char *line_start = body;
  while (*line_start) {
    const char *nl = strchr(line_start, '\n');
    int line_len = nl ? (nl - line_start) : (int)strlen(line_start);
    char tmp[128];
    if (line_len > (int)sizeof(tmp) - 1) line_len = sizeof(tmp) - 1;
    memcpy(tmp, line_start, line_len);
    tmp[line_len] = 0;
    u->drawUTF8(tx, line_y, tmp);
    line_y += 18;
    if (!nl) break;
    line_start = nl + 1;
  }

  // Action buttons along the bottom: CONFIRM inverted, CANCEL outline.
  int hy = H - BOT_H - 38;
  int half = (W - 24) / 2;
  u->setDrawColor(1);
  u->drawRBox(8, hy, half, 28, 6);
  u->setDrawColor(0);
  u->setFont(u8g2_font_helvB14_tf);
  const char *a = "[KEY long] CONFIRM";
  int aw = u->getStrWidth(a);
  u->drawStr(8 + (half - aw) / 2, hy + 19, a);
  u->setDrawColor(1);

  u->drawRFrame(W - 8 - half, hy, half, 28, 6);
  const char *c = "[BOOT] CANCEL";
  int cw = u->getStrWidth(c);
  u->drawStr(W - 8 - half + (half - cw) / 2, hy + 19, c);

  u->sendBuffer();
}

// Full-screen passkey display during pairing. Centered 6-digit number
// in a big font, with instructions.
static void drawPasskeyScreen() {
  u->clearBuffer();
  u->setDrawColor(1);
  u->setFont(u8g2_font_helvB18_tf);
  const char *t = "Pair this device";
  int tw = u->getStrWidth(t);
  u->drawStr((W - tw) / 2, 50, t);

  u->setFont(u8g2_font_6x13B_tf);
  const char *t2 = "Enter this passkey on the desktop:";
  int t2w = u->getStrWidth(t2);
  u->drawStr((W - t2w) / 2, 75, t2);

  // The passkey itself, huge.
  char buf[8];
  snprintf(buf, sizeof(buf), "%06lu", (unsigned long)g_state.passkey);
  u->setFont(u8g2_font_logisoso50_tn);
  int bw = u->getStrWidth(buf);
  u->drawStr((W - bw) / 2, 175, buf);

  u->setFont(u8g2_font_6x10_tf);
  const char *t3 = "Once accepted, the link will be encrypted.";
  int t3w = u->getStrWidth(t3);
  u->drawStr((W - t3w) / 2, 220, t3);
  const char *t4 = "Cancel pairing in the desktop window to abort.";
  int t4w = u->getStrWidth(t4);
  u->drawStr((W - t4w) / 2, 235, t4);

  u->sendBuffer();
}

bool ui::render() {
  if (!u) return false;
  uint32_t now = millis();

  // Advance animation frame at ~5 Hz.
  if (now - lastFrameMs > 200) {
    lastFrameMs = now;
    g_state.anim_frame++;
  }

  // Passkey overlay takes priority over everything.
  if (g_state.passkey_displaying) {
    // Hash includes the passkey so the screen updates if a new one is generated.
    uint32_t h = g_state.passkey ^ 0xBEEF;
    if (h != lastHash) {
      lastHash = h;
      drawPasskeyScreen();
    }
    return true;
  }

  // Settings menu (and its destructive-action confirm screen) takes
  // priority over history / normal views — the menu owns all input while
  // open so it's hidden state if we don't render it.
  if (g_state.menu_open) {
    uint32_t h = 0xDEADBEEFu
               ^ (uint32_t)g_state.menu_selected
               ^ ((uint32_t)g_state.menu_confirming << 8)
               ^ ((uint32_t)g_state.sound_on << 16);
    if (h != lastHash) {
      lastHash = h;
      if (g_state.menu_confirming) drawMenuConfirm();
      else                         drawMenu();
    }
    return true;
  }

  // History overlay takes priority over normal views (but not over passkey
  // or menu).
  // Mix entry text + connection state into the hash so the screen updates as
  // new transcripts arrive without flicker between heartbeats.
  if (g_state.history_open) {
    uint32_t h = 0xC0FFEEu;
    for (int i = 0; i < 8; i++) {
      const char *s = g_state.entries[i].c_str();
      for (const uint8_t *p = (const uint8_t *)s; *p; ++p) h = h * 31u + *p;
    }
    h ^= ble_nus::connected() ? 1 : 0;
    h ^= (uint32_t)g_state.msg.length() << 16;
    if (h != lastHash) {
      lastHash = h;
      drawHistoryOverlay();
    }
    return true;
  }

  uint32_t h = hashState();
  if (h == lastHash && (now - lastDrawMs) < 150) return false;
  lastHash = h;
  lastDrawMs = now;

  u->clearBuffer();
  u->setDrawColor(1);

  drawTopBar();

  switch (g_state.view) {
    case 1:  drawUsageView();  break;
    case 2:  drawSystemView(); break;
    case 3:  drawClockView();  break;
    default: drawMainView();   break;
  }

  drawOfflineHint();

  drawBottomBar();

  u->sendBuffer();
  return true;
}

void ui::showSleeping() {
  if (!u) return;
  u->clearBuffer();
  u->setDrawColor(1);
  blitScaledSprite(SPR_SLEEPING, (W - 96) / 2, 44, 96);
  u->setFont(u8g2_font_helvB18_tf);
  const char *t = "Sleeping";
  int tw = u->getStrWidth(t);
  u->drawStr((W - tw) / 2, 188, t);
  u->setFont(u8g2_font_6x13_tf);
  const char *t2 = "press KEY to wake";
  int t2w = u->getStrWidth(t2);
  u->drawStr((W - t2w) / 2, 212, t2);
  u->sendBuffer();
}
