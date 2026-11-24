#pragma once
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
  Serial.println("PROVISIONING (Serial only, bukan MQTT):");
  Serial.println("  SET ssid=NamaWiFi | SET pass=PasswordWiFi");
  Serial.println("  SET groq=gsk_xxx | SET auth=tokenRahasia | SET topic=abot/acak123456");
  Serial.println("  CFG?  |  CFG CLEAR");
  Serial.println("DIAGNOSA (Serial only): TLS? = tes jam + sertifikat + heap");
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

