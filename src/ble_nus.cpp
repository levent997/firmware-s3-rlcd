#include "ble_nus.h"
#include "state.h"
#include <NimBLEDevice.h>

namespace {
constexpr const char *SVC_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr const char *RX_UUID  = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"; // desktop -> device, write
constexpr const char *TX_UUID  = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"; // device -> desktop, notify

NimBLECharacteristic *g_tx = nullptr;
NimBLEServer *g_server = nullptr;
ble_nus::RxLineHandler g_on_line = nullptr;
bool g_connected = false;
String g_rx_accum;

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *) override {
    g_connected = true;
    Serial.println("[ble] connected");
  }
  void onDisconnect(NimBLEServer *s) override {
    g_connected = false;
    g_state.secure = false;
    g_state.passkey_displaying = false;
    g_state.passkey = 0;
    Serial.println("[ble] disconnected, restarting advertising");
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, ble_gap_conn_desc *) override {
    Serial.printf("[ble] mtu=%u\n", (unsigned)mtu);
  }

  // Pairing: we declare DisplayOnly IO capability so the peer prompts
  // the user for the passkey we generate.
  uint32_t onPassKeyRequest() override {
    // Random 6-digit passkey: BLE spec says 000000-999999.
    uint32_t p = esp_random() % 1000000UL;
    g_state.passkey = p;
    g_state.passkey_displaying = true;
    Serial.printf("[ble] passkey requested: %06lu (display on screen)\n",
                  (unsigned long)p);
    return p;
  }

  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    g_state.passkey_displaying = false;
    g_state.passkey = 0;
    if (desc->sec_state.encrypted) {
      g_state.secure = true;
      Serial.println("[ble] auth OK, link encrypted");
    } else {
      g_state.secure = false;
      Serial.println("[ble] auth FAILED");
    }
  }
};

class TxCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *c, ble_gap_conn_desc *, uint16_t subValue) override {
    Serial.printf("[ble] tx subscribe state=%u (1=notify,2=indicate,0=off)\n", (unsigned)subValue);
  }
};

class RxCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *c) override {
    std::string v = c->getValue();
    if (v.empty()) return;
    g_rx_accum.concat(v.c_str(), v.size());
    int nl;
    while ((nl = g_rx_accum.indexOf('\n')) >= 0) {
      String line = g_rx_accum.substring(0, nl);
      g_rx_accum.remove(0, nl + 1);
      line.trim();
      if (line.length() && g_on_line) g_on_line(line);
    }
    if (g_rx_accum.length() > 4096) g_rx_accum = ""; // overflow guard
  }
};
} // namespace

void ble_nus::begin(const String &name, RxLineHandler on_line) {
  g_on_line = on_line;

  NimBLEDevice::init(name.c_str());
  NimBLEDevice::setMTU(247);

  // Security: LE Secure Connections + MITM protection + bond. DisplayOnly
  // capability — we show the passkey on the RLCD; the desktop prompts the
  // user to type it in.
  NimBLEDevice::setSecurityAuth(/*bonding*/ true,
                                /*mitm*/    true,
                                /*sc*/      true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);

  g_server = NimBLEDevice::createServer();
  g_server->setCallbacks(new ServerCB());

  NimBLEService *svc = g_server->createService(SVC_UUID);

  // Encrypted-only access for both the TX notify and the RX write
  // characteristics. The CCCD inherits the same permission via
  // NimBLE's default policy when the characteristic is marked
  // _ENC. This forces the central to bond before doing anything.
  g_tx = svc->createCharacteristic(
      TX_UUID,
      NIMBLE_PROPERTY::NOTIFY |
      NIMBLE_PROPERTY::READ_ENC);
  g_tx->setCallbacks(new TxCB());
  NimBLECharacteristic *rx = svc->createCharacteristic(
      RX_UUID,
      NIMBLE_PROPERTY::WRITE     |
      NIMBLE_PROPERTY::WRITE_NR  |
      NIMBLE_PROPERTY::WRITE_ENC);
  rx->setCallbacks(new RxCB());
  svc->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(SVC_UUID);
  adv->setName(name.c_str());
  adv->setScanResponse(true);
  adv->start();
}

void ble_nus::loop() {}

bool ble_nus::connected() { return g_connected; }

void ble_nus::clearBonds() {
  int n = NimBLEDevice::getNumBonds();
  if (n > 0) {
    NimBLEDevice::deleteAllBonds();
    Serial.printf("[ble] cleared %d bond(s)\n", n);
  }
}

void ble_nus::sendLine(const String &line) {
  if (!g_connected || !g_tx) return;
  Serial.print("-> "); Serial.println(line);
  String out = line;
  if (!out.endsWith("\n")) out += '\n';
  size_t mtu = 20;
  if (g_server) {
    auto peers = g_server->getPeerDevices();
    if (!peers.empty()) {
      uint16_t m = g_server->getPeerMTU(peers[0]);
      if (m > 3) mtu = m - 3;
    }
  }
  const char *p = out.c_str();
  size_t remaining = out.length();
  while (remaining > 0) {
    size_t n = remaining > mtu ? mtu : remaining;
    g_tx->setValue((const uint8_t *)p, n);
    g_tx->notify();
    p += n; remaining -= n;
  }
}
