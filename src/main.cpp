#include <Arduino.h>
#include "st7305_u8g2.h"
#include "ble_nus.h"
#include "protocol.h"
#include "state.h"
#include "buttons.h"
#include "ui.h"
#include "sensors.h"

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

  buttons::begin();
  sensors::begin();

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

  // Connection liveness: 30s without heartbeat = treat as dead screen.
  // (BLE may still be linked, but we hide stale stats.)
  if (g_state.last_heartbeat_ms && (millis() - g_state.last_heartbeat_ms) > 30000) {
    g_state.total = g_state.running = g_state.waiting = 0;
    g_state.msg = "stale";
    g_state.prompt.active = false;
  }

  // Buttons:
  //   KEY  short  = next view  (forward through 3 views)
  //   BOOT short  = prev view  (backward)
  //   long-press: reserved for future use
  // Note: hardware has a 3rd physical button (power) wired to the ETA6098
  // PMIC's OUTH/KEY pin, not to a GPIO — it cannot be read from firmware.
  buttons::Event ev = buttons::poll();
  if (ev == buttons::KEY_SHORT) {
    g_state.view = (g_state.view + 1) % 3;
    Serial.printf("[ui] view=%u (next)\n", (unsigned)g_state.view);
  } else if (ev == buttons::BOOT_SHORT) {
    g_state.view = (g_state.view + 2) % 3;
    Serial.printf("[ui] view=%u (prev)\n", (unsigned)g_state.view);
  }

  // Energy / mood ticker — once per second.
  {
    static uint32_t last_tick = 0;
    uint32_t now = millis();
    if (now - last_tick > 1000) {
      last_tick = now;
      bool active = (g_state.running > 0 || g_state.waiting > 0);
      if (active) {
        g_state.energy -= 0.10f;                    // ~12 min full → 0 under sustained load
        if (g_state.run_started_ms == 0) g_state.run_started_ms = now;
        g_state.last_activity_ms = now;
      } else if (ble_nus::connected()) {
        g_state.energy += 0.05f;                    // slow recovery while idle
        if (g_state.run_started_ms) {
          g_state.turns_done++;
          g_state.last_turn_ms = now;
          g_state.run_started_ms = 0;
          g_state.energy += 2.0f;                   // satisfaction bonus on completion
        }
      } else {
        g_state.energy += 0.20f;                    // deep recovery when offline
      }
      if (g_state.energy < 0)   g_state.energy = 0;
      if (g_state.energy > 100) g_state.energy = 100;
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
