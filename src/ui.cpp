#include "ui.h"
#include "state.h"
#include "ble_nus.h"
#include "sprites.h"
#include <pgmspace.h>

namespace {
U8G2 *u = nullptr;

// Rotated to U8G2_R1 => landscape 400 x 300.
constexpr int W = 400;
constexpr int H = 300;
constexpr int TOP_H = 22;
constexpr int BOT_H = 18;

// Forward decls for helpers defined further down.
static void drawProgressBar(int x, int y, int w, int h, int pct);

// Map state to sprite ID.
SpriteId moodToSprite() {
  if (!ble_nus::connected()) return SPR_SLEEPING;
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
  if (!ble_nus::connected()) return false;
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
  for (int i = 0; i < 3; i++) mix(g_state.entries[i].c_str(), g_state.entries[i].length());
  uint8_t conn = ble_nus::connected() ? 1 : 0;
  mix(&conn, 1);
  uint32_t af = g_state.anim_frame;
  mix(&af, sizeof(af));
  return h;
}

// --- Sprite blit ---
// Pull one frame from PROGMEM into a local RAM buffer (1152 B), then drawXBM.
void drawSprite(int x, int y, SpriteId id) {
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
  drawIndicator("BT", ble_nus::connected());

  u->setDrawColor(1);
}

// --- Bottom bar ---
void drawBottomBar() {
  u->setDrawColor(1);
  u->drawHLine(0, H - BOT_H, W);
  u->setFont(u8g2_font_6x10_tf);
  const char *title;
  switch (g_state.view) {
    case 0: title = "MAIN";   break;
    case 1: title = "USAGE";  break;
    case 2: title = "SYSTEM"; break;
    default: title = "?";
  }
  char left[64];
  snprintf(left, sizeof(left), "[KEY] next   [BOOT] prev   view: %s (%u/3)",
           title, (unsigned)(g_state.view + 1));
  u->drawStr(6, H - 5, left);
  // BLE state indicator on right
  const char *r = ble_nus::connected() ? "BLE: connected" : "BLE: advertising";
  int rw = u->getStrWidth(r);
  u->drawStr(W - rw - 6, H - 5, r);
}

void drawWrappedText(int x, int y, int max_w, int line_h, const String &text, int max_lines) {
  if (text.length() == 0) return;
  u8g2_uint_t cw = u->getMaxCharWidth();
  if (cw == 0) cw = 7;
  int chars_per_line = max_w / cw;
  if (chars_per_line < 8) chars_per_line = 8;
  int i = 0, line = 0;
  while (i < (int)text.length() && line < max_lines) {
    int end = i + chars_per_line;
    if (end >= (int)text.length()) end = text.length();
    else {
      int sp = text.lastIndexOf(' ', end);
      if (sp > i + 6) end = sp;
    }
    u->drawStr(x, y + line * line_h, text.substring(i, end).c_str());
    i = end;
    while (i < (int)text.length() && text[i] == ' ') i++;
    line++;
  }
}

// Derive a single mood adjective from energy tier (0..5) + state.
static const char *moodAdjective() {
  int e = (int)g_state.energy_tier;
  bool running = g_state.running > 0;
  bool waiting = g_state.waiting > 0;
  bool offline = !ble_nus::connected();
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

static void fmtDuration(uint32_t secs, char *out, size_t n) {
  uint32_t h = secs / 3600, m = (secs / 60) % 60, s = secs % 60;
  if (h > 0) snprintf(out, n, "%luh %02lum %02lus", (unsigned long)h, (unsigned long)m, (unsigned long)s);
  else if (m > 0) snprintf(out, n, "%lum %02lus", (unsigned long)m, (unsigned long)s);
  else snprintf(out, n, "%lus", (unsigned long)s);
}

// Draw a row of N filled / empty pip discs.
static void drawPips(int x, int y, int n, int filled, int radius, int spacing) {
  for (int i = 0; i < n; i++) {
    int cx = x + i * spacing + radius;
    if (i < filled) u->drawDisc(cx, y, radius);
    else            u->drawCircle(cx, y, radius);
  }
}

// --- Main view: dashboard layout ---
void drawMainView() {
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
  int rx = spr_x + SPRITE_W + 12;
  int ry = TOP_H + 6;
  char buf[64];

  // Mood (big italic)
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(rx, ry + 10, "Mood");
  u->setFont(u8g2_font_logisoso24_tr);
  u->drawStr(rx + 50, ry + 18, moodAdjective());
  ry += 24;

  // Energy as 5 pips (matches the M5StickC reference model)
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(rx, ry + 10, "Energy");
  drawPips(rx + 50, ry + 6, 5, g_state.energy_tier, 5, 14);
  snprintf(buf, sizeof(buf), "%d/5", g_state.energy_tier);
  u->setFont(u8g2_font_6x10_tf);
  u->drawStr(rx + 50 + 5 * 14 + 6, ry + 10, buf);
  ry += 16;

  // Fed as 10 small pips. fed = (tokens % 50000) / 5000
  uint8_t fed = (uint8_t)((g_state.tokens % TOKENS_PER_LEVEL) / TOKENS_PER_FED_PIP);
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(rx, ry + 10, "Fed");
  drawPips(rx + 50, ry + 6, 10, fed, 3, 8);
  snprintf(buf, sizeof(buf), "%u/10", (unsigned)fed);
  u->setFont(u8g2_font_6x10_tf);
  u->drawStr(rx + 50 + 10 * 8 + 6, ry + 10, buf);
  ry += 16;

  // Level (large)
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(rx, ry + 10, "Level");
  u->setFont(u8g2_font_logisoso24_tr);
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)g_state.level);
  u->drawStr(rx + 50, ry + 18, buf);
  // Next-level token countdown to the right
  u->setFont(u8g2_font_6x10_tf);
  uint32_t to_next = TOKENS_PER_LEVEL - (g_state.tokens % TOKENS_PER_LEVEL);
  snprintf(buf, sizeof(buf), "%lu tok to L%lu",
           (unsigned long)to_next, (unsigned long)(g_state.level + 1));
  u->drawStr(rx + 100, ry + 18, buf);
  ry += 28;

  // Divider then Now line
  u->drawHLine(rx, ry, W - rx - 6);
  ry += 12;
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(rx, ry, "Now");
  u->setFont(u8g2_font_7x13_tf);
  if (g_state.prompt.active) {
    String s = "approval: " + g_state.prompt.tool;
    u->drawStr(rx + 44, ry, s.c_str());
  } else if (g_state.running > 0) {
    char d[24];
    uint32_t secs = g_state.run_started_ms ? (millis() - g_state.run_started_ms) / 1000 : 0;
    fmtDuration(secs, d, sizeof(d));
    snprintf(buf, sizeof(buf), "%d session%s for %s",
             g_state.running, g_state.running == 1 ? "" : "s", d);
    u->drawStr(rx + 44, ry, buf);
  } else if (!ble_nus::connected()) {
    u->drawStr(rx + 44, ry, g_state.napping ? "offline, napping (energy refilling)" : "offline, will nap in <5 min");
  } else {
    u->drawStr(rx + 44, ry, g_state.total > 0 ? "idle (sessions parked)" : "all clear");
  }
  ry += 14;

  // 2x2 KPI grid (compact)
  struct Kpi { const char *label; const char *value; };
  char b_tok[24], b_today[24], b_rate[24], b_up[24];
  fmtThousands(g_state.tokens, b_tok, sizeof(b_tok));
  fmtThousands(g_state.tokens_today, b_today, sizeof(b_today));
  unsigned long tpm = g_state.tokens_1h / 60UL;
  snprintf(b_rate, sizeof(b_rate), "%lu/min", tpm);
  fmtDuration(millis() / 1000, b_up, sizeof(b_up));

  Kpi kpis[4] = {
    {"session", b_tok},
    {"today",   b_today},
    {"rate",    b_rate},
    {"uptime",  b_up},
  };
  int col_w = (W - rx - 6) / 2;
  for (int i = 0; i < 4; i++) {
    int col = i % 2, row = i / 2;
    int x = rx + col * col_w;
    int y = ry + row * 14;
    u->setFont(u8g2_font_6x10_tf);
    u->drawStr(x, y, kpis[i].label);
    u->setFont(u8g2_font_7x13B_tf);
    u->drawStr(x + 40, y, kpis[i].value);
  }
  ry += 14 * 2 + 4;

  // ===== Bottom band: Recent activity =====
  int ey = pill_y + 28;
  if (ey < ry) ey = ry + 2;
  u->drawHLine(6, ey, W - 12);
  ey += 12;
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(6, ey, "Recent activity");
  u->setFont(u8g2_font_6x10_tf);
  if (g_state.msg.length()) {
    String m = "msg: " + g_state.msg;
    if (m.length() > 38) m = m.substring(0, 37) + "~";
    int mw = u->getStrWidth(m.c_str());
    u->drawStr(W - mw - 6, ey, m.c_str());
  }
  ey += 4;

  u->setFont(u8g2_font_7x13_tf);
  int line_y = ey + 14;
  int rows = 0;
  for (int i = 0; i < 3; i++) {
    if (g_state.entries[i].length()) {
      String s = "> " + g_state.entries[i];
      int max_chars = (W - 14) / 7;
      if ((int)s.length() > max_chars) s = s.substring(0, max_chars - 1) + "~";
      u->drawStr(8, line_y, s.c_str());
      rows++;
    }
    line_y += 14;
  }
  if (rows == 0) {
    // Official M5StickC behaviour: when `entries` is empty, fall back to
    // showing the `msg` field as the recent line.
    if (g_state.msg.length()) {
      u->setFont(u8g2_font_7x13_tf);
      String s = "> " + g_state.msg;
      int max_chars = (W - 14) / 7;
      if ((int)s.length() > max_chars) s = s.substring(0, max_chars - 1) + "~";
      u->drawStr(8, ey + 14, s.c_str());
    } else {
      u->setFont(u8g2_font_6x10_tf);
      u->drawStr(8, ey + 14, ble_nus::connected()
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
  u->setFont(u8g2_font_helvB14_tf);
  u->drawStr(8, TOP_H + 17, "Plan usage limits");
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(146, TOP_H + 17, "Max (5x)");
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
  u->setFont(u8g2_font_helvB12_tf);
  u->drawStr(8, y + 10, "Weekly limits");
  u->setFont(u8g2_font_6x10_tf);
  u->drawStr(100, y + 10, "BLE protocol does not expose these");
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

  // Render scaled-down to `avatar` x `avatar`.
  const SpriteInfo &info = SPRITES[id];
  uint8_t frame_idx = g_state.anim_frame % info.frame_count;
  static uint8_t buf[SPRITE_BYTES];
  memcpy_P(buf, info.frames[frame_idx], SPRITE_BYTES);
  // Scale 128 -> avatar via nearest-neighbor downsample with OR.
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

  // Floor line under the mascot.
  u->drawHLine(8, y + avatar + 1, W - 16);
}

// --- System view ---
// Layout: title strip, two-column info rows with memory bars, footer.
void drawSystemView() {
  // Title pill
  u->setDrawColor(1);
  u->drawBox(0, TOP_H, W, 30);
  u->setDrawColor(0);
  u->setFont(u8g2_font_logisoso24_tr);
  u->drawStr(12, TOP_H + 22, "SYSTEM");
  u->setFont(u8g2_font_helvB14_tf);
  u->drawStr(170, TOP_H + 22, "diagnostics");
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

  snprintf(buf, sizeof(buf), "%.2f V   %d%%   chip ESP32-S3", g_state.battery_v, g_state.battery_pct);
  row("Battery", buf);

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
    snprintf(buf, sizeof(buf), "synced %lus ago   tz UTC%+ld:%02ld",
             (unsigned long)elapsed,
             (long)(g_state.time_offset_sec / 3600),
             (long)((labs(g_state.time_offset_sec) / 60) % 60));
  } else strcpy(buf, "not synced (waiting for desktop)");
  row("Time", buf);

  uint32_t up = millis() / 1000;
  snprintf(buf, sizeof(buf), "%luh %02lum %02lus   turns %lu",
           up / 3600, (up / 60) % 60, up % 60, (unsigned long)g_state.turns_done);
  row("Uptime", buf);

  snprintf(buf, sizeof(buf), "energy %u/5   level %lu   mood %s",
           (unsigned)g_state.energy_tier,
           (unsigned long)g_state.level,
           moodAdjective());
  row("Stats", buf);

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

  // ---- Walking mascot strip ----
  // Below memory section, above the firmware tag + bottom bar.
  int mascot_strip_top = my + 4;
  int mascot_strip_h   = H - BOT_H - 14 - mascot_strip_top;
  if (mascot_strip_h > 30) {
    drawWalkingMascot(mascot_strip_top, mascot_strip_h);
  }

  // Build / firmware tag in bottom-left area above bottom bar.
  u->setFont(u8g2_font_6x10_tf);
  const char *build = "Claude-desktop-buddy   built " __DATE__ " " __TIME__;
  u->drawStr(12, H - BOT_H - 4, build);
}

void drawOfflineHint() {
  // Overlay when not connected — show a subtle hint band over main view.
  if (ble_nus::connected()) return;
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

bool ui::render() {
  if (!u) return false;
  uint32_t now = millis();

  // Advance animation frame at ~5 Hz.
  if (now - lastFrameMs > 200) {
    lastFrameMs = now;
    g_state.anim_frame++;
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
    default: drawMainView();   break;
  }

  drawOfflineHint();

  drawBottomBar();

  u->sendBuffer();
  return true;
}
