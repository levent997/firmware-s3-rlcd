// Host-side unit tests for src/token_window.* (run on `pio test -e native`).
// Covers the subtle bits called out as gotchas: cold-start must NOT backfill
// the desktop's whole cumulative, desktop-restart must re-anchor (not count
// a huge negative/positive delta), and the 1h/5h windows must be correct.
#include <unity.h>
#include "token_window.h"

using tokenwin::TokenWindow;

void setUp() {}
void tearDown() {}

void test_cold_start_no_backfill() {
  TokenWindow w;
  uint32_t boot = 0;
  // First heartbeat carries the desktop's lifetime total — must NOT be
  // added to our per-boot accumulator.
  w.update(184502, 1000, boot);
  TEST_ASSERT_EQUAL_UINT32(0, boot);
}

void test_delta_accumulates() {
  TokenWindow w;
  uint32_t boot = 0;
  w.update(184502, 1000, boot);   // anchor
  w.update(184602, 2000, boot);   // +100
  w.update(184652, 3000, boot);   // +50
  TEST_ASSERT_EQUAL_UINT32(150, boot);
}

void test_desktop_restart_reanchors() {
  TokenWindow w;
  uint32_t boot = 0;
  w.update(184502, 1000, boot);   // anchor
  w.update(184602, 2000, boot);   // +100 -> boot 100
  TEST_ASSERT_EQUAL_UINT32(100, boot);
  w.update(50, 3000, boot);       // desktop restarted (count dropped) -> re-anchor
  TEST_ASSERT_EQUAL_UINT32(100, boot);   // no spurious delta
  w.update(150, 4000, boot);      // +100 from the new anchor (50)
  TEST_ASSERT_EQUAL_UINT32(200, boot);
}

void test_1h_5h_windows() {
  TokenWindow w;
  uint32_t boot = 0;
  // Anchor at t=0, then +100 tokens every minute for 90 minutes.
  uint32_t tok = 1000;
  w.update(tok, 0, boot);
  for (int m = 1; m <= 90; m++) {
    tok += 100;
    w.update(tok, (uint32_t)m * 60000u, boot);
  }
  TEST_ASSERT_EQUAL_UINT32(9000, boot);        // 90 * 100
  // At t=90min the 1h cutoff is t=30min; cumulative there was 30*100=3000.
  TEST_ASSERT_EQUAL_UINT32(6000, w.tokens_1h); // 9000 - 3000
  // 5h window spans the whole run (only 90 min of data) -> all 9000.
  TEST_ASSERT_EQUAL_UINT32(9000, w.tokens_5h);
}

void test_same_minute_no_double_sample_but_delta_counts() {
  TokenWindow w;
  uint32_t boot = 0;
  w.update(1000, 0, boot);        // anchor + first sample
  // Two updates within the same minute: deltas still accumulate, even though
  // only one sample/minute is stored.
  w.update(1100, 10000, boot);    // +100
  w.update(1250, 20000, boot);    // +150
  TEST_ASSERT_EQUAL_UINT32(250, boot);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_cold_start_no_backfill);
  RUN_TEST(test_delta_accumulates);
  RUN_TEST(test_desktop_restart_reanchors);
  RUN_TEST(test_1h_5h_windows);
  RUN_TEST(test_same_minute_no_double_sample_but_delta_counts);
  return UNITY_END();
}
