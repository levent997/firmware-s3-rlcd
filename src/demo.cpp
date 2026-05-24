#include "demo.h"
#include "state.h"
#include "ble_nus.h"
#include <Arduino.h>

namespace {
// One row per scene. Each scene drives ~7 s of UI before rotating; over a
// full lap (currently 7 scenes) you see idle, single session, approval
// prompt, multi-session, compaction, error, and success. That covers all
// the moodToSprite() branches in ui.cpp.
struct Scene {
  const char *msg;
  int total, running, waiting;
  uint32_t tokens_delta;   // adds onto a running counter (so token KPIs animate)
  const char *entries[8];
  bool prompt_active;
  const char *prompt_tool;
  const char *prompt_hint;
};

const Scene SCENES[] = {
  // 0: all clear / idle
  {"all clear", 0, 0, 0, 0,
   {"", "", "", "", "", "", "", ""},
   false, "", ""},
  // 1: single session working
  {"editing src/ui.cpp", 1, 1, 0, 850,
   {"[10:32] Edit src/ui.cpp",
    "[10:31] Read CLAUDE.md",
    "[10:30] Bash 'pio run'",
    "", "", "", "", ""},
   false, "", ""},
  // 2: approval requested
  {"requires approval", 1, 0, 1, 0,
   {"[10:33] Bash 'rm -rf node_modules'",
    "[10:32] Edit src/ui.cpp",
    "[10:31] Read CLAUDE.md",
    "", "", "", "", ""},
   true, "Bash", "rm -rf node_modules && npm install"},
  // 3: multiple sessions juggling
  {"3 sessions running", 3, 2, 1, 1200,
   {"[10:34] agent A: lint warnings",
    "[10:33] agent B: 12 tests passing",
    "[10:32] agent C: deploy to staging",
    "[10:31] Read CHANGELOG.md",
    "[10:30] Edit src/db.ts",
    "", "", ""},
   false, "", ""},
  // 4: compaction (drives SWEEPING sprite via msg keyword)
  {"compacting transcript", 1, 1, 0, 400,
   {"[10:35] Compacting transcript...",
    "[10:34] Edit src/ui.cpp",
    "[10:33] Bash 'cargo build'",
    "[10:32] Read docs/api.md",
    "", "", "", ""},
   false, "", ""},
  // 5: error (drives ERROR sprite)
  {"error: build failed", 1, 0, 0, 0,
   {"[10:36] ERROR: linker exit 1",
    "[10:35] Bash 'make' failed",
    "[10:34] Edit src/error.c",
    "", "", "", "", ""},
   false, "", ""},
  // 6: success / done (drives HAPPY sprite via msg keyword + running==0)
  {"deploy successful", 0, 0, 0, 2500,
   {"[10:37] Success: deploy OK",
    "[10:36] Bash 'systemctl restart nginx'",
    "[10:35] Edit nginx.conf",
    "[10:34] Read /var/log/syslog",
    "", "", "", ""},
   false, "", ""},
};
constexpr int N_SCENES = sizeof(SCENES) / sizeof(SCENES[0]);

uint32_t last_switch_ms = 0;
int scene_idx = 0;
uint32_t fake_tokens = 12345;

// BLE connection state captured at the moment demo was toggled on. We only
// auto-disable demo when the connection state transitions FROM disconnected
// TO connected after toggle — i.e. a fresh desktop connection. If the user
// turned demo on while already connected, that was a deliberate choice and
// we leave it alone until they toggle off or BLE actually drops.
bool was_connected_at_toggle = false;
} // namespace

void demo::toggle() {
  g_state.demo_mode = !g_state.demo_mode;
  if (g_state.demo_mode) {
    last_switch_ms = 0;   // force immediate first scene on next tick
    scene_idx = 0;
    fake_tokens = 12345;
    was_connected_at_toggle = ble_nus::connected();
    Serial.printf("[demo] ON (ble was %s)\n",
                  was_connected_at_toggle ? "connected" : "disconnected");
  } else {
    // Reset transient state so the UI returns to a clean offline frame.
    g_state.total = g_state.running = g_state.waiting = 0;
    g_state.msg = "";
    g_state.prompt.active = false;
    g_state.prompt.id = "";
    g_state.prompt.tool = "";
    g_state.prompt.hint = "";
    for (int i = 0; i < 8; i++) g_state.entries[i] = "";
    Serial.println("[demo] OFF");
  }
}

void demo::tick() {
  if (!g_state.demo_mode) return;

  // Auto-off only on the rising edge of a *new* BLE connection. Users
  // toggling demo on a live link want to keep seeing it; users toggling on
  // while offline expect demo to step aside the moment Claude reconnects.
  bool now_conn = ble_nus::connected();
  if (now_conn && !was_connected_at_toggle) {
    Serial.println("[demo] auto-off (BLE just connected)");
    g_state.demo_mode = false;
    return;
  }
  // If BLE drops while demo is on, reset the baseline so the NEXT reconnect
  // will trigger auto-off correctly.
  if (!now_conn && was_connected_at_toggle) {
    was_connected_at_toggle = false;
  }

  uint32_t now = millis();
  if (last_switch_ms && now - last_switch_ms < 7000) return;
  last_switch_ms = now;

  const Scene &s = SCENES[scene_idx];
  g_state.msg = s.msg;
  g_state.total = s.total;
  g_state.running = s.running;
  g_state.waiting = s.waiting;
  fake_tokens += s.tokens_delta;
  g_state.tokens = fake_tokens;
  g_state.tokens_today = fake_tokens + 200000;
  for (int i = 0; i < 8; i++) g_state.entries[i] = s.entries[i];

  if (s.prompt_active) {
    g_state.prompt.active = true;
    g_state.prompt.id = String("demo-") + scene_idx;
    g_state.prompt.tool = s.prompt_tool;
    g_state.prompt.hint = s.prompt_hint;
    g_state.prompt_started_ms = now;
  } else {
    g_state.prompt.active = false;
    g_state.prompt.id = "";
    g_state.prompt.tool = "";
    g_state.prompt.hint = "";
  }

  // Suppress the 30s-stale fallback in main.cpp's loop.
  g_state.last_heartbeat_ms = now;

  Serial.printf("[demo] scene %d/%d: %s\n",
                scene_idx + 1, N_SCENES, s.msg);
  scene_idx = (scene_idx + 1) % N_SCENES;
}
