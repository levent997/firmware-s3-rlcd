#pragma once
#include <stdint.h>

// QMI8658C 6-axis IMU @ I2C 0x6A (default; 0x6B alternate). Shares the
// SDA/SCL bus with SHTC3/PCF85063/ES8311. We drive accel-only at low ODR
// and consume the results in two ways:
//
//   * Face-down detection (Z accel sustained < -0.7g for 5 s) replaces the
//     BLE-disconnect-> nap heuristic. The buddy actually goes to sleep
//     when you flip the board screen-down on the desk.
//   * Shake detection (peak |a| > 2g) flips g_state.dizzy_until_ms a few
//     seconds into the future. moodToSprite renders SPR_ANNOYED while
//     dizzy_until_ms is in the future.
namespace imu {
  void begin();
  bool isPresent();
  void loop();          // call from main loop; polls ~5 Hz

  // Latest raw reading (g units), or 0 if absent.
  float lastAxG();
  float lastAyG();
  float lastAzG();
}
