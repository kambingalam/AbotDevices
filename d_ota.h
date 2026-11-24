#pragma once
// Tab .ino tidak boleh diawali preprocessor directive (bug ##line Arduino IDE).
#include <Arduino.h>
#include <memory>
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

  otaProgress = 0;
  otaStageIdx = 0;
  otaActive = true;
  markUserActivity();
  String effectiveTarget = targetVersion.length() ? targetVersion : otaReportedVersion;
  String accepted = String("{\"t\":\"ota_accepted\",\"id\":\"") + requestId + "\",\"from\":\"" + otaReportedVersion + "\",\"to\":\"" + effectiveTarget + "\",\"sim\":true}";
  linkSend(accepted);
  abotLog("INFO", "OTA Simulation Mode aktif; target=%s", effectiveTarget.c_str());

  const char* stages[] = { "wifi", "manifest", "download", "verify", "install" };
  const int stageProgress[] = { 100, 100, 25, 75, 100 };

  for (int i = 0; i < 5; i++) {
    otaProgress = stageProgress[i];
    otaStageIdx = (i >= 3) ? 2 : (i == 2 ? 1 : 0);
    linkSend(String("{\"t\":\"ota_progress\",\"id\":\"") + requestId + "\",\"stage\":\"" + stages[i] + "\",\"progress\":" + String(stageProgress[i]) + ",\"sim\":true}");
    delay(i < 2 ? 300 : 500);
  }

  otaReportedVersion = effectiveTarget;
  otaProgress = 100;
  otaStageIdx = 3;
  linkSend(String("{\"t\":\"ota_done\",\"id\":\"") + requestId + "\",\"to\":\"" + effectiveTarget + "\",\"sim\":true}");
  delay(900);
  linkSend(String("{\"t\":\"ota_result\",\"id\":\"") + requestId + "\",\"ok\":true,\"ver\":\"" + effectiveTarget + "\",\"sim\":true}");
  linkSend(String("{\"t\":\"ota_simulated\",\"id\":\"") + requestId + "\",\"ver\":\"" + effectiveTarget + "\"}");
  abotLog("INFO", "Simulasi OTA selesai; versi virtual=%s", effectiveTarget.c_str());
  otaActive = false;
  return;
#else
  // ---------- OTA BENERAN ----------
  if (expectedSha256.length() != 64) {
    linkSend(String("{\"t\":\"ota_error\",\"id\":\"") + jsonEscape(requestId) + "\",\"code\":\"bad_hash\",\"message\":\"sha256 wajib 64 karakter\"}");
    return;
  }
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

  // Layar OLED langsung beralih ke tampilan progress OTA
  otaProgress = 0;
  otaStageIdx = 0;
  otaActive = true;
  {
    StateLock lock;
    markUserActivity();  // bangunkan dari sleep mode, state tidur bersih
  }
  const String effectiveTarget =
    targetVersion.length() ? targetVersion : otaReportedVersion;

  abotLog("INFO", "OTA mulai: %s -> %s", otaReportedVersion.c_str(),
          effectiveTarget.c_str());

  // Simpan target versi ke NVS sebelum mulai
  otaPrefs.begin("ota", false);
  otaPrefs.putString("target", effectiveTarget);
  otaPrefs.putBool("pending", true);
  otaPrefs.end();

  linkSend(String("{\"t\":\"ota_accepted\",\"id\":\"") + requestId + "\",\"from\":\"" + otaReportedVersion + "\",\"to\":\"" + effectiveTarget + "\"}");

  // flashed = true setelah Update.end(true) berhasil. Sebelum itu, kalau OTA
  // gagal, aman untuk membersihkan pending (partisi belum di-swap, jadi tidak
  // akan ada false "rolled_back" report).
  bool flashed = false;
  auto sendError = [&](const char* code, const String& msg) {
    String errMsg = String("{\"t\":\"ota_error\",\"id\":\"") + requestId + "\",\"code\":\"" + code + "\"";
    if (msg.length() > 0) errMsg += ",\"message\":\"" + jsonEscape(msg) + "\"";
    errMsg += "}";
    linkSend(errMsg);

    if (!flashed) {
      // Belum sempat Update.end(true). Aman hapus pending supaya boot
      // berikutnya tidak salah lapor "rollback".
      otaPrefs.begin("ota", false);
      otaPrefs.putBool("pending", false);
      otaPrefs.end();
    }
    // Kalau sudah flashed, biarkan pending = true. Boot berikutnya akan
    // verifikasi lewat loop() (30 detik).

    Serial.printf("[OTA] sisa stack minimum: %u byte\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
    // Tampilkan GAGAL di OLED sebentar sebelum layar kembali normal
    otaStageIdx = 4;
    delay(2500);
    otaActive = false;
  };

  linkSend(String("{\"t\":\"ota_progress\",\"id\":\"") + requestId + "\",\"stage\":\"download\",\"progress\":0}");
  abotLog("INFO", "OTA heap awal %u", (unsigned)ESP.getFreeHeap());
  // Blok TLS 16 KB per arah sulit didapat saat heap terfragmentasi.
  if (ESP.getFreeHeap() < 45000) {
    sendError("low_heap", "heap " + String(ESP.getFreeHeap()));
    return;
  }

  // Redirect diikuti manual: satu client baru per hop, tiap langkah dicatat.
  // TLS tanpa CA: integritas dijaga sha256 dari manifest yang dikirim lewat MQTT
  // terverifikasi. Verifikasi CA butuh merantai sertifikat + 32 KB buffer TLS,
  // dan bersamaan dengan MQTT heap tidak cukup (SSL - Memory allocation failed).
  std::unique_ptr<WiFiClientSecure> secureClient;
  std::unique_ptr<WiFiClient> plainClient;
  HTTPClient http;
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  String target = url;
  int httpCode = -1;
  char tlsMsg[64] = {0};

  for (int hop = 0; hop < 4; hop++) {
    http.end();
    secureClient.reset();
    plainClient.reset();

    bool begun = false;
    if (target.startsWith("https")) {
      secureClient.reset(new WiFiClientSecure());
      secureClient->setInsecure();
      secureClient->setHandshakeTimeout(30);
      // Buffer TLS diset via setInsecure() + setHandshakeTimeout(30) saja.
      // setBufferSizes() tidak tersedia di ESP32 core 3.3.12.
      begun = http.begin(*secureClient, target);
    } else {
      plainClient.reset(new WiFiClient());
      begun = http.begin(*plainClient, target);
    }
    if (!begun) {
      httpCode = -1;
      break;
    }

    // WAJIB: tanpa collectHeaders, http.header("Location") selalu kosong, jadi
    // redirect GitHub (302 -> release-assets) tidak pernah diikuti dan OTA gagal
    // dengan "HTTP 302". Harus dipanggil setelah begin() dan sebelum GET().
    static const char* kHdr[] = { "Location" };
    http.collectHeaders(kHdr, 1);

    httpCode = http.GET();
    tlsMsg[0] = 0;
    if (secureClient) secureClient->lastError(tlsMsg, sizeof(tlsMsg));
    abotLog("INFO", "OTA hop=%d code=%d err=%s heap=%u", hop, httpCode,
            tlsMsg[0] ? tlsMsg : "-", (unsigned)ESP.getFreeHeap());

    if (httpCode != 301 && httpCode != 302 && httpCode != 303 &&
        httpCode != 307 && httpCode != 308) {
      break;
    }

    String loc = http.header("Location");
    if (loc.length() == 0) {
      Serial.printf("[OTA] redirect %d tanpa header Location\n", httpCode);
      break;
    }
    if (loc.startsWith("/")) {
      int proto = target.indexOf("://");
      int slash = proto < 0 ? -1 : target.indexOf('/', proto + 3);
      loc = (slash < 0 ? target : target.substring(0, slash)) + loc;
    }
    target = loc;
  }

  if (httpCode != 200) {
    sendError("download_failed", "HTTP " + String(httpCode) + " err " +
                                   String(tlsMsg[0] ? tlsMsg : "-") + " heap " +
                                   String(ESP.getFreeHeap()));
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
    esp_task_wdt_reset();

    if (otaCancelRequested) {
      Update.abort();
      mbedtls_sha256_free(&shaCtx);
      http.end();
      sendError("cancelled", "OTA dibatalkan dari aplikasi");
      return;
    }

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
    otaProgress = progress;  // untuk layar OLED (halus, tiap iterasi)
    // Kirim ke HP: progress 1% pertama + setiap kelipatan 5%.
    if (progress == 1 || progress / 5 != lastProgress / 5) {
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

  otaProgress = 100;
  otaStageIdx = 1;  // VERIFIKASI
  linkSend(String("{\"t\":\"ota_progress\",\"id\":\"") + requestId + "\",\"stage\":\"verify\",\"progress\":100}");

  {  // SHA256 selalu diverifikasi (panjangnya sudah dipastikan 64 di atas)
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

  if (otaCancelRequested) {
    Update.abort();
    sendError("cancelled", "OTA dibatalkan dari aplikasi");
    return;
  }

  otaStageIdx = 2;  // MEMASANG
  linkSend(String("{\"t\":\"ota_progress\",\"id\":\"") + requestId + "\",\"stage\":\"install\",\"progress\":100}");

  if (!Update.end(true)) {
    sendError("write_failed", Update.errorString());
    return;
  }
  // Update.end(true) berhasil. Partisi sudah di-swap. Jangan clear pending.
  flashed = true;

  if (!Update.isFinished()) {
    sendError("write_failed", "Update tidak selesai");
    return;
  }

  otaStageIdx = 3;  // SELESAI
  abotLog("INFO", "OTA selesai, restart...");
  Serial.printf("[OTA] sisa stack minimum: %u byte\n", (unsigned)uxTaskGetStackHighWaterMark(NULL));
  linkSend(String("{\"t\":\"ota_done\",\"id\":\"") + requestId + "\",\"to\":\"" + effectiveTarget + "\"}");
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

