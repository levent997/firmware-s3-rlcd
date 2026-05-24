#include "imu.h"
#include "state.h"
#include "persist.h"
#include <Arduino.h>
#include <Wire.h>
#include <math.h>

namespace {
constexpr uint8_t QMI_ADDR_PRIMARY = 0x6A;
constexpr uint8_t QMI_ADDR_ALT     = 0x6B;

// QMI8658 register map (from QST datasheet).
constexpr uint8_t REG_WHO_AM_I = 0x00;   // = 0x05
constexpr uint8_t REG_CTRL1    = 0x02;   // serial interface
constexpr uint8_t REG_CTRL2    = 0x03;   // accel: scale + ODR
constexpr uint8_t REG_CTRL3    = 0x04;   // gyro
constexpr uint8_t REG_CTRL5    = 0x06;   // LPF
constexpr uint8_t REG_CTRL7    = 0x08;   // sensor enable: bit0 = accel
constexpr uint8_t REG_AX_L     = 0x35;
constexpr uint8_t WHO_AM_I_OK  = 0x05;

uint8_t addr = 0;
bool present = false;

float ax_g = 0.0f, ay_g = 0.0f, az_g = 0.0f;
uint32_t last_poll_ms = 0;
uint32_t face_down_since_ms = 0;     // first ms we saw Z < -0.7g; 0 if not currently
bool was_facing_down = false;

bool writeReg(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

int readReg(uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((int)addr, 1) != 1) return -1;
  return Wire.read();
}

bool readBlock(uint8_t reg, uint8_t *out, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if ((size_t)Wire.requestFrom((int)addr, (int)n) != n) return false;
  for (size_t i = 0; i < n; i++) out[i] = Wire.read();
  return true;
}

bool probe(uint8_t a) {
  addr = a;
  int w = readReg(REG_WHO_AM_I);
  if (w == WHO_AM_I_OK) {
    Serial.printf("[imu] QMI8658 detected at 0x%02X (WHO_AM_I=0x%02X)\n", a, w);
    return true;
  }
  return false;
}
} // namespace

void imu::begin() {
  // Wire is initialised by sensors::begin() before us.

  // Scan every address and try to read register 0x00 (= WHO_AM_I for many
  // chips). Whichever device returns 0x05 is our QMI8658, regardless of
  // what address it sits at -- we don't trust the datasheet's 0x6A/0x6B
  // promise after the first try found nothing there.
  Serial.print("[imu] I2C scan: ");
  uint8_t found_addr = 0;
  for (uint8_t a = 0x08; a < 0x78; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() != 0) continue;
    addr = a;
    int who = readReg(REG_WHO_AM_I);
    Serial.printf("[0x%02X reg0=0x%02X] ", a, who);
    if (who == WHO_AM_I_OK && found_addr == 0) {
      found_addr = a;
    }
  }
  Serial.println();

  if (found_addr) {
    addr = found_addr;
    Serial.printf("[imu] QMI8658 found at 0x%02X\n", found_addr);
  } else if (!probe(QMI_ADDR_PRIMARY) && !probe(QMI_ADDR_ALT)) {
    Serial.println("[imu] QMI8658 not on bus (may not be populated on this board variant)");
    addr = 0;
    return;
  }
  // CTRL1: auto-increment on (bit 6), default everything else
  writeReg(REG_CTRL1, 0x60);
  // CTRL2: accel scale +/-4g (010 << 4 = 0x20), ODR 62.5 Hz (0x05)
  writeReg(REG_CTRL2, 0x25);
  // CTRL3: gyro off (we don't use it -- saves power)
  writeReg(REG_CTRL3, 0x00);
  // CTRL5: LPF for accel (default LP bypass)
  writeReg(REG_CTRL5, 0x00);
  // CTRL7: enable accel only (bit 0)
  writeReg(REG_CTRL7, 0x01);
  delay(20);
  present = true;
  Serial.println("[imu] ready (accel only, +/-4g @ 62.5 Hz)");
}

bool imu::isPresent() { return present; }
float imu::lastAxG() { return ax_g; }
float imu::lastAyG() { return ay_g; }
float imu::lastAzG() { return az_g; }

void imu::loop() {
  if (!present) return;
  uint32_t now = millis();
  if (now - last_poll_ms < 200) return;   // 5 Hz poll is plenty
  last_poll_ms = now;

  uint8_t buf[6];
  if (!readBlock(REG_AX_L, buf, 6)) {
    Serial.println("[imu] read failed");
    return;
  }
  int16_t ax_raw = (int16_t)(buf[0] | (buf[1] << 8));
  int16_t ay_raw = (int16_t)(buf[2] | (buf[3] << 8));
  int16_t az_raw = (int16_t)(buf[4] | (buf[5] << 8));
  // +/-4g full scale -> 1 LSB = 4/32768 g
  constexpr float LSB_TO_G = 4.0f / 32768.0f;
  ax_g = ax_raw * LSB_TO_G;
  ay_g = ay_raw * LSB_TO_G;
  az_g = az_raw * LSB_TO_G;

  // --- Face-down nap detection ---
  // Board flat & screen-down means gravity reads ~ -1g on the Z axis.
  // We trigger nap after the condition holds for >= 5 s so a brief flip
  // while picking up the board doesn't put it to sleep.
  bool facing_down = (az_g < -0.7f);
  if (facing_down) {
    if (face_down_since_ms == 0) face_down_since_ms = now;
    if (!g_state.napping && (now - face_down_since_ms) > 5000UL) {
      g_state.napping = true;
      Serial.println("[imu] face-down -> napping");
    }
  } else {
    face_down_since_ms = 0;
    if (was_facing_down) {
      Serial.println("[imu] face-up again");
    }
  }
  if (was_facing_down && !facing_down && g_state.napping) {
    // Nap ended via flip-up: restore energy (mirrors the BLE-reconnect path
    // in main.cpp's nap handler) and persist the new nap-end timestamp.
    g_state.energy_at_nap = 5;
    g_state.last_nap_end_ms = now;
    g_state.napping = false;
    persist::onNapEnd();
    Serial.println("[imu] face-up -> nap ended");
  }
  was_facing_down = facing_down;

  // --- Shake detection ---
  // Magnitude excluding gravity. Sqrt is fine at 5 Hz, but we can use
  // squared magnitude to skip the sqrt: threshold is (2g)^2 = 4.
  float mag2 = ax_g*ax_g + ay_g*ay_g + az_g*az_g;
  if (mag2 > 4.0f) {
    // Hold dizzy state for 5 s after the last shake spike.
    g_state.dizzy_until_ms = now + 5000UL;
    Serial.printf("[imu] shake! |a|=%.2fg\n", sqrtf(mag2));
  }
}
