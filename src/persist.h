// NVS-backed persistence, modelled on the M5StickC reference
// firmware's src/stats.h.
//
// Write strategy: NVS sectors have ~100K write cycles. We only write on
// real events (approval, denial, nap end, explicit name change). NEVER
// write on a timer or every loop.
#pragma once
#include <Arduino.h>

namespace persist {
  // Load all persisted fields into g_state. Call once from setup().
  void load();

  // Event-driven save hooks. Each one writes only the dirty keys.
  void onApprovalOrDenial();       // counters changed
  void onNapEnd();                 // energy_at_nap + last_nap_end_offset_s
  void onTokensProgress();         // tokens / level changed (throttled internally)
  void onPetNameChanged();
  void onOwnerNameChanged();

  // Erase everything we own. Used by `cmd:unpair` cousin / factory reset.
  void wipe();
}
