#include "persist.h"
#include "state.h"
#include <Preferences.h>

namespace {
constexpr const char *NVS_NS = "buddy";

// Throttle tokens-progress writes: we get heartbeat updates every few
// seconds and the tokens counter changes constantly. Writing every time
// would burn NVS sectors. Save at most once every TOK_SAVE_INTERVAL_MS
// AND only when the level (50K boundary) crosses.
constexpr uint32_t TOK_SAVE_INTERVAL_MS = 5UL * 60 * 1000;  // 5 minutes
uint32_t last_tok_save_ms = 0;
uint32_t last_saved_level = 0;
uint32_t last_saved_tokens = 0;

Preferences prefs;

// Wrap begin()/end() so we never forget to close.
struct Scope {
  bool ok;
  Scope(bool readOnly) { ok = prefs.begin(NVS_NS, readOnly); }
  ~Scope() { if (ok) prefs.end(); }
};
}  // namespace

void persist::load() {
  Scope s(true);
  if (!s.ok) {
    Serial.println("[persist] NVS open failed; using defaults");
    return;
  }

  g_state.tokens_boot     = prefs.getUInt("tok_boot", 0);
  g_state.level           = prefs.getUInt("level", 0);
  g_state.approvals       = prefs.getUInt("appr", 0);
  g_state.denies          = prefs.getUInt("deny", 0);
  g_state.turns_done      = prefs.getUInt("turns", 0);

  // Energy: store the tier at last nap end and an offset (seconds since
  // boot when nap ended). We can't persist millis() values directly
  // across reboots, so we treat last_nap_end_ms as 0 at boot and rebuild
  // the tier from the elapsed time delta in main.cpp's ticker.
  g_state.energy_at_nap   = prefs.getUChar("e_at_nap", 3);
  if (g_state.energy_at_nap > 5) g_state.energy_at_nap = 3;
  // last_nap_end_ms stays 0 at boot — the tier ticker will recompute.

  String pet = prefs.getString("petname", "");
  if (pet.length()) g_state.name = pet;
  String own = prefs.getString("owner", "");
  if (own.length()) g_state.owner = own;
  g_state.sound_on = prefs.getBool("sound", true);

  last_saved_level  = g_state.level;
  last_saved_tokens = g_state.tokens_boot;

  Serial.printf("[persist] loaded: tok_boot=%lu level=%lu appr=%lu deny=%lu turns=%lu "
                "e_at_nap=%u name=%s owner=%s\n",
                (unsigned long)g_state.tokens_boot,
                (unsigned long)g_state.level,
                (unsigned long)g_state.approvals,
                (unsigned long)g_state.denies,
                (unsigned long)g_state.turns_done,
                (unsigned)g_state.energy_at_nap,
                g_state.name.c_str(),
                g_state.owner.c_str());
}

void persist::onApprovalOrDenial() {
  Scope s(false);
  if (!s.ok) return;
  prefs.putUInt("appr",  g_state.approvals);
  prefs.putUInt("deny",  g_state.denies);
  prefs.putUInt("turns", g_state.turns_done);
}

void persist::onNapEnd() {
  Scope s(false);
  if (!s.ok) return;
  prefs.putUChar("e_at_nap", g_state.energy_at_nap);
}

void persist::onTokensProgress() {
  uint32_t now = millis();
  bool level_crossed = (g_state.level != last_saved_level);
  bool interval_due  = (now - last_tok_save_ms) > TOK_SAVE_INTERVAL_MS;
  // Also save if a significant token delta accumulated (>= 5K = one fed pip).
  bool big_delta = (g_state.tokens_boot >= last_saved_tokens + 5000UL);

  if (!level_crossed && !interval_due && !big_delta) return;

  Scope s(false);
  if (!s.ok) return;
  prefs.putUInt("tok_boot", g_state.tokens_boot);
  prefs.putUInt("level",    g_state.level);
  prefs.putUInt("turns",    g_state.turns_done);
  last_tok_save_ms  = now;
  last_saved_level  = g_state.level;
  last_saved_tokens = g_state.tokens_boot;
}

void persist::onPetNameChanged() {
  Scope s(false);
  if (!s.ok) return;
  prefs.putString("petname", g_state.name);
}

void persist::onOwnerNameChanged() {
  Scope s(false);
  if (!s.ok) return;
  prefs.putString("owner", g_state.owner);
}

void persist::onSoundChanged() {
  Scope s(false);
  if (!s.ok) return;
  prefs.putBool("sound", g_state.sound_on);
}

void persist::resetStats() {
  // Zero out the usage counters in RAM first so the UI updates immediately.
  g_state.tokens_boot  = 0;
  g_state.level        = 0;
  g_state.approvals    = 0;
  g_state.denies       = 0;
  g_state.turns_done   = 0;
  g_state.velocity_count = 0;
  g_state.velocity_idx   = 0;
  for (int i = 0; i < 8; i++) g_state.velocity[i] = 0;
  last_saved_level  = 0;
  last_saved_tokens = 0;
  Scope s(false);
  if (!s.ok) return;
  prefs.putUInt("tok_boot", 0);
  prefs.putUInt("level",    0);
  prefs.putUInt("appr",     0);
  prefs.putUInt("deny",     0);
  prefs.putUInt("turns",    0);
  Serial.println("[persist] reset usage counters");
}

void persist::wipe() {
  Scope s(false);
  if (!s.ok) return;
  prefs.clear();
  Serial.println("[persist] wiped NVS namespace");
}
