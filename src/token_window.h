#pragma once
#include <stdint.h>

// Pure, Arduino-free rolling token-usage window so it can be unit-tested on
// the host (see test/test_token). protocol.cpp feeds it each heartbeat's
// cumulative token count + millis(); it maintains the trailing 1h / 5h
// deltas. The lifetime accumulator `tokens_boot` is owned by the CALLER
// (it's NVS-persisted in g_state), so it's passed in/out by reference.
namespace tokenwin {

class TokenWindow {
 public:
  // Outputs after update():
  uint32_t tokens_1h = 0;
  uint32_t tokens_5h = 0;

  // Feed one heartbeat. tokens_now is the desktop's cumulative count;
  // tokens_boot is the persisted lifetime accumulator, advanced in place.
  void update(uint32_t tokens_now, uint32_t now_ms, uint32_t &tokens_boot);

  static constexpr int HISTORY = 300;   // 300 min @ 1/min = 5h

 private:
  struct Sample { uint32_t t_ms; uint32_t cum; };
  Sample hist_[HISTORY] = {};
  int count_ = 0;
  int head_ = 0;
  // Sentinel distinguishes "never seen a heartbeat" from "last was 0" — the
  // first heartbeat after boot must NOT backfill the desktop's whole
  // cumulative into tokens_boot.
  uint32_t last_tokens_seen_ = UINT32_MAX;
  uint32_t last_sample_ms_ = 0;
};

}  // namespace tokenwin
