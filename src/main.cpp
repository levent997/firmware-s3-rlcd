#include <Arduino.h>
#include "st7305_u8g2.h"
#include "ble_nus.h"
#include "protocol.h"
#include "state.h"
#include "buttons.h"
#include "ui.h"
#include "sensors.h"
#include "persist.h"
#include "demo.h"
#include "rtc.h"
#include "xfer.h"
#include "pack.h"
#include "audio.h"
#include "imu.h"
#include "menu.h"
#include "log.h"
#include "esp_pm.h"
#include "esp32s3/pm.h"   // Arduino-ESP32 2.0.x = IDF 4.4: chip-specific pm config type
#include "esp_sleep.h"
#include "esp_system.h"   // esp_restart
#include "driver/rtc_io.h"

BuddyState g_state;

static ST7305_U8g2 lcd(11, 12, 5, 40, 41);

// --- Power management knobs ---
// Enter deep sleep when BLE has been disconnected AND no button has been
// pressed for this long. The reflective LCD holds its last (sleeping)
// frame; a KEY press wakes via ext1 and the device reboots (~2 s) and
// reloads stats from NVS + time from the RTC chip. Set to 0 to disable
// (keep the device always-on as a desk clock even when disconnected).
// 0 = disabled. Was 30 min, but idle deep sleep is temporarily off while we
// confirm button behaviour — deep sleep can only be woken by KEY (ext1), so
// a slept device looks like "buttons don't work" (BOOT does nothing, KEY
// reboots). Reintroduce once verified, ideally with KEY+BOOT both as wake
// sources and a longer / connection-aware trigger.
constexpr uint32_t IDLE_DEEP_SLEEP_MS = 0;
constexpr int PIN_KEY_WAKE = 18;                            // KEY button, RTC-capable

static uint32_t g_last_button_ms = 0;

static void onLine(const String &line) {
  LOGD("<- %s\n", line.c_str());
  protocol::handleLine(line);
}

static void enterDeepSleep() {
  LOGI("[pm] deep sleep (idle %lu ms); KEY (GPIO%d) wakes\n",
       (unsigned long)IDLE_DEEP_SLEEP_MS, PIN_KEY_WAKE);
  ui::showSleeping();
  delay(50);
  Serial.flush();
  // KEY is active-LOW with a pull-up. Hold the pull-up in the RTC domain so
  // the pin idles HIGH and a press pulls it LOW to trigger ext1 wake.
  rtc_gpio_pullup_en((gpio_num_t)PIN_KEY_WAKE);
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_KEY_WAKE);
  // IDF 4.4 ext1 on S3 offers ALL_LOW / ANY_HIGH (no ANY_LOW). With a
  // single pin in the mask, ALL_LOW == "this pin is low" == button pressed.
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_KEY_WAKE, ESP_EXT1_WAKEUP_ALL_LOW);
  esp_deep_sleep_start();
}

// --- Software watchdog ---
// The Arduino loop() runs on core 1; this monitor task is pinned to core 0
// so it keeps running even if the loop task is wedged (deadlocked BLE
// callback, runaway decode, ...). loop() stamps g_loop_alive_ms every
// iteration; if that goes stale past WDT_TIMEOUT_MS we reboot. The timeout
// is deliberately generous so a legitimately slow GIF-pack decode (which
// blocks the loop for a few seconds) never trips it.
constexpr uint32_t WDT_TIMEOUT_MS = 30000;
static volatile uint32_t g_loop_alive_ms = 0;

static void watchdogTask(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    uint32_t alive = g_loop_alive_ms;
    if (alive == 0) continue;                  // loop() not running yet
    uint32_t age = millis() - alive;
    if (age > WDT_TIMEOUT_MS) {
      LOGE("[wdt] loop stalled %lu ms -> reboot\n", (unsigned long)age);
      delay(20);
      esp_restart();
    }
  }
}

// Loop-rate sampler: counts iterations between 1 s windows so the SYSTEM
// view can show the actual achieved loop frequency. Defined here so we
// can update it from inside loop() without polluting state.h with a
// purely-diagnostic counter that other modules never read.
namespace diag {
  volatile uint32_t loop_hz = 0;
}

void setup() {
  // Drop CPU from default 240 MHz to 80 MHz. The workload here is tiny
  // (BLE NUS + a 5 Hz UI refresh + occasional ADC/I2C reads) so the
  // higher clock just burns ~40-60 mA of idle current. 80 MHz is still
  // plenty for the worst-case path (GIF decode in pack::tick) — that
  // path is already gated on a new pack arriving and runs once per push.
  setCpuFrequencyMhz(80);

  Serial.begin(115200);
  delay(200);
  Serial.printf("\nESP32-S3-RLCD Claude buddy starting (cpu=%u MHz)\n",
                (unsigned)getCpuFrequencyMhz());

  // Report deep-sleep wake so the logs explain the ~2 s reboot.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
    LOGI("[pm] woke from deep sleep (KEY press)\n");
  }

  // NOTE: automatic light sleep (esp_pm_configure light_sleep_enable=true)
  // was disabled — it can interfere with the polled-GPIO button input on
  // this build (buttons stopped switching views). We keep a plain fixed
  // 80 MHz (set above) which is the bulk of the idle-current saving anyway.
  // If reintroduced, gate buttons on a GPIO interrupt wake source first.
  {
    esp_pm_config_esp32s3_t pm = {};
    pm.max_freq_mhz = 80;
    pm.min_freq_mhz = 80;            // no DFS downscale
    pm.light_sleep_enable = false;   // <-- was true; disabled
    esp_err_t rc = esp_pm_configure(&pm);
    LOGI("[pm] esp_pm_configure -> %s (light-sleep OFF, fixed 80MHz)\n",
         esp_err_to_name(rc));
  }

  lcd.begin(0, U8G2_R1);
  ui::begin(lcd.getU8g2());

  // persist::load() up front so g_state.sound_on (and pet name, owner, ...)
  // are valid before any subsystem that consults them initialises.
  persist::load();
  buttons::begin();
  sensors::begin();
  rtc::begin();
  imu::begin();
  xfer::begin();
  pack::init();
  audio::begin();

  // If the RTC has valid time (i.e. either VBAT was preserved across resets
  // or the desktop has synced us before), seed g_state so the top-bar clock
  // is correct from t=0, before BLE has a chance to reconnect.
  //
  // We pretend (epoch = local_seconds, offset = 0, sync_ms = now). The
  // top-bar formula `local = epoch + (now - sync_ms) + offset` then yields
  // the correct local time. A subsequent BLE sync will overwrite with real
  // (utc_epoch, real_offset) and the formula keeps working.
  if (rtc::hasValidTime()) {
    uint32_t local_seconds = 0;
    if (rtc::readLocalEpoch(&local_seconds)) {
      g_state.time_epoch = local_seconds;
      g_state.time_offset_sec = 0;
      g_state.time_sync_ms = millis();
      Serial.printf("[rtc] seeded clock from chip (local epoch %lu)\n",
                    (unsigned long)local_seconds);
    }
  }

  // Advertise with a Claude- prefix so the desktop picker filters to us.
  uint64_t mac = ESP.getEfuseMac();
  char name[24];
  snprintf(name, sizeof(name), "Claude-%02X%02X",
           (unsigned)((mac >> 8) & 0xFF), (unsigned)(mac & 0xFF));
  g_state.name = name;
  ble_nus::begin(name, onLine);

  Serial.print("Advertising as "); Serial.println(name);

  // Software watchdog on core 0 (loop runs on core 1).
  g_loop_alive_ms = millis();
  xTaskCreatePinnedToCore(watchdogTask, "wdt", 2048, nullptr, 5, nullptr, 0);

  ui::render();
}

void loop() {
  g_loop_alive_ms = millis();   // feed the software watchdog
  ble_nus::loop();
  sensors::loop();
  imu::loop();
  demo::tick();
  pack::tick();
  menu::tick();

  // Connection liveness: 30s without heartbeat = treat as dead screen.
  // (BLE may still be linked, but we hide stale stats.)
  if (g_state.last_heartbeat_ms && (millis() - g_state.last_heartbeat_ms) > 30000) {
    g_state.total = g_state.running = g_state.waiting = 0;
    g_state.msg = "stale";
    g_state.prompt.active = false;
  }

  // Buttons:
  //   History overlay open (any view):
  //     any short or long press = close overlay (returns to previous view)
  //   With an active permission prompt:
  //     KEY  short = approve   BOOT short = deny
  //     long-press of either   = open history overlay (escape from approval)
  //   On MAIN view, no prompt:
  //     KEY/BOOT long = open history overlay (8-row transcript)
  //   Otherwise (any view, no prompt, no history):
  //     KEY  short = next view    BOOT short = prev view
  //     KEY  long  = next view    BOOT long  = prev view  (same as short)
  //
  // Note: hardware has a 3rd physical button (power) wired to the ETA6098
  // PMIC's OUTH/KEY pin, not a GPIO — it cannot be read from firmware.
  buttons::Event ev = buttons::poll();
  if (ev != buttons::NONE) {
    g_last_button_ms = millis();   // any interaction defers idle deep sleep
    bool active_prompt = g_state.prompt.active && g_state.prompt.id.length();
    if (menu::isOpen()) {
      // Menu owns all button input while open.
      switch (ev) {
        case buttons::KEY_SHORT:  menu::onKeyShort();  break;
        case buttons::BOOT_SHORT: menu::onBootShort(); break;
        case buttons::KEY_LONG:   menu::onKeyLong();   break;
        case buttons::BOOT_LONG:  menu::onBootLong();  break;
        default: break;
      }
    } else if (g_state.history_open) {
      // Any press closes the transcript history overlay.
      g_state.history_open = false;
      Serial.println("[ui] history closed");
    } else if (active_prompt && ev == buttons::KEY_SHORT) {
      Serial.printf("[approval] %s APPROVE\n", g_state.prompt.id.c_str());
      protocol::sendPermission(g_state.prompt.id, true);
    } else if (active_prompt && ev == buttons::BOOT_SHORT) {
      Serial.printf("[approval] %s DENY\n", g_state.prompt.id.c_str());
      protocol::sendPermission(g_state.prompt.id, false);
    } else if (ev == buttons::KEY_LONG || ev == buttons::BOOT_LONG) {
      // Long-press semantics depend on what the user is looking at:
      //   * active prompt or MAIN view  -> open transcript history overlay
      //   * USAGE view                   -> open settings menu
      //   * SYSTEM view                  -> toggle demo mode (and jump to MAIN
      //                                     so the user sees the showcase)
      //   * CLOCK view                   -> same as short (cycle direction)
      if (active_prompt || g_state.view == 0) {
        g_state.history_open = true;
        Serial.println("[ui] history opened");
      } else if (g_state.view == 1) {
        menu::open();
      } else if (g_state.view == 2) {
        demo::toggle();
        if (g_state.demo_mode) g_state.view = 0;   // jump to MAIN to see it
      } else if (ev == buttons::KEY_LONG) {
        g_state.view = (g_state.view + 1) % 4;
        Serial.printf("[ui] view=%u (next)\n", (unsigned)g_state.view);
      } else {
        g_state.view = (g_state.view + 3) % 4;
        Serial.printf("[ui] view=%u (prev)\n", (unsigned)g_state.view);
      }
    } else if (ev == buttons::KEY_SHORT) {
      g_state.view = (g_state.view + 1) % 4;
      Serial.printf("[ui] view=%u (next)\n", (unsigned)g_state.view);
    } else if (ev == buttons::BOOT_SHORT) {
      g_state.view = (g_state.view + 3) % 4;
      Serial.printf("[ui] view=%u (prev)\n", (unsigned)g_state.view);
    }
  }

  // Tamagotchi stats — aligned with the M5StickC reference firmware
  // (src/stats.h in the parent project).
  //
  //   energy_tier: 0..5, drains 1 tier per 2 h awake, refills to 5 on nap end.
  //   level:       tokens / 50,000.
  //   fed:         derived from tokens, not stored here.
  //
  // Nap trigger on this board (no IMU): BLE has been disconnected for >5 min.
  {
    static uint32_t last_tick = 0;
    uint32_t now = millis();
    if (now - last_tick > 1000) {
      last_tick = now;

      bool active = (g_state.running > 0 || g_state.waiting > 0);
      if (active) {
        if (g_state.run_started_ms == 0) g_state.run_started_ms = now;
        g_state.last_activity_ms = now;
      } else if (g_state.run_started_ms) {
        // running just dropped to 0 → count a completed turn
        g_state.turns_done++;
        g_state.last_turn_ms = now;
        g_state.run_started_ms = 0;
      }

      // Nap detection: BLE disconnected for sustained period.
      if (!ble_nus::connected()) {
        if (g_state.nap_started_ms == 0) g_state.nap_started_ms = now;
        if (!g_state.napping && (now - g_state.nap_started_ms) > 5UL * 60 * 1000) {
          g_state.napping = true;
        }
      } else {
        if (g_state.napping) {
          // Nap ended — restore energy to full.
          g_state.energy_at_nap = 5;
          g_state.last_nap_end_ms = now;
          g_state.napping = false;
          persist::onNapEnd();
        }
        g_state.nap_started_ms = 0;
      }

      // Energy tier: drain 1 per 2 h since last nap end (or boot).
      uint32_t hours_since = (now - g_state.last_nap_end_ms) / 3600000UL;
      int e = (int)g_state.energy_at_nap - (int)(hours_since / 2);
      if (e < 0) e = 0;
      if (e > 5) e = 5;
      g_state.energy_tier = (uint8_t)e;

      // Level (derived; recompute every tick is fine, monotonic-ish).
      g_state.level = g_state.tokens_boot / TOKENS_PER_LEVEL;

      // Throttle-checked NVS save for tokens / level progress.
      persist::onTokensProgress();
    }
  }

  ui::render();

  // Loop-rate counter: count iters per 1 s window, publish to diag::loop_hz.
  {
    static uint32_t window_start = 0;
    static uint32_t iters = 0;
    iters++;
    uint32_t now = millis();
    if (now - window_start >= 1000) {
      diag::loop_hz = iters;
      iters = 0;
      window_start = now;
    }
  }

  // Heartbeat print so we can confirm the device is alive whenever monitor
  // opens. Dialled down to every 30 s, and LOGD so a release build
  // (-DLOG_LEVEL<3) compiles it out entirely — the periodic USB-CDC write
  // keeps the host stack (and our USB PHY) awake and drawing current.
  static uint32_t last_hb = 0;
  if (millis() - last_hb > 30000) {
    last_hb = millis();
    LOGD("[hb] up=%lus cpu=%u connected=%d total=%d running=%d waiting=%d prompt=%d heap=%lu bat=%.2fV(%d%%) chg=%c loopHz=%lu\n",
         (unsigned long)(millis() / 1000),
         (unsigned)getCpuFrequencyMhz(),
         ble_nus::connected() ? 1 : 0,
         g_state.total, g_state.running, g_state.waiting,
         g_state.prompt.active ? 1 : 0,
         (unsigned long)ESP.getFreeHeap(),
         g_state.battery_v, g_state.battery_pct,
         g_state.charging_reason,
         (unsigned long)diag::loop_hz);
  }

  // Idle deep sleep: disconnected AND untouched for IDLE_DEEP_SLEEP_MS.
  // nap_started_ms is the BLE-disconnect anchor maintained above; require
  // both it and the last button press to be old enough. Skipped entirely
  // when IDLE_DEEP_SLEEP_MS == 0 (always-on mode) or while a prompt is up.
  if (IDLE_DEEP_SLEEP_MS > 0 && !ble_nus::connected() && !g_state.prompt.active) {
    uint32_t now = millis();
    bool disc_long = g_state.nap_started_ms &&
                     (now - g_state.nap_started_ms) > IDLE_DEEP_SLEEP_MS;
    bool idle_long = (now - g_last_button_ms) > IDLE_DEEP_SLEEP_MS;
    if (disc_long && idle_long) enterDeepSleep();
  }

  delay(20);
}
