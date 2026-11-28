#pragma once

static void voiceTask(void* pv);

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

// CATATAN V3.53.0: ESP32-audioI2S 4.x tidak lagi memanggil fungsi bebas
// audio_info()/audio_eof_speech()/audio_eof_stream(). Semua event masuk lewat
// Audio::audio_info_callback yang dipasang audioInstallCallbacks() di z_main.h.

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
  client.setCACert(ROOT_CA_BUNDLE);
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
  auth += cfgGroqKey;
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
  while (millis() - t0 < 35000) {
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

  vTaskDelay(pdMS_TO_TICKS(500));

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

