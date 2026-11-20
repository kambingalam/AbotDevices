#pragma once

// ---------------------------------------------------------------------------
// Build flags
// ---------------------------------------------------------------------------
#define FW_VERSION "3.52.4"

#define OTA_SIMULATION_MODE 0

// ---------------------------------------------------------------------------
// Shared state mutex
// ---------------------------------------------------------------------------
SemaphoreHandle_t stateMutex = nullptr;
SemaphoreHandle_t mqttMutex = nullptr;
// true selama OTA: MQTT TLS dilepas supaya heap cukup untuk TLS unduhan.
volatile bool otaHoldMqtt = false;

struct StateLock {
  StateLock() {
    if (stateMutex) xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY);
  }
  ~StateLock() {
    if (stateMutex) xSemaphoreGiveRecursive(stateMutex);
  }
};

// Hasil jawaban LLM (harus di atas: Arduino menaruh prototipe fungsi di sini)
struct VoiceAct {
  String say, show, title, card, city, mood, volume, verdict, music, query, genre;
  bool customWeather = false;
  bool more = false;
};

// ---------------------------------------------------------------------------
// Forward decls
// ---------------------------------------------------------------------------
void linkSend(const String& json);
extern TaskHandle_t voiceTaskHandle;

// ---------------------------------------------------------------------------
// Kredensial runtime (dari NVS, namespace "cfg")
// ---------------------------------------------------------------------------
String cfgGroqKey = "";
String cfgWifiSsid = "";
String cfgWifiPass = "";
String cfgAuthToken = "";
String cfgTopic = "";  // basis topik MQTT, kosong = default
volatile bool cfgWifiChanged = false;

void cfgLoad() {
  Preferences p;
  p.begin("cfg", true);
  cfgGroqKey   = p.getString("groq", "");
  cfgWifiSsid  = p.getString("ssid", "");
  cfgWifiPass  = p.getString("pass", "");
  cfgAuthToken = p.getString("auth", "");
  cfgTopic     = p.getString("topic", "");
  p.end();
}

// Perintah serial (TIDAK diterima dari MQTT):
//   SET groq=gsk_xxx | SET ssid=NamaWiFi | SET pass=PasswordWiFi | SET auth=token
//   CFG?  -> tampilkan status (nilai rahasia disamarkan)
//   CFG CLEAR -> hapus semua kredensial
// Nilai rahasia sengaja tidak pernah dicetak ulang ke Serial/MQTT.
void cfgHandleLine(String line);

// ---------------------------------------------------------------------------
// MultiPartStream
// ---------------------------------------------------------------------------
class MultiPartStream : public Stream {
public:
  MultiPartStream(const String& head, const uint8_t* body, size_t bodyLen,
                  const String& tail)
    : head_(head), body_(body), bodyLen_(bodyLen), tail_(tail),
      headPos_(0), bodyPos_(0), tailPos_(0) {
    totalLen_ = head_.length() + bodyLen_ + tail_.length();
  }
  size_t totalLength() const {
    return totalLen_;
  }
  int available() override {
    return (int)(totalLen_ - position());
  }
  int read() override {
    if (headPos_ < head_.length()) return (uint8_t)head_[headPos_++];
    if (bodyPos_ < bodyLen_) return (uint8_t)body_[bodyPos_++];
    if (tailPos_ < tail_.length()) return (uint8_t)tail_[tailPos_++];
    return -1;
  }
  int peek() override {
    if (headPos_ < head_.length()) return (uint8_t)head_[headPos_];
    if (bodyPos_ < bodyLen_) return (uint8_t)body_[bodyPos_];
    if (tailPos_ < tail_.length()) return (uint8_t)tail_[tailPos_];
    return -1;
  }
  void flush() override {}
  size_t write(uint8_t) override {
    return 0;
  }

private:
  size_t position() const {
    return headPos_ + bodyPos_ + tailPos_;
  }
  String head_;
  const uint8_t* body_;
  size_t bodyLen_;
  String tail_;
  size_t totalLen_;
  size_t headPos_;
  size_t bodyPos_;
  size_t tailPos_;
};

// ===========================================================================
//  KONFIGURASI HARDWARE
// ===========================================================================
constexpr int SCREEN_WIDTH = 128;
constexpr int SCREEN_HEIGHT = 64;
constexpr int OLED_SDA = 8;
constexpr int OLED_SCL = 9;
constexpr uint32_t OLED_I2C_CLOCK_HZ = 400000;
constexpr int BUTTON_TALK = 10;
constexpr int BUTTON_ACTION = 11;
constexpr unsigned long BUTTON_DEBOUNCE_MS = 35;
constexpr unsigned long BUTTON_HOLD_MS = 450;

constexpr uint8_t COLOR_BLACK = 0;
constexpr uint8_t COLOR_WHITE = 1;

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(
  U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

