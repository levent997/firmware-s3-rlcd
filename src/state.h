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
  String entries[3];
  uint32_t tokens = 0;
  uint32_t tokens_today = 0;
  uint32_t tokens_boot = 0;        // local monotonic accumulator since device boot
  uint32_t tokens_1h = 0;          // estimated tokens in trailing 1h (local samples)
  uint32_t tokens_5h = 0;          // estimated tokens in trailing 5h (local samples)

  PromptInfo prompt;

  String name = "Clawd";
  String owner;

  uint32_t approvals = 0;
  uint32_t denies = 0;

  // Local sensors / clock
  int battery_pct = -1;            // -1 = unknown
  float battery_v = 0.0f;
  float temp_c = NAN;
  float humidity_pct = NAN;
  uint32_t time_epoch = 0;         // last synced epoch seconds
  int32_t time_offset_sec = 0;     // tz offset (seconds east of UTC)
  uint32_t time_sync_ms = 0;       // millis() when synced

  // UI
  uint8_t view = 0;                // 0=main, 1=usage
  uint32_t anim_frame = 0;

  // Derived "tamagotchi" stats — gives the buddy personality.
  float energy = 80.0f;            // 0..100, ticks down while working, up while idle
  uint32_t run_started_ms = 0;     // millis() when running first went > 0 (this turn)
  uint32_t last_turn_ms = 0;       // last time we saw a completion
  uint32_t last_activity_ms = 0;   // last time we saw running/waiting > 0 or new msg
  uint32_t turns_done = 0;         // local counter of completed turns this boot
};

extern BuddyState g_state;
