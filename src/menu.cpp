#include "menu.h"
#include "state.h"
#include "persist.h"
#include "ble_nus.h"
#include <Arduino.h>
#include <LittleFS.h>

namespace {
enum Item {
  ITEM_SOUND = 0,
  ITEM_RESET_STATS,
  ITEM_REMOVE_PACKS,
  ITEM_FACTORY_RESET,
  ITEM_REBOOT,
  ITEM_CANCEL,
  ITEM_COUNT
};

const char *LABELS[ITEM_COUNT] = {
  "Sound",
  "Reset stats",
  "Remove packs",
  "Factory reset",
  "Reboot",
  "Cancel / close",
};

// True if activating the item requires the second-press confirm screen.
bool DESTRUCTIVE[ITEM_COUNT] = {
  false,  // Sound — toggle, no confirm
  true,   // Reset stats
  true,   // Remove packs
  true,   // Factory reset
  true,   // Reboot — yes, ask first
  false,  // Cancel — closes immediately
};

constexpr uint32_t INACTIVITY_TIMEOUT_MS = 30 * 1000;

void touch() { g_state.menu_last_input_ms = millis(); }

// Recursive wipe of /<pack> directories — same helper pattern as xfer.cpp.
void wipeLittleFSContents() {
  File root = LittleFS.open("/");
  if (!root || !root.isDirectory()) return;
  // Collect names first; mutating mid-iteration is unsafe.
  String victims[16];
  int n = 0;
  File e;
  while ((e = root.openNextFile()) && n < 16) {
    victims[n++] = String(e.path());
    e.close();
  }
  root.close();
  for (int i = 0; i < n; i++) {
    // Best effort: rmdir works only if empty, but for /<pack>/<file> we
    // need to descend. Use Linux-style rm -rf via LittleFS.remove which
    // doesn't recurse; fall back to format() if anything blocks. The
    // format wipes everything including any stray files; acceptable for
    // a "Remove packs" / factory reset operation.
    Serial.printf("[menu] removing %s\n", victims[i].c_str());
  }
  LittleFS.format();
  Serial.println("[menu] LittleFS formatted");
}

void activate(int item) {
  switch (item) {
    case ITEM_SOUND:
      g_state.sound_on = !g_state.sound_on;
      persist::onSoundChanged();
      Serial.printf("[menu] sound = %s\n", g_state.sound_on ? "ON" : "OFF");
      break;
    case ITEM_RESET_STATS:
      persist::resetStats();
      menu::close();
      break;
    case ITEM_REMOVE_PACKS:
      wipeLittleFSContents();
      // Active pack overrides linger in PSRAM until reboot; warn user.
      menu::close();
      break;
    case ITEM_FACTORY_RESET:
      Serial.println("[menu] factory reset");
      ble_nus::clearBonds();
      persist::wipe();
      wipeLittleFSContents();
      delay(200);
      ESP.restart();
      break;
    case ITEM_REBOOT:
      Serial.println("[menu] reboot");
      delay(100);
      ESP.restart();
      break;
    case ITEM_CANCEL:
      menu::close();
      break;
  }
}
} // namespace

void menu::open() {
  g_state.menu_open = true;
  g_state.menu_selected = 0;
  g_state.menu_confirming = false;
  touch();
  Serial.println("[menu] opened");
}

bool menu::isOpen() { return g_state.menu_open; }

void menu::close() {
  g_state.menu_open = false;
  g_state.menu_confirming = false;
  Serial.println("[menu] closed");
}

void menu::onKeyShort() {
  if (g_state.menu_confirming) return;   // confirm screen ignores nav
  g_state.menu_selected = (g_state.menu_selected + 1) % ITEM_COUNT;
  touch();
}

void menu::onBootShort() {
  if (g_state.menu_confirming) return;
  g_state.menu_selected =
      (g_state.menu_selected + ITEM_COUNT - 1) % ITEM_COUNT;
  touch();
}

void menu::onKeyLong() {
  touch();
  int item = g_state.menu_selected;
  if (g_state.menu_confirming) {
    // Confirm phase: actually execute.
    g_state.menu_confirming = false;
    activate(item);
    return;
  }
  if (DESTRUCTIVE[item]) {
    g_state.menu_confirming = true;
    return;
  }
  activate(item);
}

void menu::onBootLong() {
  touch();
  if (g_state.menu_confirming) {
    g_state.menu_confirming = false;
    return;
  }
  close();
}

void menu::tick() {
  if (!g_state.menu_open) return;
  if (g_state.menu_last_input_ms &&
      millis() - g_state.menu_last_input_ms > INACTIVITY_TIMEOUT_MS) {
    Serial.println("[menu] auto-closed (inactivity)");
    close();
  }
}

int menu::itemCount() { return ITEM_COUNT; }
const char *menu::itemLabel(int idx) {
  if (idx < 0 || idx >= ITEM_COUNT) return "";
  return LABELS[idx];
}
const char *menu::itemValue(int idx) {
  if (idx == ITEM_SOUND) return g_state.sound_on ? "ON" : "OFF";
  return "";
}
bool menu::itemIsDestructive(int idx) {
  if (idx < 0 || idx >= ITEM_COUNT) return false;
  return DESTRUCTIVE[idx];
}

const char *menu::confirmTitle() {
  switch (g_state.menu_selected) {
    case ITEM_RESET_STATS:   return "Reset usage stats?";
    case ITEM_REMOVE_PACKS:  return "Remove all character packs?";
    case ITEM_FACTORY_RESET: return "Factory reset?";
    case ITEM_REBOOT:        return "Reboot device?";
    default:                 return "Confirm?";
  }
}
const char *menu::confirmBody() {
  switch (g_state.menu_selected) {
    case ITEM_RESET_STATS:
      return "Zeros tokens, level, approvals, denies, turns,\n"
             "velocity ring. Keeps name, owner, sound, bonds,\n"
             "and character packs.";
    case ITEM_REMOVE_PACKS:
      return "Erases every /<pack>/ directory on LittleFS.\n"
             "Active sprites stay in PSRAM until reboot.";
    case ITEM_FACTORY_RESET:
      return "Wipes EVERYTHING: NVS settings, BLE bonds,\n"
             "LittleFS packs. Device will reboot.";
    case ITEM_REBOOT:
      return "Soft restart. NVS data preserved.";
    default:
      return "";
  }
}
