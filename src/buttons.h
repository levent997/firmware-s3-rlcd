#pragma once
#include <Arduino.h>

namespace buttons {
  enum Event { NONE, KEY_SHORT, KEY_LONG, BOOT_SHORT, BOOT_LONG };

  void begin();
  Event poll();
}
