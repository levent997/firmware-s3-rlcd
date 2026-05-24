#pragma once
#include <stdint.h>

// Settings menu — accessible by long-press KEY/BOOT on the USAGE view.
//
// Inside the menu:
//   KEY  short = move selection DOWN
//   BOOT short = move selection UP
//   KEY  long  = activate selected item (toggle / open confirm screen / run)
//   BOOT long  = back out (close confirm if open, close menu otherwise)
//
// Destructive items (factory reset, remove packs, reset stats) display a
// confirm screen first; a second KEY-long there actually fires.
//
// 30 s of no input auto-closes the menu so the user doesn't get stuck.
namespace menu {
  // Lifecycle
  void open();             // call from main.cpp's button handler
  bool isOpen();
  void close();

  // Button input dispatch — main.cpp calls these instead of its usual
  // view-navigation logic while menu is open.
  void onKeyShort();       // next item
  void onBootShort();      // previous item
  void onKeyLong();        // activate / confirm
  void onBootLong();       // back / cancel

  // Per-loop hook for inactivity timeout.
  void tick();

  // For ui.cpp render dispatch.
  int itemCount();
  const char *itemLabel(int idx);
  const char *itemValue(int idx);     // "" if no inline value
  bool itemIsDestructive(int idx);

  // Confirm-screen content for the currently selected destructive item.
  const char *confirmTitle();
  const char *confirmBody();
}
