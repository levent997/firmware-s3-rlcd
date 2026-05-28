#include "battery_math.h"

namespace battery {

int socFromVoltage(float v) {
  static const float pts[][2] = {
    {3.30f,   0.0f}, {3.50f,   5.0f}, {3.65f,  10.0f}, {3.70f,  20.0f},
    {3.75f,  30.0f}, {3.80f,  45.0f}, {3.85f,  55.0f}, {3.90f,  65.0f},
    {3.95f,  75.0f}, {4.00f,  82.0f}, {4.05f,  88.0f}, {4.10f,  93.0f},
    {4.15f,  97.0f}, {4.20f, 100.0f},
  };
  const int N = sizeof(pts) / sizeof(pts[0]);
  if (v <= pts[0][0])     return 0;
  if (v >= pts[N - 1][0]) return 100;
  for (int i = 1; i < N; i++) {
    if (v < pts[i][0]) {
      float t = (v - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
      float s = pts[i - 1][1] + t * (pts[i][1] - pts[i - 1][1]);
      int   r = (int)(s + 0.5f);
      if (r < 0) r = 0;
      if (r > 100) r = 100;
      return r;
    }
  }
  return 100;
}

void BatteryEstimator::update(float raw, uint32_t now_ms) {
  v_raw = raw;

  // EWMA across ticks (first sample seeds the filter directly).
  if (ewma_ < 1.0f) ewma_ = raw;
  else              ewma_ = ewma_ + EWMA_ALPHA * (raw - ewma_);

  // 60 s circular history of the filtered voltage.
  float oldest = hist_[pos_];
  hist_[pos_] = ewma_;
  pos_ = (pos_ + 1) % HIST;
  if (pos_ == 0) filled_ = true;

  float step = (prev_raw_ > 1.0f) ? (raw - prev_raw_) : 0.0f;
  prev_raw_ = raw;

  dv60 = filled_ ? (ewma_ - oldest) : 0.0f;
  bool triggered = false;
  char r = '-';
  if      (step > JUMP_V)                 { triggered = true; r = 'J'; }
  else if (ewma_ > FULL_V)                { triggered = true; r = 'F'; }
  else if (filled_ && dv60 > TREND_V)     { triggered = true; r = 'U'; }

  if (triggered) last_trigger_ms_ = now_ms;
  bool chg = triggered ||
             (last_trigger_ms_ != 0 && (now_ms - last_trigger_ms_) < STICKY_MS);
  if (!triggered && chg) r = 'S';

  // Falling edge of charge (just unplugged): snap EWMA to the current raw
  // so the released SOC tracks the cell's relaxation, not the stale-high
  // charger voltage.
  if (was_charge_ && !chg) ewma_ = raw;
  was_charge_ = chg;

  charging   = chg;
  reason     = r;
  v_filtered = ewma_;

  // SOC: frozen while charging (except the near-full plateau, where we let
  // it climb the last few % so the gauge can reach 100).
  int soc_now = socFromVoltage(ewma_);
  if (chg) {
    if (soc < 0) {
      soc = soc_now;
    } else if (r == 'F' && ewma_ >= FULL_V) {
      if (soc_now > soc) soc = soc_now;
    }
    // else: hold whatever was shown before charging started
  } else {
    soc = soc_now;
  }
}

}  // namespace battery
