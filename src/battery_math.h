#pragma once
#include <stdint.h>

// Pure, Arduino-free battery math so it can be unit-tested on the host
// (see test/test_battery). sensors.cpp owns the hardware (ADC read, divider
// math) and delegates all the filtering / charge-state / SOC decisions here.
namespace battery {

// LiPo terminal voltage -> state-of-charge percent (0..100). 14-point
// piecewise-linear approximation of the typical 1C-discharge curve; the
// flat 3.7-4.0 V mid-band is exactly what a naive linear scaling gets wrong.
int socFromVoltage(float v);

// Stateful estimator fed one raw cell-voltage sample per tick (~5 s on the
// device). It EWMA-filters the voltage, detects charging, and produces a
// displayed SOC that is FROZEN while charging (so plugging in doesn't make
// the gauge leap up the charger's CV voltage).
//
// Charge triggers (priority order), surfaced in `reason`:
//   'J' raw jumped > 60 mV in one tick (plug-in, caught instantly)
//   'F' filtered voltage > 4.18 V (near-full plateau)
//   'U' filtered voltage rose > 30 mV over the trailing 60 s
//   'S' sticky carry-over (held for STICKY_MS after the last trigger)
//   '-' not charging
class BatteryEstimator {
 public:
  // Outputs, valid after update():
  float v_filtered = 0.0f;   // EWMA-filtered voltage (what the UI shows)
  float v_raw      = 0.0f;   // the raw sample just fed in
  int   soc        = -1;     // displayed SOC %, -1 = unknown, frozen on charge
  bool  charging   = false;
  char  reason     = '-';
  float dv60       = 0.0f;   // 60 s filtered-voltage delta (the 'U' rule input)

  void update(float raw_voltage, uint32_t now_ms);

  static constexpr float EWMA_ALPHA   = 0.2f;
  static constexpr float JUMP_V       = 0.060f;
  static constexpr float FULL_V       = 4.18f;
  static constexpr float TREND_V      = 0.030f;
  static constexpr uint32_t STICKY_MS = 30000;
  static constexpr int HIST = 12;        // 12 slots * 5 s = 60 s window

 private:
  float ewma_ = 0.0f;
  float hist_[HIST] = {0};
  int   pos_ = 0;
  bool  filled_ = false;
  float prev_raw_ = 0.0f;
  uint32_t last_trigger_ms_ = 0;
  bool was_charge_ = false;
};

}  // namespace battery
