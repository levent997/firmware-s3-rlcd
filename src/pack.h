#pragma once
#include <stdint.h>
#include <Arduino.h>
#include "sprites.h"   // SpriteId, SPR_COUNT, SPRITE_BYTES, SPRITE_W/H

// Loads pushed character packs from LittleFS and overrides the built-in
// PROGMEM sprite frames at runtime. Files are expected to live at
// /<pack_name>/<sprite>.gif where <sprite> is the lower-case sprite name
// (e.g. /clawd/idle.gif overrides SPR_IDLE).
//
// Frames are decoded once into PSRAM-allocated 1bpp buffers (one buffer
// per (sprite, frame) pair). Sprites without an override file keep using
// the built-in PROGMEM data. So a "minimal" pack with just idle.gif
// replaces only the idle animation; everything else continues to render
// from the firmware.
namespace pack {
  void init();                                  // Auto-load first pack on disk
  bool loadPack(const String &name);            // Load (or reload) by name (BLOCKING — seconds)
  void unloadActive();                          // Free PSRAM, revert to built-in
  const String &activeName();                   // "" if none loaded

  // Deferred reload — call from a BLE callback context (e.g. char_end ack)
  // to queue a load. The actual GIF decode runs from the next pack::tick()
  // call so the ack can go out immediately.
  void requestLoad(const String &name);
  void tick();                                  // call from main loop
  bool loadPending();                            // for UI hint

  // True if a runtime override exists for this sprite slot.
  bool hasOverride(SpriteId id);
  uint8_t overrideFrameCount(SpriteId id);
  const uint8_t *overrideFrame(SpriteId id, uint8_t frame_idx);

  // For SYSTEM-view diagnostics.
  int overrideCount();              // how many of the 16 sprites overridden
  unsigned long psramBytesUsed();   // total PSRAM held by overrides
}
