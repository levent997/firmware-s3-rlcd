#include "rtc.h"
#include <Arduino.h>
#include <Wire.h>

namespace {
constexpr uint8_t PCF85063_ADDR = 0x51;

// Register map (PCF85063A datasheet § 8.2):
//   0x00 Control_1   bit 5 = STOP (0 to run), bit 1 = 12/24h (0 = 24h)
//   0x04 Seconds     bit 7 = OS  (oscillator stopped since last write)
//   0x05 Minutes
//   0x06 Hours       24h mode
//   0x07 Days
//   0x08 Weekdays
//   0x09 Months
//   0x0A Years       2-digit BCD; we add 2000

bool present = false;
bool valid_time = false;

inline uint8_t toBcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }
inline uint8_t fromBcd(uint8_t v) { return ((v >> 4) * 10) + (v & 0x0F); }

inline bool isLeap(int y) {
  return (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
}

bool writeReg(uint8_t reg, const uint8_t *data, size_t n) {
  Wire.beginTransmission(PCF85063_ADDR);
  Wire.write(reg);
  for (size_t i = 0; i < n; i++) Wire.write(data[i]);
  return Wire.endTransmission() == 0;
}

bool readReg(uint8_t reg, uint8_t *data, size_t n) {
  Wire.beginTransmission(PCF85063_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  size_t got = Wire.requestFrom((int)PCF85063_ADDR, (int)n);
  if (got != n) return false;
  for (size_t i = 0; i < n; i++) data[i] = Wire.read();
  return true;
}

// Convert (year, mon, day, h, m, s) — local time — into seconds since
// 1970-01-01 00:00:00 in the same local frame.
uint32_t calendarToLocalEpoch(int year, int mon, int day,
                              int hour, int minute, int sec) {
  static const uint8_t month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  uint32_t days = 0;
  for (int y = 1970; y < year; y++) days += isLeap(y) ? 366 : 365;
  for (int m = 0; m < mon - 1; m++) {
    uint32_t dm = month_days[m];
    if (m == 1 && isLeap(year)) dm = 29;
    days += dm;
  }
  days += (day - 1);
  return days * 86400UL + hour * 3600UL + minute * 60UL + sec;
}

// Inverse of calendarToLocalEpoch.
void localEpochToCalendar(uint32_t local_seconds,
                          int *year, int *mon, int *day,
                          int *hour, int *minute, int *sec) {
  *sec    = local_seconds % 60; local_seconds /= 60;
  *minute = local_seconds % 60; local_seconds /= 60;
  *hour   = local_seconds % 24; local_seconds /= 24;
  uint32_t days = local_seconds;
  int y = 1970;
  while (true) {
    uint32_t yd = isLeap(y) ? 366 : 365;
    if (days < yd) break;
    days -= yd;
    y++;
  }
  *year = y;
  static const uint8_t month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  int m = 1;
  for (int i = 0; i < 12; i++) {
    uint32_t dm = month_days[i];
    if (i == 1 && isLeap(y)) dm = 29;
    if (days < dm) break;
    days -= dm;
    m++;
  }
  *mon = m;
  *day = days + 1;
}
} // namespace

void rtc::begin() {
  // Wire is already up courtesy of sensors::begin(). Just probe.
  Wire.beginTransmission(PCF85063_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.println("[rtc] PCF85063 not detected on I2C 0x51");
    return;
  }
  uint8_t ctrl1 = 0;
  if (!readReg(0x00, &ctrl1, 1)) {
    Serial.println("[rtc] CTRL1 read failed after probe");
    return;
  }
  // Clear STOP bit if set (chip out of reset can come up stopped).
  if (ctrl1 & 0x20) {
    uint8_t cleared = ctrl1 & ~0x20;
    writeReg(0x00, &cleared, 1);
    Serial.printf("[rtc] cleared STOP bit (CTRL1 0x%02X -> 0x%02X)\n",
                  ctrl1, cleared);
    ctrl1 = cleared;
  }
  // Check OS flag — set means time has been invalid since last write.
  uint8_t sec_reg = 0;
  if (readReg(0x04, &sec_reg, 1)) {
    valid_time = !(sec_reg & 0x80);
  }
  present = true;
  Serial.printf("[rtc] PCF85063 ready (CTRL1=0x%02X, time %s)\n",
                ctrl1, valid_time ? "VALID" : "INVALID (OS flag set)");
}

bool rtc::isPresent() { return present; }
bool rtc::hasValidTime() { return present && valid_time; }

bool rtc::setLocalEpoch(uint32_t local_seconds) {
  if (!present) return false;
  int year, mon, day, hour, minute, sec;
  localEpochToCalendar(local_seconds, &year, &mon, &day, &hour, &minute, &sec);
  if (year < 2000 || year > 2099) {
    Serial.printf("[rtc] year %d outside chip's 2000-2099 window, skip\n", year);
    return false;
  }
  uint8_t buf[7];
  // Writing to Seconds clears the OS flag (bit 7 = 0) by definition.
  buf[0] = toBcd(sec)    & 0x7F;
  buf[1] = toBcd(minute) & 0x7F;
  buf[2] = toBcd(hour)   & 0x3F;
  buf[3] = toBcd(day)    & 0x3F;
  buf[4] = 0;                                  // weekday — informational
  buf[5] = toBcd(mon)    & 0x1F;
  buf[6] = toBcd(year - 2000);
  bool ok = writeReg(0x04, buf, 7);
  if (ok) {
    valid_time = true;
    Serial.printf("[rtc] wrote %04d-%02d-%02d %02d:%02d:%02d (local)\n",
                  year, mon, day, hour, minute, sec);
  } else {
    Serial.println("[rtc] write failed");
  }
  return ok;
}

bool rtc::readLocalEpoch(uint32_t *out) {
  if (!present || !out) return false;
  uint8_t buf[7];
  if (!readReg(0x04, buf, 7)) return false;
  if (buf[0] & 0x80) {
    valid_time = false;
    return false;
  }
  int sec    = fromBcd(buf[0] & 0x7F);
  int minute = fromBcd(buf[1] & 0x7F);
  int hour   = fromBcd(buf[2] & 0x3F);
  int day    = fromBcd(buf[3] & 0x3F);
  int mon    = fromBcd(buf[5] & 0x1F);
  int year   = 2000 + fromBcd(buf[6]);
  // Sanity: any garbage out of range means the chip never had a valid set.
  if (mon < 1 || mon > 12 || day < 1 || day > 31 ||
      hour > 23 || minute > 59 || sec > 59) {
    valid_time = false;
    return false;
  }
  *out = calendarToLocalEpoch(year, mon, day, hour, minute, sec);
  valid_time = true;
  return true;
}
