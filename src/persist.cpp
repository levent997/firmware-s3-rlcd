#include "persist.h"
#include "state.h"
#include "settings.h"
#include "log.h"

namespace {
// Throttle tokens-progress writes: we get heartbeat updates every few
// seconds and the tokens counter changes constantly. Writing every time
// would burn NVS sectors. Save at most once every TOK_SAVE_INTERVAL_MS
// AND only when the level (50K boundary) crosses.
constexpr uint32_t TOK_SAVE_INTERVAL_MS = 5UL * 60 * 1000;  // 5 minutes
uint32_t last_tok_save_ms = 0;
uint32_t last_saved_level = 0;
uint32_t last_saved_tokens = 0;
}  // namespace

void persist::load() {
  settings::Settings s("buddy", /*readOnly=*/true);
  if (!s.ok()) {
    LOGE("[persist] NVS open failed; using defaults\n");
    return;
  }

  g_state.tokens_boot = s.getUInt("tok_boot", 0);
  g_state.level       = s.getUInt("level", 0);
  g_state.approvals   = s.getUInt("appr", 0);
  g_state.denies      = s.getUInt("deny", 0);
  g_state.turns_done  = s.getUInt("turns", 0);

  // Energy: store the tier at last nap end. last_nap_end_ms stays 0 at boot
  // and the tier ticker in main.cpp recomputes from the elapsed delta (we
  // can't persist millis() across reboots).
  g_state.energy_at_nap = s.getUChar("e_at_nap", 3);
  if (g_state.energy_at_nap > 5) g_state.energy_at_nap = 3;

  String pet = s.getString("petname", "");
  if (pet.length()) g_state.name = pet;
  String own = s.getString("owner", "");
  if (own.length()) g_state.owner = own;
  g_state.sound_on = s.getBool("sound", true);

  last_saved_level  = g_state.level;
  last_saved_tokens = g_state.tokens_boot;

  LOGI("[persist] loaded: tok_boot=%lu level=%lu appr=%lu deny=%lu turns=%lu "
       "e_at_nap=%u name=%s owner=%s\n",
       (unsigned long)g_state.tokens_boot, (unsigned long)g_state.level,
       (unsigned long)g_state.approvals, (unsigned long)g_state.denies,
       (unsigned long)g_state.turns_done, (unsigned)g_state.energy_at_nap,
       g_state.name.c_str(), g_state.owner.c_str());
}

void persist::onApprovalOrDenial() {
  settings::Settings s;
  if (!s.ok()) return;
  s.putUInt("appr",  g_state.approvals);
  s.putUInt("deny",  g_state.denies);
  s.putUInt("turns", g_state.turns_done);
}

void persist::onNapEnd() {
  settings::Settings s;
  if (!s.ok()) return;
  s.putUChar("e_at_nap", g_state.energy_at_nap);
}

void persist::onTokensProgress() {
  uint32_t now = millis();
  bool level_crossed = (g_state.level != last_saved_level);
  bool interval_due  = (now - last_tok_save_ms) > TOK_SAVE_INTERVAL_MS;
  // Also save if a significant token delta accumulated (>= 5K = one fed pip).
  bool big_delta = (g_state.tokens_boot >= last_saved_tokens + 5000UL);

  if (!level_crossed && !interval_due && !big_delta) return;

  settings::Settings s;
  if (!s.ok()) return;
  s.putUInt("tok_boot", g_state.tokens_boot);
  s.putUInt("level",    g_state.level);
  s.putUInt("turns",    g_state.turns_done);
  last_tok_save_ms  = now;
  last_saved_level  = g_state.level;
  last_saved_tokens = g_state.tokens_boot;
}

void persist::onPetNameChanged() {
  settings::Settings s;
  if (!s.ok()) return;
  s.putString("petname", g_state.name);
}

void persist::onOwnerNameChanged() {
  settings::Settings s;
  if (!s.ok()) return;
  s.putString("owner", g_state.owner);
}

void persist::onSoundChanged() {
  settings::Settings s;
  if (!s.ok()) return;
  s.putBool("sound", g_state.sound_on);
}

void persist::resetStats() {
  // Zero out the usage counters in RAM first so the UI updates immediately.
  g_state.tokens_boot    = 0;
  g_state.level          = 0;
  g_state.approvals      = 0;
  g_state.denies         = 0;
  g_state.turns_done     = 0;
  g_state.velocity_count = 0;
  g_state.velocity_idx   = 0;
  for (int i = 0; i < 8; i++) g_state.velocity[i] = 0;
  last_saved_level  = 0;
  last_saved_tokens = 0;

  settings::Settings s;
  if (!s.ok()) return;
  s.putUInt("tok_boot", 0);
  s.putUInt("level",    0);
  s.putUInt("appr",     0);
  s.putUInt("deny",     0);
  s.putUInt("turns",    0);
  LOGI("[persist] reset usage counters\n");
}

void persist::wipe() {
  settings::Settings s;
  if (!s.ok()) return;
  s.clear();
  LOGI("[persist] wiped NVS namespace\n");
}
