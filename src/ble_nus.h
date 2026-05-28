#pragma once
#include <Arduino.h>

namespace ble_nus {
  using RxLineHandler = void (*)(const String &line);

  void begin(const String &device_name, RxLineHandler on_line);
  void loop();
  bool connected();
  void sendLine(const String &line);
  void clearBonds();   // for cmd:unpair

  // Advertising interval in milliseconds (single value; we use the same
  // min and max). Higher = lower idle current draw, but slower to be
  // discovered by the desktop scanner. Exposed for the SYSTEM diag view.
  uint16_t advertisingIntervalMs();
}
