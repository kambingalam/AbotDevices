#pragma once

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

    if (mqttMutex != nullptr && xSemaphoreTake(mqttMutex, portMAX_DELAY) == pdTRUE) {
      mqttClient.publish(MQTT_TOPIC_STATUS, out.c_str());
      xSemaphoreGive(mqttMutex);
    }
    delay(10);
  }
}

