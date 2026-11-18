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
      client.setCACert(ROOT_CA_BUNDLE);
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
      client.setCACert(ROOT_CA_BUNDLE);
      HTTPClient http;
      http.setTimeout(CHAT_TIMEOUT_MS);

      if (http.begin(client, GROQ_CHAT_URL)) {
        http.addHeader("Content-Type", "application/json");
        http.addHeader("Accept", "application/json");
        String authorization = "Bearer ";
        authorization += cfgGroqKey;
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
  if (text.length() == 0) {
    presentFrame();
    return;
  }

  const int maxChars = 21;
  const int len = text.length();
  int pos = 0, lines = 0, y = 13;

  while (pos < len && lines < 5) {
    int end = pos + maxChars;
    if (end >= len) {
      end = len;
    } else {
      int sp = text.lastIndexOf(' ', end);
      if (sp > pos) end = sp;
    }

    String line = text.substring(pos, end);
    line.trim();

    if (line.length() > 0) {
      gfxText(0, y, line);
      y += 10;
      lines++;
    }

    pos = end;
    while (pos < len && text[pos] == ' ') pos++;
  }

  presentFrame();
}

