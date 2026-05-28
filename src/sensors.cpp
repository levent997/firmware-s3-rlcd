#include "sensors.h"
#include "state.h"
#include <Arduino.h>
#include <Wire.h>

// Battery sense: ADC1_CH3 = GPIO4, with 1/3 voltage divider (R21=200K, R23=100K).
// vol = adc_voltage * 3.
//
// SHTC3 temp/humidity: I2C addr 0x70 on SDA=13, SCL=14.

namespace {
constexpr int PIN_BAT = 4;
constexpr int PIN_SDA = 13;
constexpr int PIN_SCL = 14;
constexpr uint8_t SHTC3_ADDR = 0x70;

uint32_t last_read_ms = 0;
bool shtc3_present = false;

bool shtc3_cmd(uint16_t cmd) {
  Wire.beginTransmission(SHTC3_ADDR);
  Wire.write(cmd >> 8);
  Wire.write(cmd & 0xFF);
  return Wire.endTransmission() == 0;
}

uint8_t shtc3_crc(uint8_t a, uint8_t b) {
  uint8_t crc = 0xFF;
  uint8_t data[2] = {a, b};
  for (int i = 0; i < 2; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
  }
  return crc;
}

bool shtc3_read(float *t_c, float *rh) {
  // Wake from sleep, then measure (T first, normal mode, clock stretching disabled)
  shtc3_cmd(0x3517); delay(1);
  if (!shtc3_cmd(0x7866)) return false;
  delay(15);
  Wire.requestFrom((int)SHTC3_ADDR, 6);
  if (Wire.available() < 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();
  if (shtc3_crc(b[0], b[1]) != b[2]) return false;
  if (shtc3_crc(b[3], b[4]) != b[5]) return false;
  uint16_t traw = (b[0] << 8) | b[1];
  uint16_t hraw = (b[3] << 8) | b[4];
  *t_c = -45.0f + 175.0f * (traw / 65535.0f);
  *rh = 100.0f * (hraw / 65535.0f);
  shtc3_cmd(0xB098); // back to sleep
  return true;
}
}

void sensors::begin() {
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_BAT, ADC_11db);

  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  // probe SHTC3 with wakeup + soft reset
  shtc3_cmd(0x3517);   // wakeup
  delay(1);
  if (shtc3_cmd(0x805D)) { // soft reset
    delay(10);
    shtc3_present = true;
    Serial.println("[sensors] SHTC3 OK");
  } else {
    Serial.println("[sensors] SHTC3 not responding");
  }
  shtc3_cmd(0xB098); // back to sleep
}

// LiPo voltage -> state-of-charge, piecewise-linear approximation of the
// typical 1C-discharge curve. Returns 0..100. Tuned for 4.20V full / 3.30V
// empty; flat mid-band (3.7-4.0V is 30-80%) is what makes a naive linear
// 0.85 V scaling wildly over-report mid-life and under-report near empty.
static int lipoSocFromV(float v) {
  static const float pts[][2] = {
    {3.30f,   0.0f},
    {3.50f,   5.0f},
    {3.65f,  10.0f},
    {3.70f,  20.0f},
    {3.75f,  30.0f},
    {3.80f,  45.0f},
    {3.85f,  55.0f},
    {3.90f,  65.0f},
    {3.95f,  75.0f},
    {4.00f,  82.0f},
    {4.05f,  88.0f},
    {4.10f,  93.0f},
    {4.15f,  97.0f},
    {4.20f, 100.0f},
  };
  const int N = sizeof(pts) / sizeof(pts[0]);
  if (v <= pts[0][0])     return 0;
  if (v >= pts[N - 1][0]) return 100;
  for (int i = 1; i < N; i++) {
    if (v < pts[i][0]) {
      float t = (v - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
      float s = pts[i - 1][1] + t * (pts[i][1] - pts[i - 1][1]);
      int   r = (int)(s + 0.5f);
      if (r < 0) r = 0; if (r > 100) r = 100;
      return r;
    }
  }
  return 100;
}

void sensors::loop() {
  uint32_t now = millis();
  if (now - last_read_ms < 5000) return;
  last_read_ms = now;
  g_state.battery_samples++;

  // ---- Battery: read, filter, decide charge state, then assign SOC ----
  uint32_t sum_mv = 0;
  const int N = 8;
  for (int i = 0; i < N; i++) sum_mv += analogReadMilliVolts(PIN_BAT);
  float pin_mv = sum_mv / (float)N;
  float bat_v_raw = (pin_mv / 1000.0f) * 3.0f;
  g_state.battery_pin_mv = pin_mv;
  g_state.battery_v_raw  = bat_v_raw;

  // EWMA across ticks. alpha=0.2 -> ~22 s effective time constant at 5 s
  // tick. Smooths over BLE-burst voltage sag without lagging real changes
  // by more than half a minute. First sample seeds the filter directly.
  static float bv_ewma = 0.0f;
  if (bv_ewma < 1.0f) bv_ewma = bat_v_raw;
  else                bv_ewma = bv_ewma + 0.2f * (bat_v_raw - bv_ewma);
  g_state.battery_v = bv_ewma;

  // Charging detection — heuristic since the ETA6098 STAT pin is not
  // wired to a GPIO. Three triggers, in priority order:
  //
  //   (J) one-tick raw rise > 60 mV: catches plug-in INSTANTLY. Without
  //       this, the only triggers were (F) and (U) below — but (F) only
  //       fires once you're already near full, and (U) needs 60 s of
  //       history. So during the first minute of charging starting from
  //       low SOC, nothing fired and the SOC display tracked rising
  //       voltage straight up. (This is the bug that produced the
  //       "10%->20% in 10 s" complaint after the first patch.)
  //
  //   (F) raw > 4.18 V: near-full plateau. Always charging in practice.
  //
  //   (U) 60-s smoothed-voltage rise > 30 mV: catches slow charging that
  //       (J) might miss (e.g. low-current trickle, or starting near
  //       fully-charged), and ride-throughs once (J) has stopped pulsing.
  //
  // The result is sticky for STICKY_MS after the last trigger so a brief
  // load-spike dip mid-charge doesn't unfreeze the gauge.
  static float bv_hist[12] = {0};
  static int   bv_pos = 0;
  static bool  bv_filled = false;
  float oldest = bv_hist[bv_pos];
  bv_hist[bv_pos] = bv_ewma;
  bv_pos = (bv_pos + 1) % 12;
  if (bv_pos == 0) bv_filled = true;

  static float prev_raw = 0.0f;
  float raw_step = (prev_raw > 1.0f) ? (bat_v_raw - prev_raw) : 0.0f;
  prev_raw = bat_v_raw;

  float dv60 = bv_filled ? (bv_ewma - oldest) : 0.0f;
  bool  triggered = false;
  char  reason    = '-';
  if      (raw_step > 0.060f)               { triggered = true; reason = 'J'; }
  else if (bat_v_raw > 4.18f)               { triggered = true; reason = 'F'; }
  else if (bv_filled && dv60 > 0.03f)       { triggered = true; reason = 'U'; }

  // Stickiness: hold charging=true for STICKY_MS after the last trigger.
  // Without this, an inter-tick load spike can flip charge->false for
  // one tick and the SOC freeze would briefly release, snapping the
  // displayed % up to whatever lipoSocFromV(bv_ewma) is at that instant.
  constexpr uint32_t STICKY_MS = 30000;
  static uint32_t last_trigger_ms = 0;
  if (triggered) last_trigger_ms = now;
  bool charge = triggered ||
                (last_trigger_ms != 0 && (now - last_trigger_ms) < STICKY_MS);
  if (!triggered && charge) reason = 'S';  // sticky carry-over

  // On falling edge of charge (just unplugged), snap EWMA to the current
  // raw reading. Otherwise the EWMA is still anchored to the charger's CV
  // voltage from the last several ticks, and as soon as the freeze
  // releases the SOC would jump to that stale-high value. Snapping makes
  // SOC track the cell's actual relaxation curve from t=0 of unplug.
  static bool was_charge = false;
  if (was_charge && !charge) bv_ewma = bat_v_raw;
  was_charge = charge;

  g_state.charging        = charge;
  g_state.charging_reason = reason;
  g_state.battery_v       = bv_ewma;   // re-publish, in case snap edited it
  g_state.battery_dv_60s  = dv60;

  // SOC: when charging, FREEZE the displayed percentage. Cell terminal
  // voltage during charge is dominated by the charger's CV pin (≈4.05 V
  // at moderate current, climbing to 4.20 V) which is NOT a meaningful
  // proxy for true state-of-charge — that's what causes the infamous
  // "plug in, jumps to 90%" UX. Freeze until either we hit ≥4.18 V
  // (close to fully charged) or charging stops.
  int soc_now = lipoSocFromV(bv_ewma);
  if (charge) {
    if (g_state.battery_pct < 0) {
      g_state.battery_pct = soc_now;        // never been read; allow first display
    } else if (reason == 'F' && bv_ewma >= 4.18f) {
      // Near-full plateau — allow SOC to climb the last few % so the
      // UI eventually reads 100%.
      if (soc_now > g_state.battery_pct) g_state.battery_pct = soc_now;
    }
    // else: hold whatever was last shown before charging started
  } else {
    g_state.battery_pct = soc_now;
  }

  // SHTC3
  if (shtc3_present) {
    float t, h;
    if (shtc3_read(&t, &h)) {
      g_state.temp_c = t;
      g_state.humidity_pct = h;
    }
  }
}
