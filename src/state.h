#pragma once
#include <Arduino.h>
#include <math.h>

struct PromptInfo {
  bool active = false;
  String id;
  String tool;
  String hint;
};

struct BuddyState {
  bool connected = false;
  uint32_t last_heartbeat_ms = 0;

  int total = 0;
  int running = 0;
  int waiting = 0;
  String msg;
  // Heartbeat carries up to 8 transcript entries per snapshot. We store all of
  // them; the MAIN view shows the latest 3 for layout reasons, and the
  // long-press history overlay (`history_open`) reveals all 8.
  String entries[8];
  uint32_t tokens = 0;
  uint32_t tokens_today = 0;
  uint32_t tokens_boot = 0;        // local monotonic accumulator since device boot
  uint32_t tokens_1h = 0;          // estimated tokens in trailing 1h (local samples)
  uint32_t tokens_5h = 0;          // estimated tokens in trailing 5h (local samples)

  PromptInfo prompt;

  String name = "Clawd";
  String owner;

  // BLE pairing state.
  bool secure = false;             // link is LE Secure Connections-encrypted
  bool passkey_displaying = false; // showing passkey screen overlay
  uint32_t passkey = 0;            // 6-digit passkey to display

  uint32_t approvals = 0;
  uint32_t denies = 0;

  // Local sensors / clock
  int battery_pct = -1;            // -1 = unknown
  float battery_v = 0.0f;
  bool charging = false;           // heuristic: voltage trending up or near-full
  float temp_c = NAN;
  float humidity_pct = NAN;
  uint32_t time_epoch = 0;         // last synced epoch seconds
  int32_t time_offset_sec = 0;     // tz offset (seconds east of UTC)
  uint32_t time_sync_ms = 0;       // millis() when synced

  // UI
  uint8_t view = 0;                // 0=main, 1=usage, 2=system
  bool history_open = false;       // long-press on MAIN opens the 8-row transcript overlay
  bool demo_mode = false;          // long-press on SYSTEM rotates fake heartbeats for showcase
  uint32_t anim_frame = 0;

  // Derived "tamagotchi" stats — modelled on the M5StickC reference
  // firmware (src/stats.h in the parent project).
  //
  //   Energy: 0..5 tiers. Boots at 3. Drains 1 tier per 2 h of being
  //           awake. Refilled to 5 when the buddy "naps" (we use
  //           BLE disconnected > 5 min as the nap trigger, since this
  //           board has no IMU for face-down detection).
  //   Fed:    0..9 pips. = (tokens % 50000) / 5000.
  //   Level:  tokens / 50000. Triggers a celebrate on level-up.
  uint8_t energy_tier = 3;         // 0..5
  uint32_t last_nap_end_ms = 0;    // millis() when nap last ended
  uint8_t energy_at_nap = 3;       // tier we restored to at last nap end
  uint32_t nap_started_ms = 0;     // BLE-disconnect anchor for nap detection
  bool napping = false;

  uint32_t fed_baseline_tokens = 0;  // initial seen tokens, subtracted
  bool fed_synced = false;
  uint32_t level = 0;

  uint32_t run_started_ms = 0;     // millis() when running first went > 0 (this turn)
  uint32_t last_turn_ms = 0;       // last time we saw a completion
  uint32_t last_activity_ms = 0;   // last time we saw running/waiting > 0 or new msg
  uint32_t turns_done = 0;         // local counter of completed turns this boot

  // Velocity ring buffer: response time (seconds) for the last 8 approvals.
  // Mirrors the M5StickC reference firmware's stats.velocity[].
  uint16_t velocity[8] = {0};
  uint8_t  velocity_idx = 0;
  uint8_t  velocity_count = 0;

  // When the current prompt first appeared (millis), to compute response time.
  uint32_t prompt_started_ms = 0;
};

constexpr uint32_t TOKENS_PER_LEVEL = 50000;
constexpr uint32_t TOKENS_PER_FED_PIP = 5000;


extern BuddyState g_state;
