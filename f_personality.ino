// ===========================================================================
//  PERSONALITY & EYES
// ===========================================================================
const char* abotPersonalityName(AbotPersonality p) {
  switch (p) {
    case ABOT_MORNING: return "PAGI / SEMANGAT";
    case ABOT_DAY: return "SIANG / MARAH";
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
    case ABOT_DAY: return EYE_ANGRY;  // siang: mata marah
    case ABOT_AFTERNOON: return EYE_TIRED;
    case ABOT_NIGHT: return EYE_TIRED;
    default: return EYE_DEFAULT;
  }
}

String abotPromptHint() {
  switch (currentAbotPersonality) {
    case ABOT_MORNING: return " Suasana pagi: nada kamu bersemangat dan ceria.";
    case ABOT_DAY: return " Suasana siang: nada kamu agak ketus dan pura-pura kesal karena panas, tapi tetap peduli dan hangat.";
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
    (desired == EYE_HAPPY)   ? CHARACTER_HAPPY
    : (desired == EYE_ANGRY) ? CHARACTER_ANGRY
    : (desired == EYE_TIRED) ? CHARACTER_TIRED
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
    !bootTransitionActive && !listeningActive && !voicePipelineActive && !startupOnboardingActive && !destinationAskActive && !infoModeActive && !tripIntroActive && !companionGenerating && !companionActive && !companionNextMenuActive && !companionNextMenuPending && !conversationActive && systemState != STATE_THINKING && !mapRequested && !hudActive && !arrivalActive && !characterReactionActive && !otaActive && !otaConfirmActive && !musicScreenWanted();
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

