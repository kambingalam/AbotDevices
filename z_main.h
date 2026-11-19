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

  cfgLoad();
  cfgApplyTopic();
  Serial.printf("[CFG] groq=%s ssid=%s pass=%s auth=%s\n",
                cfgGroqKey.length() ? "OK" : "KOSONG",
                cfgWifiSsid.length() ? "OK" : "KOSONG",
                cfgWifiPass.length() ? "OK" : "KOSONG",
                cfgAuthToken.length() ? "OK" : "KOSONG");
  if (cfgGroqKey.length() == 0) {
    Serial.println("[CFG] Key Groq belum diatur. Kirim lewat Serial: SET groq=gsk_...");
  }
  if (cfgWifiSsid.length() == 0) {
    Serial.println("[CFG] SSID WiFi belum diatur. Kirim lewat Serial: SET ssid=...");
  }
  if (cfgAuthToken.length() == 0) {
    Serial.println("[CFG] Auth token belum diatur. Kirim lewat Serial: SET auth=...");
    Serial.println("[CFG] Tanpa auth token, HP tidak bisa connect ke perangkat.");
  }
  if (cfgTopic.length() == 0) {
    Serial.println("[CFG] PERINGATAN: topik MQTT masih default (publik di GitHub).");
    Serial.println("[CFG] Siapa pun bisa membaca status/transkrip dan melihat token auth.");
    Serial.println("[CFG] Ganti: SET topic=abot/<acak-panjang>, lalu samakan di aplikasi HP.");
  }

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
  mqttBegin();  // BLE start otomatis saat WiFi mati (bleUpdate)

  systemState = STATE_WIFI_CONNECTING;
  WiFi.mode(WIFI_STA);
  if (cfgWifiSsid.length() > 0) {
    WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());
    Serial.println("[WIFI] Connecting...");
  } else {
    Serial.println("[CFG] WiFi belum diatur. Kirim lewat Serial: SET ssid=... lalu SET pass=...");
  }

  unsigned long wifiStart = millis();
  while (cfgWifiSsid.length() > 0 && WiFi.status() != WL_CONNECTED) {
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
  bleUpdate();
  checkHeapHealth();

  sfxUpdate();

  {
    StateLock lock;
    updateCharacterEmotion();
    updateAbotPersonality();
    updateNaturalEyeBehavior();
    updateButtons();
    updateOtaConfirm();
    updateOtaDeferred();

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
    // Jangan tandai valid sebelum versi benar-benar cocok. Kalau tidak cocok,
    // batalkan slot baru dan reboot ke firmware lama.
    otaPrefs.begin("ota", false);
    otaPrefs.putBool("pending", false);
    otaPrefs.putString("target", "");
    otaPrefs.end();
    pendingOtaVerification = false;

    if (String(FW_VERSION) == otaTargetVersion) {
      esp_ota_mark_app_valid_cancel_rollback();
      linkSend(String("{\"t\":\"ota_result\",\"ok\":true,\"ver\":\"") + FW_VERSION + "\"}");
      abotLog("INFO", "OTA Sukses terverifikasi (v%s)", FW_VERSION);
    } else {
      linkSend(String("{\"t\":\"ota_result\",\"ok\":false,\"ver\":\"") + FW_VERSION + "\",\"rolled_back\":true}");
      abotLog("ERR", "OTA Gagal! Versi sekarang %s, target %s", FW_VERSION, otaTargetVersion.c_str());
      delay(200);
      esp_ota_mark_app_invalid_rollback_and_reboot();
    }
  }
}
