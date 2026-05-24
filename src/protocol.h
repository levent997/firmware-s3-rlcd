#pragma once
#include <Arduino.h>

namespace protocol {
  void handleLine(const String &line);
  void sendPermission(const String &id, bool approve);
}
