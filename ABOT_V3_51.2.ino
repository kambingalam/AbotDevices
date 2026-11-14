/*
  ============================================================================
   OJOL ASSISTEN (ABOT) V3.51.0 — VOICE AI CHATBOT + LAYAR DINAMIS + MUSIK
  ============================================================================
   Fungsi    : Display navigasi + AI companion + voice chat + Music Player
   Target    : ESP32-S3 + OLED SSD1306 128x64 (I2C)
   Display   : U8g2 full buffer (HW I2C) + FluxGarage RoboEyes
   Link      : JSON lines via Serial / MQTT (dari HP driver)
   Audio     : Mic INMP441 (i2s_std, port 0) + Speaker MAX98357A (audioI2S, port 1)
   Voice AI  : Groq Whisper (STT) + Groq GPT-OSS (LLM) + Google Translate TTS
   Wajib     : secrets.h  ->  WIFI_SSID_STR, WIFI_PASS_STR, GROQ_API_KEY_STR, JAMENDO_CLIENT_ID_STR
   Library   : U8g2, FluxGarage RoboEyes, ArduinoJson (v7), PubSubClient,
               ESP32-audioI2S

   V3.51.0 PERUBAHAN:
     • OTA BENERAN (bukan simulasi): download firmware dari URL, verify SHA256.
     • sfxPlay() dipasang di event-event penting (boot, klik, record, reply, error).
     • Perbaikan bug penumpukan pesan di mqttCallback.
     • Cek heap dilewati saat voice pipeline sibuk (cegah restart palsu).
     • Perketat deteksi perintah suara "jam".
     • Tambah perintah serial '$' untuk tes buzzer.
  ============================================================================
*/

// ---------------------------------------------------------------------------
// Auto-prototype workaround untuk Arduino IDE
// ---------------------------------------------------------------------------
enum CompanionType : unsigned char;
enum AbotPersonality : unsigned char;
enum EyeMood : unsigned char;
enum NetworkJob : unsigned char;
enum SystemState : unsigned char;
enum CharacterEmotion : unsigned char;
enum PersonalityMode : unsigned char;
enum StartupStage : unsigned char;
enum ListeningState : unsigned char;
enum CompanionPlaybackState : unsigned char;
enum WakeIntroStage : unsigned char;
enum DestinationStage : unsigned char;

// ---------------------------------------------------------------------------
// Includes
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>
#include <U8g2lib.h>
#include <esp_task_wdt.h>
#include <Update.h>
#include <freertos/semphr.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <Audio.h>

#include <Preferences.h>
#include <esp_ota_ops.h>

#include "secrets.h"
#include <FluxGarage_RoboEyes.h>
#include "bitmaps.h"

// ---------------------------------------------------------------------------
// Build flags
// ---------------------------------------------------------------------------
#define FW_VERSION "3.51.2"

#define OTA_SIMULATION_MODE 0

// ---------------------------------------------------------------------------
// Shared state mutex
// ---------------------------------------------------------------------------
SemaphoreHandle_t stateMutex = nullptr;
SemaphoreHandle_t mqttMutex = nullptr;

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

// ===========================================================================
//  AUDIO HARDWARE
// ===========================================================================
constexpr int MIC_SCK = 5;
constexpr int MIC_WS = 4;
constexpr int MIC_SD = 6;
constexpr int SPK_BCLK = 15;
constexpr int SPK_LRC = 16;
constexpr int SPK_DIN = 17;

constexpr int AUDIO_SAMPLE_RATE = 16000;
constexpr i2s_port_t AUDIO_MIC_PORT = I2S_NUM_0;

constexpr int MIC_GAIN = 8;
constexpr int REC_SECONDS_PSRAM = 20;
constexpr int REC_SECONDS_HEAP = 2;

#ifndef GROQ_API_KEY_STR
#error "GROQ_API_KEY_STR wajib ada di secrets.h (ambil gratis di console.groq.com/keys)"
#endif
const char* GROQ_STT_URL = "https://api.groq.com/openai/v1/audio/transcriptions";
const char* GROQ_STT_MODEL = "whisper-large-v3-turbo";
const char* GROQ_CHAT_URL = "https://api.groq.com/openai/v1/chat/completions";
const char* GROQ_MODELS_URL = "https://api.groq.com/openai/v1/models";
const char* GROQ_CHAT_MODEL = "openai/gpt-oss-120b";
const char* GROQ_CHAT_MODEL_2 = "qwen/qwen3.8-27b";

const unsigned long STT_TIMEOUT_MS = 15000;
const unsigned long CHAT_TIMEOUT_MS = 10000;
const size_t STT_MIN_BYTES = 19200;

static int sttLastCode = 0;
static int lastLlmHttp = 0;

enum AudioCmdType : unsigned char { ACMD_REC_START = 1,
                                    ACMD_REC_STOP };

struct AudioCmd {
  uint8_t type;
  uint8_t arg;
};

QueueHandle_t audioQueue = nullptr;
volatile bool audioMicReady = false;
volatile int audioPending = 0;
volatile bool audioRecording = false;
volatile int audioMicBars = 0;

int volumeLevel = 3;
int volumeBeforeMute = 3;
constexpr int VOLUME_LEVEL_MAX = 5;

int16_t* recBuf = nullptr;
size_t recCapacity = 0;
size_t recLength = 0;
int recPeak = 0;
bool recAutoFinished = false;

i2s_chan_handle_t micRxHandle = nullptr;

Audio audioI2S(I2S_NUM_1);
volatile bool ttsActive = false;
volatile bool voicePipelineActive = false;
volatile bool ttsEof = false;
volatile bool ttsStop = false;
volatile bool speakAborted = false;

void audio_info(const char* info) {
  Serial.printf("[AUDIO] %s\n", info);
}
void audio_eof_speech(const char* info) {
  ttsEof = true;
}

// ===========================================================================
//  I2S DRIVER INIT (MIC)
// ===========================================================================
static bool audioMicDriverBegin() {
  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_MIC_PORT, I2S_ROLE_MASTER);
  chanCfg.dma_desc_num = 6;
  chanCfg.dma_frame_num = 256;
  chanCfg.auto_clear = true;

  if (i2s_new_channel(&chanCfg, NULL, &micRxHandle) != ESP_OK) return false;

  i2s_std_config_t stdCfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)MIC_SCK,
      .ws = (gpio_num_t)MIC_WS,
      .dout = I2S_GPIO_UNUSED,
      .din = (gpio_num_t)MIC_SD,
      .invert_flags = { false, false, false },
    },
  };
  stdCfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

  if (i2s_channel_init_std_mode(micRxHandle, &stdCfg) != ESP_OK) return false;
  if (i2s_channel_enable(micRxHandle) != ESP_OK) return false;
  return true;
}

// ===========================================================================
//  STT (Groq Whisper)
// ===========================================================================
static void writeWavHeader(uint8_t* out, size_t pcmBytes, int sampleRate) {
  const uint32_t chunkSize = 36 + pcmBytes;
  const uint32_t subchunk2 = pcmBytes;
  const uint16_t audioFormat = 1;
  const uint16_t numChannels = 1;
  const uint16_t bitsPerSamp = 16;
  const uint32_t byteRate = sampleRate * numChannels * bitsPerSamp / 8;
  const uint16_t blockAlign = numChannels * bitsPerSamp / 8;

  memcpy(out + 0, "RIFF", 4);
  memcpy(out + 4, &chunkSize, 4);
  memcpy(out + 8, "WAVE", 4);
  memcpy(out + 12, "fmt ", 4);
  const uint32_t fmtSize = 16;
  memcpy(out + 16, &fmtSize, 4);
  memcpy(out + 20, &audioFormat, 2);
  memcpy(out + 22, &numChannels, 2);
  memcpy(out + 24, &sampleRate, 4);
  memcpy(out + 28, &byteRate, 4);
  memcpy(out + 32, &blockAlign, 2);
  memcpy(out + 34, &bitsPerSamp, 2);
  memcpy(out + 36, "data", 4);
  memcpy(out + 40, &subchunk2, 4);
}

static String groqSttRequest(const uint8_t* wav, size_t wavLen,
                             const char* language) {
  const String boundary = "----AbotBoundary" + String(millis());

  String head = "--" + boundary + "\r\n";
  head += "Content-Disposition: form-data; name=\"file\"; filename=\"a.wav\"\r\n";
  head += "Content-Type: audio/wav\r\n\r\n";

  String tail = "";
  auto field = [&](const char* name, const String& val) {
    tail += "\r\n--" + boundary + "\r\n";
    tail += "Content-Disposition: form-data; name=\"" + String(name) + "\"\r\n\r\n";
    tail += val;
  };
  field("model", GROQ_STT_MODEL);
  field("response_format", "json");
  field("temperature", "0");
  if (language && strlen(language) > 0) field("language", language);
  tail += "\r\n--" + boundary + "--\r\n";

  MultiPartStream stream(head, wav, wavLen, tail);

  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(STT_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setTimeout(STT_TIMEOUT_MS);
  http.setReuse(false);

  if (!http.begin(client, GROQ_STT_URL)) {
    Serial.println("[STT] HTTP begin failed");
    sttLastCode = -1;
    return "";
  }

  String auth = "Bearer ";
  auth += GROQ_API_KEY_STR;
  http.addHeader("Authorization", auth);
  http.addHeader("Content-Type",
                 "multipart/form-data; boundary=" + boundary);

  Serial.printf("[STT] Upload %u bytes (lang=%s)...\n",
                (unsigned)wavLen,
                (language && strlen(language) > 0) ? language : "auto");
  const unsigned long t0 = millis();
  int code = http.sendRequest("POST", &stream, stream.totalLength());
  sttLastCode = code;
  const unsigned long elapsed = millis() - t0;
  Serial.printf("[STT] HTTP %d in %lu ms\n", code, elapsed);

  String out = "";

  if (code == 200) {
    String body = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (!err) {
      const char* text = doc["text"] | "";
      out = String(text);
      out.trim();
      Serial.printf("[STT] Transkrip: \"%s\"\n", out.c_str());
    } else {
      Serial.printf("[STT] JSON parse error: %s\n", err.c_str());
    }
  } else if (code == 429) {
    Serial.println("[STT] Rate limit Groq — coba lagi nanti");
  } else if (code > 0) {
    String body = http.getString();
    Serial.printf("[STT] HTTP %d body: %s\n", code, body.c_str());
  } else {
    Serial.printf("[STT] HTTP error %d: %s\n",
                  code, http.errorToString(code).c_str());
  }

  http.end();
  return out;
}

static String groqTranscribe(const int16_t* pcm, size_t samples) {
  if (samples == 0) return "";

  const size_t pcmBytes = samples * sizeof(int16_t);
  const size_t wavLen = 44 + pcmBytes;

  uint8_t* wav = (uint8_t*)heap_caps_malloc(
    wavLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!wav) wav = (uint8_t*)malloc(wavLen);
  if (!wav) {
    Serial.println("[STT] OOM: gagal alokasi WAV buffer");
    return "";
  }

  writeWavHeader(wav, pcmBytes, AUDIO_SAMPLE_RATE);
  memcpy(wav + 44, pcm, pcmBytes);

  String result = groqSttRequest(wav, wavLen, "id");

  if (result.length() == 0 && sttLastCode != 200) {
    Serial.println("[STT] Retry tanpa language...");
    vTaskDelay(pdMS_TO_TICKS(800));
    result = groqSttRequest(wav, wavLen, "");
  }

  free(wav);
  return result;
}

// ===========================================================================
//  TTS
// ===========================================================================
static int splitSpeech(const String& text, String* out, int maxOut) {
  int n = 0;
  String cur = "";
  for (size_t i = 0; i < text.length() && n < maxOut; i++) {
    char c = text[i];
    cur += c;
    bool endSent = (c == '.' || c == '!' || c == '?' || c == '\n');
    bool tooLong = (cur.length() >= 90 && c == ' ') || cur.length() >= 100;
    if ((endSent && cur.length() >= 25) || tooLong) {
      cur.trim();
      if (cur.length()) out[n++] = cur;
      cur = "";
    }
  }
  cur.trim();
  if (cur.length() && n < maxOut) out[n++] = cur;
  return n;
}

static bool speakChunk(const String& s) {
  ttsEof = false;
  if (!audioI2S.connecttospeech(s.c_str(), "id")) {
    Serial.println("[TTS] connecttospeech gagal");
    return false;
  }
  const unsigned long t0 = millis();
  bool started = false;
  while (millis() - t0 < 25000) {
    audioI2S.loop();
    if (audioI2S.isRunning()) started = true;
    if (ttsEof) break;
    if (ttsStop) {
      audioI2S.stopSong();
      return true;
    }
    if (!started && millis() - t0 > 6000) {
      Serial.println("[TTS] tidak mulai");
      return false;
    }
    if (started && !audioI2S.isRunning()) break;
    vTaskDelay(1);
  }
  return true;
}

static String currentTtsPart = "";

static bool speakText(const String& text) {
  if (volumeLevel == 0) {
    Serial.println("[TTS] mute, skip");
    return false;
  }
  if (text.length() == 0) return false;

  String parts[12];
  int n = splitSpeech(text, parts, 12);
  Serial.printf("[TTS] Speak %d bagian\n", n);

  ttsActive = true;
  ttsStop = false;
  speakAborted = false;
  bool ok = false;

  for (int i = 0; i < n && !ttsStop; i++) {
    {
      StateLock lock;
      currentTtsPart = parts[i];
    }
    if (speakChunk(parts[i])) ok = true;
  }

  {
    StateLock lock;
    currentTtsPart = "";
  }

  audioI2S.stopSong();
  speakAborted = ttsStop;
  ttsActive = false;
  ttsStop = false;
  return ok;
}

// ===========================================================================
//  AUDIO TASK (MIC ONLY)
// ===========================================================================
static inline void audioPendingDone() {
  if (audioPending > 0) __atomic_sub_fetch(&audioPending, 1, __ATOMIC_SEQ_CST);
}

static void audioReportRecordingStats(bool emptyResult) {
  audioMicBars = 0;
  if (emptyResult || recLength == 0) {
    Serial.println("[VOICE] rekaman kosong, tidak diproses");
    return;
  }
  const uint32_t dur = (uint32_t)((uint64_t)recLength * 1000ULL / AUDIO_SAMPLE_RATE);
  Serial.printf("[VOICE] rekaman selesai: %lu ms, peak %d\n",
                (unsigned long)dur, recPeak);
  if (voiceTaskHandle) {
    xTaskNotify(voiceTaskHandle, 0x01, eSetBits);
  } else {
    Serial.println("[VOICE] WARN: voiceTaskHandle NULL, STT dilewati");
    linkSend("{\"t\":\"stt_skip\",\"why\":\"voice_task_off\"}");
  }
}

static void audioTask(void* pv) {
  AudioCmd cmd;
  int32_t blk[256];
  float dc = 0.0f, level = 0.0f;
  bool dcInit = false;
  int discardBlocks = 0;

  for (;;) {
    if (!audioRecording) {
      if (xQueueReceive(audioQueue, &cmd, pdMS_TO_TICKS(40)) != pdTRUE) continue;

      if (cmd.type == ACMD_REC_START) {
        recAutoFinished = false;
        if (audioMicReady && recBuf) {
          recLength = 0;
          recPeak = 0;
          dcInit = false;
          level = 0.0f;
          discardBlocks = 4;
          audioRecording = true;
        }
      } else if (cmd.type == ACMD_REC_STOP) {
        if (recAutoFinished) recAutoFinished = false;
        else {
          recLength = 0;
          recPeak = 0;
          audioReportRecordingStats(true);
        }
      }
      audioPendingDone();
      continue;
    }

    size_t bytesRead = 0;
    if (micRxHandle) {
      i2s_channel_read(micRxHandle, blk, sizeof(blk), &bytesRead, pdMS_TO_TICKS(40));
    }
    int n = (int)(bytesRead / sizeof(int32_t));
    if (discardBlocks > 0) {
      discardBlocks--;
      n = 0;
    }

    int blockPeak = 0;
    for (int i = 0; i < n; i++) {
      float s = (float)(blk[i] >> 16);
      if (!dcInit) {
        dc = s;
        dcInit = true;
      }
      dc += (s - dc) / 512.0f;

      int32_t v = (int32_t)((s - dc) * MIC_GAIN);
      if (v > 32767) v = 32767;
      if (v < -32768) v = -32768;

      if (recLength < recCapacity) recBuf[recLength++] = (int16_t)v;
      int a = (v < 0) ? -v : v;
      if (a > recPeak) recPeak = a;
      if (a > blockPeak) blockPeak = a;
    }

    level = ((float)blockPeak > level * 0.80f) ? (float)blockPeak : level * 0.80f;
    audioMicBars = (level >= 3500) ? 4 : (level >= 1200) ? 3
                                       : (level >= 400)  ? 2
                                       : (level >= 120)  ? 1
                                                         : 0;

    bool stopNow = false;
    if (xQueueReceive(audioQueue, &cmd, 0) == pdTRUE) {
      if (cmd.type == ACMD_REC_STOP) stopNow = true;
      audioPendingDone();
    }

    if (!stopNow && recLength >= recCapacity) {
      stopNow = true;
      recAutoFinished = true;
    }

    if (stopNow) {
      audioRecording = false;
      audioReportRecordingStats(false);
    }
  }
}

bool audioIsBusy() {
  return audioPending > 0 || audioRecording;
}

static bool audioEnqueue(uint8_t type, uint8_t arg) {
  if (!audioQueue) return false;
  AudioCmd cmd = { type, arg };
  __atomic_add_fetch(&audioPending, 1, __ATOMIC_SEQ_CST);
  if (xQueueSend(audioQueue, &cmd, 0) != pdTRUE) {
    __atomic_sub_fetch(&audioPending, 1, __ATOMIC_SEQ_CST);
    return false;
  }
  return true;
}

void audioStartRecording() {
  if (!audioMicReady) return;
  audioMicBars = 0;
  audioEnqueue(ACMD_REC_START, 0);
}

void audioStopRecording() {
  if (!audioMicReady) return;
  if (!audioEnqueue(ACMD_REC_STOP, 0)) {
    Serial.println("[VOICE] WARN: gagal enqueue REC_STOP");
  }
}

void audioSetVolume(int level) {
  if (level < 0) level = 0;
  if (level > VOLUME_LEVEL_MAX) level = VOLUME_LEVEL_MAX;
  volumeLevel = level;
  if (level > 0) volumeBeforeMute = level;
  audioI2S.setVolume(min(level * 4, 21));
  Serial.printf("[AUDIO] Volume %d/%d%s\n", level, VOLUME_LEVEL_MAX,
                level == 0 ? " MUTE" : "");
}

void audioVolumeStep(int delta) {
  audioSetVolume(volumeLevel + delta);
}

void audioToggleMute() {
  if (volumeLevel > 0) {
    volumeBeforeMute = volumeLevel;
    audioSetVolume(0);
  } else {
    audioSetVolume(volumeBeforeMute > 0 ? volumeBeforeMute : 3);
  }
}

static void voiceTask(void* pv);

void audioBegin() {
  Serial.printf("[AUDIO] Pre-begin: heap=%u, psram=%u, psramFound=%d\n",
                (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getPsramSize(),
                psramFound());

  audioQueue = xQueueCreate(8, sizeof(AudioCmd));
  audioMicReady = audioMicDriverBegin();
  Serial.printf("[AUDIO] Mic driver: %d\n", audioMicReady);

  if (audioMicReady) {
    const bool hasPsram = psramFound();
    size_t seconds = hasPsram ? REC_SECONDS_PSRAM : REC_SECONDS_HEAP;
    size_t bytes = seconds * AUDIO_SAMPLE_RATE * sizeof(int16_t);
    Serial.printf("[AUDIO] Ingin alokasi %u bytes (%s)\n",
                  (unsigned)bytes, hasPsram ? "PSRAM" : "heap");

    if (hasPsram) {
      recBuf = (int16_t*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
      Serial.printf("[AUDIO] PSRAM alloc -> %p\n", recBuf);
    }
    if (!recBuf) {
      seconds = REC_SECONDS_HEAP;
      bytes = seconds * AUDIO_SAMPLE_RATE * sizeof(int16_t);
      recBuf = (int16_t*)malloc(bytes);
      Serial.printf("[AUDIO] Heap alloc 2s %u bytes -> %p\n",
                    (unsigned)bytes, recBuf);
    }
    if (!recBuf) {
      seconds = 1;
      bytes = AUDIO_SAMPLE_RATE * sizeof(int16_t);
      recBuf = (int16_t*)malloc(bytes);
      Serial.printf("[AUDIO] Heap alloc 1s %u bytes -> %p\n",
                    (unsigned)bytes, recBuf);
    }

    if (recBuf) {
      recCapacity = bytes / sizeof(int16_t);
      Serial.printf("[AUDIO] Buffer rekam %u detik OK\n", (unsigned)seconds);
    } else {
      audioMicReady = false;
      Serial.println("[AUDIO] FAIL: semua alokasi gagal, mic dimatikan");
    }
  }

  Serial.printf("[AUDIO] Mic %s | Volume awal %d/%d\n",
                audioMicReady ? "OK" : "GAGAL", volumeLevel, VOLUME_LEVEL_MAX);

  if (audioQueue && audioMicReady) {
    BaseType_t r = xTaskCreatePinnedToCore(audioTask, "AudioTask", 6144,
                                           NULL, 2, NULL, 0);
    if (r != pdPASS) Serial.println("[AUDIO] FAIL: audioTask spawn");
    else Serial.println("[AUDIO] audioTask OK");
  }

  audioI2S.setPinout(SPK_BCLK, SPK_LRC, SPK_DIN);
  audioI2S.setVolume(min(volumeLevel * 4, 21));
  Serial.println("[AUDIO] TTS (audioI2S) ready");
}

// ===========================================================================
//  TIPE DATA
// ===========================================================================
enum StartupStage : unsigned char { STARTUP_NORMAL,
                                    STARTUP_USAGE,
                                    STARTUP_CLOCK,
                                    STARTUP_WEATHER };
enum ListeningState : unsigned char { LISTENING_IDLE,
                                      LISTENING_ACTIVE };
enum SystemState : unsigned char { STATE_BOOT,
                                   STATE_WIFI_CONNECTING,
                                   STATE_READY,
                                   STATE_GETTING_ROUTE,
                                   STATE_THINKING,
                                   STATE_AI_RESPONSE,
                                   STATE_ERROR };
enum EyeMood : unsigned char { EYE_DEFAULT,
                               EYE_HAPPY,
                               EYE_ANGRY,
                               EYE_TIRED };
enum CharacterEmotion : unsigned char { CHARACTER_DEFAULT,
                                        CHARACTER_LISTENING,
                                        CHARACTER_THINKING,
                                        CHARACTER_HAPPY,
                                        CHARACTER_LAUGH,
                                        CHARACTER_CONFUSED,
                                        CHARACTER_ANGRY,
                                        CHARACTER_TIRED };
enum AbotPersonality : unsigned char { ABOT_NEUTRAL,
                                       ABOT_MORNING,
                                       ABOT_DAY,
                                       ABOT_AFTERNOON,
                                       ABOT_NIGHT };
enum PersonalityMode : unsigned char { PERSONALITY_NORMAL,
                                       PERSONALITY_PLAYFUL,
                                       PERSONALITY_EXCITED };
enum CompanionType : unsigned char { COMPANION_NONE,
                                     COMPANION_QUIZ,
                                     COMPANION_TRIVIA,
                                     COMPANION_HUMOR,
                                     COMPANION_STORY,
                                     COMPANION_CARD };
enum CompanionPlaybackState : unsigned char { COMPANION_IDLE,
                                              COMPANION_TYPING,
                                              COMPANION_HOLD };
enum WakeIntroStage : unsigned char { WAKE_DARK,
                                      WAKE_ABOT_HELLO,
                                      WAKE_ABOT_PERSONALITY,
                                      WAKE_FINISHED };
enum NetworkJob : unsigned char { NET_NONE,
                                  NET_OSRM,
                                  NET_NAVIGATION_AI,
                                  NET_COMPANION,
                                  NET_WEATHER,
                                  NET_GEOCODE };
enum DestinationStage : unsigned char { DEST_ASK,
                                        DEST_GEOCODING,
                                        DEST_CONFIRMED,
                                        DEST_FAILED };

struct RoutePoint {
  double lat;
  double lon;
};

struct NavigationStep {
  String instruction;
  String road;
  String type;
  String modifier;
  float distance;
  int routeIndex;
  float routeDistance;
  bool important;
};

struct WeatherData {
  bool valid = false;
  float temperature = 0.0f;
  float tempMax = 0.0f;
  float tempMin = 0.0f;
  int weatherCode = -1;
  String condition = "-";
  unsigned long updatedAt = 0;
};

// ===========================================================================
//  KONFIGURASI JARINGAN
// ===========================================================================
#ifndef WIFI_SSID_STR
#define WIFI_SSID_STR "Imam"
#define WIFI_PASS_STR "12345678"
#endif
const char* WIFI_SSID = WIFI_SSID_STR;
const char* WIFI_PASSWORD = WIFI_PASS_STR;

#define HOME_CITY "Medan"
#define HOME_LAT "3.5952"
#define HOME_LON "98.6722"
const char* WEATHER_URL =
  "https://api.open-meteo.com/v1/forecast"
  "?latitude=" HOME_LAT "&longitude=" HOME_LON
  "&current=temperature_2m,weather_code"
  "&daily=temperature_2m_max,temperature_2m_min"
  "&timezone=Asia%2FJakarta";

const char* MQTT_HOST = "broker.emqx.io";
const uint16_t MQTT_PORT = 1883;
const char* MQTT_TOPIC_COMMAND = "abot/wokwi/477375325542330369/v337/command";
const char* MQTT_TOPIC_STATUS = "abot/wokwi/477375325542330369/v337/status";
const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;
const size_t MQTT_PACKET_BUFFER_SIZE = 10000;

const char* AUTH_TOKEN = "abot-secret-2025";

// ===========================================================================
//  KONFIGURASI TIMING
// ===========================================================================
const unsigned long INITIAL_MAP_DURATION = 8000;
const unsigned long USER_MAP_DURATION = 8000;
const unsigned long ROUTE_MAP_DURATION = 5000;
const unsigned long HUD_SUPPRESS_DURATION = 2500;

const unsigned long COMPANION_HOLD_DURATION = 4000;
const unsigned long COMPANION_FINAL_HOLD = 6000;
const unsigned long TYPING_INTERVAL = 16;
const unsigned long COMPANION_NEXT_MENU_PAGE_MS = 2600;
const unsigned long COMPANION_NEXT_MENU_TIMEOUT = 20000;

const unsigned long STARTUP_USAGE_DURATION = 7500;
const unsigned long STARTUP_USAGE_LINK_MS = 3000;
const unsigned long STARTUP_CLOCK_DURATION = 4000;
const unsigned long STARTUP_WEATHER_DURATION = 4500;
const unsigned long INFO_CLOCK_DURATION = 3500;
const unsigned long INFO_WEATHER_DURATION = 4500;

const unsigned long SLEEP_IDLE_TIMEOUT = 20000;
const unsigned long ARRIVAL_DISPLAY_DURATION = 10000;
const unsigned long TRIP_INTRO_DURATION = 3300;
const unsigned long TRIP_INTRO_LINK_DURATION = 1200;
const unsigned long BOOT_TRANSITION_MS = 1400;

const unsigned long WAKE_DARK_MS = 250;
const unsigned long WAKE_ABOT_HELLO_MS = 1700;
const unsigned long WAKE_ABOT_PERSONALITY_MS = 1900;

const unsigned long DEST_ASK_TIMEOUT = 12000;
const unsigned long DEST_CONFIRM_DURATION = 2200;
const unsigned long DEST_FAILED_DURATION = 3000;

const unsigned long LLM_COOLDOWN = 2000;
const unsigned long WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;
const unsigned long WEATHER_RETRY_MS = 60UL * 1000UL;
const unsigned long OSRM_RETRY_MS = 10000;
const int OSRM_MAX_RETRIES = 3;
const unsigned long WIFI_CHECK_INTERVAL_MS = 2000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 5000;
const unsigned long HEAP_CHECK_INTERVAL_MS = 10000;
const uint32_t HEAP_CRITICAL_BYTES = 15000;

const unsigned long CHARACTER_REACTION_HAPPY_MS = 1200;
const unsigned long CHARACTER_REACTION_CONFUSED_MS = 1400;
const unsigned long CHARACTER_REACTION_ANGRY_MS = 1200;
const unsigned long CHARACTER_REACTION_LAUGH_MS = 1700;

const unsigned long NATURAL_EYE_FIRST_ACTION_MS = 4200;
const unsigned long NATURAL_EYE_ACTION_INTERVAL_MS = 5200;
const unsigned long NATURAL_EYE_GAZE_DURATION_MS = 850;
const unsigned long NATURAL_EYE_WINK_INTERVAL_MS = 18000;

// ===========================================================================
//  KONFIGURASI TUNING
// ===========================================================================
const float HUD_LEAD_SECONDS = 7.0f;
const float HUD_MIN_TRIGGER_M = 40.0f;
const float HUD_MAX_TRIGGER_M = 140.0f;
const unsigned long HUD_MAX_SHOW_MS = 7000;
const float HUD_PASSED_MARGIN_M = 3.0f;
const float HUD_PREEMPT_MAP_M = 60.0f;

const float GPS_SPEED_MPS = 12.0;
const float OFFROUTE_METERS = 60.0f;
const unsigned long OFFROUTE_REPORT_MS = 5000;
const unsigned long LINK_GPS_FRESH_MS = 3000;
const unsigned long LINK_STATUS_MS = 5000;
const unsigned long LINK_TIMEOUT_MS = 30000;
const unsigned long DEAD_RECKON_MAX_MS = 1500;
const unsigned long MIN_GPS_INTERVAL_MS = 100;
const unsigned long MIN_ROUTE_INTERVAL_MS = 1000;
const size_t LINK_LINE_MAX = 8192;
const bool SIM_WHEN_NO_GPS = false;
const bool NAV_AI_ON_ROUTE = false;

#define MAX_ROUTE_POINTS 120
#define MAX_NAV_STEPS 40
#define LOG_RING_SIZE 20
#define LOG_LINE_MAX 96

const int MAX_COMPANION_PAGES = 8;
const int MAX_COMPANION_LINES = 6;
const int MAX_LINE_CHARS = 20;

// ===========================================================================
//  STATE GLOBAL
// ===========================================================================
StartupStage startupStage = STARTUP_NORMAL;
unsigned long startupStageStarted = 0;
bool startupOnboardingActive = false;
bool startupOnboardingCompleted = false;
bool startupOnboardingManual = false;
bool infoModeActive = false;
unsigned long infoModeStarted = 0;
int infoPage = 0;

ListeningState listeningState = LISTENING_IDLE;
bool listeningActive = false;

bool talkLastReading = HIGH, talkStableState = HIGH, talkLongTriggered = false;
bool actionLastReading = HIGH, actionStableState = HIGH;
unsigned long talkLastChange = 0, talkPressedAt = 0;
unsigned long actionLastChange = 0, actionPressedAt = 0;

SystemState systemState = STATE_BOOT;
EyeMood currentEyeMood = EYE_DEFAULT;
CharacterEmotion currentCharacterEmotion = CHARACTER_DEFAULT;
unsigned long characterReactionUntil = 0;
bool characterReactionActive = false;

AbotPersonality currentAbotPersonality = ABOT_NEUTRAL;
AbotPersonality appliedAbotPersonality = ABOT_NEUTRAL;
unsigned long abotHoldUntil = 0;

bool naturalEyeBehaviorEnabled = true;
unsigned long naturalEyeNextActionAt = 0;
unsigned long naturalEyeReturnAt = 0;
unsigned long naturalEyeNextWinkAt = 0;
bool naturalEyeGazeActive = false;
int naturalEyeSequence = 0;

unsigned long thinkingGazeNextAt = 0;
int thinkingGazeStep = 0;
bool thinkingVisualPrepared = false;
unsigned long thinkingDemoUntil = 0;

PersonalityMode personalityMode = PERSONALITY_NORMAL;
CompanionType currentCompanion = COMPANION_NONE;
CompanionPlaybackState companionPlayback = COMPANION_IDLE;

bool quizWaitingAnswer = false;
String currentQuizQuestion = "";

bool companionNextMenuActive = false;
bool companionNextMenuPending = false;
unsigned long companionNextMenuAt = 0;
unsigned long companionNextMenuPageAt = 0;
unsigned long companionNextMenuOpenedAt = 0;
unsigned long companionNextMenuDelayMs = 350;
int companionNextMenuPage = 0;

String companionPages[MAX_COMPANION_PAGES][MAX_COMPANION_LINES];
int companionPageCount = 0;
int companionPage = 0;
int companionTypedChars = 0;
String previousCompanionText = "";
String cardTitle = "INFO";

bool conversationActive = false;
bool voiceTextActive = false;

bool mapRequested = false;
bool companionActive = false;
bool companionGenerating = false;
bool initialMapShown = false;
bool introductionFinished = false;
bool wifiConnected = false;

bool tripActive = false;
bool tripArrived = false;
bool arrivalActive = false;
bool tripMapPending = false;
unsigned long arrivalStartedAt = 0;
bool tripIntroActive = false;
unsigned long tripIntroStartedAt = 0;
bool tripIntroTriggerAsk = false;

bool sleepModeActive = false;
unsigned long lastUserActivityMs = 0;
unsigned long mapStartTime = 0;
unsigned long companionStateStart = 0;
unsigned long companionLastType = 0;
unsigned long hudSuppressUntil = 0;

WakeIntroStage wakeIntroStage = WAKE_DARK;
unsigned long wakeIntroStageStartedAt = 0;

bool bootTransitionActive = false;
unsigned long bootTransitionStartedAt = 0;

bool destinationAskActive = false;
DestinationStage destinationStage = DEST_ASK;
unsigned long destinationStageStarted = 0;
float destLat = 0.0f;
float destLon = 0.0f;
String destName = "";
String destAddress = "";
bool destinationResolved = false;
float originLat = -6.175400f;
float originLon = 106.827202f;

String activeOsrmUrl = "";
int osrmFailCount = 0;
unsigned long osrmGaveUpAt = 0;
unsigned long lastOsrmAttempt = 0;

unsigned long lastWiFiCheck = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastHeapCheck = 0;

RoutePoint routePoints[MAX_ROUTE_POINTS];
float routeCumulativeDistance[MAX_ROUTE_POINTS];
int routePointCount = 0;
NavigationStep navSteps[MAX_NAV_STEPS];
int navStepCount = 0;
int activeStep = 0;
float osrmTotalDistance = 0;
float osrmTotalDuration = 0;
float routeGeometryDistance = 0;
float simulatedRouteDistance = 0;
int simulatedRouteIndex = 0;
unsigned long gpsLastMs = 0;

bool routeReady = false;
bool routeRequested = false;
bool navigationAIStarted = false;
bool pendingNavigationAI = false;

bool hudActive = false;
int hudLockedStep = -1;
int hudCompletedStep = -1;
unsigned long hudShownAt = 0;

unsigned long lastLlmRequest = 0;
volatile bool networkBusy = false;
volatile bool networkDone = false;
volatile bool networkSuccess = false;
volatile int networkHttpCode = -1;
volatile NetworkJob networkJob = NET_NONE;
volatile uint32_t networkRequestId = 0;
String networkPrompt = "";
String networkResponse = "";
uint32_t activeCompanionRequestId = 0;
uint32_t companionRequestCounter = 0;
unsigned long networkJobStartedAt = 0;
bool networkJobWasBusy = false;

WeatherData weatherData;
WeatherData cityWeather;
String weatherViewLabel = "";
bool weatherViewCustom = false;
bool weatherFetching = false;
unsigned long lastWeatherAttempt = 0;

WiFiClient mqttNetworkClient;
PubSubClient mqttClient(mqttNetworkClient);
String mqttPendingDispatch = "";
unsigned long mqttLastReconnectAttempt = 0;
unsigned long mqttBackoffMs = MQTT_RECONNECT_INTERVAL_MS;

bool isAuthorized = false;
bool linkEnabled = true;
bool linkRouteReceived = false;
bool hpConnected = false;
unsigned long lastLinkMessageAt = 0;
unsigned long lastGpsMsgAt = 0;
unsigned long lastRouteMsgAt = 0;
unsigned long lastLinkGpsAt = 0;
unsigned long lastOffRouteReportAt = 0;
bool routeFromLink = false;
int routeStride = 1;
float linkDistScale = 1.0f;

bool mapForNewRoute = false;
float speedMps = 8.0f;
unsigned long speedLastAt = 0;
float speedLastDist = 0;
unsigned long drLastMs = 0;

bool otaActive = false;

Preferences otaPrefs;
bool pendingOtaVerification = false;
String otaTargetVersion = "";
unsigned long bootTimeMs = 0;

String otaReportedVersion = FW_VERSION;

TaskHandle_t displayTaskHandle = NULL;
TaskHandle_t voiceTaskHandle = NULL;

char logRing[LOG_RING_SIZE][LOG_LINE_MAX];
uint8_t logRingHead = 0;
uint8_t logRingCount = 0;

// ===========================================================================
//  LAYER GRAFIS
// ===========================================================================
void beginFrame() {
  u8g2.clearBuffer();
}
void presentFrame() {
  u8g2.sendBuffer();
}

void gfxSelectFont(uint8_t size) {
  u8g2.setFont(size >= 2 ? u8g2_font_10x20_tr : u8g2_font_6x10_tr);
  u8g2.setFontMode(1);
  u8g2.setFontPosTop();
}

int gfxTextWidth(const String& text, uint8_t size = 1) {
  gfxSelectFont(size);
  return u8g2.getStrWidth(text.c_str());
}

void gfxText(int x, int y, const String& text, uint8_t size = 1,
             uint8_t color = COLOR_WHITE) {
  gfxSelectFont(size);
  u8g2.setDrawColor(color);
  u8g2.drawStr(x, y, text.c_str());
}

void gfxPixel(int x, int y, uint8_t color = COLOR_WHITE) {
  if (x < 0 || y < 0 || x >= SCREEN_WIDTH || y >= SCREEN_HEIGHT) return;
  u8g2.setDrawColor(color);
  u8g2.drawPixel(x, y);
}

void gfxFillRect(int x, int y, int w, int h, uint8_t color = COLOR_WHITE) {
  if (w <= 0 || h <= 0) return;
  u8g2.setDrawColor(color);
  u8g2.drawBox(x, y, w, h);
}

void gfxFrame(int x, int y, int w, int h, uint8_t color = COLOR_WHITE) {
  if (w <= 0 || h <= 0) return;
  u8g2.setDrawColor(color);
  u8g2.drawFrame(x, y, w, h);
}

void gfxFillRoundRect(int x, int y, int w, int h, int r,
                      uint8_t color = COLOR_WHITE) {
  if (w <= 0 || h <= 0) return;
  int maxRadius = (min(w, h) / 2) - 1;
  if (r > maxRadius) r = maxRadius;
  u8g2.setDrawColor(color);
  if (r <= 0) u8g2.drawBox(x, y, w, h);
  else u8g2.drawRBox(x, y, w, h, r);
}

void gfxRoundFrame(int x, int y, int w, int h, int r,
                   uint8_t color = COLOR_WHITE) {
  if (w <= 0 || h <= 0) return;
  int maxRadius = (min(w, h) / 2) - 1;
  if (r > maxRadius) r = maxRadius;
  u8g2.setDrawColor(color);
  if (r <= 0) u8g2.drawFrame(x, y, w, h);
  else u8g2.drawRFrame(x, y, w, h, r);
}

void gfxFillTriangle(int x0, int y0, int x1, int y1, int x2, int y2,
                     uint8_t color = COLOR_WHITE) {
  u8g2.setDrawColor(color);
  u8g2.drawTriangle(x0, y0, x1, y1, x2, y2);
}

void gfxCircle(int x, int y, int r, uint8_t color = COLOR_WHITE) {
  u8g2.setDrawColor(color);
  u8g2.drawCircle(x, y, r, U8G2_DRAW_ALL);
}

void gfxFillCircle(int x, int y, int r, uint8_t color = COLOR_WHITE) {
  u8g2.setDrawColor(color);
  u8g2.drawDisc(x, y, r, U8G2_DRAW_ALL);
}

bool gfxClipLine(int& x0, int& y0, int& x1, int& y1) {
  constexpr int XMAX = SCREEN_WIDTH - 1;
  constexpr int YMAX = SCREEN_HEIGHT - 1;

  auto outcode = [](int x, int y) {
    int code = 0;
    if (x < 0) code |= 1;
    else if (x > XMAX) code |= 2;
    if (y < 0) code |= 4;
    else if (y > YMAX) code |= 8;
    return code;
  };

  int c0 = outcode(x0, y0);
  int c1 = outcode(x1, y1);

  while (true) {
    if ((c0 | c1) == 0) return true;
    if (c0 & c1) return false;

    bool first = (c0 != 0);
    int code = first ? c0 : c1;
    int64_t x = 0, y = 0;

    if (code & 8) {
      x = x0 + (int64_t)(x1 - x0) * (YMAX - y0) / (y1 - y0);
      y = YMAX;
    } else if (code & 4) {
      x = x0 + (int64_t)(x1 - x0) * (0 - y0) / (y1 - y0);
      y = 0;
    } else if (code & 2) {
      y = y0 + (int64_t)(y1 - y0) * (XMAX - x0) / (x1 - x0);
      x = XMAX;
    } else {
      y = y0 + (int64_t)(y1 - y0) * (0 - x0) / (x1 - x0);
      x = 0;
    }

    if (first) {
      x0 = (int)x;
      y0 = (int)y;
      c0 = outcode(x0, y0);
    } else {
      x1 = (int)x;
      y1 = (int)y;
      c1 = outcode(x1, y1);
    }
  }
}

void gfxLine(int x0, int y0, int x1, int y1, uint8_t color = COLOR_WHITE) {
  if (!gfxClipLine(x0, y0, x1, y1)) return;
  u8g2.setDrawColor(color);
  u8g2.drawLine(x0, y0, x1, y1);
}

void gfxBitmap(int x, int y, const uint8_t* bitmap, int w, int h,
               uint8_t color = COLOR_WHITE) {
  u8g2.setDrawColor(color);
  const int bytesPerRow = (w + 7) / 8;

  for (int row = 0; row < h; row++) {
    int runStart = -1;
    int col = 0;
    while (col < w) {
      const uint8_t packed = pgm_read_byte(&bitmap[row * bytesPerRow + (col >> 3)]);
      const int bitsLeft = min(8 - (col & 7), w - col);
      for (int bit = 0; bit < bitsLeft; bit++, col++) {
        const bool on = (packed & (0x80 >> (col & 7))) != 0;
        if (on && runStart < 0) runStart = col;
        else if (!on && runStart >= 0) {
          u8g2.drawHLine(x + runStart, y + row, col - runStart);
          runStart = -1;
        }
      }
    }
    if (runStart >= 0) u8g2.drawHLine(x + runStart, y + row, w - runStart);
  }
}

// ===========================================================================
//  ROBOEYES PROXY
// ===========================================================================
volatile bool overlayListening = false;
volatile bool overlayThinking = false;

class RoboEyesDisplayProxy {
public:
  explicit RoboEyesDisplayProxy(U8G2& target)
    : gfx_(&target) {}
  void clearDisplay() {
    gfx_->clearBuffer();
  }
  void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h,
                     int16_t r, uint16_t color) {
    gfxFillRoundRect(x, y, w, h, r, (uint8_t)color);
  }
  void fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                    int16_t x2, int16_t y2, uint16_t color) {
    gfxFillTriangle(x0, y0, x1, y1, x2, y2, (uint8_t)color);
  }
  void display() {
    if (overlayListening) drawListeningOverlay();
    else if (overlayThinking) drawThinkingOverlay();
    gfx_->sendBuffer();
  }

private:
  U8G2* gfx_;

  static void drawListeningOverlay() {
    gfxFillRect(0, 0, 26, 15, COLOR_BLACK);
    gfxRoundFrame(3, 2, 5, 8, 1);
    gfxLine(1, 7, 1, 9);
    gfxLine(9, 7, 9, 9);
    gfxLine(5, 10, 5, 12);
    gfxLine(3, 13, 7, 13);

    if (!audioMicReady) return;
    const int level = (int)audioMicBars;
    for (int i = 0; i < 4; i++) {
      if (i < level) {
        int h = 2 + i * 2;
        gfxLine(13 + i * 3, 13 - h, 13 + i * 3, 13);
      }
    }
  }

  static void drawThinkingOverlay() {
    const int phase = (millis() / 450UL) % 3;
    const char* dots = (phase == 0) ? "." : (phase == 1) ? ".."
                                                         : "...";
    gfxFillRect(0, 0, 20, 8, COLOR_BLACK);
    gfxText(2, 0, dots, 1, COLOR_WHITE);
  }
};

#ifdef N
#undef N
#endif
#ifdef NE
#undef NE
#endif
#ifdef E
#undef E
#endif
#ifdef SE
#undef SE
#endif
#ifdef S
#undef S
#endif
#ifdef SW
#undef SW
#endif
#ifdef W
#undef W
#endif
#ifdef NW
#undef NW
#endif

constexpr uint8_t OJOL_POS_N = 1;
constexpr uint8_t OJOL_POS_NE = 2;
constexpr uint8_t OJOL_POS_E = 3;
constexpr uint8_t OJOL_POS_W = 7;
constexpr uint8_t OJOL_POS_NW = 8;

RoboEyesDisplayProxy roboEyesDisplay(u8g2);
RoboEyes<RoboEyesDisplayProxy> roboEyes(roboEyesDisplay);

// ===========================================================================
//  HELPER UMUM
// ===========================================================================
void drawCenteredText(const String& text, int y, uint8_t textSize = 1) {
  int x = (SCREEN_WIDTH - gfxTextWidth(text, textSize)) / 2;
  if (x < 0) x = 0;
  gfxText(x, y, text, textSize);
}

String stripNonAscii(String text) {
  String result = "";
  result.reserve(text.length());
  for (size_t i = 0; i < text.length(); i++) {
    uint8_t c = (uint8_t)text[i];
    if (c == '\n' || c == '\r' || (c >= 32 && c <= 126)) result += (char)c;
  }
  return result;
}

String jsonEscape(String s) {
  s.replace("\\", "\\\\");
  s.replace("\"", "\\\"");
  s.replace("\n", " ");
  s.replace("\r", "");
  return s;
}

bool getLocalTimeSafe(struct tm* out) {
  time_t now = time(nullptr);
  if (now < 1700000000) return false;
  return localtime_r(&now, out) != nullptr;
}

bool networkIsBusy() {
  return networkBusy || networkDone;
}

String companionName(CompanionType type) {
  switch (type) {
    case COMPANION_QUIZ: return "QUIZ";
    case COMPANION_TRIVIA: return "TRIVIA";
    case COMPANION_HUMOR: return "HUMOR";
    case COMPANION_STORY: return "STORY";
    case COMPANION_CARD: return cardTitle;
    default: return "COMPANION";
  }
}

const char* HARI_NAMA[] = { "Minggu", "Senin", "Selasa", "Rabu", "Kamis", "Jumat", "Sabtu" };
const char* BULAN_NAMA[] = { "Jan", "Feb", "Mar", "Apr", "Mei", "Jun",
                             "Jul", "Agu", "Sep", "Okt", "Nov", "Des" };

void abotLog(const char* level, const char* fmt, ...) {
  char msg[LOG_LINE_MAX];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);

  Serial.printf("[%s] %s\n", level, msg);

  StateLock lock;
  snprintf(logRing[logRingHead], LOG_LINE_MAX, "%s|%s", level, msg);
  logRingHead = (logRingHead + 1) % LOG_RING_SIZE;
  if (logRingCount < LOG_RING_SIZE) logRingCount++;
}

void flushLogRing() {
  if (!mqttClient.connected()) return;
  char lines[LOG_RING_SIZE][LOG_LINE_MAX];
  int count = 0;
  {
    StateLock lock;
    count = logRingCount;
    if (count == 0) return;
    int start = (logRingHead - count + LOG_RING_SIZE) % LOG_RING_SIZE;
    for (int i = 0; i < count; i++) {
      memcpy(lines[i], logRing[(start + i) % LOG_RING_SIZE], LOG_LINE_MAX);
    }
    logRingCount = 0;
    logRingHead = 0;
  }
    for (int i = 0; i < count; i++) {
    String out = "{\"t\":\"log\",\"l\":\"" + jsonEscape(String(lines[i])) + "\"}";
    
    // Tambahkan palang pintu di sini
    if (mqttMutex != nullptr && xSemaphoreTake(mqttMutex, portMAX_DELAY) == pdTRUE) {
      mqttClient.publish(MQTT_TOPIC_STATUS, out.c_str());
      xSemaphoreGive(mqttMutex);
    }
    delay(10);
  }
}

// ===========================================================================
//  FORWARD DECLARATIONS
// ===========================================================================
void setEyeMood(EyeMood mood);
void setCharacterEmotion(CharacterEmotion emotion);
void triggerCharacterReaction(CharacterEmotion emotion);
void resetNaturalEyeBehavior();
void triggerNaturalWink();
void markUserActivity();
void wakeFromSleep();
void startMapDisplay();
void startInfoMode();
void startStartupOnboarding();
void finishStartupOnboarding();
void startIntroduction();
void startTripIntroduction();
void startNewTrip();
void startCompanion(CompanionType type);
void finishCompanionGeneration(const String& text, CompanionType type);
void finishCompanion();
void renderCompanion();
void renderCompanionNextMenu();
void openCompanionNextMenu(unsigned long delayMs);
void closeCompanionNextMenu();
void handleArrival();
void startNavigationAI();
void startOSRM();
void startWeatherFetch();
void beginThinkingVisual();
void endThinkingVisual();
void cancelThinking();
void networkTask(void* parameter);
bool isImportantStep(const NavigationStep& step);
int nextImportantStep();
void startDestinationAsk();
void finishDestinationAsk();
void renderDestinationAsk();
void startGeocode(const String& placeName);
String buildOsrmUrl(float fromLat, float fromLon, float toLat, float toLon);
int decodePolyline(const String& encoded, RoutePoint* out, int maxPoints);
void handleCommand(char cmd);
String abotPromptHint();

void linkBegin();
void linkUpdate();
void linkDispatchLine(const String& line);
void linkReportStatus();
void mqttBegin();
void mqttUpdate();
void mqttCallback(char* topic, byte* payload, unsigned int length);
float snapToRoute(double lat, double lon, int from, int to, float& along);
void skipStartupSequence();
void syncRouteProgress();
void updateSpeedEstimate(float newDist);
void deadReckonStep();
float hudTriggerDistance();
void onLinkRoute(JsonDocument& doc);
void onLinkGps(JsonDocument& doc);
void onLinkTripStart();
void onLinkTripEnd();
void onLinkReset();
void onLinkPing();
void startOTAUpdate(const String& url, const String& requestId = "",
                    const String& targetVersion = "",
                    const String& expectedSha256 = "");
void updateDisplay();

// ===========================================================================
//  INCLUDE MODUL EKSTERNAL
// ===========================================================================
#define MUSIC_USE_VU 0
#include "buzzer_sfx.h"
#include "music_player.h"

// ===========================================================================
//  AI ASISTEN
// ===========================================================================
String weatherCodeToText(int code);
void handleQuizAnswer(char cmd);

static String activeChatModel = GROQ_CHAT_MODEL;
static bool modelChecked = false;

static bool groqModelUsable(const String& id) {
  String s = id;
  s.toLowerCase();
  const char* bad[] = { "whisper", "orpheus", "guard", "tts", "safeguard",
                        "embed", "distil", "playai", "compound" };
  for (auto b : bad)
    if (s.indexOf(b) >= 0) return false;
  return true;
}

static void groqEnsureModel(bool force = false) {
  static unsigned long lastTry = 0;
  if (modelChecked && !force) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!force && lastTry != 0 && millis() - lastTry < 30000UL) return;
  lastTry = millis();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, GROQ_MODELS_URL)) return;
  String auth = "Bearer ";
  auth += GROQ_API_KEY_STR;
  http.addHeader("Authorization", auth);
  int code = http.GET();
  Serial.printf("[MODELS] HTTP %d\n", code);
  if (code != 200) {
    if (code > 0) Serial.printf("[MODELS] body: %s\n", http.getString().c_str());
    http.end();
    return;
  }

  JsonDocument filter;
  filter["data"][0]["id"] = true;
  JsonDocument doc;
  DeserializationError err =
    deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("[MODELS] JSON error: %s\n", err.c_str());
    return;
  }

  const String pref[5] = { String(GROQ_CHAT_MODEL), String(GROQ_CHAT_MODEL_2),
                           String("openai/gpt-oss-20b"), String("llama-3.3-70b-versatile"),
                           String("llama-3.1-8b-instant") };
  bool found[5] = { false, false, false, false, false };
  String firstOther = "";
  String all = "";
  for (JsonObject m : doc["data"].as<JsonArray>()) {
    String id = String((const char*)(m["id"] | ""));
    all += id;
    all += " ";
    for (int i = 0; i < 5; i++)
      if (id == pref[i]) found[i] = true;
    if (firstOther.length() == 0 && groqModelUsable(id)) firstOther = id;
  }
  Serial.printf("[MODELS] tersedia: %s\n", all.c_str());

  String pick = "";
  for (int i = 0; i < 5; i++)
    if (found[i]) {
      pick = pref[i];
      break;
    }
  if (pick.length() == 0) pick = firstOther;
  if (pick.length() > 0) {
    activeChatModel = pick;
    modelChecked = true;
    Serial.printf("[MODELS] dipakai: %s\n", activeChatModel.c_str());
  } else {
    Serial.println("[MODELS] tidak ada model chat yang bisa dipakai key ini");
  }
}

// ---------------------------------------------------------------------------
//  Memori percakapan + konteks perjalanan
// ---------------------------------------------------------------------------
constexpr int HIST_TURNS = 4;
static String histUser[HIST_TURNS], histBot[HIST_TURNS];
static int histCount = 0;

static void histAdd(const String& u, const String& b) {
  if (histCount == HIST_TURNS) {
    for (int i = 1; i < HIST_TURNS; i++) {
      histUser[i - 1] = histUser[i];
      histBot[i - 1] = histBot[i];
    }
    histCount--;
  }
  histUser[histCount] = u;
  histBot[histCount] = b;
  histCount++;
}

static String buildContext() {
  String c = "";
  StateLock lock;
  c += " Kota asal perangkat: ";
  c += HOME_CITY;
  c += ".";

  struct tm ti;
  if (getLocalTimeSafe(&ti)) {
    char b[48];
    snprintf(b, sizeof(b), " Sekarang jam %02d:%02d WIB.", ti.tm_hour, ti.tm_min);
    c += b;
  }
  if (weatherData.valid) {
    String w = weatherData.condition;
    w.toLowerCase();
    c += " Cuaca di ";
    c += HOME_CITY;
    c += " sekarang " + w + ", " + String(weatherData.temperature, 0) + " derajat";
    c += ", hari ini terendah " + String(weatherData.tempMin, 0) + " dan tertinggi " + String(weatherData.tempMax, 0) + " derajat.";
  }
  if (routeReady && tripActive) {
    float remain = routeGeometryDistance - simulatedRouteDistance;
    if (remain < 0) remain = 0;
    float v = (speedMps < 3.0f) ? 8.0f : speedMps;
    int minutes = (int)(remain / v / 60.0f + 0.5f);
    c += " Perjalanan aktif, sisa sekitar ";
    c += (remain >= 1000.0f) ? String(remain / 1000.0f, 1) + " kilometer"
                             : String((int)remain) + " meter";
    c += ", perkiraan " + String(minutes) + " menit lagi.";
    int s = nextImportantStep();
    if (s >= 0 && s < navStepCount) {
      float d = navSteps[s].routeDistance - simulatedRouteDistance;
      if (d < 0) d = 0;
      String ins = navSteps[s].instruction;
      ins.toLowerCase();
      c += " Belokan berikutnya: " + ins + " dalam " + String((int)d) + " meter.";
    }
  } else {
    c += " Saat ini tidak ada perjalanan aktif.";
  }
  c += abotPromptHint();
  return c;
}

static String buildSystemPrompt(const String& extra) {
  String p;
  p.reserve(2200);
  p += "Kamu Abot, asisten suara berupa robot bermata di helm driver ojol. "
       "Bicara bahasa Indonesia yang santai, hangat, dan ekspresif. "
       "Jawab SEMUA permintaan pengguna: pertanyaan umum, saran, hitungan, ngobrol, candaan, "
       "bahkan akting suara. Ikuti gaya yang diminta pengguna (misal terkejut, lucu, formal).\n"
       "Balas HANYA satu objek JSON tanpa teks lain, dengan field:\n"
       "say: kalimat yang dibacakan suara. Tanpa emoji dan markdown, enak diucapkan, "
       "1-3 kalimat (cerita boleh sampai 5 kalimat).\n"
       "show: tampilan layar OLED, salah satu: none, map (peta rute), clock (jam), "
       "weather (cuaca), quiz, trivia, humor, story, music (putar lagu), "
       "usage (cara pakai), intro (animasi perkenalan), card (teks bebas di layar).\n"
       "title: judul pendek huruf kapital maksimal 14 karakter, untuk card.\n"
       "card: teks di layar untuk show quiz, trivia, humor, story, atau card. "
       "ASCII saja tanpa emoji, maksimal 220 karakter. Boleh dikosongkan kalau sama dengan say.\n"
       "city: nama kota HANYA jika show=weather dan pengguna menyebut kota tertentu.\n"
       "mood: ekspresi mata sesuai nada jawaban: default, happy, laugh, confused, angry, tired.\n"
       "volume: none, up, down, mute, unmute.\n"
       "verdict: hanya saat ada kuis aktif dan pengguna menjawabnya: correct, wrong, atau unknown. Selain itu kosongkan.\n"
       "music: hanya saat show=music: play, pause, resume, next, atau stop. Kalau pengguna minta putar lagu, isi play.\n"
       "query: judul lagu atau nama artis kalau pengguna menyebutnya, selain itu kosongkan.\n"
       "genre: tag musik Jamendo huruf kecil bahasa Inggris (pop, rock, lofi, chillout, lounge, jazz, "
       "electronic, hiphop, classical, acoustic, ambient, relaxation, folk, reggae, blues, metal) "
       "kalau pengguna minta suasana atau genre, selain itu kosongkan. Katalog Jamendo hanya berisi musik "
       "bebas lisensi, bukan lagu populer. Kalau diminta lagu populer, tetap show=music dengan genre "
       "terdekat dan jelaskan singkat di say.\n"
       "more: true kalau kamu sedang menceritakan cerita panjang dan masih ada bagian berikutnya, selain itu false.\n"
       "Cerita: kalau pengguna tidak menyebut panjang, ceritakan utuh dalam satu balasan (more=false). "
       "Kalau pengguna minta cerita panjang atau menyebut durasi, ceritakan bertahap: tiap balasan satu "
       "bagian 4-5 kalimat, show=none, more=true sampai bagian penutup yang more=false. "
       "Satu bagian kira-kira 40 detik bicara, sesuaikan jumlah bagian dengan durasi yang diminta.\n"
       "Aturan: pakai show=card kalau pengguna minta sesuatu ditampilkan di layar "
       "(tulisan, daftar, angka, hasil hitungan, ringkasan, kata-kata) walau tidak ada di daftar. "
       "Kalau pengguna minta tampilan tertentu, pilih show yang sesuai (misal tampilkan peta -> map). "
       "Kalau tidak perlu tampilan pakai none. Untuk quiz, say berisi pertanyaan lalu ajak pengguna "
       "menjawab, jangan bocorkan jawabannya. Transkrip suara bisa salah dengar, tebak maksud yang "
       "paling masuk akal. Jangan mengarang data jam, cuaca, atau rute, pakai data di bawah. "
       "Kamu belum punya data GPS lokasi saat ini.\n"
       "Data saat ini:";
  p += buildContext();
  p += extra;
  return p;
}

static int groqPost(const String& model, bool jsonMode, JsonDocument& doc, String& out) {
  doc["model"] = model;
  const bool gptOss = model.startsWith("openai/gpt-oss");
  const bool thinky = gptOss || model.indexOf("qwen") >= 0;
  doc["max_tokens"] = thinky ? 900 : 380;
  if (jsonMode && gptOss) doc["reasoning_effort"] = "low";
  else doc.remove("reasoning_effort");
  if (jsonMode) doc["response_format"]["type"] = "json_object";
  else doc.remove("response_format");

  String payload;
  serializeJson(doc, payload);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(CHAT_TIMEOUT_MS);
  if (!http.begin(client, GROQ_CHAT_URL)) {
    lastLlmHttp = -1;
    return -1;
  }

  String auth = "Bearer ";
  auth += GROQ_API_KEY_STR;
  http.addHeader("Authorization", auth);
  http.addHeader("Content-Type", "application/json");

  Serial.printf("[LLM] Request model='%s' (len %d) json=%d\n",
                model.c_str(), (int)model.length(), jsonMode ? 1 : 0);
  const unsigned long t0 = millis();
  int code = http.POST(payload);
  lastLlmHttp = code;
  Serial.printf("[LLM] HTTP %d in %lu ms\n", code, millis() - t0);

  out = "";
  if (code == 200) {
    JsonDocument resp;
    if (!deserializeJson(resp, http.getString())) {
      out = String((const char*)(resp["choices"][0]["message"]["content"] | ""));
    }
  } else if (code > 0) {
    Serial.printf("[LLM] body: %s\n", http.getString().c_str());
  }
  http.end();
  return code;
}

static String lc(String s) {
  s.toLowerCase();
  return s;
}

static void cleanSpeech(String& s) {
  s.replace("*", "");
  s.replace("#", "");
  s.replace("`", "");
  s.trim();
}

static bool groqAsk(const String& userText, const String& extra, VoiceAct& act) {
  JsonDocument doc;
  doc["temperature"] = 0.7;
  JsonArray msgs = doc["messages"].to<JsonArray>();

  JsonObject sys = msgs.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = buildSystemPrompt(extra);

  for (int i = 0; i < histCount; i++) {
    JsonObject u = msgs.add<JsonObject>();
    u["role"] = "user";
    u["content"] = histUser[i];
    JsonObject a = msgs.add<JsonObject>();
    a["role"] = "assistant";
    a["content"] = histBot[i];
  }
  JsonObject usr = msgs.add<JsonObject>();
  usr["role"] = "user";
  usr["content"] = userText;

  String content = "";
  String model = activeChatModel;
  int code = groqPost(model, true, doc, content);
  if (code == 400) code = groqPost(model, false, doc, content);

  if ((code == 404 || code == 403) && content.length() == 0) {
    modelChecked = false;
    groqEnsureModel(true);
    if (activeChatModel != model) {
      model = activeChatModel;
      code = groqPost(model, true, doc, content);
      if (code == 400) code = groqPost(model, false, doc, content);
    }
  }
  if (content.length() == 0 && code > 0 && code != 401 && model != String(GROQ_CHAT_MODEL_2)) {
    code = groqPost(String(GROQ_CHAT_MODEL_2), true, doc, content);
    if (code == 400) code = groqPost(String(GROQ_CHAT_MODEL_2), false, doc, content);
  }

  int th = content.lastIndexOf("</think>");
  if (th >= 0) content = content.substring(th + 8);
  content.trim();
  if (content.length() == 0) return false;

  act = VoiceAct();
  bool parsed = false;
  int a = content.indexOf('{');
  int b = content.lastIndexOf('}');
  if (a >= 0 && b > a) {
    JsonDocument j;
    if (!deserializeJson(j, content.substring(a, b + 1))) {
      act.say = String((const char*)(j["say"] | ""));
      act.show = lc(String((const char*)(j["show"] | "none")));
      act.title = String((const char*)(j["title"] | ""));
      act.card = String((const char*)(j["card"] | ""));
      act.city = String((const char*)(j["city"] | ""));
      act.mood = lc(String((const char*)(j["mood"] | "default")));
      act.volume = lc(String((const char*)(j["volume"] | "none")));
      act.verdict = lc(String((const char*)(j["verdict"] | "")));
      act.music = lc(String((const char*)(j["music"] | "")));
      act.query = String((const char*)(j["query"] | ""));
      act.genre = lc(String((const char*)(j["genre"] | "")));
      act.more = j["more"] | false;
      parsed = true;
    }
  }
  if (!parsed) {
    act.say = content;
    act.show = "none";
  }
  if (act.say.length() == 0 && act.card.length() > 0) act.say = act.card;
  if (act.say.length() == 0) return false;

  cleanSpeech(act.say);
  act.show.trim();
  act.city.trim();
  act.title.trim();

  const char* okShow[] = { "none", "map", "clock", "weather", "quiz", "trivia", "humor",
                           "story", "music", "usage", "intro", "card" };
  bool valid = false;
  for (auto s : okShow)
    if (act.show == s) valid = true;
  if (!valid) act.show = "none";
  if (act.more && act.show != "none") act.show = "none";
  act.query.trim();
  act.genre.trim();
  return true;
}

static String llmErrorText() {
  if (WiFi.status() != WL_CONNECTED || lastLlmHttp < 0)
    return "Koneksi internet lagi putus, coba lagi nanti ya.";
  if (lastLlmHttp == 0) return "Koneksi ke server lagi lambat, coba ulangi ya.";
  if (lastLlmHttp == 429) return "Aku lagi kebanyakan mikir, tunggu sebentar ya.";
  if (lastLlmHttp == 401) return "Kunci API-nya bermasalah, tolong dicek.";
  if (lastLlmHttp == 404 || lastLlmHttp == 403)
    return "Model AI-nya belum bisa diakses, tolong cek pengaturan Groq.";
  return "Maaf, aku belum bisa jawab itu sekarang.";
}

static bool isWhisperHallucination(String t) {
  t.toLowerCase();
  if (t.length() < 2) return true;
  const char* bad[] = { "terima kasih telah menonton", "terima kasih sudah menonton",
                        "thanks for watching", "amara.org", "subtitle", "www." };
  for (auto b : bad)
    if (t.indexOf(b) >= 0) return true;
  return false;
}

// ---------------------------------------------------------------------------
//  Cuaca kota
// ---------------------------------------------------------------------------
static String urlEncode(const String& s) {
  String o = "";
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    }
  }
  return o;
}

static bool fetchCityWeather(const String& city, String& outName, String& outSummary) {
  double lat = 0, lon = 0;
  {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(8000);
    String url = String("https://geocoding-api.open-meteo.com/v1/search?count=1&language=id&format=json&name=")
                 + urlEncode(city);
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    Serial.printf("[GEO] HTTP %d (%s)\n", code, city.c_str());
    if (code != 200) {
      http.end();
      return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    http.end();
    if (err || doc["results"][0].isNull()) return false;
    lat = doc["results"][0]["latitude"] | 0.0;
    lon = doc["results"][0]["longitude"] | 0.0;
    outName = String((const char*)(doc["results"][0]["name"] | city.c_str()));
  }

  WeatherData w;
  {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setTimeout(8000);
    String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) + "&longitude=" + String(lon, 4) + "&current=temperature_2m,weather_code"
                                                                                                                        "&daily=temperature_2m_max,temperature_2m_min&timezone=auto";
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    Serial.printf("[WX] HTTP %d (%s)\n", code, outName.c_str());
    if (code != 200) {
      http.end();
      return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    http.end();
    if (err) return false;
    JsonObject cur = doc["current"];
    if (cur.isNull()) return false;
    w.temperature = cur["temperature_2m"] | 0.0f;
    w.weatherCode = cur["weather_code"] | -1;
    w.condition = weatherCodeToText(w.weatherCode);
    w.tempMax = doc["daily"]["temperature_2m_max"][0] | w.temperature;
    w.tempMin = doc["daily"]["temperature_2m_min"][0] | w.temperature;
    w.valid = true;
    w.updatedAt = millis();
  }

  {
    StateLock lock;
    cityWeather = w;
    weatherViewLabel = outName;
  }
  String c = w.condition;
  c.toLowerCase();
  outSummary = c + ", suhu " + String(w.temperature, 0) + " derajat, hari ini terendah " + String(w.tempMin, 0) + " dan tertinggi " + String(w.tempMax, 0) + " derajat";
  return true;
}

// ---------------------------------------------------------------------------
//  Aksi layar
// ---------------------------------------------------------------------------
static void voiceTextMode(bool on) {
  StateLock lock;
  voiceTextActive = on;
}

static void voiceClearScreens() {
  mapRequested = false;
  hudActive = false;
  infoModeActive = false;
  companionActive = false;
  companionNextMenuActive = false;
  companionNextMenuPending = false;
  conversationActive = false;
  hudSuppressUntil = millis() + HUD_SUPPRESS_DURATION;
}

static void showWeatherView(bool custom) {
  voiceClearScreens();
  cancelThinking();
  weatherViewCustom = custom;
  infoModeActive = true;
  infoPage = 1;
  infoModeStarted = millis();
  if (!custom && !weatherData.valid) lastWeatherAttempt = 0;
}

static void voiceShowIntro() {
  voiceClearScreens();
  startupOnboardingActive = false;
  bootTransitionActive = false;
  cancelThinking();
  startIntroduction();
}

static void voiceShowCard(CompanionType type, const String& title, const String& text) {
  voiceClearScreens();
  if (type == COMPANION_CARD) {
    String t = title.length() ? title : String("INFO");
    t = stripNonAscii(t);
    t.toUpperCase();
    if (t.length() > 16) t = t.substring(0, 16);
    cardTitle = t;
  }
  companionNextMenuDelayMs = 350;
  currentCompanion = type;
  finishCompanionGeneration(text, type);
}

static CharacterEmotion moodFromString(const String& m) {
  if (m == "happy") return CHARACTER_HAPPY;
  if (m == "laugh") return CHARACTER_LAUGH;
  if (m == "confused") return CHARACTER_CONFUSED;
  if (m == "angry") return CHARACTER_ANGRY;
  if (m == "tired") return CHARACTER_TIRED;
  return CHARACTER_HAPPY;
}

static void voiceApply(const VoiceAct& act) {
  StateLock lock;
  markUserActivity();

  if (act.volume == "up") audioVolumeStep(+1);
  else if (act.volume == "down") audioVolumeStep(-1);
  else if (act.volume == "mute") {
    if (volumeLevel > 0) audioSetVolume(0);
  } else if (act.volume == "unmute") {
    if (volumeLevel == 0) audioSetVolume(volumeBeforeMute > 0 ? volumeBeforeMute : 3);
  }

  const String& s = act.show;
  const String body = act.card.length() ? act.card : act.say;
  voiceTextActive = (s == "none" || act.more);

  if (quizWaitingAnswer && act.verdict.length() > 0) {
    if (act.verdict == "correct") handleQuizAnswer('J');
    else if (act.verdict == "wrong") handleQuizAnswer('K');
    else handleQuizAnswer('L');
    return;
  }
  if (s == "map") handleCommand('M');
  else if (s == "clock") startInfoMode();
  else if (s == "weather") showWeatherView(act.customWeather);
  else if (s == "music") {
    voiceClearScreens();
    musicPostFromAct(act.music, act.query, act.genre);
  } else if (s == "usage") handleCommand('U');
  else if (s == "intro") voiceShowIntro();
  else if (s == "quiz") voiceShowCard(COMPANION_QUIZ, "", body);
  else if (s == "trivia") voiceShowCard(COMPANION_TRIVIA, "", body);
  else if (s == "humor") voiceShowCard(COMPANION_HUMOR, "", body);
  else if (s == "story") voiceShowCard(COMPANION_STORY, "", body);
  else if (s == "card") voiceShowCard(COMPANION_CARD, act.title, body);

  setCharacterEmotion(moodFromString(act.mood));
}

// ---------------------------------------------------------------------------
//  Perintah lokal
// ---------------------------------------------------------------------------
static int wordCount(const String& s) {
  int n = 0;
  bool in = false;
  for (size_t i = 0; i < s.length(); i++) {
    bool sp = (s[i] == ' ');
    if (!sp && !in) {
      n++;
      in = true;
    } else if (sp) in = false;
  }
  return n;
}

static String timeSentence() {
  struct tm ti;
  if (!getLocalTimeSafe(&ti)) return "Jamnya belum sinkron, tunggu sebentar ya.";
  char b[64];
  if (ti.tm_min == 0) snprintf(b, sizeof(b), "Sekarang jam %d tepat.", ti.tm_hour);
  else snprintf(b, sizeof(b), "Sekarang jam %d lewat %d menit.", ti.tm_hour, ti.tm_min);
  return String(b);
}

static String homeWeatherSentence() {
  StateLock lock;
  if (!weatherData.valid) return "Data cuaca belum tersedia, coba lagi sebentar lagi.";
  String c = weatherData.condition;
  c.toLowerCase();
  return String("Cuaca di ") + HOME_CITY + " sekarang " + c + ", suhunya sekitar " + String(weatherData.temperature, 0) + " derajat. Hari ini antara " + String(weatherData.tempMin, 0) + " sampai " + String(weatherData.tempMax, 0) + " derajat.";
}

static bool voiceLocalCommand(String t, String& say, bool allowContent) {
  t.toLowerCase();
  say = "";
  StateLock lock;
  markUserActivity();
  voiceTextActive = false;

  if (t.indexOf("diam") >= 0 || t.indexOf("mute") >= 0 || t.indexOf("bisu") >= 0) {
    audioToggleMute();
    say = (volumeLevel > 0) ? "Oke, suara aktif lagi." : "";
    voiceTextActive = true;
    return true;
  }
  if (t.indexOf("volume naik") >= 0 || t.indexOf("lebih keras") >= 0) {
    audioVolumeStep(+1);
    say = "Volume dinaikkan.";
    voiceTextActive = true;
    return true;
  }
  if (t.indexOf("volume turun") >= 0 || t.indexOf("lebih pelan") >= 0) {
    audioVolumeStep(-1);
    say = "Volume diturunkan.";
    voiceTextActive = true;
    return true;
  }
  if (t.indexOf("peta") >= 0 || t.indexOf("navigasi") >= 0) {
    handleCommand('M');
    say = "Oke, ini petanya.";
    return true;
  }
  if (t.indexOf("cuaca") >= 0) {
    showWeatherView(false);
    say = homeWeatherSentence();
    return true;
  }
  if (t == "jam" || t.indexOf("jam berapa") >= 0 || t.indexOf("sekarang jam") >= 0) {
    startInfoMode();
    say = timeSentence();
    return true;
  }
  if (t.indexOf("lagu berikutnya") >= 0 || t.indexOf("lagu selanjutnya") >= 0 || t.indexOf("ganti lagu") >= 0 || t.indexOf("skip") >= 0) {
    musicRequestPost(MCMD_NEXT, "", "");
    say = "Oke, lagu berikutnya.";
    return true;
  }
  if (t.indexOf("stop musik") >= 0 || t.indexOf("matikan musik") >= 0 || t.indexOf("matikan lagu") >= 0 || t.indexOf("berhenti musik") >= 0) {
    musicRequestPost(MCMD_STOP, "", "");
    say = "Oke, musiknya kumatikan.";
    return true;
  }
  if (t.indexOf("lanjut musik") >= 0 || t.indexOf("lanjutkan musik") >= 0 || t.indexOf("lanjut lagu") >= 0 || t.indexOf("putar lagi") >= 0) {
    musicRequestPost(MCMD_RESUME, "", "");
    say = "Siap, kulanjutkan.";
    return true;
  }
  if (t.indexOf("intro") >= 0) {
    voiceShowIntro();
    say = "Oke, kita mulai dari awal.";
    return true;
  }
  if (allowContent) {
    if (t.indexOf("kuis") >= 0 || t.indexOf("quiz") >= 0) {
      handleCommand('Q');
      say = "Oke, ini kuisnya.";
      return true;
    }
    if (t.indexOf("trivia") >= 0) {
      handleCommand('T');
      say = "Oke, ini trivia buat kamu.";
      return true;
    }
    if (t.indexOf("humor") >= 0 || t.indexOf("lawak") >= 0) {
      handleCommand('H');
      say = "Oke, siap-siap ketawa ya.";
      return true;
    }
    if (t.indexOf("cerita") >= 0) {
      handleCommand('S');
      say = "Oke, aku ceritakan ya.";
      return true;
    }
  }
  return false;
}

// ===========================================================================
//  VOICE TASK
// ===========================================================================
static void voiceReact(CharacterEmotion e) {
  StateLock lock;
  triggerCharacterReaction(e);
}
static void voiceThinkOn() {
  StateLock lock;
  setCharacterEmotion(CHARACTER_THINKING);
  beginThinkingVisual();
}
static void voiceThinkOff() {
  StateLock lock;
  endThinkingVisual();
}
static void voiceEmotion(CharacterEmotion e) {
  StateLock lock;
  setCharacterEmotion(e);
}

static String quizKeywordFallback(const String& transcript) {
  String low = transcript;
  low.toLowerCase();
  if (low.indexOf("benar") >= 0 || low.indexOf("betul") >= 0) {
    handleQuizAnswer('J');
    return "Wah hebat, jawabanmu benar!";
  }
  if (low.indexOf("salah") >= 0) {
    handleQuizAnswer('K');
    return "Yah, sayang sekali salah.";
  }
  if (low.indexOf("tidak tahu") >= 0 || low.indexOf("gak tahu") >= 0 || low.indexOf("bingung") >= 0) {
    handleQuizAnswer('L');
    return "Tidak apa-apa, tetap semangat ya.";
  }
  handleQuizAnswer('L');
  return "Aku belum bisa menilai jawaban itu, anggap tidak tahu ya.";
}

constexpr int STORY_MAX_SEGMENTS = 24;

static void storyContinue(const String& request, const VoiceAct& first) {
  String told = first.say;
  bool more = first.more;
  for (int seg = 1; seg < STORY_MAX_SEGMENTS && more; seg++) {
    if (ttsStop || speakAborted || volumeLevel == 0) break;
    String tail = (told.length() > 500) ? told.substring(told.length() - 500) : told;
    String extra = " Kamu sedang menceritakan cerita bertahap untuk permintaan pengguna: \"" + request + "\". Ini bagian ke-" + String(seg + 1) + ". Bagian terakhir yang sudah kamu ceritakan: \"" + tail + "\". Lanjutkan 4-5 kalimat berikutnya tanpa mengulang. Isi more=true kalau cerita masih "
                                                                                                                                                                                                             "berlanjut, atau more=false kalau ini bagian penutup. show harus none.";
    VoiceAct nxt;
    if (!groqAsk("Lanjutkan ceritanya.", extra, nxt) || nxt.say.length() == 0) {
      voiceTextMode(true);
      speakText("Maaf, ceritanya terputus karena koneksi. Coba minta lagi nanti ya.");
      break;
    }
    if (ttsStop || speakAborted) break;
    told += " ";
    told += nxt.say;
    more = nxt.more;
    voiceEmotion(moodFromString(nxt.mood));
    voiceTextMode(true);
    speakText(nxt.say);
  }
  ttsStop = false;
}

static void voiceProcess() {
  musicDropStream();
  voiceTextMode(false);
  if (WiFi.status() != WL_CONNECTED) {
    voiceReact(CHARACTER_CONFUSED);
    voiceTextMode(true);
    speakText("Koneksi internet lagi putus, coba lagi nanti ya.");
    return;
  }

  size_t samples = 0;
  int16_t* snapshot = nullptr;
  {
    StateLock lock;
    samples = recLength;
    if (samples * sizeof(int16_t) < STT_MIN_BYTES) {
      Serial.println("[VOICE] rekaman terlalu pendek, skip");
      linkSend("{\"t\":\"stt_skip\",\"why\":\"too_short\"}");
      return;
    }
    if (recPeak < 400) {
      Serial.println("[VOICE] rekaman terlalu pelan, skip");
      linkSend("{\"t\":\"stt_skip\",\"why\":\"too_quiet\"}");
      triggerCharacterReaction(CHARACTER_CONFUSED);
      return;
    }
    snapshot = (int16_t*)heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!snapshot) snapshot = (int16_t*)malloc(samples * sizeof(int16_t));
    if (!snapshot) {
      Serial.println("[VOICE] OOM snapshot");
      return;
    }
    memcpy(snapshot, recBuf, samples * sizeof(int16_t));
  }

  voiceThinkOn();
  String transcript = groqTranscribe(snapshot, samples);
  free(snapshot);

  if (transcript.length() == 0 || isWhisperHallucination(transcript)) {
    voiceThinkOff();
    Serial.println("[VOICE] STT kosong/halusinasi, diabaikan");
    voiceReact(CHARACTER_CONFUSED);
    linkSend("{\"t\":\"stt_result\",\"ok\":0}");
    if (sttLastCode != 200 && sttLastCode != 0) speakText("Aku kurang jelas dengar, coba ulangi ya.");
    sfxPlay(SFX_ERROR);
    return;
  }

  linkSend("{\"t\":\"stt_result\",\"ok\":1,\"text\":\"" + jsonEscape(transcript) + "\"}");
  Serial.printf("[VOICE] STT OK: %s\n", transcript.c_str());

  String say;
  if (!quizWaitingAnswer && wordCount(transcript) <= 4 && voiceLocalCommand(transcript, say, false)) {
    voiceThinkOff();
    if (say.length() > 0) {
      voiceEmotion(CHARACTER_HAPPY);
      speakText(say);
      voiceEmotion(CHARACTER_DEFAULT);
    }
    return;
  }

  groqEnsureModel();
  VoiceAct act;
  String quizExtra = "";
  if (quizWaitingAnswer) {
    quizExtra = " Kuis aktif di layar dengan pertanyaan: \"" + currentQuizQuestion + "\". Jika pesan pengguna adalah jawaban kuis, nilai jawabannya, isi verdict "
                                                                                     "(correct, wrong, atau unknown bila pengguna menyerah), dan sebutkan jawaban yang benar. "
                                                                                     "Jika pesan bukan jawaban kuis, abaikan kuis dan kosongkan verdict.";
  }
  bool ok = groqAsk(transcript, quizExtra, act);

  if (ok && act.show == "weather" && act.city.length() > 0) {
    String resolved, summary;
    if (fetchCityWeather(act.city, resolved, summary)) {
      String extra = " Data cuaca terbaru untuk " + resolved + ": " + summary + ". Jawab pakai data ini dan set show=weather.";
      VoiceAct act2;
      if (groqAsk(transcript, extra, act2)) {
        act = act2;
        act.show = "weather";
      } else {
        act.say = "Cuaca di " + resolved + " sekarang " + summary + ".";
        act.show = "weather";
      }
      act.customWeather = true;
    } else {
      act.say = "Maaf, aku tidak menemukan data cuaca untuk " + act.city + ".";
      act.show = "none";
    }
  }

  voiceThinkOff();

  if (!ok) {
    Serial.printf("[VOICE] LLM gagal (HTTP %d)\n", lastLlmHttp);
    sfxPlay(SFX_ERROR);
    if (quizWaitingAnswer) {
      say = quizKeywordFallback(transcript);
      voiceTextMode(true);
      speakText(say);
      return;
    }
    if (voiceLocalCommand(transcript, say, true)) {
      if (say.length() > 0) {
        voiceEmotion(CHARACTER_HAPPY);
        speakText(say);
        voiceEmotion(CHARACTER_DEFAULT);
      }
      return;
    }
    voiceReact(CHARACTER_CONFUSED);
    voiceTextMode(true);
    speakText(llmErrorText());
    return;
  }

  histAdd(transcript, act.say);
  linkSend("{\"t\":\"llm_reply\",\"ok\":1,\"show\":\"" + jsonEscape(act.show) + "\",\"text\":\"" + jsonEscape(act.say) + "\"}");
  Serial.printf("[VOICE] LLM OK (show=%s mood=%s): %s\n",
                act.show.c_str(), act.mood.c_str(), act.say.c_str());

  sfxPlay(SFX_REPLY);
  voiceApply(act);
  speakText(act.say);
  if (act.more) storyContinue(transcript, act);
  voiceEmotion(CHARACTER_DEFAULT);
}

static void voiceTask(void* pv) {
  unsigned long lastModelCheck = 0;
  for (;;) {
    uint32_t bits = 0;
    const bool fast = (musicState == MUS_PLAYING || musicState == MUS_LOADING || musicStopReq);
    const TickType_t wait = fast ? pdMS_TO_TICKS(2) : pdMS_TO_TICKS(500);

    if (xTaskNotifyWait(0, 0xFFFFFFFF, &bits, wait) == pdFALSE) {
      musicTick();
      if (!fast && !modelChecked && millis() > 6000 && millis() - lastModelCheck > 3000) {
        lastModelCheck = millis();
        groqEnsureModel();
      }
      continue;
    }
    if (bits & 0x01) {
      voicePipelineActive = true;
      ttsStop = false;
      voiceProcess();
      voicePipelineActive = false;
    }
    if (bits & 0x02) musicServiceRequest();
  }
}

// ===========================================================================
//  MQTT
// ===========================================================================
void mqttBegin() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setSocketTimeout(1);
  mqttNetworkClient.setTimeout(1);
  mqttClient.setKeepAlive(15);

  if (!mqttClient.setBufferSize(MQTT_PACKET_BUFFER_SIZE)) {
    Serial.println("[MQTT] WARNING: gagal mengatur packet buffer");
  }
  Serial.println("[MQTT] Client configured");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  if (strcmp(topic, MQTT_TOPIC_COMMAND) != 0) return;
  if (length == 0 || length >= LINK_LINE_MAX) return;
  if (payload[0] != '{') return;
  mqttPendingDispatch = "";
  mqttPendingDispatch.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) mqttPendingDispatch += (char)payload[i];
  mqttPendingDispatch.trim();
}

void mqttHeartbeat() {
  static unsigned long last = 0;
  if (millis() - last < 10000UL) return;
  last = millis();

  char buf[220];
  snprintf(buf, sizeof(buf),
           "{\"t\":\"hb\",\"ver\":\"%s\",\"up\":%lu,\"heap\":%u,\"route\":%d,\"trip\":%d,\"batt\":100,\"chg\":1}",
           otaReportedVersion.c_str(), millis() / 1000UL,
           (unsigned)ESP.getFreeHeap(), routeReady ? 1 : 0, tripActive ? 1 : 0);
           
  // Tambahkan palang pintu di sini
  if (mqttMutex != nullptr && xSemaphoreTake(mqttMutex, portMAX_DELAY) == pdTRUE) {
    mqttClient.publish(MQTT_TOPIC_STATUS, buf);
    xSemaphoreGive(mqttMutex);
  }
}

void mqttUpdate() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  if (mqttClient.connected()) {
    if (mqttMutex != nullptr && xSemaphoreTake(mqttMutex, portMAX_DELAY) == pdTRUE) {
      mqttClient.loop();
      xSemaphoreGive(mqttMutex);
    }
    
    if (mqttPendingDispatch.length() > 0) {
      String msg = mqttPendingDispatch;
      mqttPendingDispatch = "";
      linkDispatchLine(msg);
    }
    mqttHeartbeat();
    return;
  }
  
  // ... (sisa kode mqttUpdate di bawahnya biarkan sama)

  unsigned long now = millis();
  if (now - mqttLastReconnectAttempt < mqttBackoffMs) return;
  mqttLastReconnectAttempt = now;

  char clientId[40];
  snprintf(clientId, sizeof(clientId), "ABOT-V3500-%08lx%04x",
           (unsigned long)(ESP.getEfuseMac() & 0xFFFFFFFF),
           (unsigned)random(0xFFFF));

  const char* willMsg = "{\"t\":\"device\",\"online\":false}";
  Serial.print("[MQTT] Connecting to ");
  Serial.println(MQTT_HOST);

  if (mqttClient.connect(clientId, MQTT_TOPIC_STATUS, 0, true, willMsg)) {
    Serial.println("[MQTT] Connected");
    mqttBackoffMs = MQTT_RECONNECT_INTERVAL_MS;
    if (!mqttClient.subscribe(MQTT_TOPIC_COMMAND, 0)) {
      Serial.println("[MQTT] Subscribe FAILED");
    }
    String onlineMsg = String("{\"t\":\"device\",\"online\":true,\"ver\":\"") + otaReportedVersion + "\"}";
    mqttClient.publish(MQTT_TOPIC_STATUS, onlineMsg.c_str(), true);
  } else {
    Serial.print("[MQTT] Connect failed, state=");
    Serial.println(mqttClient.state());
    mqttBackoffMs = (mqttBackoffMs * 2 > 60000UL) ? 60000UL : mqttBackoffMs * 2;
  }
}

// ===========================================================================
//  LINK LAYER
// ===========================================================================
void linkBegin() {
  linkEnabled = true;
  hpConnected = false;
  linkRouteReceived = false;
  Serial.println("[LINK] Serial mode ready");
}

void linkSend(const String& json) {
  Serial.print("[HP] ");
  Serial.println(json);
  
  if (mqttMutex != nullptr && xSemaphoreTake(mqttMutex, portMAX_DELAY) == pdTRUE) {
    if (mqttClient.connected()) {
      bool ok = mqttClient.publish(MQTT_TOPIC_STATUS, json.c_str());
      if (!ok) Serial.println("[MQTT] Publish status gagal");
    }
    xSemaphoreGive(mqttMutex);
  }
}

void linkUpdate() {
  static String buffer;
  static bool overflow = false;
  static bool reserved = false;
  if (!reserved) {
    buffer.reserve(LINK_LINE_MAX);
    reserved = true;
  }

  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (overflow) {
        Serial.println("[LINK] Baris terlalu panjang, dibuang");
        linkSend("{\"t\":\"nack\",\"why\":\"too_long\"}");
        overflow = false;
        buffer = "";
        continue;
      }
      buffer.trim();
      if (buffer.length() > 0) linkDispatchLine(buffer);
      buffer = "";
      continue;
    }
    if (overflow) continue;
    if (buffer.length() >= LINK_LINE_MAX) {
      overflow = true;
      continue;
    }
    buffer += c;
  }
}

void linkDispatchLine(const String& line) {
  if (line[0] != '{') {
    if (line.length() == 1) {
      StateLock lock;
      markUserActivity();
      handleCommand(line[0]);
    }
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.print("[LINK] JSON error: ");
    Serial.println(err.c_str());
    linkSend("{\"t\":\"nack\",\"why\":\"bad_json\"}");
    return;
  }

  const char* t = doc["t"] | "";

  StateLock lock;
  lastLinkMessageAt = millis();

  if (strcmp(t, "auth") == 0) {
    const char* token = doc["token"] | "";
    if (strcmp(token, AUTH_TOKEN) == 0) {
      isAuthorized = true;
      hpConnected = true;
      abotLog("INFO", "AUTH OK. HP terhubung & diotorisasi.");
      linkSend("{\"t\":\"auth_ok\"}");
    } else {
      isAuthorized = false;
      abotLog("WARN", "AUTH GAGAL. Token salah.");
      linkSend("{\"t\":\"auth_fail\",\"why\":\"bad_token\"}");
    }
    return;
  }

  if (!isAuthorized) {
    abotLog("WARN", "Perintah ditolak. Belum AUTH.");
    linkSend("{\"t\":\"nack\",\"why\":\"unauthorized\"}");
    return;
  }

  hpConnected = true;

  if (strcmp(t, "route") == 0) {
    if (millis() - lastRouteMsgAt < MIN_ROUTE_INTERVAL_MS) return;
    lastRouteMsgAt = millis();
    onLinkRoute(doc);
  } else if (strcmp(t, "gps") == 0) {
    if (millis() - lastGpsMsgAt < MIN_GPS_INTERVAL_MS) return;
    lastGpsMsgAt = millis();
    onLinkGps(doc);
  } else if (strcmp(t, "trip_start") == 0) onLinkTripStart();
  else if (strcmp(t, "trip_end") == 0) onLinkTripEnd();
  else if (strcmp(t, "reset") == 0) onLinkReset();
  else if (strcmp(t, "ping") == 0) onLinkPing();
  else if (strcmp(t, "ota_start") == 0) {
    const char* requestId = doc["id"] | "";
    const char* targetVersion = doc["ver"] | "";
    const char* firmwareUrl = doc["url"] | "";
    const char* firmwareSha = doc["sha256"] | "";

    // 1. Wajibkan SHA256 (Keamanan)
    if (strlen(firmwareSha) != 64) {
      linkSend(String("{\"t\":\"ota_error\",\"id\":\"") + requestId + "\",\"code\":\"bad_hash\",\"message\":\"sha256 wajib 64 karakter\"}");
      return;
    }

    // 2. Guard: Jangan OTA kalau sedang sibuk (Musik/Rekam)
    if (voicePipelineActive || listeningActive || musicState != MUS_STOPPED) {
      linkSend(String("{\"t\":\"ota_error\",\"id\":\"") + requestId + "\",\"code\":\"busy\",\"message\":\"Selesaikan aktivitas saat ini\"}");
      return;
    }

    if (strlen(firmwareUrl) == 0) {
      linkSend(String("{\"t\":\"ota_error\",\"id\":\"") + requestId + "\",\"code\":\"manifest_invalid\",\"message\":\"URL kosong\"}");
    } else {
      // 3. Jalankan OTA di Task Terpisah agar layar & MQTT tidak freeze
      struct OtaParams {
        String url;
        String id;
        String ver;
        String sha;
      };
      OtaParams* params = new OtaParams{ String(firmwareUrl), String(requestId), String(targetVersion), String(firmwareSha) };

      xTaskCreatePinnedToCore([](void* pv) {
        OtaParams* p = (OtaParams*)pv;
        startOTAUpdate(p->url, p->id, p->ver, p->sha);
        delete p;
        vTaskDelete(NULL);
      },
                              "OtaTask", 8192, params, 1, NULL, 0);
    }
  } else if (strcmp(t, "ota_cancel") == 0) {
    const char* requestId = doc["id"] | "";
    String cancelMsg = String("{\"t\":\"ota_error\",\"id\":\"") + requestId + "\",\"code\":\"cancelled\",\"message\":\"OTA dibatalkan\"}";
    linkSend(cancelMsg);
  } else if (strcmp(t, "vol") == 0) {
    if (!doc["v"].isNull()) audioSetVolume(doc["v"].as<int>());
    char volBuf[64];
    snprintf(volBuf, sizeof(volBuf), "{\"t\":\"ack\",\"for\":\"vol\",\"v\":%d}", volumeLevel);
    linkSend(String(volBuf));
  } else {
    abotLog("WARN", "Unknown link type: %s", t);
  }
}

void linkReportStatus() {
  static unsigned long last = 0;
  if (!hpConnected || millis() - last < LINK_STATUS_MS) return;
  last = millis();

  char buf[160];
  snprintf(buf, sizeof(buf),
           "{\"t\":\"status\",\"route\":%d,\"trip\":%d,\"prog\":%.0f,\"step\":%d,\"heap\":%u,\"auth\":%d}",
           routeReady ? 1 : 0, tripActive ? 1 : 0, simulatedRouteDistance,
           activeStep, (unsigned)ESP.getFreeHeap(), isAuthorized ? 1 : 0);
  linkSend(String(buf));
  flushLogRing();
}

// ===========================================================================
//  OTA (BENERAN)
// ===========================================================================
void startOTAUpdate(const String& url, const String& requestId,
                    const String& targetVersion, const String& expectedSha256) {
#if OTA_SIMULATION_MODE
  if (otaActive) {
    String busyMsg = String("{\"t\":\"ota_error\",\"id\":\"") + requestId + "\",\"code\":\"busy\"}";
    linkSend(busyMsg);
    return;
  }

  otaActive = true;
  String effectiveTarget = targetVersion.length() ? targetVersion : otaReportedVersion;
  String accepted = String("{\"t\":\"ota_accepted\",\"id\":\"") + requestId + "\",\"from\":\"" + otaReportedVersion + "\",\"to\":\"" + effectiveTarget + "\",\"sim\":true}";
  linkSend(accepted);
  abotLog("INFO", "OTA Simulation Mode aktif; target=%s", effectiveTarget.c_str());

  const char* stages[] = { "wifi", "manifest", "download", "verify", "install" };
  const int stageProgress[] = { 100, 100, 25, 75, 100 };

  for (int i = 0; i < 5; i++) {
    beginFrame();
    drawCenteredText("SIMULASI OTA", 12, 1);
    drawCenteredText(String(stageProgress[i]) + "%", 32, 2);
    drawCenteredText(stages[i], 52, 1);
    presentFrame();

    String progressMsg = String("{\"t\":\"ota_progress\",\"id\":\"") + requestId + "\",\"stage\":\"" + stages[i] + "\",\"progress\":" + String(stageProgress[i]) + ",\"sim\":true}";
    linkSend(progressMsg);
    delay(i < 2 ? 300 : 500);
  }

  otaReportedVersion = effectiveTarget;
  beginFrame();
  drawCenteredText("OTA SIMULASI", 18, 1);
  drawCenteredText("SELESAI", 38, 2);
  presentFrame();

  linkSend(String("{\"t\":\"ota_done\",\"id\":\"") + requestId + "\",\"to\":\"" + effectiveTarget + "\",\"sim\":true}");
  delay(900);
  linkSend(String("{\"t\":\"ota_result\",\"id\":\"") + requestId + "\",\"ok\":true,\"ver\":\"" + effectiveTarget + "\",\"sim\":true}");
  linkSend(String("{\"t\":\"ota_simulated\",\"id\":\"") + requestId + "\",\"ver\":\"" + effectiveTarget + "\"}");
  abotLog("INFO", "Simulasi OTA selesai; versi virtual=%s", effectiveTarget.c_str());
  otaActive = false;
  return;
#else
  // ---------- OTA BENERAN ----------
  if (url.length() == 0) {
    linkSend(String("{\"t\":\"ota_error\",\"id\":\"") + requestId + "\",\"code\":\"manifest_invalid\",\"message\":\"URL kosong\"}");
    return;
  }
  if (otaActive) {
    linkSend(String("{\"t\":\"ota_error\",\"id\":\"") + requestId + "\",\"code\":\"busy\"}");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    linkSend(String("{\"t\":\"ota_error\",\"id\":\"") + requestId + "\",\"code\":\"no_wifi\"}");
    return;
  }

  otaActive = true;
  const String effectiveTarget =
    targetVersion.length() ? targetVersion : otaReportedVersion;
  
  abotLog("INFO", "OTA mulai: %s -> %s", otaReportedVersion.c_str(),
          effectiveTarget.c_str());
          
  // Simpan target versi ke NVS sebelum mulai
  otaPrefs.begin("ota", false);
  otaPrefs.putString("target", effectiveTarget);
  otaPrefs.putBool("pending", true);
  otaPrefs.end();

  linkSend(String("{\"t\":\"ota_accepted\",\"id\":\"") + requestId +
           "\",\"from\":\"" + otaReportedVersion + "\",\"to\":\"" +
           effectiveTarget + "\"}");

  auto sendError = [&](const char* code, const String& msg) {
    String errMsg = String("{\"t\":\"ota_error\",\"id\":\"") + requestId + "\",\"code\":\"" + code + "\"";
    if (msg.length() > 0) errMsg += ",\"message\":\"" + jsonEscape(msg) + "\"";
    errMsg += "}";
    linkSend(errMsg);
    otaActive = false;
  };

  linkSend(String("{\"t\":\"ota_progress\",\"id\":\"") + requestId + "\",\"stage\":\"download\",\"progress\":0}");

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  WiFiClient plainClient;

  HTTPClient http;
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  bool begun = url.startsWith("https")
                 ? http.begin(secureClient, url)
                 : http.begin(plainClient, url);
  if (!begun) {
    sendError("download_failed", "begin gagal");
    return;
  }

  int httpCode = http.GET();
  if (httpCode != 200) {
    sendError("download_failed", "HTTP " + String(httpCode));
    http.end();
    return;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    sendError("download_failed", "Content-Length invalid");
    http.end();
    return;
  }

  if (!Update.begin(contentLength)) {
    sendError("no_space", "Update.begin gagal");
    http.end();
    return;
  }

  mbedtls_sha256_context shaCtx;
  mbedtls_sha256_init(&shaCtx);
  mbedtls_sha256_starts(&shaCtx, 0);

  WiFiClient* stream = http.getStreamPtr();
  size_t written = 0;
  uint8_t buffer[1024];
  const unsigned long dlStart = millis();
  int lastProgress = -1;

  while (written < (size_t)contentLength) {
    esp_task_wdt_reset();  // <--- TAMBAHKAN BARIS INI (Lapor WDT)

    if (!http.connected()) break;
    size_t avail = stream->available();
    if (avail == 0) {
      if (millis() - dlStart > 120000UL) {
        Update.abort();
        mbedtls_sha256_free(&shaCtx);
        http.end();
        sendError("download_failed", "timeout 120s");
        return;
      }
      delay(1);
      continue;
    }

    size_t toRead = avail > sizeof(buffer) ? sizeof(buffer) : avail;

    size_t readBytes = stream->readBytes(buffer, toRead);
    if (readBytes == 0) continue;

    mbedtls_sha256_update(&shaCtx, buffer, readBytes);

    if (Update.write(buffer, readBytes) != readBytes) {
      Update.abort();
      mbedtls_sha256_free(&shaCtx);
      http.end();
      sendError("write_failed", "Update.write gagal");
      return;
    }
    written += readBytes;

    int progress = (int)((written * 100) / (size_t)contentLength);
    if (progress / 5 != lastProgress / 5) {
      lastProgress = progress;
      linkSend(String("{\"t\":\"ota_progress\",\"id\":\"") + requestId + "\",\"stage\":\"download\",\"progress\":" + String(progress) + "}");
    }
  }
  http.end();

  uint8_t digest[32];
  mbedtls_sha256_finish(&shaCtx, digest);
  mbedtls_sha256_free(&shaCtx);

  if (written != (size_t)contentLength) {
    Update.abort();
    sendError("download_failed",
              "terunduh " + String((unsigned)written) + "/" + String(contentLength));
    return;
  }

  linkSend(String("{\"t\":\"ota_progress\",\"id\":\"") + requestId + "\",\"stage\":\"verify\",\"progress\":100}");

  if (expectedSha256.length() > 0) {
    String calcHex = "";
    char hexBuf[3];
    for (int i = 0; i < 32; i++) {
      snprintf(hexBuf, sizeof(hexBuf), "%02x", digest[i]);
      calcHex += hexBuf;
    }
    String expected = expectedSha256;
    expected.toLowerCase();
    if (calcHex != expected) {
      Update.abort();
      sendError("bad_hash", "sha256 mismatch");
      return;
    }
    abotLog("INFO", "SHA256 cocok");
  }

  linkSend(String("{\"t\":\"ota_progress\",\"id\":\"") + requestId + "\",\"stage\":\"install\",\"progress\":100}");

  if (!Update.end(true)) {
    sendError("write_failed", Update.errorString());
    return;
  }
  if (!Update.isFinished()) {
    sendError("write_failed", "Update tidak selesai");
    return;
  }

  abotLog("INFO", "OTA selesai, restart...");
  linkSend(String("{\"t\":\"ota_done\",\"id\":\"") + requestId +
           "\",\"to\":\"" + effectiveTarget + "\"}");
  delay(1500);
  
  // Matikan sementara watchdog sebelum restart agar tidak bentrok
  esp_task_wdt_delete(NULL); 
  ESP.restart();
#endif
}

// ===========================================================================
//  LINK HELPERS
// ===========================================================================
void skipStartupSequence() {
  if (bootTransitionActive) bootTransitionActive = false;
  if (!introductionFinished) {
    wakeIntroStage = WAKE_FINISHED;
    introductionFinished = true;
    roboEyes.setAutoblinker(ON, 3, 2);
    roboEyes.setIdleMode(ON, 2, 1);
  }
  if (startupOnboardingActive) {
    startupOnboardingActive = false;
    startupStage = STARTUP_NORMAL;
    startupOnboardingManual = false;
  }
  startupOnboardingCompleted = true;
  infoModeActive = false;
  resetNaturalEyeBehavior();
  lastUserActivityMs = millis();
}

void syncRouteProgress() {
  while (simulatedRouteIndex < routePointCount - 1 && routeCumulativeDistance[simulatedRouteIndex + 1] < simulatedRouteDistance) {
    simulatedRouteIndex++;
  }
  int newStep = 0;
  for (int i = 0; i < navStepCount; i++) {
    if (navSteps[i].routeDistance <= simulatedRouteDistance) newStep = i;
  }
  if (newStep != activeStep) {
    activeStep = newStep;
    Serial.print("[NAV] Passed step -> ");
    Serial.println(newStep);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"t\":\"step\",\"i\":%d,\"prog\":%.0f}",
             newStep, simulatedRouteDistance);
    linkSend(String(buf));
  }
}

void updateSpeedEstimate(float newDist) {
  unsigned long now = millis();
  if (speedLastAt != 0) {
    float dt = (now - speedLastAt) / 1000.0f;
    if (dt >= 0.2f && dt <= 5.0f) {
      float v = (newDist - speedLastDist) / dt;
      if (v < 0.0f) v = 0.0f;
      if (v > 35.0f) v = 35.0f;
      speedMps = speedMps * 0.6f + v * 0.4f;
    }
  }
  speedLastAt = now;
  speedLastDist = newDist;
}

void deadReckonStep() {
  unsigned long now = millis();
  if (drLastMs == 0) {
    drLastMs = now;
    return;
  }
  float dt = (now - drLastMs) / 1000.0f;
  drLastMs = now;
  if (dt > 0.25f) dt = 0.25f;
  if (now - lastLinkGpsAt > DEAD_RECKON_MAX_MS) return;
  if (speedMps < 1.0f) return;
  float limit = routeGeometryDistance - 16.0f;
  if (simulatedRouteDistance >= limit) return;
  simulatedRouteDistance += speedMps * dt;
  if (simulatedRouteDistance > limit) simulatedRouteDistance = limit;
  syncRouteProgress();
}

// ===========================================================================
//  LINK HANDLERS
// ===========================================================================
void onLinkRoute(JsonDocument& doc) {
  JsonArray coords = doc["coords"].as<JsonArray>();
  size_t total = coords.size();

  if (total < 2) {
    linkSend("{\"t\":\"nack\",\"for\":\"route\",\"why\":\"no_coords\"}");
    return;
  }

  markUserActivity();
  Serial.printf("[LINK] >>> ROUTE dari HP (%u titik)\n", (unsigned)total);

  osrmTotalDistance = doc["dist"] | 0.0f;
  osrmTotalDuration = doc["dur"] | 0.0f;

  routeStride = (int)((total + MAX_ROUTE_POINTS - 1) / MAX_ROUTE_POINTS);
  if (routeStride < 1) routeStride = 1;

  routePointCount = 0;
  size_t i = 0;
  for (JsonArray c : coords) {
    bool isLast = (i == total - 1);
    if ((i % routeStride) == 0 || isLast) {
      int slot = (routePointCount < MAX_ROUTE_POINTS)
                   ? routePointCount++
                   : (MAX_ROUTE_POINTS - 1);
      routePoints[slot].lon = c[0].as<double>();
      routePoints[slot].lat = c[1].as<double>();
    }
    i++;
  }

  int validCount = 0;
  for (int k = 0; k < routePointCount; k++) {
    if (routePoints[k].lat < -90.0 || routePoints[k].lat > 90.0 || routePoints[k].lon < -180.0 || routePoints[k].lon > 180.0) continue;
    if (validCount != k) routePoints[validCount] = routePoints[k];
    validCount++;
  }
  routePointCount = validCount;

  if (routePointCount < 2) {
    linkSend("{\"t\":\"nack\",\"for\":\"route\",\"why\":\"no_valid_coords\"}");
    return;
  }

  routeGeometryDistance = 0;
  routeCumulativeDistance[0] = 0;
  for (int k = 1; k < routePointCount; k++) {
    double lat1 = routePoints[k - 1].lat;
    double lon1 = routePoints[k - 1].lon;
    double lat2 = routePoints[k].lat;
    double lon2 = routePoints[k].lon;
    double dx = (lon2 - lon1) * 111320.0 * cos(lat1 * DEG_TO_RAD);
    double dy = (lat2 - lat1) * 110540.0;
    routeGeometryDistance += sqrt(dx * dx + dy * dy);
    routeCumulativeDistance[k] = routeGeometryDistance;
  }

  if (routeGeometryDistance < 50.0f) {
    linkSend("{\"t\":\"nack\",\"for\":\"route\",\"why\":\"degenerate_route\"}");
    routeReady = false;
    return;
  }

  linkDistScale = 1.0f;
  if (osrmTotalDistance > 100.0f && routeGeometryDistance > 100.0f) {
    linkDistScale = routeGeometryDistance / osrmTotalDistance;
    if (linkDistScale < 0.5f || linkDistScale > 1.5f) linkDistScale = 1.0f;
  }

  navStepCount = 0;
  for (JsonObject s : doc["steps"].as<JsonArray>()) {
    if (navStepCount >= MAX_NAV_STEPS) break;
    NavigationStep& nav = navSteps[navStepCount];
    nav.instruction = s["instr"] | "LANJUT";
    nav.road = s["road"] | "";
    nav.distance = s["dist"] | 0.0f;
    int idx = (s["idx"] | 0) / routeStride;
    if (idx >= routePointCount) idx = routePointCount - 1;
    if (idx < 0) idx = 0;
    nav.routeIndex = idx;
    nav.routeDistance = (s["routeDist"] | 0.0f) * linkDistScale;
    nav.important = s["imp"] | false;
    navStepCount++;
  }

  for (int a = 1; a < navStepCount; a++) {
    NavigationStep key = navSteps[a];
    int b = a - 1;
    while (b >= 0 && navSteps[b].routeDistance > key.routeDistance) {
      navSteps[b + 1] = navSteps[b];
      b--;
    }
    navSteps[b + 1] = key;
  }

  routeReady = true;
  routeRequested = false;
  routeFromLink = true;
  linkRouteReceived = true;
  activeOsrmUrl = "";
  osrmFailCount = 0;
  tripIntroTriggerAsk = false;
  tripActive = true;
  tripArrived = false;
  arrivalActive = false;
  initialMapShown = true;
  destinationAskActive = false;

  simulatedRouteDistance = 0;
  simulatedRouteIndex = 0;
  gpsLastMs = 0;
  activeStep = 0;
  hudLockedStep = -1;
  hudCompletedStep = -1;
  hudActive = false;
  hudShownAt = 0;
  navigationAIStarted = false;
  systemState = STATE_READY;

  speedMps = 8.0f;
  speedLastAt = 0;
  speedLastDist = 0;
  drLastMs = 0;
  lastLinkGpsAt = 0;

  skipStartupSequence();
  tripIntroActive = false;
  tripMapPending = false;

  startMapDisplay();
  mapForNewRoute = true;
  pendingNavigationAI = NAV_AI_ON_ROUTE;

  char buf[128];
  snprintf(buf, sizeof(buf),
           "{\"t\":\"ack\",\"for\":\"route\",\"pts\":%d,\"steps\":%d,\"scale\":%.3f}",
           routePointCount, navStepCount, linkDistScale);
  linkSend(String(buf));
}

float snapToRoute(double lat, double lon, int from, int to, float& along) {
  double cosLat = cos(lat * DEG_TO_RAD);
  float best = 1e9f;
  along = simulatedRouteDistance;
  if (from < 0) from = 0;
  if (to > routePointCount - 2) to = routePointCount - 2;

  for (int i = from; i <= to; i++) {
    double ax = (routePoints[i].lon - lon) * 111320.0 * cosLat;
    double ay = (routePoints[i].lat - lat) * 110540.0;
    double bx = (routePoints[i + 1].lon - lon) * 111320.0 * cosLat;
    double by = (routePoints[i + 1].lat - lat) * 110540.0;
    double dx = bx - ax, dy = by - ay;
    double len2 = dx * dx + dy * dy;
    double t = (len2 > 0.0) ? -(ax * dx + ay * dy) / len2 : 0.0;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    double px = ax + t * dx, py = ay + t * dy;
    float d = (float)sqrt(px * px + py * py);
    if (d < best) {
      best = d;
      along = routeCumulativeDistance[i] + (float)t * (routeCumulativeDistance[i + 1] - routeCumulativeDistance[i]);
    }
  }
  return best;
}

void onLinkGps(JsonDocument& doc) {
  if (!routeReady || routePointCount < 2) return;
  float dist = simulatedRouteDistance;

  if (!doc["progress"].isNull()) {
    float rawP = doc["progress"].as<float>();
    if (isnan(rawP) || isinf(rawP)) return;
    float p = rawP * linkDistScale;
    if (p < simulatedRouteDistance - 40.0f) return;
    if (p < 0.0f) return;
    dist = p;
  } else if (!doc["lat"].isNull() && !doc["lon"].isNull()) {
    double lat = doc["lat"].as<double>();
    double lon = doc["lon"].as<double>();
    if (isnan(lat) || isinf(lat) || isnan(lon) || isinf(lon) || lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return;
    float along = 0;
    int from = simulatedRouteIndex - 2;
    int to = simulatedRouteIndex + 30;
    float off = snapToRoute(lat, lon, from, to, along);
    if (off > OFFROUTE_METERS) off = snapToRoute(lat, lon, 0, routePointCount - 2, along);
    if (off > OFFROUTE_METERS) {
      if (millis() - lastOffRouteReportAt > OFFROUTE_REPORT_MS) {
        lastOffRouteReportAt = millis();
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"t\":\"offroute\",\"lat\":%.6f,\"lon\":%.6f,\"off\":%.0f}",
                 lat, lon, off);
        linkSend(String(buf));
        triggerCharacterReaction(CHARACTER_CONFUSED);
      }
      return;
    }
    if (along < simulatedRouteDistance - 40.0f) return;
    dist = along;
  } else {
    return;
  }

  lastLinkGpsAt = millis();
  drLastMs = millis();
  if (dist < 0) dist = 0;
  if (dist > routeGeometryDistance) dist = routeGeometryDistance;
  updateSpeedEstimate(dist);
  simulatedRouteDistance = dist;
  syncRouteProgress();
  if (simulatedRouteDistance >= routeGeometryDistance - 15.0f) {
    if (tripActive && !tripArrived && !arrivalActive) handleArrival();
  }
}

void onLinkTripStart() {
  markUserActivity();
  skipStartupSequence();
  tripActive = true;
  tripArrived = false;
  arrivalActive = false;
  tripMapPending = false;
  routeReady = false;
  routeRequested = false;
  routePointCount = 0;
  navStepCount = 0;
  simulatedRouteDistance = 0;
  simulatedRouteIndex = 0;
  gpsLastMs = 0;
  activeStep = 0;
  hudLockedStep = -1;
  hudCompletedStep = -1;
  hudActive = false;
  hudShownAt = 0;
  mapRequested = false;
  navigationAIStarted = false;
  pendingNavigationAI = false;
  activeOsrmUrl = "";
  osrmFailCount = 0;
  linkRouteReceived = false;
  routeFromLink = false;
  tripIntroTriggerAsk = false;
  lastLinkGpsAt = 0;
  speedMps = 8.0f;
  speedLastAt = 0;
  drLastMs = 0;
  destinationAskActive = false;
  tripIntroActive = true;
  tripIntroStartedAt = millis();
  setCharacterEmotion(CHARACTER_DEFAULT);
  resetNaturalEyeBehavior();
}

void onLinkTripEnd() {
  tripActive = false;
  tripArrived = false;
  arrivalActive = false;
  tripMapPending = false;
  routeReady = false;
  routeRequested = false;
  routeFromLink = false;
  linkRouteReceived = false;
  mapRequested = false;
  routePointCount = 0;
  navStepCount = 0;
  activeStep = 0;
  hudLockedStep = -1;
  hudCompletedStep = -1;
  simulatedRouteDistance = 0;
  simulatedRouteIndex = 0;
  gpsLastMs = 0;
  lastLinkGpsAt = 0;
  pendingNavigationAI = false;
  navigationAIStarted = false;
  activeOsrmUrl = "";
  hudActive = false;
  initialMapShown = false;
  setCharacterEmotion(CHARACTER_DEFAULT);
  resetNaturalEyeBehavior();
  linkSend("{\"t\":\"trip_ended\",\"route\":0,\"trip\":0}");
}

void onLinkReset() {
  simulatedRouteDistance = 0;
  simulatedRouteIndex = 0;
  gpsLastMs = 0;
  activeStep = 0;
  hudLockedStep = -1;
  hudCompletedStep = -1;
  hudActive = false;
  hudShownAt = 0;
  arrivalActive = false;
  tripArrived = false;
  tripActive = true;
  speedLastAt = 0;
  drLastMs = 0;
}

void onLinkPing() {
  char buf[96];
  snprintf(buf, sizeof(buf),
           "{\"t\":\"pong\",\"ver\":\"" FW_VERSION "\",\"up\":%lu}",
           millis() / 1000UL);
  linkSend(String(buf));
}

// ===========================================================================
//  ROUTING UTILITIES
// ===========================================================================
int decodePolyline(const String& encoded, RoutePoint* out, int maxPoints) {
  int idx = 0, count = 0, lat = 0, lng = 0;
  int len = encoded.length();
  while (idx < len && count < maxPoints) {
    int b, shift, result;
    shift = 0;
    result = 0;
    do {
      if (idx >= len) return count;
      b = encoded.charAt(idx++) - 63;
      result |= (b & 0x1f) << shift;
      shift += 5;
    } while (b >= 0x20);
    int dlat = ((result & 1) ? ~(result >> 1) : (result >> 1));
    lat += dlat;
    shift = 0;
    result = 0;
    do {
      if (idx >= len) return count;
      b = encoded.charAt(idx++) - 63;
      result |= (b & 0x1f) << shift;
      shift += 5;
    } while (b >= 0x20);
    int dlng = ((result & 1) ? ~(result >> 1) : (result >> 1));
    lng += dlng;
    out[count].lat = lat / 1e5;
    out[count].lon = lng / 1e5;
    count++;
  }
  return count;
}

String buildOsrmUrl(float fromLat, float fromLon, float toLat, float toLon) {
  String url = "http://router.project-osrm.org/route/v1/driving/";
  url += String(fromLon, 6) + "," + String(fromLat, 6) + ";";
  url += String(toLon, 6) + "," + String(toLat, 6);
  url += "?overview=simplified&geometries=polyline&steps=true";
  return url;
}

// ===========================================================================
//  PERSONALITY & EYES
// ===========================================================================
const char* abotPersonalityName(AbotPersonality p) {
  switch (p) {
    case ABOT_MORNING: return "PAGI / SEMANGAT";
    case ABOT_DAY: return "SIANG / LELAH";
    case ABOT_AFTERNOON: return "SORE / LEMAS";
    case ABOT_NIGHT: return "MALAM / NGANTUK";
    default: return "NETRAL";
  }
}

AbotPersonality abotPersonalityByHour(int hour) {
  if (hour >= 5 && hour < 11) return ABOT_MORNING;
  if (hour >= 11 && hour < 16) return ABOT_DAY;
  if (hour >= 16 && hour < 19) return ABOT_AFTERNOON;
  return ABOT_NIGHT;
}

EyeMood abotEyeFor(AbotPersonality p) {
  switch (p) {
    case ABOT_MORNING: return EYE_HAPPY;
    case ABOT_DAY: return EYE_TIRED;
    case ABOT_AFTERNOON: return EYE_TIRED;
    case ABOT_NIGHT: return EYE_TIRED;
    default: return EYE_DEFAULT;
  }
}

String abotPromptHint() {
  switch (currentAbotPersonality) {
    case ABOT_MORNING: return " Suasana pagi: nada kamu bersemangat dan ceria.";
    case ABOT_DAY: return " Suasana siang: nada kamu agak lelah tapi tetap hangat.";
    case ABOT_AFTERNOON: return " Suasana sore: nada kamu santai dan sedikit lemas tapi tetap ramah.";
    case ABOT_NIGHT: return " Suasana malam: nada kamu tenang dan sedikit mengantuk tapi tetap hangat.";
    default: return "";
  }
}

void updateAbotPersonality() {
  struct tm ti;
  AbotPersonality p = getLocalTimeSafe(&ti)
                        ? abotPersonalityByHour(ti.tm_hour)
                        : ABOT_NEUTRAL;
  currentAbotPersonality = p;

  if (!introductionFinished || bootTransitionActive || sleepModeActive || listeningActive || voicePipelineActive || companionActive || companionGenerating || companionNextMenuActive || companionNextMenuPending || conversationActive || mapRequested || hudActive || arrivalActive || startupOnboardingActive || infoModeActive || destinationAskActive || tripIntroActive || systemState == STATE_THINKING || characterReactionActive || millis() < abotHoldUntil || otaActive) {
    return;
  }

  if (appliedAbotPersonality != p) {
    appliedAbotPersonality = p;
    abotLog("INFO", "Personality: %s", abotPersonalityName(p));
  }

  EyeMood desired = abotEyeFor(p);
  if (currentEyeMood == desired) return;
  setEyeMood(desired);
  currentCharacterEmotion =
    (desired == EYE_HAPPY) ? CHARACTER_HAPPY : (desired == EYE_TIRED) ? CHARACTER_TIRED
                                                                      : CHARACTER_DEFAULT;
}

void setEyeMood(EyeMood mood) {
  if (currentEyeMood == mood) return;
  currentEyeMood = mood;
  switch (mood) {
    case EYE_DEFAULT: roboEyes.setMood(DEFAULT); break;
    case EYE_HAPPY: roboEyes.setMood(HAPPY); break;
    case EYE_ANGRY: roboEyes.setMood(ANGRY); break;
    case EYE_TIRED: roboEyes.setMood(TIRED); break;
  }
}

void setCharacterEmotion(CharacterEmotion emotion) {
  currentCharacterEmotion = emotion;
  characterReactionActive = false;
  characterReactionUntil = 0;
  switch (emotion) {
    case CHARACTER_DEFAULT: setEyeMood(EYE_DEFAULT); break;
    case CHARACTER_LISTENING: setEyeMood(EYE_HAPPY); break;
    case CHARACTER_THINKING: setEyeMood(EYE_DEFAULT); break;
    case CHARACTER_HAPPY: setEyeMood(EYE_HAPPY); break;
    case CHARACTER_LAUGH:
      setEyeMood(EYE_HAPPY);
      roboEyes.anim_laugh();
      break;
    case CHARACTER_CONFUSED:
      setEyeMood(EYE_DEFAULT);
      roboEyes.anim_confused();
      break;
    case CHARACTER_ANGRY: setEyeMood(EYE_ANGRY); break;
    case CHARACTER_TIRED: setEyeMood(EYE_TIRED); break;
  }
}

void triggerCharacterReaction(CharacterEmotion emotion) {
  characterReactionActive = true;
  currentCharacterEmotion = emotion;
  switch (emotion) {
    case CHARACTER_HAPPY:
      setEyeMood(EYE_HAPPY);
      characterReactionUntil = millis() + CHARACTER_REACTION_HAPPY_MS;
      break;
    case CHARACTER_LAUGH:
      setEyeMood(EYE_HAPPY);
      roboEyes.anim_laugh();
      characterReactionUntil = millis() + CHARACTER_REACTION_LAUGH_MS;
      break;
    case CHARACTER_CONFUSED:
      setEyeMood(EYE_DEFAULT);
      roboEyes.anim_confused();
      characterReactionUntil = millis() + CHARACTER_REACTION_CONFUSED_MS;
      break;
    case CHARACTER_ANGRY:
      setEyeMood(EYE_ANGRY);
      characterReactionUntil = millis() + CHARACTER_REACTION_ANGRY_MS;
      break;
    default:
      setCharacterEmotion(emotion);
      break;
  }
}

void updateCharacterEmotion() {
  if (!characterReactionActive) return;
  if (millis() < characterReactionUntil) return;
  characterReactionActive = false;
  characterReactionUntil = 0;
  if (listeningActive) {
    setCharacterEmotion(CHARACTER_LISTENING);
    return;
  }
  if (companionGenerating || systemState == STATE_THINKING || voicePipelineActive) {
    setCharacterEmotion(CHARACTER_THINKING);
    return;
  }
  setCharacterEmotion(CHARACTER_DEFAULT);
}

void resetNaturalEyeBehavior() {
  unsigned long now = millis();
  naturalEyeGazeActive = false;
  naturalEyeReturnAt = 0;
  naturalEyeNextActionAt = now + NATURAL_EYE_FIRST_ACTION_MS;
  naturalEyeNextWinkAt = now + NATURAL_EYE_WINK_INTERVAL_MS;
  naturalEyeSequence = 0;
  roboEyes.setPosition(DEFAULT);
}

void triggerNaturalWink() {
  roboEyes.blink(0, 1);
  naturalEyeNextWinkAt =
    millis() + NATURAL_EYE_WINK_INTERVAL_MS + (naturalEyeSequence % 3) * 3000;
}

void updateNaturalEyeBehavior() {
  if (!naturalEyeBehaviorEnabled) return;
  if (!introductionFinished || bootTransitionActive || sleepModeActive || listeningActive || voicePipelineActive || companionActive || companionGenerating || companionNextMenuActive || mapRequested || hudActive || arrivalActive || startupOnboardingActive || infoModeActive || destinationAskActive || tripIntroActive || conversationActive || systemState == STATE_THINKING || characterReactionActive || otaActive) return;

  if (naturalEyeGazeActive) {
    if (millis() >= naturalEyeReturnAt) {
      naturalEyeGazeActive = false;
      roboEyes.setPosition(DEFAULT);
      naturalEyeNextActionAt = millis() + NATURAL_EYE_ACTION_INTERVAL_MS;
    }
    return;
  }
  if (millis() >= naturalEyeNextWinkAt) {
    triggerNaturalWink();
    return;
  }
  if (millis() < naturalEyeNextActionAt) return;

  switch (naturalEyeSequence % 3) {
    case 0: roboEyes.setPosition(OJOL_POS_W); break;
    case 1: roboEyes.setPosition(OJOL_POS_E); break;
    default: roboEyes.setPosition(OJOL_POS_N); break;
  }
  naturalEyeSequence++;
  naturalEyeGazeActive = true;
  naturalEyeReturnAt = millis() + NATURAL_EYE_GAZE_DURATION_MS;
}

void updateThinkingGaze() {
  if (!thinkingVisualPrepared) return;
  unsigned long now = millis();
  if (now < thinkingGazeNextAt) return;
  switch (thinkingGazeStep % 3) {
    case 0: roboEyes.setPosition(OJOL_POS_NW); break;
    case 1: roboEyes.setPosition(OJOL_POS_NE); break;
    default: roboEyes.setPosition(DEFAULT); break;
  }
  thinkingGazeStep++;
  thinkingGazeNextAt = now + 1200;
}

void renderEyes() {
  roboEyes.update();
}

// ===========================================================================
//  SLEEP MODE
// ===========================================================================
void markUserActivity() {
  lastUserActivityMs = millis();
  if (sleepModeActive) wakeFromSleep();
}

void enterSleepMode() {
  if (sleepModeActive) return;
  sleepModeActive = true;
  currentEyeMood = EYE_DEFAULT;
  roboEyes.setPosition(DEFAULT);
  roboEyes.setMood(DEFAULT);
  roboEyes.setAutoblinker(OFF, 0, 0);
  roboEyes.setIdleMode(OFF, 0, 0);
  roboEyes.close();
  naturalEyeGazeActive = false;
  naturalEyeNextActionAt = millis() + NATURAL_EYE_ACTION_INTERVAL_MS;
  Serial.println("[SLEEP] 20s idle -> SLEEP MODE");
}

void wakeFromSleep() {
  if (!sleepModeActive) {
    lastUserActivityMs = millis();
    return;
  }
  sleepModeActive = false;
  lastUserActivityMs = millis();
  currentEyeMood = EYE_DEFAULT;
  roboEyes.setMood(DEFAULT);
  roboEyes.setPosition(DEFAULT);
  roboEyes.open();
  roboEyes.setAutoblinker(ON, 3, 2);
  roboEyes.setIdleMode(ON, 2, 1);
  resetNaturalEyeBehavior();
  currentCharacterEmotion = CHARACTER_DEFAULT;
  characterReactionActive = false;
  characterReactionUntil = 0;
  Serial.println("[SLEEP] WAKE UP");
}

void updateSleepMode() {
  if (!introductionFinished) return;
  bool safeForSleep =
    !bootTransitionActive && !listeningActive && !voicePipelineActive && !startupOnboardingActive && !destinationAskActive && !infoModeActive && !tripIntroActive && !companionGenerating && !companionActive && !companionNextMenuActive && !companionNextMenuPending && !conversationActive && systemState != STATE_THINKING && !mapRequested && !hudActive && !arrivalActive && !characterReactionActive && !otaActive && !musicScreenWanted();
  if (!safeForSleep) {
    lastUserActivityMs = millis();
    if (sleepModeActive) wakeFromSleep();
    return;
  }
  if (!sleepModeActive && millis() - lastUserActivityMs >= SLEEP_IDLE_TIMEOUT) {
    enterSleepMode();
  }
}

void renderSleep() {
  roboEyes.update();
}

// ===========================================================================
//  LISTENING / THINKING VISUALS
// ===========================================================================
void renderListening() {
  roboEyes.update();
}

void beginThinkingVisual() {
  if (thinkingVisualPrepared) return;
  roboEyes.setAutoblinker(ON, 3, 2);
  roboEyes.setIdleMode(ON, 2, 1);
  setCharacterEmotion(CHARACTER_THINKING);
  overlayListening = false;
  overlayThinking = true;
  thinkingGazeStep = 0;
  thinkingGazeNextAt = millis();
  roboEyes.setPosition(OJOL_POS_NW);
  thinkingVisualPrepared = true;
}

void endThinkingVisual() {
  if (!thinkingVisualPrepared) return;
  roboEyes.setAutoblinker(ON, 3, 2);
  roboEyes.setIdleMode(ON, 2, 1);
  overlayThinking = false;
  thinkingVisualPrepared = false;
  thinkingGazeStep = 0;
  thinkingGazeNextAt = 0;
  roboEyes.setPosition(DEFAULT);
  setCharacterEmotion(CHARACTER_DEFAULT);
  roboEyes.blink();
}

void cancelThinking() {
  companionGenerating = false;
  companionRequestCounter++;
  activeCompanionRequestId = companionRequestCounter;
  thinkingDemoUntil = 0;
  if (systemState == STATE_THINKING) systemState = STATE_READY;
  endThinkingVisual();
}

void renderThinking() {
  roboEyes.update();
  updateThinkingGaze();
}

void startListening() {
  if (!audioMicReady) {
    Serial.println("[VOICE] Mic tidak aktif — TALK tidak memulai rekaman");
    triggerCharacterReaction(CHARACTER_CONFUSED);
    return;
  }
  if (ttsActive) {
    ttsStop = true;
    Serial.println("[VOICE] TTS dihentikan (barge-in), tahan TALK lagi untuk bicara");
    return;
  }
  if (voicePipelineActive) {
    ttsStop = true;
    Serial.println("[VOICE] Pipeline sibuk — tunggu selesai");
    return;
  }

  mapRequested = false;
  hudActive = false;
  companionActive = false;
  companionNextMenuActive = false;
  companionNextMenuPending = false;
  cancelThinking();

  musicStopForVoice();
  conversationActive = true;
  listeningActive = true;
  listeningState = LISTENING_ACTIVE;
  overlayListening = true;

  setCharacterEmotion(CHARACTER_LISTENING);
  sfxPlay(SFX_REC_START);
  audioStartRecording();
  Serial.println("[LISTENING] ACTIVE (mic merekam)");
}

void stopListening() {
  if (!listeningActive) return;
  listeningActive = false;
  overlayListening = false;
  listeningState = LISTENING_IDLE;
  conversationActive = false;
  sfxPlay(SFX_REC_STOP);
  audioStopRecording();
  setCharacterEmotion(CHARACTER_DEFAULT);
  Serial.println("[LISTENING] STOP");
}

// ===========================================================================
//  DESTINATION STUBS
// ===========================================================================
void startGeocode(const String& placeName) {
  destinationResolved = false;
  destinationStage = DEST_FAILED;
  destinationStageStarted = millis();
  Serial.print("[GEOCODE] Preset lokal dinonaktifkan; tunggu tujuan dari HP: ");
  Serial.println(placeName);
}

void startDestinationAsk() {
  destinationAskActive = false;
  destinationResolved = false;
  Serial.println("[ABOT] Menunggu rute dari aplikasi HP.");
}

void finishDestinationAsk() {
  destinationAskActive = false;
  destinationStage = DEST_ASK;
}

void renderDestinationAsk() {
  destinationAskActive = false;
}

// ===========================================================================
//  BUTTONS
// ===========================================================================
void startActionButtonFunction() {
  if (listeningActive || voicePipelineActive) return;
  companionActive = false;
  companionNextMenuActive = false;
  companionNextMenuPending = false;
  conversationActive = false;
  hudActive = false;
  cancelThinking();
  infoModeActive = false;
  mapRequested = false;
  musicRequestPost(MCMD_TOGGLE, "", "");
  Serial.println("[BUTTON ACTION] SHORT -> MUSIK");
}

void startActionCompanionRandom() {
  if (destinationAskActive) return;
  CompanionType types[4] = {
    COMPANION_HUMOR, COMPANION_QUIZ, COMPANION_TRIVIA, COMPANION_STORY
  };
  CompanionType selected = types[random(0, 4)];
  startCompanion(selected);
}

void updateButtons() {
  const unsigned long now = millis();

  bool talkReading = digitalRead(BUTTON_TALK);
  if (talkReading != talkLastReading) {
    talkLastChange = now;
    talkLastReading = talkReading;
  }

  if (now - talkLastChange >= BUTTON_DEBOUNCE_MS && talkReading != talkStableState) {
    talkStableState = talkReading;
    if (talkStableState == LOW) {
      markUserActivity();
      talkPressedAt = now;
      talkLongTriggered = false;
      sfxPlay(SFX_CLICK);
    } else {
      unsigned long held = now - talkPressedAt;
      if (talkLongTriggered && listeningActive) stopListening();
      else if (!talkLongTriggered && held < BUTTON_HOLD_MS) {
        startInfoMode();
      }
    }
  }

  if (talkStableState == LOW && !talkLongTriggered && now - talkPressedAt >= BUTTON_HOLD_MS) {
    talkLongTriggered = true;
    startListening();
    Serial.println(listeningActive ? "[BUTTON TALK] HOLD CONFIRMED (rekam)"
                                   : "[BUTTON TALK] HOLD gagal");
  }

  bool actionReading = digitalRead(BUTTON_ACTION);
  if (actionReading != actionLastReading) {
    actionLastChange = now;
    actionLastReading = actionReading;
  }

  if (now - actionLastChange >= BUTTON_DEBOUNCE_MS && actionReading != actionStableState) {
    actionStableState = actionReading;
    if (actionStableState == LOW) {
      markUserActivity();
      actionPressedAt = now;
      sfxPlay(SFX_CLICK);
    } else {
      unsigned long held = now - actionPressedAt;
      if (listeningActive) return;
      if (held < BUTTON_HOLD_MS) startActionButtonFunction();
      else if (musicHasSession()) musicRequestPost(MCMD_NEXT, "", "");
      else startActionCompanionRandom();
    }
  }
}

// ===========================================================================
//  SYSTEM HEALTH
// ===========================================================================
void ensureWiFiConnected() {
  if (millis() - lastWiFiCheck < WIFI_CHECK_INTERVAL_MS) return;
  lastWiFiCheck = millis();
  if (WiFi.status() == WL_CONNECTED) {
    if (!wifiConnected) {
      wifiConnected = true;
      configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
      abotLog("INFO", "WiFi tersambung");
      sfxPlay(SFX_WIFI_UP);
    }
    return;
  }
  if (wifiConnected) {
    wifiConnected = false;
    abotLog("WARN", "WiFi koneksi terputus");
    sfxPlay(SFX_WIFI_DOWN);
  }
  if (millis() - lastWiFiAttempt < WIFI_RETRY_INTERVAL_MS) return;
  lastWiFiAttempt = millis();
  Serial.println("[WIFI] Mencoba reconnect...");
  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void checkHeapHealth() {
  if (voicePipelineActive || ttsActive) return;
  if (millis() - lastHeapCheck < HEAP_CHECK_INTERVAL_MS) return;
  lastHeapCheck = millis();
  uint32_t freeHeap = ESP.getFreeHeap();
  Serial.printf("[HEAP] Free: %u bytes\n", freeHeap);
  if (freeHeap < HEAP_CRITICAL_BYTES) {
    abotLog("ERR", "HEAP KRITIS -> restart terkontrol");
    delay(200);
    ESP.restart();
  }
}

// ===========================================================================
//  BOOT & INTRODUCTION
// ===========================================================================
void renderWifiConnecting() {
  beginFrame();
  drawCenteredText("MENGHUBUNGKAN", 24, 1);
  drawCenteredText("WIFI...", 36, 1);
  presentFrame();
}

void startBootTransition() {
  bootTransitionActive = true;
  bootTransitionStartedAt = millis();
  roboEyes.setAutoblinker(OFF, 0, 0);
  roboEyes.setIdleMode(OFF, 0, 0);
  sfxPlay(SFX_BOOT);
  Serial.println("[BOOT] Selesai -> sapaan Abot");
}

void renderBootTransition() {
  unsigned long elapsed = millis() - bootTransitionStartedAt;
  beginFrame();
  drawCenteredText("ABOT", 20, 2);
  drawCenteredText(wifiConnected ? "ONLINE" : "OFFLINE", 47, 1);
  presentFrame();
  if (elapsed >= BOOT_TRANSITION_MS) {
    bootTransitionActive = false;
    startIntroduction();
  }
}

void startIntroduction() {
  introductionFinished = false;
  wakeIntroStage = WAKE_DARK;
  wakeIntroStageStartedAt = millis();
  beginFrame();
  presentFrame();
  roboEyes.setAutoblinker(OFF, 0, 0);
  roboEyes.setIdleMode(OFF, 0, 0);
  roboEyes.setMood(DEFAULT);
  roboEyes.setPosition(DEFAULT);
  currentEyeMood = EYE_DEFAULT;
  Serial.println("[ABOT] Perkenalan dimulai");
}

void renderIntroduction() {
  unsigned long now = millis();
  unsigned long elapsed = now - wakeIntroStageStartedAt;
  beginFrame();
  gfxFillRect(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_WHITE);
  gfxBitmap(0, 0, abotIntroBitmap, 128, 64, COLOR_BLACK);
  presentFrame();

  if (wakeIntroStage == WAKE_DARK) {
    if (elapsed >= WAKE_DARK_MS) {
      wakeIntroStage = WAKE_ABOT_HELLO;
      wakeIntroStageStartedAt = now;
    }
    return;
  }
  if (wakeIntroStage == WAKE_ABOT_HELLO) {
    if (elapsed >= WAKE_ABOT_HELLO_MS) {
      wakeIntroStage = WAKE_ABOT_PERSONALITY;
      wakeIntroStageStartedAt = now;
    }
    return;
  }
  if (wakeIntroStage == WAKE_ABOT_PERSONALITY) {
    updateAbotPersonality();
    if (elapsed >= WAKE_ABOT_PERSONALITY_MS) {
      wakeIntroStage = WAKE_FINISHED;
      introductionFinished = true;
      roboEyes.setAutoblinker(ON, 3, 2);
      roboEyes.setIdleMode(ON, 2, 1);
      resetNaturalEyeBehavior();
      lastUserActivityMs = millis();
      if (!startupOnboardingCompleted) {
        startupOnboardingManual = false;
        startStartupOnboarding();
      } else if (routeReady) {
        startMapDisplay();
      }
    }
    return;
  }
  roboEyes.update();
}

void startTripIntroduction() {
  tripIntroActive = true;
  tripIntroStartedAt = millis();
  updateAbotPersonality();
  setCharacterEmotion(CHARACTER_DEFAULT);
  resetNaturalEyeBehavior();
}

void renderTripIntroduction() {
  if (!tripIntroActive) return;
  unsigned long elapsed = millis() - tripIntroStartedAt;
  beginFrame();
  switch (currentAbotPersonality) {
    case ABOT_MORNING:
      drawCenteredText("PAGI!", 12, 1);
      drawCenteredText("ABOT TEMENIN LAGI", 32, 1);
      drawCenteredText("SEMANGAT!", 51, 1);
      break;
    case ABOT_DAY:
      drawCenteredText("HAI LAGI...", 12, 1);
      drawCenteredText("ABOT TEMENIN LAGI", 32, 1);
      drawCenteredText("JANGAN JAUH-JAUH YA", 51, 1);
      break;
    case ABOT_AFTERNOON:
      drawCenteredText("SORE...", 12, 1);
      drawCenteredText("KITA JALAN LAGI", 32, 1);
      drawCenteredText("ABOT MASIH KUAT", 51, 1);
      break;
    case ABOT_NIGHT:
      drawCenteredText("MALAM...", 12, 1);
      drawCenteredText("KITA JALAN LAGI", 32, 1);
      drawCenteredText("ABOT TEMENIN KAMU", 51, 1);
      break;
    default:
      drawCenteredText("HAI!", 12, 1);
      drawCenteredText("KITA JALAN LAGI", 32, 1);
      drawCenteredText("ABOT TEMENIN KAMU", 51, 1);
      break;
  }
  presentFrame();

  unsigned long introMs = hpConnected ? TRIP_INTRO_LINK_DURATION
                                      : TRIP_INTRO_DURATION;
  if (elapsed >= introMs) {
    tripIntroActive = false;
    if (tripIntroTriggerAsk) {
      tripIntroTriggerAsk = false;
      if (!hpConnected && !routeReady) startDestinationAsk();
    }
    if (tripMapPending && routeReady) {
      tripMapPending = false;
      startMapDisplay();
      pendingNavigationAI = NAV_AI_ON_ROUTE;
    }
  }
}

// ===========================================================================
//  CLOCK, WEATHER, INFO
// ===========================================================================
void renderClockScreen(const String& header) {
  beginFrame();
  drawCenteredText(header, 3, 1);
  gfxLine(12, 14, 115, 14);
  struct tm ti;
  if (getLocalTimeSafe(&ti)) {
    char clockText[10];
    char dateText[28];
    snprintf(clockText, sizeof(clockText), "%02d:%02d:%02d",
             ti.tm_hour, ti.tm_min, ti.tm_sec);
    snprintf(dateText, sizeof(dateText), "%s, %d %s",
             HARI_NAMA[ti.tm_wday], ti.tm_mday, BULAN_NAMA[ti.tm_mon]);
    drawCenteredText(String(clockText), 24, 2);
    drawCenteredText(String(dateText), 49, 1);
  } else {
    drawCenteredText("--:--:--", 24, 2);
    drawCenteredText("WAKTU BELUM SIAP", 49, 1);
  }
  presentFrame();
}

String weatherCodeToText(int code) {
  if (code == 0) return "CERAH";
  if (code == 1 || code == 2) return "CERAH BERAWAN";
  if (code == 3) return "BERAWAN";
  if (code == 45 || code == 48) return "BERKABUT";
  if (code >= 51 && code <= 57) return "GERIMIS";
  if (code >= 61 && code <= 67) return "HUJAN";
  if (code >= 71 && code <= 77) return "SALJU";
  if (code >= 80 && code <= 82) return "HUJAN";
  if (code >= 85 && code <= 86) return "SALJU";
  if (code >= 95 && code <= 99) return "BADAI";
  return "TIDAK DIKETAHUI";
}

void ensureWeatherData() {
  if (!introductionFinished || networkIsBusy() || weatherFetching || !wifiConnected) return;
  unsigned long interval = weatherData.valid ? WEATHER_REFRESH_MS
                                             : WEATHER_RETRY_MS;
  if (lastWeatherAttempt == 0 || millis() - lastWeatherAttempt >= interval) {
    startWeatherFetch();
  }
}

void renderWeatherScreen(const String& header) {
  const WeatherData& w = weatherViewCustom ? cityWeather : weatherData;
  String title = weatherViewCustom ? weatherViewLabel : (header + " " + String(HOME_CITY));
  title = stripNonAscii(title);
  title.toUpperCase();
  if (title.length() > 20) title = title.substring(0, 20);
  beginFrame();
  drawCenteredText(title, 1, 1);
  gfxLine(10, 12, 117, 12);
  if (!w.valid) {
    if (weatherFetching) drawCenteredText("MEMUAT CUACA...", 30, 1);
    else {
      drawCenteredText("CUACA OFFLINE", 23, 1);
      drawCenteredText("DATA BELUM TERSEDIA", 38, 1);
      drawCenteredText("COBA LAGI NANTI", 53, 1);
    }
    presentFrame();
    return;
  }
  String temp = String(w.temperature, 1) + " C";
  String range = "H:" + String(w.tempMax, 0) + "  L:" + String(w.tempMin, 0);
  drawCenteredText(temp, 18, 2);
  drawCenteredText(w.condition, 42, 1);
  drawCenteredText(range, 56, 1);
  presentFrame();
}

void startInfoMode() {
  if (destinationAskActive) return;
  mapRequested = false;
  hudActive = false;
  companionActive = false;
  companionNextMenuActive = false;
  companionNextMenuPending = false;
  conversationActive = false;
  weatherViewCustom = false;
  infoModeActive = true;
  infoPage = 0;
  infoModeStarted = millis();
  if (!weatherData.valid) lastWeatherAttempt = 0;
}

void renderInfoMode() {
  if (!infoModeActive) return;
  unsigned long elapsed = millis() - infoModeStarted;
  if (infoPage == 0) {
    renderClockScreen("JAM");
    if (elapsed >= INFO_CLOCK_DURATION) {
      infoPage = 1;
      infoModeStarted = millis();
    }
  } else {
    renderWeatherScreen("CUACA");
    if (elapsed >= INFO_WEATHER_DURATION) {
      infoModeActive = false;
      infoPage = 0;
      weatherViewCustom = false;
    }
  }
}

// ===========================================================================
//  STARTUP ONBOARDING
// ===========================================================================
void startStartupOnboarding() {
  startupOnboardingActive = true;
  startupStage = STARTUP_USAGE;
  startupStageStarted = millis();
  infoModeActive = false;
  mapRequested = false;
  hudActive = false;
  companionActive = false;
  conversationActive = false;
  cancelThinking();
}

void finishStartupOnboarding() {
  startupOnboardingCompleted = true;
  startupOnboardingActive = false;
  startupStage = STARTUP_NORMAL;
  introductionFinished = true;
  infoModeActive = false;
  resetNaturalEyeBehavior();
  if (!startupOnboardingManual) {
    if (hpConnected) {
    } else {
      startDestinationAsk();
    }
  }
  startupOnboardingManual = false;
}

void renderStartupOnboarding() {
  if (!startupOnboardingActive) return;
  unsigned long elapsed = millis() - startupStageStarted;
  beginFrame();
  bool fastLink = hpConnected && !startupOnboardingManual;

  if (startupStage == STARTUP_USAGE) {
    drawCenteredText("CARA MENGGUNAKAN", 1, 1);
    gfxLine(10, 13, 117, 13);
    gfxText(2, 20, "TALK tahan=bicara");
    gfxText(2, 32, "TALK tekan=jam/cuaca");
    gfxText(2, 44, "ACTION tekan=MUSIK");
    gfxText(2, 56, "ACTION tahan=hiburan");
    presentFrame();
    unsigned long usageMs = fastLink ? STARTUP_USAGE_LINK_MS
                                     : STARTUP_USAGE_DURATION;
    if (elapsed >= usageMs) {
      if (fastLink) {
        finishStartupOnboarding();
        return;
      }
      startupStage = STARTUP_CLOCK;
      startupStageStarted = millis();
    }
    return;
  }
  if (startupStage == STARTUP_CLOCK) {
    renderClockScreen("JAM");
    if (elapsed >= STARTUP_CLOCK_DURATION) {
      startupStage = STARTUP_WEATHER;
      startupStageStarted = millis();
    }
    return;
  }
  if (startupStage == STARTUP_WEATHER) {
    renderWeatherScreen("CUACA");
    if (elapsed >= STARTUP_WEATHER_DURATION) finishStartupOnboarding();
  }
}

// ===========================================================================
//  MAP & HUD
// ===========================================================================
void startMapDisplay() {
  mapRequested = true;
  mapStartTime = millis();
  mapForNewRoute = false;
  if (routeReady) hudActive = false;
}

bool isImportantStep(const NavigationStep& step) {
  return step.important;
}

int nextImportantStep() {
  for (int i = 0; i < navStepCount; i++) {
    if (!isImportantStep(navSteps[i])) continue;
    if (navSteps[i].routeDistance < simulatedRouteDistance - HUD_PASSED_MARGIN_M) continue;
    return i;
  }
  return -1;
}

void drawMapVehicle(int x, int y) {
  gfxFillTriangle(x, y - 4, x - 3, y + 3, x + 3, y + 3);
  gfxPixel(x, y + 4);
}

void drawMapDestination(int x, int y) {
  gfxFrame(x - 2, y - 2, 5, 5);
  gfxPixel(x, y);
}

void drawMiniTurnIcon(const String& instruction, int cx, int cy) {
  String text = instruction;
  text.toUpperCase();
  if (text.indexOf("KIRI") >= 0 || text.indexOf("LEFT") >= 0) {
    gfxLine(cx + 5, cy, cx - 4, cy);
    gfxLine(cx - 4, cy, cx, cy - 4);
    gfxLine(cx - 4, cy, cx, cy + 4);
  } else if (text.indexOf("KANAN") >= 0 || text.indexOf("RIGHT") >= 0) {
    gfxLine(cx - 5, cy, cx + 4, cy);
    gfxLine(cx + 4, cy, cx, cy - 4);
    gfxLine(cx + 4, cy, cx, cy + 4);
  } else if (text.indexOf("PUTAR") >= 0 || text.indexOf("U-TURN") >= 0) {
    gfxCircle(cx, cy, 5);
    gfxLine(cx - 4, cy - 3, cx - 1, cy - 5, COLOR_BLACK);
    gfxPixel(cx, cy - 5, COLOR_BLACK);
  } else {
    gfxLine(cx, cy + 5, cx, cy - 5);
    gfxLine(cx, cy - 5, cx - 3, cy - 2);
    gfxLine(cx, cy - 5, cx + 3, cy - 2);
  }
}

void drawMapHeader() {
  gfxText(0, 0, "NAV");
  if (osrmTotalDistance > 0) {
    gfxText(38, 0, String(osrmTotalDistance / 1000.0, 1) + "k");
  }
  gfxText(91, 0, String(osrmTotalDuration / 60.0, 0) + "m");
  gfxLine(0, 9, 127, 9);
}

void drawMapFooter() {
  gfxLine(0, 52, 127, 52);
  int s = nextImportantStep();
  if (s >= 0 && s < navStepCount) {
    NavigationStep& step = navSteps[s];
    float distanceToTurn = step.routeDistance - simulatedRouteDistance;
    if (distanceToTurn < 0) distanceToTurn = 0;
    drawMiniTurnIcon(step.instruction, 7, 59);
    String label = (distanceToTurn >= 1000.0f)
                     ? String(distanceToTurn / 1000.0f, 1) + "k "
                     : String((int)distanceToTurn) + "m ";
    String instruction = step.instruction;
    if (instruction.length() > 11) instruction = instruction.substring(0, 11);
    gfxText(16, 55, label + instruction);
  } else {
    gfxText(16, 55, "RUTE AKTIF");
  }
}

void renderMap() {
  beginFrame();
  if (!routeReady || routePointCount < 2) {
    drawMapHeader();
    drawCenteredText("MENCARI RUTE", 28, 1);
    presentFrame();
    return;
  }
  const int centerX = 64;
  const int centerY = 42;
  const float pixelsPerMeter = 0.13f;
  const float behindMeters = 90.0f;
  const float aheadMeters = 280.0f;

  int seg = 0;
  while (seg < routePointCount - 2 && routeCumulativeDistance[seg + 1] < simulatedRouteDistance) seg++;
  float segStart = routeCumulativeDistance[seg];
  float segEnd = routeCumulativeDistance[seg + 1];
  float t = (segEnd > segStart)
              ? (simulatedRouteDistance - segStart) / (segEnd - segStart)
              : 0.0f;
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;

  double vehicleLat = routePoints[seg].lat + (routePoints[seg + 1].lat - routePoints[seg].lat) * t;
  double vehicleLon = routePoints[seg].lon + (routePoints[seg + 1].lon - routePoints[seg].lon) * t;
  double cosLat = cos(vehicleLat * DEG_TO_RAD);

  double hdx = (routePoints[seg + 1].lon - routePoints[seg].lon) * 111320.0 * cosLat;
  double hdy = (routePoints[seg + 1].lat - routePoints[seg].lat) * 110540.0;
  double hlen = sqrt(hdx * hdx + hdy * hdy);
  double sinB = 0.0, cosB = 1.0;
  if (hlen > 0.5) {
    sinB = hdx / hlen;
    cosB = hdy / hlen;
  }

  auto toScreen = [&](double lat, double lon, int& sx, int& sy) {
    double e = (lon - vehicleLon) * 111320.0 * cosLat;
    double n = (lat - vehicleLat) * 110540.0;
    double forward = e * sinB + n * cosB;
    double right = e * cosB - n * sinB;
    sx = centerX + (int)lround(right * pixelsPerMeter);
    sy = centerY - (int)lround(forward * pixelsPerMeter);
  };

  for (int i = 1; i < routePointCount; i++) {
    float d1 = routeCumulativeDistance[i - 1];
    float d2 = routeCumulativeDistance[i];
    if (d2 < simulatedRouteDistance - behindMeters) continue;
    if (d1 > simulatedRouteDistance + aheadMeters) break;
    int x1, y1, x2, y2;
    toScreen(routePoints[i - 1].lat, routePoints[i - 1].lon, x1, y1);
    toScreen(routePoints[i].lat, routePoints[i].lon, x2, y2);
    gfxLine(x1, y1, x2, y2);
  }

  {
    int dx, dy;
    toScreen(routePoints[routePointCount - 1].lat,
             routePoints[routePointCount - 1].lon, dx, dy);
    if (dx >= 3 && dx <= 124 && dy >= 13 && dy <= 49) drawMapDestination(dx, dy);
  }

  gfxFillRect(0, 0, SCREEN_WIDTH, 10, COLOR_BLACK);
  gfxFillRect(0, 52, SCREEN_WIDTH, SCREEN_HEIGHT - 52, COLOR_BLACK);

  drawMapVehicle(centerX, centerY);
  drawMapHeader();
  drawMapFooter();
  presentFrame();
}

void drawTurnArrow(const String& instruction) {
  String text = instruction;
  text.toUpperCase();
  int cx = 64, cy = 34;
  if (text.indexOf("KIRI") >= 0 || text.indexOf("LEFT") >= 0) {
    gfxLine(cx + 18, cy, cx - 12, cy);
    gfxLine(cx - 12, cy, cx - 2, cy - 8);
    gfxLine(cx - 12, cy, cx - 2, cy + 8);
  } else if (text.indexOf("KANAN") >= 0 || text.indexOf("RIGHT") >= 0) {
    gfxLine(cx - 18, cy, cx + 12, cy);
    gfxLine(cx + 12, cy, cx + 2, cy - 8);
    gfxLine(cx + 12, cy, cx + 2, cy + 8);
  } else if (text.indexOf("PUTAR") >= 0 || text.indexOf("U-TURN") >= 0) {
    gfxCircle(cx, cy, 14);
    gfxFillCircle(cx, cy - 14, 3, COLOR_BLACK);
  } else {
    gfxLine(cx, cy + 12, cx, cy - 12);
    gfxLine(cx, cy - 12, cx - 7, cy - 4);
    gfxLine(cx, cy - 12, cx + 7, cy - 4);
  }
}

void renderHUD() {
  beginFrame();
  int displayStep = (hudLockedStep >= 0 && hudLockedStep < navStepCount)
                      ? hudLockedStep
                      : nextImportantStep();
  if (displayStep >= 0 && displayStep < navStepCount) {
    NavigationStep& step = navSteps[displayStep];
    float distanceToTurn = step.routeDistance - simulatedRouteDistance;
    if (distanceToTurn < 0) distanceToTurn = 0;
    String distanceText = (distanceToTurn >= 1000.0f)
                            ? String(distanceToTurn / 1000.0f, 1) + "k"
                            : String((int)distanceToTurn) + "m";
    gfxText(0, 0, distanceText, 2);
    drawTurnArrow(step.instruction);
    String instruction = step.instruction;
    if (instruction.length() > 20) instruction = instruction.substring(0, 20);
    gfxText(0, 49, instruction);
    if (step.road.length() > 0) {
      String road = step.road;
      if (road.length() > 20) road = road.substring(0, 20);
      gfxText(0, 58, road);
    }
  } else {
    drawCenteredText("LANJUTKAN", 28, 1);
  }
  presentFrame();
}

// ===========================================================================
//  ARRIVAL
// ===========================================================================
void handleArrival() {
  if (arrivalActive) return;
  if (sleepModeActive) wakeFromSleep();
  arrivalActive = true;
  tripArrived = true;
  tripActive = false;
  arrivalStartedAt = millis();
  mapRequested = false;
  hudActive = false;
  hudLockedStep = -1;
  hudCompletedStep = navStepCount > 0 ? navStepCount - 1 : -1;
  companionActive = false;
  companionNextMenuActive = false;
  companionNextMenuPending = false;
  conversationActive = false;
  infoModeActive = false;
  startupOnboardingActive = false;
  cancelThinking();
  setCharacterEmotion(CHARACTER_HAPPY);
  lastUserActivityMs = millis();
  linkSend("{\"t\":\"arrived\"}");
  abotLog("INFO", "TUJUAN TERCAPAI");
  sfxPlay(SFX_ARRIVE);
}

void renderArrival() {
  beginFrame();
  drawCenteredText("SUDAH SAMPAI!", 1, 1);
  gfxLine(25, 11, 102, 11);
  drawCenteredText("Pastikan barang", 16, 1);
  drawCenteredText("bawaan sudah lengkap", 26, 1);
  drawCenteredText("dan pembayaran sudah", 36, 1);
  drawCenteredText("dilakukan ya.", 46, 1);
  drawCenteredText("Terima kasih!", 56, 1);
  presentFrame();
  if (millis() - arrivalStartedAt >= ARRIVAL_DISPLAY_DURATION) {
    arrivalActive = false;
    tripArrived = true;
    tripActive = false;
    setCharacterEmotion(CHARACTER_DEFAULT);
    resetNaturalEyeBehavior();
    lastUserActivityMs = millis();
  }
}

void startNewTrip() {
  if (networkIsBusy()) return;
  markUserActivity();
  tripActive = true;
  tripArrived = false;
  arrivalActive = false;
  tripMapPending = false;
  routeReady = false;
  routeRequested = false;
  routeFromLink = false;
  lastLinkGpsAt = 0;
  lastOsrmAttempt = 0;
  routePointCount = 0;
  navStepCount = 0;
  simulatedRouteDistance = 0;
  simulatedRouteIndex = 0;
  gpsLastMs = 0;
  activeStep = 0;
  hudLockedStep = -1;
  hudCompletedStep = -1;
  hudActive = false;
  hudShownAt = 0;
  mapRequested = false;
  navigationAIStarted = false;
  pendingNavigationAI = false;
  systemState = STATE_READY;
  activeOsrmUrl = "";
  osrmFailCount = 0;
  osrmGaveUpAt = 0;
  destinationResolved = false;
  tripIntroTriggerAsk = true;
  linkRouteReceived = false;
  startTripIntroduction();
}

// ===========================================================================
//  COMPANION
// ===========================================================================
String personalityInstruction() {
  switch (personalityMode) {
    case PERSONALITY_PLAYFUL: return "Gunakan gaya santai, playful, dan ramah.";
    case PERSONALITY_EXCITED: return "Gunakan gaya antusias dan energik.";
    default: return "Gunakan gaya natural, hangat, dan ramah.";
  }
}

String createCompanionPrompt(CompanionType type) {
  String prompt = "Kamu adalah Abot, AI Travel Companion di helm ojol. ";
  prompt += personalityInstruction();
  prompt += abotPromptHint();
  prompt += " Konten cocok untuk customer di kendaraan. ";
  prompt += "Jangan pakai markdown, bullet, tanda ---, atau emoji.\n\n";

  if (type == COMPANION_QUIZ) {
    prompt += "Buat SATU pertanyaan quiz singkat. JANGAN beri jawaban. "
              "JANGAN beri pilihan A B C. Maks ~100 karakter.";
  } else if (type == COMPANION_TRIVIA) {
    prompt += "Buat satu trivia menarik singkat. Maks ~180 karakter.";
  } else if (type == COMPANION_HUMOR) {
    prompt += "Buat satu humor ringan, aman, tidak kasar. Maks ~180 karakter.";
  } else if (type == COMPANION_STORY) {
    prompt += "Buat cerita pendek menarik. Maks ~500 karakter.";
  }

  if (previousCompanionText.length() > 0) {
    prompt += "\n\nKonten sebelumnya:\n";
    String prev = previousCompanionText;
    if (prev.length() > 350) prev = prev.substring(0, 350);
    prompt += prev;
    prompt += "\n\nJangan mengulang konten sebelumnya.";
  }
  return prompt;
}

String fallbackCompanion(CompanionType type) {
  static int quizIndex = 0, triviaIndex = 0, humorIndex = 0, storyIndex = 0;
  if (type == COMPANION_QUIZ) {
    const char* bank[] = {
      "Apa warna lampu lalu lintas yang berarti berhenti?",
      "Hewan apa yang dikenal sebagai raja hutan?",
      "Berapa jumlah hari dalam satu minggu?"
    };
    return String(bank[quizIndex++ % 3]);
  }
  if (type == COMPANION_TRIVIA) {
    const char* bank[] = {
      "Tahukah kamu? Gurita memiliki tiga jantung.",
      "Tahukah kamu? Madu bisa bertahan sangat lama.",
      "Tahukah kamu? Pisang termasuk buah beri."
    };
    return String(bank[triviaIndex++ % 3]);
  }
  if (type == COMPANION_HUMOR) {
    const char* bank[] = {
      "Customer: Mas, masih jauh? Driver: Tenang, yang jauh itu kenangan.",
      "Kalau perjalanan terasa lama, mungkin kamu terlalu fokus lihat jam.",
      "GPS bilang belok kanan. Hati bilang pulang. Ikuti GPS dulu."
    };
    return String(bank[humorIndex++ % 3]);
  }
  if (type == COMPANION_STORY) {
    const char* bank[] = {
      "Bayangkan perjalanan kecil yang awalnya biasa. Di tengah jalan kamu menemukan sesuatu yang membuatnya jadi kenangan.",
      "Ada orang yang setiap hari lewat jalan sama. Suatu hari ia memperhatikan hal kecil. Sejak itu perjalanan terasa berbeda.",
      "Seorang penumpang berkata, perjalanan terbaik bukan yang tercepat, tapi yang membuat kita pulang dengan cerita baru."
    };
    return String(bank[storyIndex++ % 3]);
  }
  return "Mari kita lanjutkan perjalanan.";
}

String cleanCompanionText(String text) {
  text = stripNonAscii(text);
  text.replace("\r", "");
  text.replace("```", "");
  text.replace("---", "");
  text.replace("**", "");
  text.replace("*", "");
  text.trim();
  return text;
}

void buildCompanionPages(const String& rawText) {
  companionPageCount = 0;
  companionPage = 0;
  companionTypedChars = 0;
  for (int p = 0; p < MAX_COMPANION_PAGES; p++) {
    for (int l = 0; l < MAX_COMPANION_LINES; l++) companionPages[p][l] = "";
  }
  String text = cleanCompanionText(rawText);
  text.replace("\n", " ");
  text.trim();
  if (text.length() == 0) text = "Maaf, coba lagi sebentar.";

  int pg = 0, ln = 0;
  auto pushLine = [&](const String& s) -> bool {
    if (pg >= MAX_COMPANION_PAGES) return false;
    companionPages[pg][ln] = s;
    ln++;
    if (ln >= MAX_COMPANION_LINES) {
      ln = 0;
      pg++;
    }
    return true;
  };

  String cur = "";
  int pos = 0;
  bool full = false;
  while (pos < (int)text.length() && !full) {
    int sp = text.indexOf(' ', pos);
    String word;
    if (sp == -1) {
      word = text.substring(pos);
      pos = text.length();
    } else {
      word = text.substring(pos, sp);
      pos = sp + 1;
    }
    word.trim();
    if (word.length() == 0) continue;
    while ((int)word.length() > MAX_LINE_CHARS) {
      if (cur.length() > 0) {
        if (!pushLine(cur)) {
          full = true;
          break;
        }
        cur = "";
      }
      if (!pushLine(word.substring(0, MAX_LINE_CHARS))) {
        full = true;
        break;
      }
      word = word.substring(MAX_LINE_CHARS);
    }
    if (full) break;
    if (cur.length() == 0) {
      cur = word;
    } else if ((int)(cur.length() + 1 + word.length()) <= MAX_LINE_CHARS) {
      cur += " ";
      cur += word;
    } else {
      if (!pushLine(cur)) {
        full = true;
        break;
      }
      cur = word;
    }
  }
  if (!full && cur.length() > 0) pushLine(cur);
  int count = (ln == 0) ? pg : pg + 1;
  if (count > MAX_COMPANION_PAGES) count = MAX_COMPANION_PAGES;
  if (count < 1) count = 1;
  companionPageCount = count;
}

int companionPageTotalChars() {
  int total = 0;
  for (int l = 0; l < MAX_COMPANION_LINES; l++) {
    total += companionPages[companionPage][l].length();
  }
  return total;
}

bool startNetworkJob(NetworkJob job, const char* taskName,
                     const String& prompt, uint32_t requestId) {
  if (networkIsBusy()) return false;
  networkJob = job;
  networkRequestId = requestId;
  networkPrompt = prompt;
  networkDone = false;
  networkSuccess = false;
  networkHttpCode = -1;
  networkBusy = true;
  networkJobStartedAt = millis();
  networkJobWasBusy = true;

  BaseType_t result = xTaskCreatePinnedToCore(
    networkTask, taskName, 20000, NULL, 1, NULL, 0);
  if (result != pdPASS) {
    networkBusy = false;
    return false;
  }
  return true;
}

bool llmCooldownActive() {
  if (lastLlmRequest == 0) return false;
  return (millis() - lastLlmRequest) < LLM_COOLDOWN;
}

void startWeatherFetch() {
  if (weatherFetching || !wifiConnected || WiFi.status() != WL_CONNECTED) return;
  lastWeatherAttempt = millis();
  if (!startNetworkJob(NET_WEATHER, "weatherTask", "", millis())) return;
  weatherFetching = true;
}

void startOSRM() {
  if (networkIsBusy()) return;
  if (activeOsrmUrl.length() == 0) return;
  if (osrmFailCount >= OSRM_MAX_RETRIES) return;
  lastOsrmAttempt = millis();
  if (!startNetworkJob(NET_OSRM, "osrmTask", activeOsrmUrl, millis())) {
    systemState = STATE_ERROR;
    return;
  }
  systemState = STATE_GETTING_ROUTE;
  routeRequested = true;
}

String createNavigationPrompt() {
  String prompt = "Kamu adalah Abot, AI navigasi ojol. ";
  prompt += "Berikan respon singkat natural untuk customer. Bahasa Indonesia. ";
  prompt += abotPromptHint();
  prompt += " Jangan pakai markdown/emoji. Buat customer merasa ditemani.\n\n";
  if (destName.length() > 0) {
    prompt += "Tujuan: ";
    prompt += destName;
    prompt += "\n";
  }
  prompt += "Jarak total: ";
  prompt += String(osrmTotalDistance / 1000.0, 2);
  prompt += " km\nEstimasi: ";
  prompt += String(osrmTotalDuration / 60.0, 1);
  prompt += " menit\n";
  int nextStep = nextImportantStep();
  if (nextStep >= 0 && nextStep < navStepCount) {
    prompt += "Instruksi berikutnya: ";
    prompt += navSteps[nextStep].instruction;
    prompt += "\n";
  }
  return prompt;
}

void startNavigationAI() {
  if (!routeReady) return;
  if (networkIsBusy()) return;
  if (llmCooldownActive()) return;
  if (navigationAIStarted) return;
  navigationAIStarted = true;
  pendingNavigationAI = false;
  lastLlmRequest = millis();
  if (!startNetworkJob(NET_NAVIGATION_AI, "navAiTask",
                       createNavigationPrompt(), millis())) {
    navigationAIStarted = false;
    return;
  }
}

void startCompanion(CompanionType type) {
  companionNextMenuActive = false;
  companionNextMenuPending = false;
  companionNextMenuDelayMs = 350;
  mapRequested = false;
  hudActive = false;
  hudSuppressUntil = millis() + HUD_SUPPRESS_DURATION;
  currentCompanion = type;
  companionActive = false;
  companionGenerating = true;
  systemState = STATE_THINKING;

  setEyeMood(EYE_DEFAULT);
  beginThinkingVisual();
  companionRequestCounter++;
  activeCompanionRequestId = companionRequestCounter;
  String prompt = createCompanionPrompt(type);

  if (llmCooldownActive() || networkIsBusy()) {
    finishCompanionGeneration(fallbackCompanion(type), type);
    return;
  }
  lastLlmRequest = millis();
  if (!startNetworkJob(NET_COMPANION, "llmTask", prompt,
                       activeCompanionRequestId)) {
    finishCompanionGeneration(fallbackCompanion(type), type);
    return;
  }
}

void finishCompanionGeneration(const String& rawText, CompanionType type) {
  String text = cleanCompanionText(rawText);
  if (text.length() == 0) text = fallbackCompanion(type);

  if (type == COMPANION_QUIZ) {
    int answerPos = text.indexOf("Jawaban:");
    if (answerPos >= 0) {
      text = text.substring(0, answerPos);
      text.trim();
    }
    int answerPos2 = text.indexOf("jawabannya");
    if (answerPos2 >= 0) {
      text = text.substring(0, answerPos2);
      text.trim();
    }
    currentQuizQuestion = text;
    quizWaitingAnswer = true;
  }

  previousCompanionText = text;
  buildCompanionPages(text);
  companionPage = 0;
  companionTypedChars = 0;
  companionLastType = millis();
  companionStateStart = millis();
  companionPlayback = COMPANION_TYPING;
  companionActive = true;
  companionGenerating = false;
  endThinkingVisual();
  systemState = STATE_READY;

  if (type == COMPANION_HUMOR || type == COMPANION_QUIZ || type == COMPANION_TRIVIA) setEyeMood(EYE_HAPPY);
  else setEyeMood(EYE_DEFAULT);
}

void finishCompanion() {
  const bool wasCard = (currentCompanion == COMPANION_CARD);
  companionActive = false;
  companionGenerating = false;
  companionPlayback = COMPANION_IDLE;
  currentCompanion = COMPANION_NONE;
  setEyeMood(EYE_DEFAULT);
  hudActive = false;
  hudSuppressUntil = millis() + HUD_SUPPRESS_DURATION;
  roboEyes.blink();
  if (wasCard) lastUserActivityMs = millis();
  else openCompanionNextMenu(companionNextMenuDelayMs);
}

void openCompanionNextMenu(unsigned long delayMs) {
  companionNextMenuPending = true;
  companionNextMenuActive = false;
  companionNextMenuAt = millis() + delayMs;
  companionNextMenuPageAt = 0;
  companionNextMenuPage = 0;
  hudActive = false;
  hudSuppressUntil = millis() + HUD_SUPPRESS_DURATION;
}

void closeCompanionNextMenu() {
  companionNextMenuActive = false;
  companionNextMenuPending = false;
  companionNextMenuPage = 0;
  setEyeMood(EYE_DEFAULT);
  hudSuppressUntil = millis() + HUD_SUPPRESS_DURATION;
  lastUserActivityMs = millis();
}

void updateCompanionNextMenu() {
  if (!companionNextMenuPending) return;
  if (!companionNextMenuActive) {
    if (millis() < companionNextMenuAt) return;
    companionNextMenuActive = true;
    companionNextMenuPage = 0;
    companionNextMenuPageAt = millis();
    companionNextMenuOpenedAt = millis();
    return;
  }
  if (millis() - companionNextMenuOpenedAt >= COMPANION_NEXT_MENU_TIMEOUT) {
    closeCompanionNextMenu();
    return;
  }
  if (millis() - companionNextMenuPageAt >= COMPANION_NEXT_MENU_PAGE_MS) {
    companionNextMenuPage = (companionNextMenuPage + 1) % 2;
    companionNextMenuPageAt = millis();
  }
}

void renderCompanion() {
  beginFrame();
  String title = companionName(currentCompanion);
  int titleX = (128 - (int)title.length() * 6) / 2;
  if (titleX < 0) titleX = 0;
  gfxText(titleX, 0, title);
  gfxLine(10, 9, 117, 9);
  int remaining = companionTypedChars;
  for (int l = 0; l < MAX_COMPANION_LINES; l++) {
    String line = companionPages[companionPage][l];
    if (line.length() == 0) continue;
    String visible;
    if (remaining >= (int)line.length()) {
      visible = line;
      remaining -= line.length();
    } else if (remaining > 0) {
      visible = line.substring(0, remaining);
      remaining = 0;
    } else {
      visible = "";
    }
    gfxText(0, 12 + (l * 7), visible);
    if (visible.length() > 0 && remaining == 0 && companionPlayback == COMPANION_TYPING) {
      int cursorX = visible.length() * 6;
      if (cursorX < 126) gfxLine(cursorX, 12 + (l * 7), cursorX, 17 + (l * 7));
    }
  }
  presentFrame();
}

void renderCompanionNextMenu() {
  beginFrame();
  drawCenteredText("MAU LANJUT?", 0, 1);
  gfxLine(12, 9, 115, 9);
  if (companionNextMenuPage == 0) {
    gfxText(5, 16, "Q  QUIZ LAGI");
    gfxText(5, 28, "T  TRIVIA");
    gfxText(5, 40, "S  STORY");
    gfxText(5, 52, "D  MUSIK");
  } else {
    gfxText(5, 16, "H  HUMOR");
    gfxText(5, 28, "X  SELESAI");
    gfxText(5, 40, "Pilih dengan tombol");
    gfxText(5, 52, "atau suara");
  }
  presentFrame();
}

void updateCompanion() {
  if (!companionActive) return;
  if (companionPlayback == COMPANION_TYPING) {
    if (millis() - companionLastType >= TYPING_INTERVAL) {
      companionLastType = millis();
      if (companionTypedChars < companionPageTotalChars()) {
        companionTypedChars++;
      } else {
        companionPlayback = COMPANION_HOLD;
        companionStateStart = millis();
      }
    }
  } else if (companionPlayback == COMPANION_HOLD) {
    unsigned long holdTime = COMPANION_HOLD_DURATION;
    if (companionPage == companionPageCount - 1) holdTime = COMPANION_FINAL_HOLD;
    if (millis() - companionStateStart >= holdTime) {
      if (companionPage < companionPageCount - 1) {
        companionPage = companionPage + 1;
        companionTypedChars = 0;
        companionLastType = millis();
        companionStateStart = millis();
        companionPlayback = COMPANION_TYPING;
      } else {
        finishCompanion();
      }
    }
  }
}

void handleQuizAnswer(char cmd) {
  cmd = toupper(cmd);
  if (!quizWaitingAnswer) return;
  switch (cmd) {
    case 'J':
      quizWaitingAnswer = false;
      triggerCharacterReaction(CHARACTER_LAUGH);
      companionNextMenuDelayMs = CHARACTER_REACTION_LAUGH_MS + 350;
      sfxPlay(SFX_QUIZ_OK);
      break;
    case 'K':
      quizWaitingAnswer = false;
      triggerCharacterReaction(CHARACTER_ANGRY);
      companionNextMenuDelayMs = CHARACTER_REACTION_ANGRY_MS + 350;
      sfxPlay(SFX_QUIZ_BAD);
      break;
    case 'L':
      quizWaitingAnswer = false;
      triggerCharacterReaction(CHARACTER_CONFUSED);
      companionNextMenuDelayMs = CHARACTER_REACTION_CONFUSED_MS + 350;
      sfxPlay(SFX_ERROR);
      break;
  }
}

void networkTask(void* parameter) {
  {
    NetworkJob job = networkJob;
    String prompt = networkPrompt;
    String response = "";
    int httpCode = -1;
    bool success = false;

    if (job == NET_OSRM) {
      for (int attempt = 1; attempt <= 3; attempt++) {
        HTTPClient http;
        http.setTimeout(12000);
        http.useHTTP10(true);
        if (!http.begin(prompt)) continue;
        httpCode = http.GET();
        if (httpCode == 200) {
          response = http.getString();
          success = response.length() > 0;
          if (success) {
            http.end();
            break;
          }
        }
        http.end();
        delay(500);
      }
    } else if (job == NET_WEATHER) {
      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;
      http.setTimeout(8000);
      if (http.begin(client, WEATHER_URL)) {
        httpCode = http.GET();
        if (httpCode == 200) {
          response = http.getString();
          success = response.length() > 0;
        }
        http.end();
      }
    } else if (job == NET_NAVIGATION_AI || job == NET_COMPANION) {
      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient http;
      http.setTimeout(CHAT_TIMEOUT_MS);

      if (http.begin(client, GROQ_CHAT_URL)) {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Accept", "application/json");
        String authorization = "Bearer ";
        authorization += GROQ_API_KEY_STR;
        http.addHeader("Authorization", authorization);

        JsonDocument requestDoc;
        const bool gptOss = activeChatModel.startsWith("openai/gpt-oss");
        requestDoc["model"] = activeChatModel;
        requestDoc["temperature"] = (job == NET_COMPANION) ? 0.8 : 0.3;
        requestDoc["max_tokens"] = ((job == NET_COMPANION) ? 160 : 100) * (gptOss ? 5 : 1);
        if (gptOss) requestDoc["reasoning_effort"] = "low";

        JsonArray messages = requestDoc["messages"].to<JsonArray>();
        JsonObject systemMessage = messages.add<JsonObject>();
        systemMessage["role"] = "system";
        systemMessage["content"] =
          (job == NET_COMPANION)
            ? "Kamu Abot, AI Travel Companion. Jangan pakai emoji."
            : "Kamu Abot, AI navigasi. Jangan pakai emoji.";
        JsonObject userMessage = messages.add<JsonObject>();
        userMessage["role"] = "user";
        userMessage["content"] = prompt;

        String payload;
        serializeJson(requestDoc, payload);
        httpCode = http.POST(payload);
        if (httpCode == 200) {
          String raw = http.getString();
          JsonDocument filter;
          filter["choices"][0]["message"]["content"] = true;
          JsonDocument responseDoc;
          DeserializationError error =
            deserializeJson(responseDoc, raw, DeserializationOption::Filter(filter));
          if (!error) {
            JsonVariant content = responseDoc["choices"][0]["message"]["content"];
            if (!content.isNull()) {
              response = content.as<String>();
              success = response.length() > 0;
            }
          }
        }
        http.end();
      }
    }

    networkResponse = response;
    networkHttpCode = httpCode;
    networkSuccess = success;
    networkDone = true;
    networkBusy = false;
  }
  vTaskDelete(NULL);
}

void processNetworkDone() {
  if (!networkDone) return;
  NetworkJob completedJob = networkJob;
  bool success = networkSuccess;
  String response = networkResponse;
  int httpCode = networkHttpCode;
  uint32_t requestId = networkRequestId;
  networkDone = false;

  if (completedJob == NET_WEATHER) {
    weatherFetching = false;
    if (!success || httpCode != 200) return;
    JsonDocument doc;
    if (deserializeJson(doc, response)) return;
    JsonObject current = doc["current"];
    JsonObject daily = doc["daily"];
    if (current.isNull()) return;
    weatherData.temperature = current["temperature_2m"] | 0.0f;
    weatherData.weatherCode = current["weather_code"] | -1;
    weatherData.condition = weatherCodeToText(weatherData.weatherCode);
    weatherData.tempMax = daily["temperature_2m_max"][0] | weatherData.temperature;
    weatherData.tempMin = daily["temperature_2m_min"][0] | weatherData.temperature;
    weatherData.valid = true;
    weatherData.updatedAt = millis();
    return;
  }

  if (completedJob == NET_OSRM) {
    routeRequested = false;
    if (routeFromLink) return;
    if (!success || httpCode != 200) {
      systemState = STATE_ERROR;
      osrmFailCount++;
      return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, response)) {
      osrmFailCount++;
      return;
    }
    response = "";
    JsonObject route = doc["routes"][0];
    if (route.isNull()) {
      osrmFailCount++;
      return;
    }
    osrmTotalDistance = route["distance"].as<float>();
    osrmTotalDuration = route["duration"].as<float>();
    const char* polylineStr = route["geometry"];
    routePointCount = 0;
    if (polylineStr != nullptr && strlen(polylineStr) > 0) {
      String encoded = String(polylineStr);
      routePointCount = decodePolyline(encoded, routePoints, MAX_ROUTE_POINTS);
    }
    if (routePointCount >= 2) {
      routeGeometryDistance = 0;
      routeCumulativeDistance[0] = 0;
      for (int i = 1; i < routePointCount; i++) {
        double lat1 = routePoints[i - 1].lat;
        double lon1 = routePoints[i - 1].lon;
        double lat2 = routePoints[i].lat;
        double lon2 = routePoints[i].lon;
        double dx = (lon2 - lon1) * 111320.0 * cos(lat1 * DEG_TO_RAD);
        double dy = (lat2 - lat1) * 110540.0;
        routeGeometryDistance += sqrt(dx * dx + dy * dy);
        routeCumulativeDistance[i] = routeGeometryDistance;
      }
    }
    navStepCount = 0;
    JsonArray legs = route["legs"];
    for (JsonObject leg : legs) {
      JsonArray steps = leg["steps"];
      for (JsonObject step : steps) {
        if (navStepCount >= MAX_NAV_STEPS) break;
        NavigationStep& nav = navSteps[navStepCount];
        nav.distance = step["distance"].as<float>();
        nav.type = step["maneuver"]["type"].as<String>();
        nav.modifier = step["maneuver"]["modifier"].as<String>();
        nav.road = step["name"].as<String>();
        String type = nav.type, modifier = nav.modifier;
        type.toLowerCase();
        modifier.toLowerCase();
        nav.instruction = "LANJUT";
        if (type.indexOf("roundabout") >= 0) nav.instruction = "PUTARAN";
        else if (modifier.indexOf("uturn") >= 0) nav.instruction = "PUTAR BALIK";
        else if (modifier.indexOf("left") >= 0) nav.instruction = "BELOK KIRI";
        else if (modifier.indexOf("right") >= 0) nav.instruction = "BELOK KANAN";
        nav.important = (type == "turn" || type == "merge" || type == "fork" || type == "roundabout" || type == "on ramp" || type == "off ramp");
        double maneuverLon = step["maneuver"]["location"][0].as<double>();
        double maneuverLat = step["maneuver"]["location"][1].as<double>();
        float bestDistance = 999999.0;
        int bestIndex = 0;
        if (routePointCount > 0) {
          for (int i = 0; i < routePointCount; i++) {
            double dx = (routePoints[i].lon - maneuverLon) * 111320.0;
            double dy = (routePoints[i].lat - maneuverLat) * 110540.0;
            float d = sqrt(dx * dx + dy * dy);
            if (d < bestDistance) {
              bestDistance = d;
              bestIndex = i;
            }
          }
          nav.routeIndex = bestIndex;
          nav.routeDistance = routeCumulativeDistance[bestIndex];
        }
        navStepCount++;
      }
    }
    simulatedRouteDistance = 0;
    simulatedRouteIndex = 0;
    gpsLastMs = 0;
    activeStep = 0;
    hudLockedStep = -1;
    hudCompletedStep = -1;
    hudActive = false;
    hudShownAt = 0;
    routeReady = true;
    tripActive = true;
    tripArrived = false;
    arrivalActive = false;
    systemState = STATE_READY;
    osrmFailCount = 0;
    osrmGaveUpAt = 0;
    if (!initialMapShown && startupOnboardingCompleted && !startupOnboardingActive) {
      initialMapShown = true;
      startMapDisplay();
      pendingNavigationAI = NAV_AI_ON_ROUTE;
    } else if (tripMapPending && !tripIntroActive) {
      tripMapPending = false;
      startMapDisplay();
      pendingNavigationAI = NAV_AI_ON_ROUTE;
    }
    return;
  }

  if (completedJob == NET_COMPANION) {
    if (requestId != activeCompanionRequestId) return;
    if (!success || httpCode != 200 || response.length() == 0) {
      finishCompanionGeneration(fallbackCompanion(currentCompanion),
                                currentCompanion);
      return;
    }
    finishCompanionGeneration(response, currentCompanion);
    return;
  }

  if (completedJob == NET_NAVIGATION_AI) {
    if (!companionGenerating) {
      endThinkingVisual();
      if (systemState == STATE_THINKING) systemState = STATE_READY;
    }
    return;
  }
}

void updateGPSSimulation() {
  if (!routeReady || routePointCount < 2) return;
  if (routeFromLink) {
    bool gpsFresh = (lastLinkGpsAt != 0) && (millis() - lastLinkGpsAt <= LINK_GPS_FRESH_MS);
    if (gpsFresh) {
      deadReckonStep();
      return;
    }
    if (!SIM_WHEN_NO_GPS) return;
  }
  unsigned long now = millis();
  if (gpsLastMs == 0) {
    gpsLastMs = now;
    return;
  }
  float deltaSeconds = (now - gpsLastMs) / 1000.0;
  gpsLastMs = now;
  if (deltaSeconds > 1.0) deltaSeconds = 1.0;
  speedMps = GPS_SPEED_MPS;
  simulatedRouteDistance += GPS_SPEED_MPS * deltaSeconds;
  if (simulatedRouteDistance >= routeGeometryDistance) {
    simulatedRouteDistance = routeGeometryDistance;
    if (tripActive && !tripArrived && !arrivalActive) {
      handleArrival();
      return;
    }
  }
  syncRouteProgress();
}

float hudTriggerDistance() {
  float d = speedMps * HUD_LEAD_SECONDS;
  if (d < HUD_MIN_TRIGGER_M) d = HUD_MIN_TRIGGER_M;
  if (d > HUD_MAX_TRIGGER_M) d = HUD_MAX_TRIGGER_M;
  return d;
}

void updateHUDLatch() {
  if (conversationActive || companionActive || companionGenerating || companionNextMenuActive || companionNextMenuPending || tripIntroActive || infoModeActive || destinationAskActive || voicePipelineActive || ttsActive) {
    hudActive = false;
    return;
  }
  if (millis() < hudSuppressUntil) {
    hudActive = false;
    return;
  }
  if (navStepCount == 0) {
    hudActive = false;
    return;
  }

  if (hudLockedStep >= 0) {
    float turnDist = navSteps[hudLockedStep].routeDistance - simulatedRouteDistance;
    bool passed = (turnDist <= -HUD_PASSED_MARGIN_M);
    bool tooLong = (millis() - hudShownAt > HUD_MAX_SHOW_MS) && (turnDist > 25.0f) && (speedMps > 8.0f);
    if (passed || tooLong) {
      if (passed) {
        char buf[48];
        snprintf(buf, sizeof(buf), "{\"t\":\"passed\",\"i\":%d}", hudLockedStep);
        linkSend(String(buf));
        hudCompletedStep = hudLockedStep;
      } else {
        hudSuppressUntil = millis() + HUD_SUPPRESS_DURATION;
      }
      hudLockedStep = -1;
      hudActive = false;
      hudShownAt = 0;
      if (!passed) return;
    } else {
      hudActive = true;
      return;
    }
  }

  float trigger = hudTriggerDistance();
  int nextImportant = -1;
  float nextDist = 0;
  for (int i = 0; i < navStepCount; i++) {
    if (i <= hudCompletedStep) continue;
    if (!isImportantStep(navSteps[i])) continue;
    float d = navSteps[i].routeDistance - simulatedRouteDistance;
    if (d <= -HUD_PASSED_MARGIN_M) {
      hudCompletedStep = i;
      continue;
    }
    if (mapRequested && d > HUD_PREEMPT_MAP_M) break;
    if (d <= trigger) {
      nextImportant = i;
      nextDist = d;
    }
    break;
  }

  if (nextImportant >= 0) {
    hudLockedStep = nextImportant;
    hudShownAt = millis();
    hudActive = true;
    sfxPlay(SFX_TURN);
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"t\":\"hud\",\"i\":%d,\"m\":%.0f}",
             nextImportant, nextDist);
    linkSend(String(buf));
  } else {
    hudActive = false;
  }
}

void renderVoiceText() {
  beginFrame();
  drawCenteredText("ABOT", 0, 1);
  gfxLine(12, 10, 115, 10);

  String text = stripNonAscii(currentTtsPart);
  const int maxChars = 21;
  const int len = text.length();
  int pos = 0, lines = 0, y = 13;
  while (pos < len && lines < 5) {
    int end = pos + maxChars;
    if (end >= len) end = len;
    else {
      int sp = text.lastIndexOf(' ', end);
      if (sp > pos) end = sp;
    }
    String line = text.substring(pos, end);
    line.trim();
    gfxText(0, y, line);
    y += 10;
    lines++;
    pos = end;
    while (pos < len && text[pos] == ' ') pos++;
  }
  presentFrame();
}

void updateDisplay() {
  if (otaActive) return;
  if (listeningActive) {
    renderListening();
    return;
  }
  if (companionActive) {
    renderCompanion();
    return;
  }
  if (companionNextMenuActive) {
    renderCompanionNextMenu();
    return;
  }
  if (destinationAskActive) {
    renderDestinationAsk();
    return;
  }
  if (startupOnboardingActive) {
    renderStartupOnboarding();
    return;
  }
  if (tripIntroActive) {
    renderTripIntroduction();
    return;
  }
  if (infoModeActive) {
    renderInfoMode();
    return;
  }
  if (bootTransitionActive) {
    renderBootTransition();
    return;
  }
  if (!introductionFinished) {
    renderIntroduction();
    return;
  }

  if (ttsActive && voiceTextActive && currentTtsPart.length() > 0) {
    renderVoiceText();
    return;
  }

  if (companionGenerating || systemState == STATE_THINKING || thinkingVisualPrepared) {
    renderThinking();
    return;
  }
  if (conversationActive) {
    renderEyes();
    return;
  }
  if (arrivalActive) {
    renderArrival();
    return;
  }
  if (hudActive) {
    renderHUD();
    return;
  }
  if (mapRequested) {
    renderMap();
    return;
  }
  if (musicScreenWanted()) {
    renderMusic();
    return;
  }
  if (sleepModeActive) {
    renderSleep();
    return;
  }
  renderEyes();
}

void displayRenderTask(void* pvParameters) {
  const TickType_t period = pdMS_TO_TICKS(16);
  for (;;) {
    const TickType_t frameStart = xTaskGetTickCount();
    {
      StateLock lock;
      updateDisplay();
    }
    const TickType_t elapsed = xTaskGetTickCount() - frameStart;
    vTaskDelay(elapsed >= period ? 1 : period - elapsed);
  }
}

void printHelp() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("COMMANDS:");
  Serial.println("TOMBOL TALK   : tahan=bicara, tekan=jam/cuaca");
  Serial.println("TOMBOL ACTION : tekan=musik, tahan=lagu berikut / companion");
  Serial.println("M = MAP | U = CARA MENGGUNAKAN | G = TRIP BARU");
  Serial.println("C = CONVERSATION ON / X = OFF");
  Serial.println("Q = QUIZ | T = TRIVIA | H = HUMOR | S = STORY");
  Serial.println("D = MUSIK | > = LAGU BERIKUT | F = STOP MUSIK");
  Serial.println("A = NAVIGATION AI | B = THINKING DEMO | $ = TES BUZZER");
  Serial.println("1-6 = preset mata | W = WINK | P/N/E = personality");
  Serial.println("R = RESET GPS | V = REKAM SUARA");
  Serial.println("+ / - = VOLUME | Z = MUTE");
  Serial.println("========================================");
}

void handleCommand(char cmd) {
  cmd = toupper(cmd);
  if (companionNextMenuActive) {
    if (cmd == 'Q' || cmd == 'T' || cmd == 'S' || cmd == 'D' || cmd == 'H') {
      closeCompanionNextMenu();
      if (cmd == 'Q') startCompanion(COMPANION_QUIZ);
      else if (cmd == 'T') startCompanion(COMPANION_TRIVIA);
      else if (cmd == 'S') startCompanion(COMPANION_STORY);
      else if (cmd == 'D') musicRequestPost(MCMD_PLAY, "", "");
      else if (cmd == 'H') startCompanion(COMPANION_HUMOR);
      return;
    }
    if (cmd == 'X') {
      closeCompanionNextMenu();
      return;
    }
  }
  if (cmd == 'J' || cmd == 'K' || cmd == 'L') {
    handleQuizAnswer(cmd);
    return;
  }
  if (cmd == 'G') {
    startNewTrip();
    return;
  }
  if (cmd == 'U') {
    markUserActivity();
    startupOnboardingManual = true;
    startStartupOnboarding();
    return;
  }
  if (cmd == 'M') {
    if (destinationAskActive) return;
    companionActive = false;
    companionNextMenuActive = false;
    companionNextMenuPending = false;
    hudActive = false;
    cancelThinking();
    startMapDisplay();
    return;
  }
  if (cmd == 'C') {
    if (destinationAskActive) return;
    conversationActive = true;
    mapRequested = false;
    companionActive = false;
    companionNextMenuActive = false;
    companionNextMenuPending = false;
    hudActive = false;
    cancelThinking();
    hudSuppressUntil = millis() + HUD_SUPPRESS_DURATION;
    setEyeMood(EYE_HAPPY);
    return;
  }
  if (cmd == 'X') {
    conversationActive = false;
    hudSuppressUntil = millis() + HUD_SUPPRESS_DURATION;
    setEyeMood(EYE_DEFAULT);
    return;
  }
  if (cmd == 'Q') {
    startCompanion(COMPANION_QUIZ);
    return;
  }
  if (cmd == 'T') {
    startCompanion(COMPANION_TRIVIA);
    return;
  }
  if (cmd == 'H') {
    startCompanion(COMPANION_HUMOR);
    return;
  }
  if (cmd == 'S') {
    startCompanion(COMPANION_STORY);
    return;
  }
  if (cmd == 'D') {
    musicRequestPost(MCMD_TOGGLE, "", "");
    return;
  }
  if (cmd == '>') {
    musicRequestPost(MCMD_NEXT, "", "");
    return;
  }
  if (cmd == 'F') {
    musicRequestPost(MCMD_STOP, "", "");
    return;
  }
  if (cmd == '$') {
    sfxSelfTest();
    return;
  }
  if (cmd >= '1' && cmd <= '6') {
    abotHoldUntil = millis() + 8000;
    if (cmd == '1') setEyeMood(EYE_DEFAULT);
    else if (cmd == '2') setEyeMood(EYE_HAPPY);
    else if (cmd == '3') setEyeMood(EYE_ANGRY);
    else if (cmd == '4') setEyeMood(EYE_TIRED);
    else if (cmd == '5') {
      setEyeMood(EYE_HAPPY);
      roboEyes.anim_laugh();
    } else if (cmd == '6') {
      setEyeMood(EYE_DEFAULT);
      roboEyes.anim_confused();
    }
    return;
  }
  if (cmd == 'W') {
    abotHoldUntil = millis() + 8000;
    setEyeMood(EYE_DEFAULT);
    roboEyes.setPosition(DEFAULT);
    triggerNaturalWink();
    return;
  }
  if (cmd == 'P') {
    personalityMode = PERSONALITY_PLAYFUL;
    return;
  }
  if (cmd == 'N') {
    personalityMode = PERSONALITY_NORMAL;
    return;
  }
  if (cmd == 'E') {
    personalityMode = PERSONALITY_EXCITED;
    return;
  }
  if (cmd == 'B') {
    thinkingDemoUntil = millis() + 3000;
    systemState = STATE_THINKING;
    setEyeMood(EYE_DEFAULT);
    beginThinkingVisual();
    return;
  }
  if (cmd == 'A') {
    navigationAIStarted = false;
    startNavigationAI();
    return;
  }
  if (cmd == 'R') {
    arrivalActive = false;
    tripArrived = false;
    tripActive = true;
    simulatedRouteDistance = 0;
    simulatedRouteIndex = 0;
    gpsLastMs = 0;
    activeStep = 0;
    hudLockedStep = -1;
    hudCompletedStep = -1;
    hudActive = false;
    hudShownAt = 0;
    speedLastAt = 0;
    drLastMs = 0;
    return;
  }
  if (cmd == '+') {
    audioVolumeStep(+1);
    return;
  }
  if (cmd == '-') {
    audioVolumeStep(-1);
    return;
  }
  if (cmd == 'Z') {
    audioToggleMute();
    return;
  }
  if (cmd == 'V') {
    if (listeningActive) stopListening();
    else startListening();
    return;
  }
  if (cmd == '?') {
    printHelp();
    return;
  }
}

void oledProbeAddress() {
  Wire.begin(OLED_SDA, OLED_SCL);
  const uint8_t candidates[2] = { 0x3C, 0x3D };
  for (int i = 0; i < 2; i++) {
    Wire.beginTransmission(candidates[i]);
    if (Wire.endTransmission() == 0) {
      u8g2.setI2CAddress(candidates[i] << 1);
      Serial.printf("[OLED] Terdeteksi di 0x%02X\n", candidates[i]);
      Wire.end();
      return;
    }
  }
  Serial.println("[OLED] PERINGATAN: tidak ada I2C di 0x3C/0x3D.");
  Wire.end();
}

void setup() {
  stateMutex = xSemaphoreCreateRecursiveMutex();
  mqttMutex = xSemaphoreCreateMutex();

  Serial.begin(115200);
  randomSeed((unsigned long)micros());
  {
    unsigned long serialWaitStart = millis();
    while (!Serial && millis() - serialWaitStart < 4000) delay(10);
    delay(300);
  }

  Serial.println();
  Serial.println("========================================");
  Serial.println("       OJOL ASSISTEN (ABOT) V" FW_VERSION);
  Serial.println("  VOICE AI: STT Groq + LLM Groq + TTS Google");
  Serial.println("========================================");

  pinMode(BUTTON_TALK, INPUT_PULLUP);
  pinMode(BUTTON_ACTION, INPUT_PULLUP);

  sfxBegin();

  oledProbeAddress();
  u8g2.setBusClock(OLED_I2C_CLOCK_HZ);
  if (!u8g2.begin()) {
    Serial.println("[OLED] FAILED");
    while (true) delay(1000);
  }
  u8g2.setFontMode(1);
  u8g2.setFontPosTop();
  Serial.println("[OLED] OK");

  roboEyes.begin(SCREEN_WIDTH, SCREEN_HEIGHT, 100);
  roboEyes.setPosition(DEFAULT);
  roboEyes.setAutoblinker(ON, 3, 2);
  roboEyes.setIdleMode(ON, 2, 1);
  setEyeMood(EYE_DEFAULT);
  resetNaturalEyeBehavior();
  Serial.println("[ROBOEYES] OK");

  audioBegin();
  linkBegin();
  mqttBegin();

  systemState = STATE_WIFI_CONNECTING;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("[WIFI] Connecting...");

  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED) {
    renderWifiConnecting();
    delay(250);
    if (millis() - wifiStart > 20000) break;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("[WIFI] Connected!");
    Serial.print("[WIFI] IP: ");
    Serial.println(WiFi.localIP());
    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    Serial.println("[NTP] WIB UTC+7 sync requested");
    delay(500);
  } else {
    wifiConnected = false;
    Serial.println("[WIFI] Offline saat boot, akan reconnect otomatis");
  }

  BaseType_t r = xTaskCreatePinnedToCore(voiceTask, "VoiceTask", 16384,
                                         NULL, 3, &voiceTaskHandle, 0);
  if (r != pdPASS) {
    Serial.println("[VOICE] FAIL: voiceTask spawn");
    voiceTaskHandle = NULL;
  } else {
    Serial.println("[VOICE] voiceTask started");
  }

  systemState = STATE_READY;
  printHelp();
  startBootTransition();

  lastUserActivityMs = millis();
  sleepModeActive = false;

  Serial.println("[STATE] READY");

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 15000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  if (esp_task_wdt_reconfigure(&wdt_config) != ESP_OK) esp_task_wdt_init(&wdt_config);
#else
  esp_task_wdt_init(15, true);
#endif
  esp_task_wdt_add(NULL);
  Serial.println("[WDT] Watchdog aktif (15s)");

  xTaskCreatePinnedToCore(displayRenderTask, "DisplayTask", 8192, NULL, 2,
                          &displayTaskHandle, 1);
  Serial.println("[DISPLAY] Display task started on core 1 (~60 FPS)");

  // --- CEK APAKAH INI BOOT PERTAMA SETELAH OTA ---
  bootTimeMs = millis();
  otaPrefs.begin("ota", true);
  pendingOtaVerification = otaPrefs.getBool("pending", false);
  otaTargetVersion = otaPrefs.getString("target", "");
  otaPrefs.end();

  if (pendingOtaVerification) {
    Serial.printf("[OTA] Menunggu verifikasi untuk versi %s...\n", otaTargetVersion.c_str());
  }

}

void loop() {
  esp_task_wdt_reset();
  linkUpdate();
  mqttUpdate();
  ensureWiFiConnected();
  checkHeapHealth();

  sfxUpdate();

  {
    StateLock lock;
    updateCharacterEmotion();
    updateAbotPersonality();
    updateNaturalEyeBehavior();
    updateButtons();

    if (hpConnected && lastLinkMessageAt > 0 && millis() - lastLinkMessageAt > LINK_TIMEOUT_MS) {
      hpConnected = false;
      abotLog("WARN", "HP terputus (timeout)");
    }

    if (thinkingDemoUntil > 0 && millis() >= thinkingDemoUntil) {
      thinkingDemoUntil = 0;
      endThinkingVisual();
      if (!companionGenerating) systemState = STATE_READY;
      setEyeMood(EYE_DEFAULT);
    }

    if (activeOsrmUrl.length() > 0 && !routeReady && !routeRequested && !networkIsBusy() && wifiConnected && !hpConnected && !linkRouteReceived && osrmFailCount < OSRM_MAX_RETRIES && (lastOsrmAttempt == 0 || millis() - lastOsrmAttempt >= OSRM_RETRY_MS)) {
      startOSRM();
    }

    processNetworkDone();

    if (!networkBusy) {
      networkJobWasBusy = false;
    } else if (networkJobWasBusy && millis() - networkJobStartedAt > 45000UL) {
      abotLog("ERR", "Network job hang >45s, force-release");
      networkBusy = false;
      networkDone = false;
      networkJobWasBusy = false;
      if (systemState == STATE_THINKING) systemState = STATE_READY;
      endThinkingVisual();
    }

    ensureWeatherData();

    if (routeReady && tripActive && !arrivalActive) {
      updateGPSSimulation();
      updateHUDLatch();
    }

    unsigned long mapMs = mapForNewRoute ? ROUTE_MAP_DURATION
                                         : USER_MAP_DURATION;
    if (mapRequested && millis() - mapStartTime >= mapMs) {
      mapRequested = false;
    }

    if (pendingNavigationAI && !navigationAIStarted && routeReady && !mapRequested && !networkIsBusy()) {
      startNavigationAI();
    }

    updateCompanion();
    updateCompanionNextMenu();
    updateSleepMode();
  }

  linkReportStatus();
  delay(1);

    // --- VERIFIKASI OTA SETELAH 30 DETIK BERJALAN NORMAL ---
  if (pendingOtaVerification && (millis() - bootTimeMs > 30000)) {
    pendingOtaVerification = false;
    
    // Tandai firmware baru ini VALID (Batalkan rollback otomatis)
    esp_ota_mark_app_valid_cancel_rollback();
    
    // Cek apakah versi yang dilaporkan cocok dengan target
    if (String(FW_VERSION) == otaTargetVersion) {
      linkSend(String("{\"t\":\"ota_result\",\"ok\":true,\"ver\":\"") + FW_VERSION + "\"}");
      abotLog("INFO", "OTA Sukses terverifikasi (v%s)", FW_VERSION);
    } else {
      linkSend(String("{\"t\":\"ota_result\",\"ok\":false,\"ver\":\"") + FW_VERSION + "\",\"rolled_back\":true}");
      abotLog("ERR", "OTA Gagal! Versi sekarang %s, target %s", FW_VERSION, otaTargetVersion.c_str());
    }

    // Hapus status pending di NVS
    otaPrefs.begin("ota", false);
    otaPrefs.putBool("pending", false);
    otaPrefs.end();
  }


}
