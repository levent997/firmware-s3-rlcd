// Host-side unit tests for src/battery_math.* (run on `pio test -e native`).
// Covers the SOC curve and the charge-state machine — in particular the
// "plug in -> SOC must NOT leap up" behaviour that was the original bug.
#include <unity.h>
#include "battery_math.h"

using battery::socFromVoltage;
using battery::BatteryEstimator;

void setUp() {}
void tearDown() {}

// Drive the estimator with `n` ticks at a fixed voltage, 5 s apart, starting
// at t0. Returns the time just after the last tick.
static uint32_t feed(BatteryEstimator &e, float v, int n, uint32_t t0) {
  uint32_t t = t0;
  for (int i = 0; i < n; i++) { e.update(v, t); t += 5000; }
  return t;
}

// ---- socFromVoltage ----

void test_soc_endpoints() {
  TEST_ASSERT_EQUAL_INT(0,   socFromVoltage(3.10f));   // below empty -> clamp 0
  TEST_ASSERT_EQUAL_INT(0,   socFromVoltage(3.30f));   // empty knee
  TEST_ASSERT_EQUAL_INT(100, socFromVoltage(4.20f));   // full knee
  TEST_ASSERT_EQUAL_INT(100, socFromVoltage(4.35f));   // above full -> clamp 100
}

void test_soc_known_points() {
  TEST_ASSERT_EQUAL_INT(20, socFromVoltage(3.70f));
  TEST_ASSERT_EQUAL_INT(30, socFromVoltage(3.75f));
  TEST_ASSERT_EQUAL_INT(55, socFromVoltage(3.85f));
  TEST_ASSERT_EQUAL_INT(82, socFromVoltage(4.00f));
  TEST_ASSERT_EQUAL_INT(38, socFromVoltage(3.775f));   // interpolated midpoint
}

void test_soc_monotonic() {
  int prev = -1;
  for (float v = 3.30f; v <= 4.20f + 1e-4f; v += 0.01f) {
    int s = socFromVoltage(v);
    TEST_ASSERT_TRUE_MESSAGE(s >= prev, "SOC curve must be non-decreasing");
    prev = s;
  }
}

// ---- BatteryEstimator ----

void test_steady_discharge_not_charging() {
  BatteryEstimator e;
  uint32_t t = feed(e, 3.90f, 1, 0);
  TEST_ASSERT_FALSE(e.charging);
  TEST_ASSERT_EQUAL_INT(65, e.soc);
  // Declining voltage must never be read as charging, and SOC must not rise.
  int last = e.soc;
  for (float v = 3.88f; v >= 3.70f; v -= 0.02f) {
    e.update(v, t); t += 5000;
    TEST_ASSERT_FALSE_MESSAGE(e.charging, "discharge misread as charging");
    TEST_ASSERT_TRUE_MESSAGE(e.soc <= last, "SOC rose while discharging");
    last = e.soc;
  }
}

void test_plugin_jump_freezes_soc() {
  BatteryEstimator e;
  // Settle near-empty: ~3.70 V => 20 %.
  uint32_t t = feed(e, 3.70f, 4, 0);
  TEST_ASSERT_FALSE(e.charging);
  TEST_ASSERT_EQUAL_INT(20, e.soc);

  // Charger plugged in: raw leaps +0.35 V in one tick.
  e.update(4.05f, t); t += 5000;
  TEST_ASSERT_TRUE(e.charging);
  TEST_ASSERT_EQUAL_INT('J', e.reason);
  // The whole point: SOC stays frozen at the pre-charge value, it does NOT
  // jump to socFromVoltage(4.05) ~= 88.
  TEST_ASSERT_EQUAL_INT(20, e.soc);

  // One more charging tick (no fresh trigger) is held by stickiness.
  e.update(4.05f, t); t += 5000;
  TEST_ASSERT_TRUE(e.charging);
  TEST_ASSERT_EQUAL_INT(20, e.soc);
}

void test_sticky_then_release() {
  BatteryEstimator e;
  uint32_t t = feed(e, 3.70f, 4, 0);

  // Trigger via a jump at time T.
  uint32_t T = t;
  e.update(3.80f, T);                 // +0.10 V step -> 'J'
  TEST_ASSERT_TRUE(e.charging);
  TEST_ASSERT_EQUAL_INT('J', e.reason);

  // 5 s later, no new trigger -> still charging, carried by stickiness.
  e.update(3.80f, T + 5000);
  TEST_ASSERT_TRUE(e.charging);
  TEST_ASSERT_EQUAL_INT('S', e.reason);

  // 35 s after the last trigger, stickiness has expired -> not charging.
  e.update(3.80f, T + 35000);
  TEST_ASSERT_FALSE(e.charging);
}

void test_near_full_is_charging() {
  BatteryEstimator e;
  e.update(4.19f, 0);                 // > 4.18 V plateau
  TEST_ASSERT_TRUE(e.charging);
  TEST_ASSERT_EQUAL_INT('F', e.reason);
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_soc_endpoints);
  RUN_TEST(test_soc_known_points);
  RUN_TEST(test_soc_monotonic);
  RUN_TEST(test_steady_discharge_not_charging);
  RUN_TEST(test_plugin_jump_freezes_soc);
  RUN_TEST(test_sticky_then_release);
  RUN_TEST(test_near_full_is_charging);
  return UNITY_END();
}
