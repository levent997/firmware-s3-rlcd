#pragma once
#include <stdint.h>

// PCF85063A real-time clock at I2C 0x51 (shares the bus with SHTC3).
//
// The chip is BCD-encoded and only handles years 00-99 mapped to 2000-2099,
// so we treat it as a *local wall clock* store: writing converts the desktop
// time sync packet's (utc_epoch + tz_offset_sec) into a calendar time and
// pushes the BCD bytes; reading inverts that and returns a "local epoch
// seconds since 1970-01-01" value. That keeps the existing top-bar display
// math (which works in local time) unchanged whether the seed came from BLE
// or from the chip.
namespace rtc {
  // Call after sensors::begin() (which has already initialised Wire).
  // Detects the chip, clears any STOP bit, and prints status. Idempotent.
  void begin();

  // True if the chip ACK'd on probe. begin() must have been called.
  bool isPresent();

  // True if the chip is present AND its oscillator-stopped flag is clear,
  // i.e. the stored time is meaningful (chip was previously written and has
  // had VBAT power continuously since). Read separately from isPresent so
  // the caller can distinguish "no chip" from "fresh chip, never set".
  bool hasValidTime();

  // Write the chip with calendar broken-down form of `local_epoch_seconds`.
  // Returns true on I2C success. `local_epoch_seconds` is seconds since
  // 1970-01-01 00:00:00 in the user's local time zone (= utc_epoch + tz_offset).
  bool setLocalEpoch(uint32_t local_epoch_seconds);

  // Read the chip and return "local epoch seconds" into *out. Returns false
  // if absent, oscillator was stopped, or I2C failed.
  bool readLocalEpoch(uint32_t *out_local_seconds);
}
