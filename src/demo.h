#pragma once

// Demo mode: when the device isn't paired with Claude desktop, the user can
// toggle a scene-rotator that writes fake heartbeat values into g_state so
// the UI (sprite + KPIs + transcript + approval flow) animates on its own.
// Mirrors the M5StickC reference firmware's `_FAKES[]` array in data.h.
//
// Trigger: long-press KEY or BOOT on the SYSTEM view.
// Auto-off: any real BLE connection takes priority.
namespace demo {
  void toggle();
  void tick();   // call once per loop iteration
}
