#pragma once
#include <Arduino.h>
#include <Preferences.h>

// Thin, typed, RAII wrapper over ESP32 NVS (Preferences). Each instance
// opens its own namespaced handle on construction and closes it on
// destruction, so callers can't leak a handle or forget end(). Hold one in
// a scope to batch several reads/writes to the same namespace:
//
//   { settings::Settings s; s.putBool("sound", on); s.putUInt("level", n); }
//
// Replaces the per-field begin/end boilerplate that used to live in
// persist.cpp, and is reusable by any future module that needs NVS.
namespace settings {

class Settings {
 public:
  // ns defaults to the firmware's main namespace. readOnly=true opens a
  // read-only handle (load path); false allows writes.
  explicit Settings(const char *ns = "buddy", bool readOnly = false) {
    ok_ = prefs_.begin(ns, readOnly);
  }
  ~Settings() { if (ok_) prefs_.end(); }

  Settings(const Settings &) = delete;
  Settings &operator=(const Settings &) = delete;

  bool ok() const { return ok_; }

  uint32_t getUInt(const char *k, uint32_t def = 0)   { return prefs_.getUInt(k, def); }
  uint8_t  getUChar(const char *k, uint8_t def = 0)   { return prefs_.getUChar(k, def); }
  bool     getBool(const char *k, bool def = false)   { return prefs_.getBool(k, def); }
  String   getString(const char *k, const char *def = "") { return prefs_.getString(k, def); }

  void putUInt(const char *k, uint32_t v)   { prefs_.putUInt(k, v); }
  void putUChar(const char *k, uint8_t v)   { prefs_.putUChar(k, v); }
  void putBool(const char *k, bool v)       { prefs_.putBool(k, v); }
  void putString(const char *k, const String &v) { prefs_.putString(k, v); }

  void clear() { prefs_.clear(); }

 private:
  Preferences prefs_;
  bool ok_ = false;
};

}  // namespace settings
