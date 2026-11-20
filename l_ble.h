#pragma once
// Tab .ino tidak boleh diawali preprocessor directive (bug ##line Arduino IDE).
#include <Arduino.h>
// ===========================================================================
//  BLE UART (Nordic UART Service) — link cadangan tanpa WiFi
//  UUID sama dengan AppConfig di app Flutter:
//    service 6e400001-b5a3-f393-e0a9-e50e24dcca9e
//    RX (HP -> ABOT, write)      6e400002-...
//    TX (ABOT -> HP, notify)     6e400003-...
//  Protokol sama persis dengan Serial/MQTT: JSON per baris.
// ===========================================================================
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// Satu translation unit (.ino saja), jadi definisi static di header aman.
static BLEServer* bleServer = nullptr;
static BLECharacteristic* bleTxChar = nullptr;
static SemaphoreHandle_t bleMutex = nullptr;
static volatile bool bleClientConnected = false;
static String bleRxBuffer;

class BleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    bleClientConnected = true;
    Serial.println("[BLE] HP terhubung");
  }
  void onDisconnect(BLEServer* server) override {
    bleClientConnected = false;
    Serial.println("[BLE] HP terputus");
    // Bisa terpanggil saat bleShutdown() menghancurkan server: jangan sentuh.
    if (bleServer != nullptr && bleServer == server) server->startAdvertising();
  }
};

class BleRxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* ch) override {
    String chunk = ch->getValue();
    for (size_t i = 0; i < chunk.length(); i++) {
      char c = chunk[i];
      if (c == '\r') continue;
      if (c == '\n') {
        if (bleRxBuffer.length() > 0) {
          String line = bleRxBuffer;
          bleRxBuffer = "";
          line.trim();
          if (line.length() > 0) {
            // BLE bukan jalur tepercaya: SET/CFG hanya boleh kalau token belum
            // diatur (provisioning pertama) atau HP sudah AUTH.
            // shortcut: jendela provisioning pertama terbuka bagi siapa pun
            // dalam jangkauan; tambah konfirmasi tombol kalau perlu.
            const bool isCfg = line.startsWith("SET ") || line == "CFG?" || line == "CFG CLEAR";
            if (isCfg && cfgAuthToken.length() > 0 && !isAuthorized) {
              linkSend("{\"t\":\"nack\",\"why\":\"unauthorized\"}");
            } else {
              dispatchFromMqtt = false;  // BLE bukan MQTT: ota_start boleh
              linkDispatchLine(line);
            }
          }
        }
        continue;
      }
      if (bleRxBuffer.length() >= LINK_LINE_MAX) bleRxBuffer = "";
      bleRxBuffer += c;
    }
  }
};

// Dipanggil dari task mana pun (loop, OTA, callback BLE), jadi TX dikunci mutex.
// shortcut: notify langsung dari callback BLE; kalau HP sering putus saat
// perangkat sibuk, pindahkan pengiriman ke antrean yang di-flush di loop().
void bleSend(const String& json) {
  if (!bleClientConnected || bleTxChar == nullptr || bleMutex == nullptr) return;
  if (xSemaphoreTake(bleMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;

  const size_t kChunk = 20;  // MTU default 23; aman walau MTU belum dinegosiasi
  for (size_t off = 0; off < json.length(); off += kChunk) {
    size_t n = (kChunk < json.length() - off) ? kChunk : (json.length() - off);
    bleTxChar->setValue((uint8_t*)json.c_str() + off, n);
    bleTxChar->notify();
    if (json.length() > kChunk) delay(4);
  }
  bleTxChar->setValue((uint8_t*)"\n", 1);
  bleTxChar->notify();

  xSemaphoreGive(bleMutex);
}

void bleBegin() {
  if (bleMutex == nullptr) bleMutex = xSemaphoreCreateMutex();
  bleRxBuffer.reserve(512);

  BLEDevice::init("ABOT-01");
  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new BleServerCallbacks());

  BLEService* svc = bleServer->createService("6e400001-b5a3-f393-e0a9-e50e24dcca9e");

  BLECharacteristic* rx = svc->createCharacteristic(
      "6e400002-b5a3-f393-e0a9-e50e24dcca9e",
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new BleRxCallbacks());

  bleTxChar = svc->createCharacteristic(
      "6e400003-b5a3-f393-e0a9-e50e24dcca9e",
      BLECharacteristic::PROPERTY_NOTIFY);
  bleTxChar->addDescriptor(new BLE2902());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(svc->getUUID());
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();

  Serial.printf("[BLE] ready, heap=%u\n", (unsigned)ESP.getFreeHeap());
}

// Lepas stack BLE (Bluedroid makan ~50-60 KB RAM internal, cukup bikin TLS gagal).
// Panggil hanya dari loop(), jangan dari callback BLE.
void bleShutdown() {
  if (bleServer == nullptr) return;
  bleClientConnected = false;
  bleTxChar = nullptr;
  bleServer = nullptr;
  BLEDevice::deinit(true);
  Serial.printf("[BLE] off, heap=%u\n", (unsigned)ESP.getFreeHeap());
}

// BLE = link cadangan saat WiFi mati. Begitu WiFi hidup, BLE dilepas supaya
// MQTT/OTA punya RAM untuk TLS. Sesi yang sedang terhubung tidak dicabut.
void bleUpdate() {
  if (bleShutdownRequested) {
    bleShutdownRequested = false;
    bleShutdown();
  }
  if (bleClientConnected) return;
  if (!wifiConnected && bleServer == nullptr) {
    bleBegin();
  } else if (wifiConnected && bleServer != nullptr) {
    bleShutdown();
  }
}
