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
    if (m.indexOf("done") >= 0 || m.indexOf("success") >= 0) {
      if (g_state.running == 0) return SPR_HAPPY;
    }
  }
  if (g_state.running > 0) return SPR_BUILDING;   // building animation reads better than typing at 128px
  if (g_state.waiting > 0) return SPR_THINKING;
  return SPR_IDLE;
}

// When connected and genuinely idle (no sessions, no prompt) for a while,
// cycle through ALL animations on the main view so you can preview them.
SpriteId showcaseSprite() {
  static const SpriteId cycle[] = {
    SPR_IDLE, SPR_BUILDING, SPR_TYPING, SPR_THINKING, SPR_HAPPY,
    SPR_NOTIFICATION, SPR_ERROR, SPR_SLEEPING,
  };
  constexpr uint32_t per_sprite_ms = 6000;
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
    case SPR_BUILDING:     return "WORKING";
    case SPR_TYPING:       return "TYPING";
    case SPR_THINKING:     return "THINKING";
    case SPR_HAPPY:        return "DONE";
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

// Derive a single mood adjective from energy + state.
static const char *moodAdjective() {
  int e = (int)g_state.energy;
  bool running = g_state.running > 0;
  bool waiting = g_state.waiting > 0;
  bool offline = !ble_nus::connected();
  if (offline)                  return "asleep";
  if (g_state.prompt.active)    return "alert";
  if (waiting)                  return "pensive";
  if (running && e >= 70)       return "focused";
  if (running && e >= 35)       return "steady";
  if (running)                  return "weary";
  if (e >= 85)                  return "spry";
  if (e >= 55)                  return "content";
  if (e >= 25)                  return "drowsy";
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

// --- Main view: dashboard layout ---
// Sprite + state pill on the left, vital signs (mood/energy) + live metrics on the right,
// recent activity panel along the bottom.
void drawMainView() {
  SpriteId mood;
  bool showcase = inShowcase();
  if (showcase) mood = showcaseSprite();
  else          mood = moodToSprite();

  // ===== Left column: sprite + name plate =====
  int spr_x = 8;
  int spr_y = TOP_H + 4;
  drawSprite(spr_x, spr_y, mood);

  // State pill below sprite
  int pill_y = spr_y + SPRITE_H + 4;
  u->setDrawColor(1);
  u->drawRBox(spr_x, pill_y, SPRITE_W, 22, 4);
  u->setDrawColor(0);
  u->setFont(u8g2_font_helvB14_tf);
  const char *label = showcase ? "DEMO" : moodLabel(mood);
  int lw = u->getStrWidth(label);
  u->drawStr(spr_x + (SPRITE_W - lw) / 2, pill_y + 16, label);
  u->setDrawColor(1);

  // ===== Right column: vital signs =====
  int rx = spr_x + SPRITE_W + 14;
  int ry = TOP_H + 8;

  // Mood (big italic)
  u->setFont(u8g2_font_helvB14_tf);
  u->drawStr(rx, ry + 12, "Mood");
  u->setFont(u8g2_font_logisoso24_tr);
  u->drawStr(rx + 68, ry + 18, moodAdjective());
  ry += 28;

  // Energy bar
  u->setFont(u8g2_font_helvB14_tf);
  u->drawStr(rx, ry + 12, "Energy");
  char buf[64];
  snprintf(buf, sizeof(buf), "%d%%", (int)g_state.energy);
  int pw = u->getStrWidth(buf);
  u->drawStr(W - pw - 8, ry + 12, buf);
  int bar_x = rx + 68, bar_w = (W - 8) - (rx + 68) - (pw + 8);
  if (bar_w > 40) {
    drawProgressBar(bar_x, ry + 4, bar_w, 12, (int)g_state.energy);
  }
  ry += 22;

  // Live metrics block
  u->drawHLine(rx, ry, W - rx - 6);
  ry += 12;

  // "Now" — current activity descriptor + duration
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
  } else if (showcase) {
    snprintf(buf, sizeof(buf), "demo: %s", moodLabel(mood));
    u->drawStr(rx + 44, ry, buf);
  } else if (!ble_nus::connected()) {
    u->drawStr(rx + 44, ry, "offline — waiting to pair");
  } else {
    u->drawStr(rx + 44, ry, g_state.total > 0 ? "idle (sessions parked)" : "all clear");
  }
  ry += 14;

  // Stats grid: 2 cols x 3 rows of small KPIs
  struct Kpi { const char *label; const char *value; };
  char b_tok[24], b_today[24], b_5h[24], b_rate[24], b_turns[24], b_up[24];
  fmtThousands(g_state.tokens, b_tok, sizeof(b_tok));
  fmtThousands(g_state.tokens_today, b_today, sizeof(b_today));
  fmtThousands(g_state.tokens_5h, b_5h, sizeof(b_5h));
  unsigned long tpm = (g_state.tokens_1h * 60UL) / 3600UL; // /60 simplifies; rough mean tok/min over last hr
  snprintf(b_rate, sizeof(b_rate), "%lu/min", tpm);
  snprintf(b_turns, sizeof(b_turns), "%lu", (unsigned long)g_state.turns_done);
  fmtDuration(millis() / 1000, b_up, sizeof(b_up));

  Kpi kpis[6] = {
    {"session", b_tok},
    {"today",   b_today},
    {"5h",      b_5h},
    {"rate",    b_rate},
    {"turns",   b_turns},
    {"uptime",  b_up},
  };
  int col_w = (W - rx - 6) / 2;
  for (int i = 0; i < 6; i++) {
    int col = i % 2;
    int row = i / 2;
    int x = rx + col * col_w;
    int y = ry + row * 16;
    u->setFont(u8g2_font_6x10_tf);
    u->drawStr(x, y, kpis[i].label);
    u->setFont(u8g2_font_7x13B_tf);
    u->drawStr(x + 40, y, kpis[i].value);
  }
  ry += 16 * 3 + 2;

  // ===== Bottom band: Recent activity + status sentence =====
  int ey = pill_y + 26;            // start below the sprite pill
  if (ey < ry) ey = ry + 4;
  u->drawHLine(6, ey, W - 12);
  ey += 12;
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(6, ey, "Recent activity");
  // status sentence right-side same row
  u->setFont(u8g2_font_6x10_tf);
  if (g_state.msg.length()) {
    String m = "msg: " + g_state.msg;
    if (m.length() > 36) m = m.substring(0, 35) + "~";
    int mw = u->getStrWidth(m.c_str());
    u->drawStr(W - mw - 6, ey, m.c_str());
  } else {
    const char *m = showcase ? "demo mode" : "—";
    int mw = u->getStrWidth(m);
    u->drawStr(W - mw - 6, ey, m);
  }
  ey += 4;

  // entries
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
    u->setFont(u8g2_font_6x10_tf);
    u->drawStr(8, line_y, "(no messages yet — recent transcript will appear here)");
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

// --- Usage view: dense, claude.ai-style layout ---
void drawUsageView() {
  // Header strip
  u->setDrawColor(1);
  u->drawBox(0, TOP_H, W, 24);
  u->setDrawColor(0);
  u->setFont(u8g2_font_helvB14_tf);
  const char *t1 = "Plan usage limits";
  u->drawStr(8, TOP_H + 17, t1);
  int t1w = u->getStrWidth(t1);
  u->setFont(u8g2_font_6x13B_tf);
  u->drawStr(8 + t1w + 14, TOP_H + 17, "Max (5x) *");
  // right side: current time
  if (g_state.time_sync_ms) {
    uint32_t elapsed = (millis() - g_state.time_sync_ms) / 1000U;
    uint32_t local = g_state.time_epoch + elapsed + g_state.time_offset_sec;
    int hh = (local / 3600) % 24;
    int mm = (local / 60) % 60;
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", hh, mm);
    int tw = u->getStrWidth(buf);
    u->drawStr(W - tw - 8, TOP_H + 17, buf);
  }
  u->setDrawColor(1);

  // Layout: each row = label/subtitle on left, bar across middle, % + reset on right.
  int y = TOP_H + 32;

  constexpr unsigned long BUDGET_5H_MAX     = 500000UL;  // Max(5x) rough 5h envelope
  constexpr unsigned long BUDGET_WEEK_ALL   = 5000000UL; // weekly all-model envelope
  constexpr unsigned long BUDGET_WEEK_SON   = 2000000UL; // weekly Sonnet envelope

  // Helper: draw one row.
  auto drawRow = [&](const char *title,
                     const char *subtitle,
                     unsigned long used,
                     unsigned long budget,
                     bool show_subtitle = true) {
    // Title
    u->setFont(u8g2_font_helvB12_tf);
    u->drawStr(8, y + 10, title);
    if (show_subtitle && subtitle) {
      u->setFont(u8g2_font_6x10_tf);
      u->drawStr(8, y + 22, subtitle);
    }
    // pct
    int pct = budget ? (int)((uint64_t)used * 100UL / budget) : 0;
    if (pct > 100) pct = 100;
    char buf[24];
    snprintf(buf, sizeof(buf), "%d%% used", pct);
    u->setFont(u8g2_font_6x13B_tf);
    int tw = u->getStrWidth(buf);
    u->drawStr(W - tw - 8, y + 10, buf);
    // bar
    int bar_x = 8;
    int bar_y = y + 26;
    int bar_w = W - 16;
    drawProgressBar(bar_x, bar_y, bar_w, 6, pct);
    y += 38;
  };

  // ---- Current session (5h) ----
  int32_t secs_5h_remain = 5 * 3600 - 1; // approx — without protocol value, show "rolling"
  (void)secs_5h_remain;
  char sub_session[40];
  snprintf(sub_session, sizeof(sub_session), "Resets: rolling 5h window");
  drawRow("Current session", sub_session,
          g_state.tokens_5h, BUDGET_5H_MAX);

  // ---- Today ----
  char sub_today[40] = "Resets at local midnight";
  int32_t secs_mid = secsToMidnight();
  if (secs_mid > 0) {
    int h = secs_mid / 3600, m = (secs_mid / 60) % 60;
    if (h > 0) snprintf(sub_today, sizeof(sub_today), "Resets in %dh %02dm", h, m);
    else       snprintf(sub_today, sizeof(sub_today), "Resets in %dm", m);
  }
  drawRow("Today", sub_today,
          g_state.tokens_today, 1000000UL);

  // ---- Weekly limits section ----
  u->setFont(u8g2_font_helvB12_tf);
  u->drawStr(8, y + 10, "Weekly limits");
  u->setFont(u8g2_font_6x10_tf);
  u->drawStr(106, y + 10, "* local estimates, not Anthropic's quota");
  y += 16;

  // All models
  String wk_reset = "Resets " + fmtWeeklyReset();
  drawRow("All models", wk_reset.c_str(),
          g_state.tokens_today, BUDGET_WEEK_ALL);

  // Sonnet only (we don't know per-model; treat session tokens as Sonnet proxy)
  drawRow("Sonnet only", wk_reset.c_str(),
          g_state.tokens, BUDGET_WEEK_SON);

  // ---- Footer with detail numbers ----
  u->drawHLine(6, y + 2, W - 12);
  y += 12;
  u->setFont(u8g2_font_6x10_tf);
  char tbuf[16], tbuf2[16], tbuf3[16], tbuf4[16];
  fmtThousands(g_state.tokens, tbuf, sizeof(tbuf));
  fmtThousands(g_state.tokens_today, tbuf2, sizeof(tbuf2));
  fmtThousands(g_state.tokens_1h, tbuf3, sizeof(tbuf3));
  fmtThousands(g_state.tokens_5h, tbuf4, sizeof(tbuf4));
  char fbuf[140];
  snprintf(fbuf, sizeof(fbuf),
           "raw: session %s   today %s   1h %s   5h %s",
           tbuf, tbuf2, tbuf3, tbuf4);
  u->drawStr(8, y, fbuf);
  y += 11;
  snprintf(fbuf, sizeof(fbuf),
           "appr %lu  deny %lu  sessions %d (run %d / wait %d)  turns %lu",
           (unsigned long)g_state.approvals,
           (unsigned long)g_state.denies,
           g_state.total, g_state.running, g_state.waiting,
           (unsigned long)g_state.turns_done);
  u->drawStr(8, y, fbuf);
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

  snprintf(buf, sizeof(buf), "energy %d%%   mood: %s",
           (int)g_state.energy, moodAdjective());
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
