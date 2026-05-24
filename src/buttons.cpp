#include "buttons.h"

namespace {
constexpr int PIN_KEY  = 18;
constexpr int PIN_BOOT = 0;
constexpr uint32_t LONG_MS = 800;
constexpr uint32_t DEBOUNCE_MS = 30;

struct Btn {
  int pin = 0;
  bool stable = true;
  uint32_t change_ms = 0;
  uint32_t press_ms = 0;
  bool long_fired = false;
};

Btn g_key;
Btn g_boot;

buttons::Event pollOne(Btn &b, buttons::Event ev_short, buttons::Event ev_long) {
  bool pressed_raw = (digitalRead(b.pin) == LOW);
  uint32_t now = millis();

  // map: stable==true => not pressed; stable==false => pressed
  bool target = !pressed_raw;
  if (target != b.stable) {
    if (b.change_ms == 0) b.change_ms = now;
    if (now - b.change_ms >= DEBOUNCE_MS) {
      b.stable = target;
      b.change_ms = 0;
      if (!b.stable) { // just became pressed
        b.press_ms = now;
        b.long_fired = false;
      } else {         // just released
        if (!b.long_fired && b.press_ms && (now - b.press_ms) < LONG_MS) {
          b.press_ms = 0;
          return ev_short;
        }
        b.press_ms = 0;
      }
    }
  } else {
    b.change_ms = 0;
  }

  if (!b.stable && !b.long_fired && b.press_ms && (now - b.press_ms) >= LONG_MS) {
    b.long_fired = true;
    return ev_long;
  }
  return buttons::NONE;
}
} // namespace

void buttons::begin() {
  g_key.pin  = PIN_KEY;
  g_boot.pin = PIN_BOOT;
  pinMode(PIN_KEY,  INPUT_PULLUP);
  pinMode(PIN_BOOT, INPUT_PULLUP);
}

buttons::Event buttons::poll() {
  Event e = pollOne(g_key, KEY_SHORT, KEY_LONG);
  if (e != NONE) return e;
  return pollOne(g_boot, BOOT_SHORT, BOOT_LONG);
}
