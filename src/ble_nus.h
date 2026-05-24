#pragma once
#include <Arduino.h>

namespace ble_nus {
  using RxLineHandler = void (*)(const String &line);

  void begin(const String &device_name, RxLineHandler on_line);
  void loop();
  bool connected();
  void sendLine(const String &line);
  void clearBonds();   // for cmd:unpair
}
