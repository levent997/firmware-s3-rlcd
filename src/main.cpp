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

BuddyState g_state;

static ST7305_U8g2 lcd(11, 12, 5, 40, 41);

static void onLine(const String &line) {
  Serial.print("<- "); Serial.println(line);
  protocol::handleLine(line);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nESP32-S3-RLCD Claude buddy starting");

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
  ui::render();
}

void loop() {
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

  // Heartbeat print so we can confirm the device is alive whenever monitor opens.
  static uint32_t last_hb = 0;
  if (millis() - last_hb > 5000) {
    last_hb = millis();
    Serial.printf("[hb] up=%lus connected=%d total=%d running=%d waiting=%d prompt=%d heap=%lu\n",
                  (unsigned long)(millis() / 1000),
                  ble_nus::connected() ? 1 : 0,
                  g_state.total, g_state.running, g_state.waiting,
                  g_state.prompt.active ? 1 : 0,
                  (unsigned long)ESP.getFreeHeap());
  }

  delay(20);
}
