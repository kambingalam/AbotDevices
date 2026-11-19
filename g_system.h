// Tab .ino tidak boleh diawali preprocessor directive (bug ##line Arduino IDE).
#include <Arduino.h>
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
      if (otaConfirmActive) {
        otaConfirmCancel("Dibatalkan di perangkat");
        return;
      }
      if (otaActive || otaLaunched) return;
      if (talkLongTriggered && listeningActive) stopListening();
      else if (!talkLongTriggered && held < BUTTON_HOLD_MS) {
        startInfoMode();
      }
    }
  }

  if (talkStableState == LOW && !talkLongTriggered && !otaUiLocked() && now - talkPressedAt >= BUTTON_HOLD_MS) {
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
      if (otaConfirmActive) {
        // Hanya tekanan yang dimulai SETELAH layar konfirmasi muncul yang dihitung.
        if (actionPressedAt >= otaConfirmStartedAt) otaConfirmAccept();
        return;
      }
      if (otaActive || otaLaunched) return;
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
  if (cfgWifiSsid.length() == 0) return;  // belum diprovisioning
  Serial.println("[WIFI] Mencoba reconnect...");
  WiFi.disconnect();
  WiFi.begin(cfgWifiSsid.c_str(), cfgWifiPass.c_str());
}

void checkHeapHealth() {
  if (voicePipelineActive || ttsActive || otaActive) return;
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

