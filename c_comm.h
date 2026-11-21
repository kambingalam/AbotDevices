#pragma once
// Tab .ino tidak boleh diawali preprocessor directive (bug ##line Arduino IDE).
#include <Arduino.h>
// ===========================================================================
//  MQTT
// ===========================================================================
void mqttBegin() {
  mqttNetworkClient.setCACert(ROOT_CA_BUNDLE);
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

  if (mqttMutex != nullptr && xSemaphoreTake(mqttMutex, portMAX_DELAY) == pdTRUE) {
    mqttClient.publish(MQTT_TOPIC_STATUS, buf);
    xSemaphoreGive(mqttMutex);
  }
}

void mqttUpdate() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (otaHoldMqtt) return;

  if (mqttClient.connected()) {
    if (mqttMutex != nullptr && xSemaphoreTake(mqttMutex, portMAX_DELAY) == pdTRUE) {
      mqttClient.loop();
      xSemaphoreGive(mqttMutex);
    }

    if (mqttPendingDispatch.length() > 0) {
      String msg = mqttPendingDispatch;
      mqttPendingDispatch = "";
      // Tandai bahwa baris ini datang dari MQTT, supaya ota_start bisa ditolak.
      dispatchFromMqtt = true;
      linkDispatchLine(msg);
      dispatchFromMqtt = false;
    }
    mqttHeartbeat();
    return;
  }

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
  bleSend(json);

  if (mqttMutex != nullptr && xSemaphoreTake(mqttMutex, portMAX_DELAY) == pdTRUE) {
    if (!mqttClient.connected()) mqttClient.connect();
    bool ok = mqttClient.publish(MQTT_TOPIC_STATUS, json.c_str());
    if (!ok) Serial.println("[MQTT] Publish status gagal");
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
      if (buffer.length() > 0) {
        dispatchFromMqtt = false;  // serial path
        linkDispatchLine(buffer);
      }
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

// ===========================================================================
//  OTA: validasi + konfirmasi tombol + peluncuran task
// ===========================================================================
static bool otaUrlAllowed(const String& url) {
  if (url.length() < 20 || url.length() > 300) return false;
  if (url.indexOf("..") >= 0 || url.indexOf('@') >= 0) return false;
  String u = url;
  u.toLowerCase();
  for (const char* p : OTA_URL_PREFIXES) {
    String pre = String(p);
    pre.toLowerCase();
    if (u.startsWith(pre)) return true;
  }
  return false;
}

static bool otaSha256Valid(const char* s) {
  if (!s || strlen(s) != 64) return false;
  for (int i = 0; i < 64; i++) {
    if (!isxdigit((unsigned char)s[i])) return false;
  }
  return true;
}

static void otaSendErr(const String& id, const char* code, const char* msg) {
  linkSend(String("{\"t\":\"ota_error\",\"id\":\"") + jsonEscape(id) + "\",\"code\":\"" + code + "\",\"message\":\"" + jsonEscape(String(msg)) + "\"}");
}

// Membuat task OTA (stack 16 KB karena TLS). Dipanggil di bawah StateLock.
static void otaLaunch(const String& url, const String& id, const String& ver, const String& sha) {
  struct OtaParams {
    String url;
    String id;
    String ver;
    String sha;
  };
  OtaParams* params = new OtaParams{ url, id, ver, sha };
  otaCancelRequested = false;
  otaLaunched = true;
  markUserActivity();

  otaHoldMqtt = true;
  if (mqttMutex != nullptr && xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    if (mqttClient.connected()) mqttClient.disconnect();
    xSemaphoreGive(mqttMutex);
  }
  delay(500);  // beri waktu blok TLS MQTT benar-benar dilepas
  abotLog("INFO", "OTA: MQTT dilepas, heap=%u", (unsigned)ESP.getFreeHeap());

  BaseType_t r = xTaskCreatePinnedToCore([](void* pv) {
    OtaParams* p = (OtaParams*)pv;
    startOTAUpdate(p->url, p->id, p->ver, p->sha);
    delete p;
    otaLaunched = false;  // hanya tercapai kalau OTA gagal (sukses = restart)
    otaHoldMqtt = false;
    mqttLastReconnectAttempt = 0;
    vTaskDelete(NULL);
  },
                                         "OtaTask", 16384, params, 1, NULL, 0);
  if (r != pdPASS) {
    delete params;
    otaLaunched = false;
    otaHoldMqtt = false;
    otaSendErr(id, "no_memory", "Gagal membuat task OTA");
  }
}

static void otaConfirmCancel(const char* why) {
  if (!otaConfirmActive) return;
  otaConfirmActive = false;
  otaSendErr(otaPendId, "cancelled", why);
  abotLog("INFO", "OTA batal: %s", why);
}

static void otaConfirmAccept() {
  if (!otaConfirmActive) return;
  otaConfirmActive = false;
  if (voicePipelineActive || listeningActive || musicState != MUS_STOPPED || otaActive || otaLaunched) {
    otaSendErr(otaPendId, "busy", "Perangkat sedang sibuk");
    return;
  }
  abotLog("INFO", "OTA dikonfirmasi lewat tombol");
  otaLaunch(otaPendUrl, otaPendId, otaPendVer, otaPendSha);
}

static void updateOtaConfirm() {
  if (otaConfirmActive && (long)(millis() - otaConfirmUntil) >= 0) {
    otaConfirmCancel("Tidak dikonfirmasi di perangkat");
  }
}

// ota_start dari BLE ditunda sampai BLE benar-benar lepas dan heap naik.
static void updateOtaDeferred() {
  if (!otaDeferPending) return;
  if ((long)(millis() - otaDeferAt) < 0) return;
  otaDeferPending = false;
  abotLog("INFO", "OTA mulai, heap=%u", (unsigned)ESP.getFreeHeap());
  otaLaunch(otaPendUrl, otaPendId, otaPendVer, otaPendSha);
}

void cfgApplyTopic() {
  const char* base = cfgTopic.length() ? cfgTopic.c_str() : MQTT_TOPIC_BASE_DEFAULT;
  snprintf(MQTT_TOPIC_COMMAND, sizeof(MQTT_TOPIC_COMMAND), "%s/command", base);
  snprintf(MQTT_TOPIC_STATUS, sizeof(MQTT_TOPIC_STATUS), "%s/status", base);
}

// Terapkan topik baru: putuskan MQTT, mqttUpdate() akan menyambung + subscribe ulang.
static void mqttApplyTopicNow() {
  cfgApplyTopic();
  if (mqttMutex != nullptr && xSemaphoreTake(mqttMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    if (mqttClient.connected()) mqttClient.disconnect();
    xSemaphoreGive(mqttMutex);
  }
  mqttLastReconnectAttempt = 0;
  mqttBackoffMs = MQTT_RECONNECT_INTERVAL_MS;
}

void cfgHandleLine(String line) {
  line.trim();

  if (line == "CFG?") {
    Serial.printf("[CFG] groq=%s (len %u) | ssid=%s | pass=%s | auth=%s | topic=%s\n",
                  cfgGroqKey.length() ? "OK" : "KOSONG", (unsigned)cfgGroqKey.length(),
                  cfgWifiSsid.length() ? cfgWifiSsid.c_str() : "KOSONG",
                  cfgWifiPass.length() ? "OK" : "KOSONG",
                  cfgAuthToken.length() ? "OK" : "KOSONG",
                  cfgTopic.length() ? "CUSTOM" : "DEFAULT (publik di GitHub!)");
    return;
  }

  if (line == "CFG CLEAR") {
    Preferences p;
    p.begin("cfg", false);
    p.clear();
    p.end();
    cfgGroqKey = "";
    cfgWifiSsid = "";
    cfgWifiPass = "";
    cfgAuthToken = "";
    cfgTopic = "";
    cfgWifiChanged = true;
    modelChecked = false;
    mqttApplyTopicNow();
    Serial.println("[CFG] Semua kredensial dihapus");
    return;
  }

  // SET <kunci>=<nilai>
  int eq = line.indexOf('=');
  if (!line.startsWith("SET ") || eq < 5) {
    Serial.println("[CFG] Format: SET groq=... | SET ssid=... | SET pass=... | SET auth=... | SET topic=... | CFG? | CFG CLEAR");
    return;
  }
  String k = line.substring(4, eq);
  String v = line.substring(eq + 1);
  k.trim();
  k.toLowerCase();
  v.trim();

  if (v.length() == 0 || v.length() > 200) {
    Serial.println("[CFG] Nilai kosong atau terlalu panjang");
    return;
  }

  bool topicChanged = false;
  Preferences p;
  p.begin("cfg", false);
  if (k == "groq") {
    if (!v.startsWith("gsk_")) {
      p.end();
      Serial.println("[CFG] Key Groq harus diawali gsk_");
      return;
    }
    p.putString("groq", v);
    cfgGroqKey = v;
    modelChecked = false;
    Serial.println("[CFG] Key Groq tersimpan");
  } else if (k == "ssid") {
    p.putString("ssid", v);
    cfgWifiSsid = v;
    cfgWifiChanged = true;
    Serial.println("[CFG] SSID tersimpan");
  } else if (k == "pass") {
    p.putString("pass", v);
    cfgWifiPass = v;
    cfgWifiChanged = true;
    Serial.println("[CFG] Password WiFi tersimpan");
  } else if (k == "auth") {
    p.putString("auth", v);
    cfgAuthToken = v;
    Serial.println("[CFG] Auth token tersimpan");
  } else if (k == "topic") {
    bool okT = (v.length() >= 8 && v.length() <= 80 && v[0] != '/' && v[v.length() - 1] != '/');
    for (size_t i = 0; okT && i < v.length(); i++) {
      const char c = v[i];
      okT = isalnum((unsigned char)c) || c == '/' || c == '_' || c == '-';
    }
    if (!okT) {
      Serial.println("[CFG] Topik: 8-80 karakter (huruf, angka, _ - /), tanpa / di awal atau akhir");
    } else {
      p.putString("topic", v);
      cfgTopic = v;
      topicChanged = true;
      Serial.println("[CFG] Topik MQTT tersimpan. Samakan dengan pengaturan di aplikasi HP.");
    }
  } else {
    Serial.println("[CFG] Kunci tidak dikenal (groq / ssid / pass / auth / topic)");
  }
  p.end();
  if (topicChanged) mqttApplyTopicNow();

  // Terapkan WiFi baru: putuskan, ensureWiFiConnected() akan menyambung ulang
  if (cfgWifiChanged && cfgWifiSsid.length() > 0) {
    cfgWifiChanged = false;
    lastWiFiAttempt = 0;
    WiFi.disconnect();
  }
}

void linkDispatchLine(const String& line) {
  // Provisioning lewat Serial atau BLE (SET/CFG? dirutekan ke cfgHandleLine).
  if (line.startsWith("SET ") || line == "CFG?" || line == "CFG CLEAR") {
    cfgHandleLine(line);
    return;
  }
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
    if (cfgAuthToken.length() == 0) {
      isAuthorized = false;
      abotLog("WARN", "AUTH GAGAL. Token belum diatur di NVS. Gunakan SET auth=... via Serial.");
      linkSend("{\"t\":\"auth_fail\",\"why\":\"no_token_configured\"}");
      return;
    }
    if (strcmp(token, cfgAuthToken.c_str()) == 0) {
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

  // ping/status read-only: dibutuhkan halaman BLE Diagnostic sebelum AUTH.
  const bool isPublic = (strcmp(t, "ping") == 0) || (strcmp(t, "status") == 0);
  if (!isAuthorized && !isPublic) {
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
  else if (strcmp(t, "play") == 0) {
    const String url = String((const char*)(doc["url"] | ""));
    if (url.length() < 10) {
      linkSend("{\"t\":\"nack\",\"why\":\"bad_url\"}");
    } else {
      musicRequestUrl(url);
      linkSend("{\"t\":\"ack\",\"for\":\"play\"}");
    }
  }
  else if (strcmp(t, "music_stop") == 0) {
    musicRequestPost(MCMD_STOP, "", "");
    linkSend("{\"t\":\"ack\",\"for\":\"music_stop\"}");
  }
  else if (strcmp(t, "ota_start") == 0) {
    const String rid = String((const char*)(doc["id"] | ""));
    const char* targetVersion = doc["ver"] | "";
    const char* firmwareUrl = doc["url"] | "";
    const char* firmwareSha = doc["sha256"] | "";

    if (!otaSha256Valid(firmwareSha)) {
      otaSendErr(rid, "bad_hash", "sha256 wajib 64 karakter hex");
      return;
    }
    if (strlen(targetVersion) > 16) {
      otaSendErr(rid, "manifest_invalid", "versi terlalu panjang");
      return;
    }
    if (strlen(firmwareUrl) == 0) {
      otaSendErr(rid, "manifest_invalid", "URL kosong");
      return;
    }
    // Hanya firmware dari repo sendiri (cegah firmware dari sumber lain).
    if (!otaUrlAllowed(String(firmwareUrl))) {
      Serial.printf("[OTA] URL ditolak: %s\n", firmwareUrl);
      abotLog("WARN", "OTA ditolak: URL tidak diizinkan");
      otaSendErr(rid, "url_denied", "URL firmware bukan dari repo yang diizinkan");
      return;
    }
    if (otaActive || otaLaunched || otaConfirmActive) {
      otaSendErr(rid, "busy", "OTA sudah berjalan atau menunggu konfirmasi");
      return;
    }
    if (voicePipelineActive || listeningActive || musicState != MUS_STOPPED) {
      otaSendErr(rid, "busy", "Selesaikan aktivitas saat ini");
      return;
    }

    if (dispatchFromMqtt) {
      // Perintah dari jaringan (broker publik): minta persetujuan fisik.
      otaPendUrl = String(firmwareUrl);
      otaPendId = rid;
      otaPendVer = String(targetVersion);
      otaPendSha = String(firmwareSha);
      otaConfirmStartedAt = millis();
      otaConfirmUntil = otaConfirmStartedAt + OTA_CONFIRM_TIMEOUT_MS;
      otaConfirmActive = true;
      markUserActivity();
      sfxPlay(SFX_REPLY);
      abotLog("INFO", "OTA menunggu konfirmasi tombol");
      // Jaga agar aplikasi tetap menampilkan "sedang proses" selama menunggu.
      linkSend(String("{\"t\":\"ota_progress\",\"id\":\"") + jsonEscape(rid) + "\",\"stage\":\"manifest\",\"progress\":0,\"message\":\"Tekan ACTION di perangkat untuk konfirmasi\"}");
      return;
    }
    // Dari Serial/BLE: langsung jalan, tapi BLE dilepas dulu (TLS butuh RAM).
    otaPendUrl = String(firmwareUrl);
    otaPendId = rid;
    otaPendVer = String(targetVersion);
    otaPendSha = String(firmwareSha);
    otaDeferAt = millis() + 800;
    otaDeferPending = true;
    bleShutdownRequested = true;
    linkSend(String("{\"t\":\"ota_progress\",\"id\":\"") + jsonEscape(rid) + "\",\"stage\":\"manifest\",\"progress\":0,\"message\":\"Melepas BLE, menyiapkan unduhan\"}");
  } else if (strcmp(t, "ota_cancel") == 0) {
    const String rid = String((const char*)(doc["id"] | ""));
    if (otaConfirmActive) {
      otaConfirmCancel("Dibatalkan dari aplikasi");
    } else if (otaActive || otaLaunched) {
      if (otaActive && otaStageIdx >= 2) {
        otaSendErr(rid, "too_late", "Sudah tahap pemasangan, tidak bisa dibatalkan");
      } else {
        otaCancelRequested = true;  // task OTA yang mengirim ota_error "cancelled"
      }
    } else {
      otaSendErr(rid, "cancelled", "OTA dibatalkan");
    }
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

