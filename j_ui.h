#pragma once
// ===========================================================================
//  LAYAR PROGRESS OTA
// ===========================================================================
void renderOta() {
  static const char* STAGE_TEXT[] = { "MENGUNDUH", "VERIFIKASI", "MEMASANG",
                                      "SELESAI - RESTART", "GAGAL" };
  int st = otaStageIdx;
  if (st < 0 || st > 4) st = 0;
  int pct = otaProgress;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;

  beginFrame();
  drawCenteredText("UPDATE FIRMWARE", 0, 1);
  gfxLine(8, 10, 119, 10);
  drawCenteredText(String(pct) + "%", 15, 2);
  gfxFrame(8, 40, 112, 9);
  gfxFillRect(10, 42, (108 * pct) / 100, 5);
  drawCenteredText(STAGE_TEXT[st], 53, 1);
  presentFrame();
}

void renderOtaConfirm() {
  long leftMs = (long)(otaConfirmUntil - millis());
  int left = (leftMs > 0) ? (int)(leftMs / 1000) : 0;
  String v = stripNonAscii(otaPendVer);
  if (v.length() > 9) v = v.substring(0, 9);
  v = v.length() ? ("v" + v) : String("BARU");

  beginFrame();
  drawCenteredText("UPDATE FIRMWARE?", 0, 1);
  gfxLine(8, 10, 119, 10);
  drawCenteredText(v, 15, 2);
  drawCenteredText("ACTION = YA", 40, 1);
  drawCenteredText("TALK = BATAL (" + String(left) + "s)", 52, 1);
  presentFrame();
}

void updateDisplay() {
  if (otaActive) {
    renderOta();
    return;
  }
  if (otaConfirmActive) {
    renderOtaConfirm();
    return;
  }
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

