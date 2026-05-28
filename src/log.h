#pragma once
#include <Arduino.h>

// Compile-time log level. Define LOG_LEVEL via build_flags to control how
// much gets compiled in:
//   0 = silent      1 = error      2 = info      3 = debug (default)
//
// Debug builds keep everything (current behaviour). A release build with
// -DLOG_LEVEL=1 compiles out the periodic [hb]/[shtc3]/[ble] chatter, which
// (a) saves flash and (b) cuts USB-CDC traffic that keeps the host's USB
// stack — and thus the device's USB PHY — busy and drawing current.
#ifndef LOG_LEVEL
#define LOG_LEVEL 3
#endif

#if LOG_LEVEL >= 1
#define LOGE(...) Serial.printf(__VA_ARGS__)
#else
#define LOGE(...) ((void)0)
#endif

#if LOG_LEVEL >= 2
#define LOGI(...) Serial.printf(__VA_ARGS__)
#else
#define LOGI(...) ((void)0)
#endif

#if LOG_LEVEL >= 3
#define LOGD(...) Serial.printf(__VA_ARGS__)
#else
#define LOGD(...) ((void)0)
#endif
