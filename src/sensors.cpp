#include "sensors.h"
#include "state.h"
#include "battery_math.h"
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

void sensors::loop() {
  uint32_t now = millis();
  if (now - last_read_ms < 5000) return;
  last_read_ms = now;
  g_state.battery_samples++;

  // ---- Battery ----
  // sensors.cpp only owns the hardware: average 8 ADC reads and undo the
  // 1/3 divider. All filtering / charge-state / SOC logic lives in the
  // unit-tested battery::BatteryEstimator (see src/battery_math.* and
  // test/test_battery). estimator state persists across calls.
  uint32_t sum_mv = 0;
  const int N = 8;
  for (int i = 0; i < N; i++) sum_mv += analogReadMilliVolts(PIN_BAT);
  float pin_mv = sum_mv / (float)N;
  float bat_v_raw = (pin_mv / 1000.0f) * 3.0f;

  static battery::BatteryEstimator est;
  est.update(bat_v_raw, now);

  g_state.battery_pin_mv  = pin_mv;
  g_state.battery_v_raw   = est.v_raw;
  g_state.battery_v       = est.v_filtered;
  g_state.battery_pct     = est.soc;
  g_state.charging        = est.charging;
  g_state.charging_reason = est.reason;
  g_state.battery_dv_60s  = est.dv60;

  // SHTC3
  if (shtc3_present) {
    float t, h;
    if (shtc3_read(&t, &h)) {
      g_state.temp_c = t;
      g_state.humidity_pct = h;
    }
  }
}
