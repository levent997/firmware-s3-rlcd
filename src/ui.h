#pragma once
#include <U8g2lib.h>

namespace ui {
  void begin(U8G2 *u);
  // Returns true if a redraw actually happened.
  bool render();
  // Draw a static "sleeping, press KEY to wake" frame, flushed immediately.
  // Called right before the device enters deep sleep so the (image-holding)
  // reflective LCD shows something meaningful instead of a frozen clock.
  void showSleeping();
}
