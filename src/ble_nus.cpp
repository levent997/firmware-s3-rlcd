#include "ble_nus.h"
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
    Serial.println("[ble] disconnected, restarting advertising");
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, ble_gap_conn_desc *) override {
    Serial.printf("[ble] mtu=%u\n", (unsigned)mtu);
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

  g_server = NimBLEDevice::createServer();
  g_server->setCallbacks(new ServerCB());

  NimBLEService *svc = g_server->createService(SVC_UUID);
  g_tx = svc->createCharacteristic(TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  g_tx->setCallbacks(new TxCB());
  NimBLECharacteristic *rx = svc->createCharacteristic(
      RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
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
