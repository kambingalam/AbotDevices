#pragma once
// Tab .ino tidak boleh diawali preprocessor directive (bug ##line Arduino IDE).
#include <Arduino.h>

// ===========================================================================
//  INCLUDE MODUL EKSTERNAL
// ===========================================================================
#define MUSIC_USE_VU 0
#include "buzzer_sfx.h"
#include "music_player.h"

// ===========================================================================
//  AI ASISTEN
// ===========================================================================
String weatherCodeToText(int code);
void handleQuizAnswer(char cmd);

static String activeChatModel = GROQ_CHAT_MODEL;
static bool modelChecked = false;

static bool groqModelUsable(const String& id) {
  String s = id;
  s.toLowerCase();
  const char* bad[] = { "whisper", "orpheus", "guard", "tts", "safeguard",
                        "embed", "distil", "playai", "compound" };
  for (auto b : bad)
    if (s.indexOf(b) >= 0) return false;
  return true;
}

static void groqEnsureModel(bool force = false) {
  static unsigned long lastTry = 0;
  if (modelChecked && !force) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!force && lastTry != 0 && millis() - lastTry < 30000UL) return;
  lastTry = millis();

  WiFiClientSecure client;
  client.setCACert(ROOT_CA_BUNDLE);
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(client, GROQ_MODELS_URL)) return;
  String auth = "Bearer ";
  auth += cfgGroqKey;
  http.addHeader("Authorization", auth);
  int code = http.GET();
  Serial.printf("[MODELS] HTTP %d\n", code);
  if (code != 200) {
    if (code > 0) Serial.printf("[MODELS] body: %s\n", http.getString().c_str());
    http.end();
    return;
  }

  JsonDocument filter;
  filter["data"][0]["id"] = true;
  JsonDocument doc;
  DeserializationError err =
    deserializeJson(doc, http.getString(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("[MODELS] JSON error: %s\n", err.c_str());
    return;
  }

  const String pref[5] = { String(GROQ_CHAT_MODEL), String(GROQ_CHAT_MODEL_2),
                           String("openai/gpt-oss-20b"), String("llama-3.3-70b-versatile"),
                           String("llama-3.1-8b-instant") };
  bool found[5] = { false, false, false, false, false };
  String firstOther = "";
  String all = "";
  for (JsonObject m : doc["data"].as<JsonArray>()) {
    String id = String((const char*)(m["id"] | ""));
    all += id;
    all += " ";
    for (int i = 0; i < 5; i++)
      if (id == pref[i]) found[i] = true;
    if (firstOther.length() == 0 && groqModelUsable(id)) firstOther = id;
  }
  Serial.printf("[MODELS] tersedia: %s\n", all.c_str());

  String pick = "";
  for (int i = 0; i < 5; i++)
    if (found[i]) {
      pick = pref[i];
      break;
    }
  if (pick.length() == 0) pick = firstOther;
  if (pick.length() > 0) {
    activeChatModel = pick;
    modelChecked = true;
    Serial.printf("[MODELS] dipakai: %s\n", activeChatModel.c_str());
  } else {
    Serial.println("[MODELS] tidak ada model chat yang bisa dipakai key ini");
  }
}

// ---------------------------------------------------------------------------
//  Memori percakapan + konteks perjalanan
// ---------------------------------------------------------------------------
constexpr int HIST_TURNS = 4;
static String histUser[HIST_TURNS], histBot[HIST_TURNS];
static int histCount = 0;

static void histAdd(const String& u, const String& b) {
  if (histCount == HIST_TURNS) {
    for (int i = 1; i < HIST_TURNS; i++) {
      histUser[i - 1] = histUser[i];
      histBot[i - 1] = histBot[i];
    }
    histCount--;
  }
  histUser[histCount] = u;
  histBot[histCount] = b;
  histCount++;
}

static String buildContext() {
  String c = "";
  StateLock lock;
  c += " Kota asal perangkat: ";
  c += HOME_CITY;
  c += ".";

  struct tm ti;
  if (getLocalTimeSafe(&ti)) {
    char b[48];
    snprintf(b, sizeof(b), " Sekarang jam %02d:%02d WIB.", ti.tm_hour, ti.tm_min);
    c += b;
  }
  if (weatherData.valid) {
    String w = weatherData.condition;
    w.toLowerCase();
    c += " Cuaca di ";
    c += HOME_CITY;
    c += " sekarang " + w + ", " + String(weatherData.temperature, 0) + " derajat";
    c += ", hari ini terendah " + String(weatherData.tempMin, 0) + " dan tertinggi " + String(weatherData.tempMax, 0) + " derajat.";
  }
  if (routeReady && tripActive) {
    float remain = routeGeometryDistance - simulatedRouteDistance;
    if (remain < 0) remain = 0;
    float v = (speedMps < 3.0f) ? 8.0f : speedMps;
    int minutes = (int)(remain / v / 60.0f + 0.5f);
    c += " Perjalanan aktif, sisa sekitar ";
    c += (remain >= 1000.0f) ? String(remain / 1000.0f, 1) + " kilometer"
                             : String((int)remain) + " meter";
    c += ", perkiraan " + String(minutes) + " menit lagi.";
    int s = nextImportantStep();
    if (s >= 0 && s < navStepCount) {
      float d = navSteps[s].routeDistance - simulatedRouteDistance;
      if (d < 0) d = 0;
      String ins = navSteps[s].instruction;
      ins.toLowerCase();
      c += " Belokan berikutnya: " + ins + " dalam " + String((int)d) + " meter.";
    }
  } else {
    c += " Saat ini tidak ada perjalanan aktif.";
  }
  c += abotPromptHint();
  return c;
}

static String buildSystemPrompt(const String& extra) {
  String p;
  p.reserve(2200);
  p += "Kamu Abot, asisten suara berupa robot bermata di helm driver ojol. "
       "Bicara bahasa Indonesia yang santai, hangat, dan ekspresif. "
       "Jawab SEMUA permintaan pengguna: pertanyaan umum, saran, hitungan, ngobrol, candaan, "
       "bahkan akting suara. Ikuti gaya yang diminta pengguna (misal terkejut, lucu, formal).\n"
       "Balas HANYA satu objek JSON tanpa teks lain, dengan field:\n"
       "say: kalimat yang dibacakan suara. Tanpa emoji dan markdown, enak diucapkan, "
       "1-3 kalimat (cerita boleh sampai 5 kalimat).\n"
       "show: tampilan layar OLED, salah satu: none, map (peta rute), clock (jam), "
       "weather (cuaca), quiz, trivia, humor, story, music (putar lagu), "
       "usage (cara pakai), intro (animasi perkenalan), card (teks bebas di layar).\n"
       "title: judul pendek huruf kapital maksimal 14 karakter, untuk card.\n"
       "card: teks di layar untuk show quiz, trivia, humor, story, atau card. "
       "ASCII saja tanpa emoji, maksimal 220 karakter. Boleh dikosongkan kalau sama dengan say.\n"
       "city: nama kota HANYA jika show=weather dan pengguna menyebut kota tertentu.\n"
       "mood: ekspresi mata sesuai nada jawaban: default, happy, laugh, confused, angry, tired.\n"
       "volume: none, up, down, mute, unmute.\n"
       "verdict: hanya saat ada kuis aktif dan pengguna menjawabnya: correct, wrong, atau unknown. Selain itu kosongkan.\n"
       "music: hanya saat show=music: play, pause, resume, next, atau stop. Kalau pengguna minta putar lagu, isi play.\n"
       "query: judul lagu atau nama artis kalau pengguna menyebutnya, selain itu kosongkan.\n"
       "genre: tag musik Jamendo huruf kecil bahasa Inggris (pop, rock, lofi, chillout, lounge, jazz, "
       "electronic, hiphop, classical, acoustic, ambient, relaxation, folk, reggae, blues, metal) "
       "kalau pengguna minta suasana atau genre, selain itu kosongkan. Katalog musik ini hanya berisi musik "
       "bebas lisensi, bukan lagu artis populer seperti di Spotify. Jika pengguna meminta lagu artis populer, "
       "KOSONGKAN field query, isi genre dengan genre terdekat, dan jelaskan di say bahwa lagu tersebut tidak ada "
       "tapi kamu menawarkan lagu dengan genre serupa.\n"
       "more: true kalau kamu sedang menceritakan cerita panjang dan masih ada bagian berikutnya, selain itu false.\n"
       "Cerita: kalau pengguna tidak menyebut panjang, ceritakan utuh dalam satu balasan (more=false). "
       "Kalau pengguna minta cerita panjang atau menyebut durasi, ceritakan bertahap: tiap balasan satu "
       "bagian 4-5 kalimat, show=none, more=true sampai bagian penutup yang more=false. "
       "Satu bagian kira-kira 40 detik bicara, sesuaikan jumlah bagian dengan durasi yang diminta.\n"
       "Aturan: pakai show=card kalau pengguna minta sesuatu ditampilkan di layar "
       "(tulisan, daftar, angka, hasil hitungan, ringkasan, kata-kata) walau tidak ada di daftar. "
       "Kalau pengguna minta tampilan tertentu, pilih show yang sesuai (misal tampilkan peta -> map). "
       "Kalau tidak perlu tampilan pakai none. Untuk quiz, say berisi pertanyaan lalu ajak pengguna "
       "menjawab, jangan bocorkan jawabannya. Transkrip suara bisa salah dengar, tebak maksud yang "
       "paling masuk akal. Jangan mengarang data jam, cuaca, atau rute, pakai data di bawah. "
       "Kamu belum punya data GPS lokasi saat ini.\n"
       "Data saat ini:";
  p += buildContext();
  p += extra;
  return p;
}

static int groqPost(const String& model, bool jsonMode, JsonDocument& doc, String& out) {
  doc["model"] = model;
  const bool gptOss = model.startsWith("openai/gpt-oss");
  const bool thinky = gptOss || model.indexOf("qwen") >= 0;
  doc["max_tokens"] = thinky ? 900 : 380;
  if (jsonMode && gptOss) doc["reasoning_effort"] = "low";
  else doc.remove("reasoning_effort");
  if (jsonMode) doc["response_format"]["type"] = "json_object";
  else doc.remove("response_format");

  String payload;
  serializeJson(doc, payload);

  WiFiClientSecure client;
  client.setCACert(ROOT_CA_BUNDLE);
  HTTPClient http;
  http.setTimeout(CHAT_TIMEOUT_MS);
  if (!http.begin(client, GROQ_CHAT_URL)) {
    lastLlmHttp = -1;
    return -1;
  }

  String auth = "Bearer ";
  auth += cfgGroqKey;
  http.addHeader("Authorization", auth);
  http.addHeader("Content-Type", "application/json");

  Serial.printf("[LLM] Request model='%s' (len %d) json=%d\n",
                model.c_str(), (int)model.length(), jsonMode ? 1 : 0);
  const unsigned long t0 = millis();
  int code = http.POST(payload);
  lastLlmHttp = code;
  Serial.printf("[LLM] HTTP %d in %lu ms\n", code, millis() - t0);

  out = "";
  if (code == 200) {
    JsonDocument resp;
    if (!deserializeJson(resp, http.getString())) {
      out = String((const char*)(resp["choices"][0]["message"]["content"] | ""));
    }
  } else if (code > 0) {
    Serial.printf("[LLM] body: %s\n", http.getString().c_str());
  }
  http.end();
  return code;
}

static String lc(String s) {
  s.toLowerCase();
  return s;
}

static void cleanSpeech(String& s) {
  s.replace("*", "");
  s.replace("#", "");
  s.replace("`", "");
  s.trim();
}

static bool groqAsk(const String& userText, const String& extra, VoiceAct& act) {
  JsonDocument doc;
  doc["temperature"] = 0.7;
  JsonArray msgs = doc["messages"].to<JsonArray>();

  JsonObject sys = msgs.add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = buildSystemPrompt(extra);

  for (int i = 0; i < histCount; i++) {
    JsonObject u = msgs.add<JsonObject>();
    u["role"] = "user";
    u["content"] = histUser[i];
    JsonObject a = msgs.add<JsonObject>();
    a["role"] = "assistant";
    a["content"] = histBot[i];
  }
  JsonObject usr = msgs.add<JsonObject>();
  usr["role"] = "user";
  usr["content"] = userText;

  String content = "";
  String model = activeChatModel;
  int code = groqPost(model, true, doc, content);
  if (code == 400) code = groqPost(model, false, doc, content);

  if ((code == 404 || code == 403) && content.length() == 0) {
    modelChecked = false;
    groqEnsureModel(true);
    if (activeChatModel != model) {
      model = activeChatModel;
      code = groqPost(model, true, doc, content);
      if (code == 400) code = groqPost(model, false, doc, content);
    }
  }
  if (content.length() == 0 && code > 0 && code != 401 && model != String(GROQ_CHAT_MODEL_2)) {
    code = groqPost(String(GROQ_CHAT_MODEL_2), true, doc, content);
    if (code == 400) code = groqPost(String(GROQ_CHAT_MODEL_2), false, doc, content);
  }

  int th = content.lastIndexOf("</think>");
  if (th >= 0) content = content.substring(th + 8);
  content.trim();
  if (content.length() == 0) return false;

  act = VoiceAct();
  bool parsed = false;
  int a = content.indexOf('{');
  int b = content.lastIndexOf('}');
  if (a >= 0 && b > a) {
    JsonDocument j;
    if (!deserializeJson(j, content.substring(a, b + 1))) {
      act.say = String((const char*)(j["say"] | ""));
      act.show = lc(String((const char*)(j["show"] | "none")));
      act.title = String((const char*)(j["title"] | ""));
      act.card = String((const char*)(j["card"] | ""));
      act.city = String((const char*)(j["city"] | ""));
      act.mood = lc(String((const char*)(j["mood"] | "default")));
      act.volume = lc(String((const char*)(j["volume"] | "none")));
      act.verdict = lc(String((const char*)(j["verdict"] | "")));
      act.music = lc(String((const char*)(j["music"] | "")));
      act.query = String((const char*)(j["query"] | ""));
      act.genre = lc(String((const char*)(j["genre"] | "")));
      act.more = j["more"] | false;
      parsed = true;
    }
  }
  if (!parsed) {
    act.say = content;
    act.show = "none";
  }
  if (act.say.length() == 0 && act.card.length() > 0) act.say = act.card;
  if (act.say.length() == 0) return false;

  cleanSpeech(act.say);
  act.show.trim();
  act.city.trim();
  act.title.trim();

  const char* okShow[] = { "none", "map", "clock", "weather", "quiz", "trivia", "humor",
                           "story", "music", "usage", "intro", "card" };
  bool valid = false;
  for (auto s : okShow)
    if (act.show == s) valid = true;
  if (!valid) act.show = "none";
  if (act.more && act.show != "none") act.show = "none";
  act.query.trim();
  act.genre.trim();
  return true;
}

static String llmErrorText() {
  if (WiFi.status() != WL_CONNECTED || lastLlmHttp < 0)
    return "Koneksi internet lagi putus, coba lagi nanti ya.";
  if (lastLlmHttp == 0) return "Koneksi ke server lagi lambat, coba ulangi ya.";
  if (lastLlmHttp == 429) return "Aku lagi kebanyakan mikir, tunggu sebentar ya.";
  if (lastLlmHttp == 401) return "Kunci API-nya bermasalah, tolong dicek.";
  if (lastLlmHttp == 404 || lastLlmHttp == 403)
    return "Model AI-nya belum bisa diakses, tolong cek pengaturan Groq.";
  return "Maaf, aku belum bisa jawab itu sekarang.";
}

static bool isWhisperHallucination(String t) {
  t.toLowerCase();
  if (t.length() < 2) return true;
  const char* bad[] = { "terima kasih telah menonton", "terima kasih sudah menonton",
                        "thanks for watching", "amara.org", "subtitle", "www." };
  for (auto b : bad)
    if (t.indexOf(b) >= 0) return true;
  return false;
}

// ---------------------------------------------------------------------------
//  Cuaca kota
// ---------------------------------------------------------------------------
static String urlEncode(const String& s) {
  String o = "";
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    }
  }
  return o;
}

static bool fetchCityWeather(const String& city, String& outName, String& outSummary) {
  double lat = 0, lon = 0;
  {
    WiFiClientSecure client;
    client.setCACert(ROOT_CA_BUNDLE);
    HTTPClient http;
    http.setTimeout(8000);
    String url = String("https://geocoding-api.open-meteo.com/v1/search?count=1&language=id&format=json&name=")
                 + urlEncode(city);
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    Serial.printf("[GEO] HTTP %d (%s)\n", code, city.c_str());
    if (code != 200) {
      http.end();
      return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    http.end();
    if (err || doc["results"][0].isNull()) return false;
    lat = doc["results"][0]["latitude"] | 0.0;
    lon = doc["results"][0]["longitude"] | 0.0;
    outName = String((const char*)(doc["results"][0]["name"] | city.c_str()));
  }

  WeatherData w;
  {
    WiFiClientSecure client;
    client.setCACert(ROOT_CA_BUNDLE);
    HTTPClient http;
    http.setTimeout(8000);
    String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(lat, 4) + "&longitude=" + String(lon, 4) + "&current=temperature_2m,weather_code"
                                                                                                                        "&daily=temperature_2m_max,temperature_2m_min&timezone=auto";
    if (!http.begin(client, url)) return false;
    int code = http.GET();
    Serial.printf("[WX] HTTP %d (%s)\n", code, outName.c_str());
    if (code != 200) {
      http.end();
      return false;
    }
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    http.end();
    if (err) return false;
    JsonObject cur = doc["current"];
    if (cur.isNull()) return false;
    w.temperature = cur["temperature_2m"] | 0.0f;
    w.weatherCode = cur["weather_code"] | -1;
    w.condition = weatherCodeToText(w.weatherCode);
    w.tempMax = doc["daily"]["temperature_2m_max"][0] | w.temperature;
    w.tempMin = doc["daily"]["temperature_2m_min"][0] | w.temperature;
    w.valid = true;
    w.updatedAt = millis();
  }

  {
    StateLock lock;
    cityWeather = w;
    weatherViewLabel = outName;
  }
  String c = w.condition;
  c.toLowerCase();
  outSummary = c + ", suhu " + String(w.temperature, 0) + " derajat, hari ini terendah " + String(w.tempMin, 0) + " dan tertinggi " + String(w.tempMax, 0) + " derajat";
  return true;
}

// ---------------------------------------------------------------------------
//  Aksi layar
// ---------------------------------------------------------------------------
static void voiceTextMode(bool on) {
  StateLock lock;
  voiceTextActive = on;
}

static void voiceClearScreens() {
  mapRequested = false;
  hudActive = false;
  infoModeActive = false;
  companionActive = false;
  companionNextMenuActive = false;
  companionNextMenuPending = false;
  conversationActive = false;
  hudSuppressUntil = millis() + HUD_SUPPRESS_DURATION;
}

static void showWeatherView(bool custom) {
  voiceClearScreens();
  cancelThinking();
  weatherViewCustom = custom;
  infoModeActive = true;
  infoPage = 1;
  infoModeStarted = millis();
  if (!custom && !weatherData.valid) lastWeatherAttempt = 0;
}

static void voiceShowIntro() {
  voiceClearScreens();
  startupOnboardingActive = false;
  bootTransitionActive = false;
  cancelThinking();
  startIntroduction();
}

static void voiceShowCard(CompanionType type, const String& title, const String& text) {
  voiceClearScreens();
  if (type == COMPANION_CARD) {
    String t = title.length() ? title : String("INFO");
    t = stripNonAscii(t);
    t.toUpperCase();
    if (t.length() > 16) t = t.substring(0, 16);
    cardTitle = t;
  }
  companionNextMenuDelayMs = 350;
  currentCompanion = type;
  finishCompanionGeneration(text, type);
}

static CharacterEmotion moodFromString(const String& m) {
  if (m == "happy") return CHARACTER_HAPPY;
  if (m == "laugh") return CHARACTER_LAUGH;
  if (m == "confused") return CHARACTER_CONFUSED;
  if (m == "angry") return CHARACTER_ANGRY;
  if (m == "tired") return CHARACTER_TIRED;
  return CHARACTER_HAPPY;
}

static void voiceApply(const VoiceAct& act) {
  StateLock lock;
  markUserActivity();

  if (act.volume == "up") audioVolumeStep(+1);
  else if (act.volume == "down") audioVolumeStep(-1);
  else if (act.volume == "mute") {
    if (volumeLevel > 0) audioSetVolume(0);
  } else if (act.volume == "unmute") {
    if (volumeLevel == 0) audioSetVolume(volumeBeforeMute > 0 ? volumeBeforeMute : 3);
  }

  const String& s = act.show;
  const String body = act.card.length() ? act.card : act.say;
  voiceTextActive = (s == "none" || act.more);

  if (quizWaitingAnswer && act.verdict.length() > 0) {
    if (act.verdict == "correct") handleQuizAnswer('J');
    else if (act.verdict == "wrong") handleQuizAnswer('K');
    else handleQuizAnswer('L');
    return;
  }
  if (s == "map") handleCommand('M');
  else if (s == "clock") startInfoMode();
  else if (s == "weather") showWeatherView(act.customWeather);
  else if (s == "music") {
    voiceClearScreens();
    musicPostFromAct(act.music, act.query, act.genre);
  } else if (s == "usage") handleCommand('U');
  else if (s == "intro") voiceShowIntro();
  else if (s == "quiz") voiceShowCard(COMPANION_QUIZ, "", body);
  else if (s == "trivia") voiceShowCard(COMPANION_TRIVIA, "", body);
  else if (s == "humor") voiceShowCard(COMPANION_HUMOR, "", body);
  else if (s == "story") voiceShowCard(COMPANION_STORY, "", body);
  else if (s == "card") voiceShowCard(COMPANION_CARD, act.title, body);

  setCharacterEmotion(moodFromString(act.mood));
}

// ---------------------------------------------------------------------------
//  Perintah lokal
// ---------------------------------------------------------------------------
static int wordCount(const String& s) {
  int n = 0;
  bool in = false;
  for (size_t i = 0; i < s.length(); i++) {
    bool sp = (s[i] == ' ');
    if (!sp && !in) {
      n++;
      in = true;
    } else if (sp) in = false;
  }
  return n;
}

static String timeSentence() {
  struct tm ti;
  if (!getLocalTimeSafe(&ti)) return "Jamnya belum sinkron, tunggu sebentar ya.";
  char b[64];
  if (ti.tm_min == 0) snprintf(b, sizeof(b), "Sekarang jam %d tepat.", ti.tm_hour);
  else snprintf(b, sizeof(b), "Sekarang jam %d lewat %d menit.", ti.tm_hour, ti.tm_min);
  return String(b);
}

static String homeWeatherSentence() {
  StateLock lock;
  if (!weatherData.valid) return "Data cuaca belum tersedia, coba lagi sebentar lagi.";
  String c = weatherData.condition;
  c.toLowerCase();
  return String("Cuaca di ") + HOME_CITY + " sekarang " + c + ", suhunya sekitar " + String(weatherData.temperature, 0) + " derajat. Hari ini antara " + String(weatherData.tempMin, 0) + " sampai " + String(weatherData.tempMax, 0) + " derajat.";
}

static bool voiceLocalCommand(String t, String& say, bool allowContent) {
  t.toLowerCase();
  say = "";
  StateLock lock;
  markUserActivity();
  voiceTextActive = false;

  if (t.indexOf("diam") >= 0 || t.indexOf("mute") >= 0 || t.indexOf("bisu") >= 0) {
    audioToggleMute();
    say = (volumeLevel > 0) ? "Oke, suara aktif lagi." : "";
    voiceTextActive = true;
    return true;
  }
  if (t.indexOf("volume naik") >= 0 || t.indexOf("lebih keras") >= 0) {
    audioVolumeStep(+1);
    say = "Volume dinaikkan.";
    voiceTextActive = true;
    return true;
  }
  if (t.indexOf("volume turun") >= 0 || t.indexOf("lebih pelan") >= 0) {
    audioVolumeStep(-1);
    say = "Volume diturunkan.";
    voiceTextActive = true;
    return true;
  }
  if (t.indexOf("peta") >= 0 || t.indexOf("navigasi") >= 0) {
    handleCommand('M');
    say = "Oke, ini petanya.";
    return true;
  }
  if (t.indexOf("cuaca") >= 0) {
    showWeatherView(false);
    say = homeWeatherSentence();
    return true;
  }
  if (t == "jam" || t.indexOf("jam berapa") >= 0 || t.indexOf("sekarang jam") >= 0) {
    startInfoMode();
    say = timeSentence();
    return true;
  }
  if (t.indexOf("lagu berikutnya") >= 0 || t.indexOf("lagu selanjutnya") >= 0 || t.indexOf("ganti lagu") >= 0 || t.indexOf("skip") >= 0) {
    musicRequestPost(MCMD_NEXT, "", "");
    say = "Oke, lagu berikutnya.";
    return true;
  }
  if (t.indexOf("stop musik") >= 0 || t.indexOf("matikan musik") >= 0 || t.indexOf("matikan lagu") >= 0 || t.indexOf("berhenti musik") >= 0) {
    musicRequestPost(MCMD_STOP, "", "");
    say = "Oke, musiknya kumatikan.";
    return true;
  }
  if (t.indexOf("lanjut musik") >= 0 || t.indexOf("lanjutkan musik") >= 0 || t.indexOf("lanjut lagu") >= 0 || t.indexOf("putar lagi") >= 0) {
    musicRequestPost(MCMD_RESUME, "", "");
    say = "Siap, kulanjutkan.";
    return true;
  }
  if (t.indexOf("intro") >= 0) {
    voiceShowIntro();
    say = "Oke, kita mulai dari awal.";
    return true;
  }
  if (allowContent) {
    if (t.indexOf("kuis") >= 0 || t.indexOf("quiz") >= 0) {
      handleCommand('Q');
      say = "Oke, ini kuisnya.";
      return true;
    }
    if (t.indexOf("trivia") >= 0) {
      handleCommand('T');
      say = "Oke, ini trivia buat kamu.";
      return true;
    }
    if (t.indexOf("humor") >= 0 || t.indexOf("lawak") >= 0) {
      handleCommand('H');
      say = "Oke, siap-siap ketawa ya.";
      return true;
    }
    if (t.indexOf("cerita") >= 0) {
      handleCommand('S');
      say = "Oke, aku ceritakan ya.";
      return true;
    }
  }
  return false;
}

