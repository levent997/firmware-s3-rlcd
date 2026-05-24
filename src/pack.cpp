#include "pack.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <AnimatedGIF.h>
#include <esp_heap_caps.h>

namespace {
constexpr int MAX_FRAMES_PER_SPRITE = 8;   // matches tools/gif_to_sprites.py

struct Override {
  uint8_t frame_count = 0;
  uint8_t *frames[MAX_FRAMES_PER_SPRITE] = { nullptr };
};
Override overrides[SPR_COUNT];

String active_name;
unsigned long psram_used = 0;
int override_count_cached = 0;

// Deferred-load queue: BLE handlers stash a name here; the main loop
// drains it via pack::tick(). Only one pending load at a time (latest wins).
String pending_load;

// File-name → sprite slot map. Must mirror tools/gif_to_sprites.py.
struct NameMap { const char *fname; SpriteId id; };
const NameMap NAME_MAP[] = {
  { "idle.gif",         SPR_IDLE },
  { "idle_reading.gif", SPR_IDLE_READING },
  { "bubble.gif",       SPR_BUBBLE },
  { "building.gif",     SPR_BUILDING },
  { "typing.gif",       SPR_TYPING },
  { "thinking.gif",     SPR_THINKING },
  { "sweeping.gif",     SPR_SWEEPING },
  { "juggling.gif",     SPR_JUGGLING },
  { "carrying.gif",     SPR_CARRYING },
  { "headphones.gif",   SPR_HEADPHONES },
  { "happy.gif",        SPR_HAPPY },
  { "notification.gif", SPR_NOTIFICATION },
  { "double_jump.gif",  SPR_DOUBLE_JUMP },
  { "annoyed.gif",      SPR_ANNOYED },
  { "error.gif",        SPR_ERROR },
  { "sleeping.gif",     SPR_SLEEPING },
};
constexpr int NAME_MAP_LEN = sizeof(NAME_MAP) / sizeof(NAME_MAP[0]);

// Decode-time scratch. Reused across frames so we don't churn PSRAM.
AnimatedGIF gif;
uint8_t *current_decode_buf = nullptr;   // pointing into the override frame we're filling
int current_frame_w = 0;                  // GIF canvas size
int current_frame_h = 0;

// AnimatedGIF file callbacks ----------------------------------------------
void *gifOpen(const char *fname, int32_t *psize) {
  File *f = new File();
  *f = LittleFS.open(fname, "r");
  if (!*f) { delete f; return nullptr; }
  *psize = f->size();
  return f;
}
void gifClose(void *handle) {
  File *f = (File *)handle;
  f->close();
  delete f;
}
int32_t gifRead(GIFFILE *gif_file, uint8_t *buf, int32_t len) {
  File *f = (File *)gif_file->fHandle;
  return f->read(buf, len);
}
int32_t gifSeek(GIFFILE *gif_file, int32_t pos) {
  File *f = (File *)gif_file->fHandle;
  f->seek(pos);
  return pos;
}

// Per-line callback: AnimatedGIF gives us one row of palette indices and the
// active palette. We threshold each pixel to ink/no-ink using the same rule
// as tools/gif_to_sprites.py (skip pure white & pure black, anything else =
// ink), centred horizontally and bottom-aligned vertically inside the
// 128x128 sprite canvas.
void gifDraw(GIFDRAW *d) {
  if (!current_decode_buf) return;

  int src_y = d->iY + d->y;
  if (src_y < 0 || src_y >= current_frame_h) return;

  // Centre horizontally, bottom-align vertically (matches sprite generator).
  int x0 = (SPRITE_W - current_frame_w) / 2;
  int y0 = SPRITE_H - current_frame_h - 2;   // 2 px floor padding
  int dst_y = src_y + y0;
  if (dst_y < 0 || dst_y >= SPRITE_H) return;

  uint8_t *line = d->pPixels;
  uint8_t *pal  = (uint8_t *)d->pPalette;    // RGB565 by default — re-interpret
  // AnimatedGIF's default palette is RGB565 little-endian uint16_t per entry.
  // Force a re-cast to 16-bit pointer.
  uint16_t *pal16 = (uint16_t *)pal;

  for (int sx = 0; sx < d->iWidth; sx++) {
    int dst_x = sx + d->iX + x0;
    if (dst_x < 0 || dst_x >= SPRITE_W) continue;
    uint8_t idx = line[sx];
    if (d->ucHasTransparency && idx == d->ucTransparent) continue;

    uint16_t c = pal16[idx];
    // RGB565 -> 8-bit channels (approximate).
    uint8_t r = ((c >> 11) & 0x1F) << 3;
    uint8_t g = ((c >> 5)  & 0x3F) << 2;
    uint8_t b = ( c        & 0x1F) << 3;

    int max_c = max(r, max(g, b));
    int min_c = min(r, min(g, b));
    bool ink = false;
    if (max_c < 32) ink = false;                       // ~pure black -> hole
    else if (min_c > 220) ink = false;                 // ~pure white -> hole
    else if ((max_c - min_c) >= 16) ink = true;        // chromatic -> ink
    else if (((r + g + b) / 3) >= 40 &&
             ((r + g + b) / 3) <= 220) ink = true;     // mid-grey -> ink
    if (!ink) continue;

    int bit = dst_y * SPRITE_W + dst_x;
    current_decode_buf[bit >> 3] |= (1 << (bit & 7));
  }
}

uint8_t *allocFrame() {
  // Prefer PSRAM (we have 8 MB octal). Fall back to internal RAM if PSRAM
  // somehow exhausted.
  uint8_t *p = (uint8_t *)heap_caps_calloc(SPRITE_BYTES, 1,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = (uint8_t *)calloc(SPRITE_BYTES, 1);
  return p;
}

void freeOverride(Override &ov) {
  for (int i = 0; i < ov.frame_count; i++) {
    if (ov.frames[i]) {
      if (heap_caps_check_integrity_addr((intptr_t)ov.frames[i], false)) {
        free(ov.frames[i]);
      }
      ov.frames[i] = nullptr;
    }
  }
  ov.frame_count = 0;
}

bool decodeGifInto(const String &fullpath, SpriteId target) {
  // Free old override (if any) so a re-push fully replaces.
  freeOverride(overrides[target]);

  if (!gif.open(fullpath.c_str(),
                gifOpen, gifClose, gifRead, gifSeek, gifDraw)) {
    Serial.printf("[pack] gif.open failed: %s (errno %d)\n",
                  fullpath.c_str(), gif.getLastError());
    return false;
  }
  current_frame_w = gif.getCanvasWidth();
  current_frame_h = gif.getCanvasHeight();
  if (current_frame_w <= 0 || current_frame_h <= 0 ||
      current_frame_w > 256 || current_frame_h > 256) {
    Serial.printf("[pack] %s canvas %dx%d out of range\n",
                  fullpath.c_str(), current_frame_w, current_frame_h);
    gif.close();
    return false;
  }

  int frame_idx = 0;
  while (frame_idx < MAX_FRAMES_PER_SPRITE) {
    uint8_t *buf = allocFrame();
    if (!buf) {
      Serial.println("[pack] PSRAM alloc failed");
      break;
    }
    current_decode_buf = buf;
    int delay_ms = 0;
    int r = gif.playFrame(false, &delay_ms);
    current_decode_buf = nullptr;
    if (r < 0) {
      // playFrame returned an error (premature end). Discard this empty buf.
      free(buf);
      break;
    }
    overrides[target].frames[frame_idx] = buf;
    frame_idx++;
    psram_used += SPRITE_BYTES;
    if (r == 0) break;   // no more frames
  }
  overrides[target].frame_count = frame_idx;
  gif.close();
  Serial.printf("[pack] decoded %s -> slot %d (%d frames, canvas %dx%d)\n",
                fullpath.c_str(), (int)target, frame_idx,
                current_frame_w, current_frame_h);
  return frame_idx > 0;
}

void recountOverrides() {
  int n = 0;
  for (int i = 0; i < SPR_COUNT; i++) {
    if (overrides[i].frame_count > 0) n++;
  }
  override_count_cached = n;
}
} // namespace

void pack::init() {
  // Look for any character-pack directory under /. Load the first one
  // alphabetically. The user can re-push to switch packs at runtime.
  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()) return;
  File entry;
  String first_dir;
  while ((entry = root.openNextFile())) {
    if (entry.isDirectory()) {
      String n = String(entry.name());
      // entry.name() returns just the basename in ESP32 LittleFS.
      // Skip dotfiles/lostfound.
      if (n.length() && n[0] != '.' && n != "lost+found") {
        if (first_dir.length() == 0 || n < first_dir) first_dir = n;
      }
    }
    entry.close();
  }
  root.close();
  if (first_dir.length()) {
    Serial.printf("[pack] auto-loading first pack on disk: %s\n", first_dir.c_str());
    loadPack(first_dir);
  } else {
    Serial.println("[pack] no character packs on LittleFS (built-in sprites in use)");
  }
}

void pack::unloadActive() {
  for (int i = 0; i < SPR_COUNT; i++) freeOverride(overrides[i]);
  psram_used = 0;
  override_count_cached = 0;
  active_name = "";
}

bool pack::loadPack(const String &name) {
  if (name.length() == 0) return false;
  unloadActive();

  String dir = "/" + name;
  if (!LittleFS.exists(dir)) {
    Serial.printf("[pack] no such pack on disk: %s\n", dir.c_str());
    return false;
  }
  int found = 0;
  for (int i = 0; i < NAME_MAP_LEN; i++) {
    String full = dir + "/" + NAME_MAP[i].fname;
    if (!LittleFS.exists(full)) continue;
    if (decodeGifInto(full, NAME_MAP[i].id)) found++;
  }
  recountOverrides();
  active_name = name;
  Serial.printf("[pack] loadPack(%s): %d sprites overridden, PSRAM used %lu B\n",
                name.c_str(), found, psram_used);
  return found > 0;
}

const String &pack::activeName() { return active_name; }
bool pack::hasOverride(SpriteId id) {
  return id >= 0 && id < SPR_COUNT && overrides[id].frame_count > 0;
}
uint8_t pack::overrideFrameCount(SpriteId id) {
  if (!hasOverride(id)) return 0;
  return overrides[id].frame_count;
}
const uint8_t *pack::overrideFrame(SpriteId id, uint8_t frame_idx) {
  if (!hasOverride(id)) return nullptr;
  if (frame_idx >= overrides[id].frame_count) return nullptr;
  return overrides[id].frames[frame_idx];
}
int pack::overrideCount() { return override_count_cached; }
unsigned long pack::psramBytesUsed() { return psram_used; }

void pack::requestLoad(const String &name) {
  pending_load = name;
  Serial.printf("[pack] reload queued: %s\n", name.c_str());
}
bool pack::loadPending() { return pending_load.length() > 0; }
void pack::tick() {
  if (pending_load.length() == 0) return;
  String n = pending_load;
  pending_load = "";
  loadPack(n);   // blocking — UI may stutter for a few seconds
}
