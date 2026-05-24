#pragma once
#include <U8g2lib.h>

namespace ui {
  void begin(U8G2 *u);
  // Returns true if a redraw actually happened.
  bool render();
}
