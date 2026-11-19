/*
  ============================================================================
   ABOT MUSIC PLAYER (Audius + Jamendo)  -  dipakai oleh ABOT V3.51
  ============================================================================
   - Sumber lagu : Audius (utama, tanpa API key) -> Jamendo (cadangan)
   - Streaming   : ESP32-audioI2S (objek audioI2S yang sama dengan TTS)
   - OLED        : judul berjalan, animasi robot + equalizer, waktu,
                   ikon play/pause, progress bar
   - Semua logika audio berjalan di voiceTask (satu-satunya pemilik audioI2S),
     task lain hanya mengirim permintaan lewat musicRequestPost().

   Opsional di secrets.h : #define JAMENDO_CLIENT_ID_STR "xxxxxxxx"
   Opsional              : #define AUDIUS_APP_NAME "ABOT"
   Opsional              : music_frames.h (dibuat oleh make_frames.py)
   Opsional              : #define MUSIC_USE_VU 1  (jika getVUlevel() ada)
  ============================================================================
*/
#pragma once

#ifndef JAMENDO_CLIENT_ID_STR
#define JAMENDO_CLIENT_ID_STR ""
#endif
#ifndef AUDIUS_APP_NAME
#define AUDIUS_APP_NAME "ABOT"
#endif
#define AUDIUS_API "https://api.audius.co"

#ifndef MUSIC_USE_VU
#define MUSIC_USE_VU 0
#endif
#ifndef MUSIC_VU_GAIN
#define MUSIC_VU_GAIN 1.8f
#endif

#if __has_include("music_frames.h")
#include "music_frames.h"
#define MUSIC_HAS_FRAMES 1
#endif

// ---------------------------------------------------------------------------
//  Konstanta
// ---------------------------------------------------------------------------
constexpr uint8_t MUS_STOPPED = 0, MUS_LOADING = 1, MUS_PLAYING = 2, MUS_PAUSED = 3;
constexpr uint8_t MCMD_NONE = 0, MCMD_PLAY = 1, MCMD_TOGGLE = 2, MCMD_PAUSE = 3,
                  MCMD_RESUME = 4, MCMD_NEXT = 5, MCMD_STOP = 6, MCMD_PLAY_URL = 7;

constexpr int           MUSIC_MAX_TRACKS        = 10;
constexpr unsigned long MUSIC_LOAD_TIMEOUT_MS   = 20000UL;
constexpr unsigned long MUSIC_SOFT_PAUSE_MAX_MS = 90000UL;

constexpr int MUSIC_AREA_X = 2, MUSIC_AREA_Y = 12, MUSIC_AREA_W = 56, MUSIC_AREA_H = 40;

// ---------------------------------------------------------------------------
//  State
// ---------------------------------------------------------------------------
struct MusicTrack {
  String title;
  String artist;
  String url;
  int    duration = 0;
};

static MusicTrack musicList[MUSIC_MAX_TRACKS];
static int        musicCount = 0;
static int        musicIndex = -1;

static volatile uint8_t musicState     = MUS_STOPPED;
static volatile bool    musicStopReq   = false;
static volatile bool    musicAbortFlag = false;
static volatile bool    musicEof       = false;   // diset audio_eof_stream()

static volatile uint8_t musicReqCmd = MCMD_NONE;
static String           musicReqQuery = "";
static String           musicReqGenre = "";
static String           musicReqUrl   = "";

static char musicTitleBuf[48]  = "";
static char musicArtistBuf[32] = "";
static int  musicDurationSec   = 0;

static unsigned long musicLoadStartMs    = 0;
static unsigned long musicPlayStartMs    = 0;
static unsigned long musicPauseStartMs   = 0;
static unsigned long musicPausedAccumMs  = 0;
static bool          musicHasStarted     = false;
static int           musicFailStreak     = 0;
static unsigned long musicLastRunningMs  = 0; // <--- FIX: Untuk mendeteksi buffering

static inline bool musicScreenWanted() { return musicState != MUS_STOPPED; }
static inline bool musicHasSession()   { return musicState != MUS_STOPPED; }

// Callback ESP32-audioI2S (dipanggil di dalam audioI2S.loop()).
// Aman didefinisikan walau versi library tidak memanggilnya.
void audio_eof_stream(const char* info) {
  Serial.printf("[MUSIC] EOF stream: %s\n", info ? info : "");
  musicEof = true;
}

// ---------------------------------------------------------------------------
//  Helper
// ---------------------------------------------------------------------------
static uint32_t musicElapsedSec() {
  if (!musicHasStarted) return 0;
  const unsigned long ref = (musicState == MUS_PAUSED) ? musicPauseStartMs : millis();
  const unsigned long ms  = ref - musicPlayStartMs - musicPausedAccumMs;
  return (uint32_t)(ms / 1000UL);
}

static void musicSetDisplay(const String& title, const String& artist, int durationSec) {
  String t = stripNonAscii(title);
  t.trim();
  String a = stripNonAscii(artist);
  a.trim();
  if (t.length() == 0) t = "Lagu";
  StateLock lock;
  strncpy(musicTitleBuf, t.c_str(), sizeof(musicTitleBuf) - 1);
  musicTitleBuf[sizeof(musicTitleBuf) - 1] = 0;
  strncpy(musicArtistBuf, a.c_str(), sizeof(musicArtistBuf) - 1);
  musicArtistBuf[sizeof(musicArtistBuf) - 1] = 0;
  musicDurationSec = durationSec;
}

static String musicCleanTag(String g) {
  g.toLowerCase();
  g.trim();
  String o = "";
  for (size_t i = 0; i < g.length() && o.length() < 20; i++) {
    const char c = g[i];
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') o += c;
  }
  return o;
}

static String musicUrlEnc(const String& s) {
  String o = "";
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    const uint8_t c = (uint8_t)s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
  }
  return o;
}

static void musicSay(const String& msg) {
  { StateLock lock; voiceTextActive = true; }
  speakText(msg);
}

// Ikuti redirect manual (Audius mengarahkan /stream ke node penyimpanan).
static String musicResolveUrl(const String& url) {
  String cur = url;
  for (int hop = 0; hop < 3; hop++) {
    if (musicAbortFlag) break;
    WiFiClientSecure sclient;
    sclient.setCACert(ROOT_CA_BUNDLE);
    WiFiClient pclient;
    HTTPClient http;
    http.setTimeout(5000); // FIX: Diturunkan dari 8000ms agar tidak terlalu lama menunggu
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    const char* keys[] = {"Location"};
    http.collectHeaders(keys, 1);

    const bool https = cur.startsWith("https");
    const bool ok = https ? http.begin(sclient, cur) : http.begin(pclient, cur);
    if (!ok) break;
    const int code = http.GET();
    String loc = (code >= 300 && code < 400) ? http.header("Location") : String("");
    http.end();
    if (loc.length() == 0) break;
    cur = loc;
  }
  return cur;
}

// ---------------------------------------------------------------------------
//  Sumber cadangan: Jamendo
// ---------------------------------------------------------------------------
static int musicFetchJamendo(const String& query, const String& genre) {
  if (strlen(JAMENDO_CLIENT_ID_STR) == 0) return -2;

  String url = "https://api.jamendo.com/v3.0/tracks/?client_id=" JAMENDO_CLIENT_ID_STR
               "&format=json&limit=10&audioformat=mp31&groupby=artist_id";
  if (query.length() > 0) {
    url += "&order=relevance&search=" + musicUrlEnc(query);
  } else if (genre.length() > 0) {
    url += "&order=popularity_week&tags=" + musicUrlEnc(genre) +
           "&offset=" + String((int)random(0, 25));
  } else {
    url += "&order=popularity_week&offset=" + String((int)random(0, 100));
  }

  Serial.printf("[MUSIC] Jamendo query='%s' genre='%s'\n", query.c_str(), genre.c_str());

  WiFiClientSecure client;
  client.setCACert(ROOT_CA_BUNDLE);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) return -1;
  const int code = http.GET();
  Serial.printf("[MUSIC] Jamendo HTTP %d\n", code);
  if (code != 200) { http.end(); return -1; }

  const String payload = http.getString();
  http.end();

  JsonDocument filter;
  filter["results"][0]["name"]        = true;
  filter["results"][0]["artist_name"] = true;
  filter["results"][0]["duration"]    = true;
  filter["results"][0]["audio"]       = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[MUSIC] Jamendo JSON error: %s\n", err.c_str());
    return -1;
  }

  musicCount = 0;
  for (JsonObject t : doc["results"].as<JsonArray>()) {
    const char* audio = t["audio"] | "";
    if (strlen(audio) == 0) continue;
    MusicTrack& m = musicList[musicCount];
    m.title    = String((const char*)(t["name"] | ""));
    m.artist   = String((const char*)(t["artist_name"] | ""));
    m.url      = String(audio);
    m.duration = t["duration"] | 0;
    if (++musicCount >= MUSIC_MAX_TRACKS) break;
  }
  Serial.printf("[MUSIC] Jamendo: %d lagu\n", musicCount);
  return musicCount;
}

// ---------------------------------------------------------------------------
//  Sumber utama: Audius
// ---------------------------------------------------------------------------
static String musicAudiusGenre(const String& tag) {
  if (tag == "pop")        return "Pop";
  if (tag == "rock")       return "Rock";
  if (tag == "metal")      return "Metal";
  if (tag == "lofi")       return "Lo-Fi";
  if (tag == "hiphop")     return "Hip-Hop/Rap";
  if (tag == "electronic") return "Electronic";
  if (tag == "jazz")       return "Jazz";
  if (tag == "classical")  return "Classical";
  if (tag == "acoustic")   return "Acoustic";
  if (tag == "ambient")    return "Ambient";
  if (tag == "chillout" || tag == "lounge" || tag == "relaxation") return "Downtempo";
  if (tag == "folk")       return "Folk";
  if (tag == "reggae")     return "Reggae";
  if (tag == "blues")      return "Blues";
  return "";
}

static int musicFetchAudius(const String& query, const String& genre) {
  String url;
  if (query.length() > 0) {
    url = String(AUDIUS_API "/v1/tracks/search?limit=10&app_name=" AUDIUS_APP_NAME "&query=") +
          musicUrlEnc(query);
  } else {
    url = String(AUDIUS_API "/v1/tracks/trending?limit=10&time=week&app_name=" AUDIUS_APP_NAME);
    const String g = musicAudiusGenre(genre);
    if (g.length() > 0) url += "&genre=" + musicUrlEnc(g);
    url += "&offset=" + String((int)random(0, 20));
  }
  Serial.printf("[MUSIC] Audius URL: %s\n", url.c_str());

  WiFiClientSecure client;
  client.setCACert(ROOT_CA_BUNDLE);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, url)) return -1;
  const int code = http.GET();
  Serial.printf("[MUSIC] Audius HTTP %d\n", code);
  if (code != 200) { http.end(); return -1; }

  const String payload = http.getString();
  http.end();

  JsonDocument filter;
  filter["data"][0]["id"]            = true;
  filter["data"][0]["title"]         = true;
  filter["data"][0]["duration"]      = true;
  filter["data"][0]["is_streamable"] = true;
  filter["data"][0]["user"]["name"]  = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, payload, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("[MUSIC] Audius JSON error: %s\n", err.c_str());
    return -1;
  }

  musicCount = 0;
  for (JsonObject t : doc["data"].as<JsonArray>()) {
    const char* id = t["id"] | "";
    if (strlen(id) == 0) continue;
    if (!(t["is_streamable"] | true)) continue;
    MusicTrack& m = musicList[musicCount];
    m.title    = String((const char*)(t["title"] | ""));
    m.artist   = String((const char*)(t["user"]["name"] | ""));
    m.url      = String(AUDIUS_API "/v1/tracks/") + id + "/stream?app_name=" AUDIUS_APP_NAME;
    m.duration = t["duration"] | 0;
    if (++musicCount >= MUSIC_MAX_TRACKS) break;
  }
  Serial.printf("[MUSIC] Audius: %d lagu\n", musicCount);
  return musicCount;
}

static int musicFetchPlaylist(const String& query, const String& genre) {
  const int n = musicFetchAudius(query, genre);
  if (n > 0) return n;
  if (musicAbortFlag) return n;
  if (strlen(JAMENDO_CLIENT_ID_STR) == 0) return n;
  Serial.println("[MUSIC] Audius kosong/gagal, coba Jamendo");
  return musicFetchJamendo(query, genre);
}

// ---------------------------------------------------------------------------
//  Kontrol pemutaran
// ---------------------------------------------------------------------------
static void musicOnTrackFail();

static bool musicStartTrack(int i) {
  if (i < 0 || i >= musicCount) return false;
  musicIndex = i;
  audioI2S.stopSong();
  musicEof = false;
  musicSetDisplay(musicList[i].title, musicList[i].artist, musicList[i].duration);
  {
    StateLock lock;
    musicState       = MUS_LOADING;
    musicHasStarted  = false;
    musicLoadStartMs = millis();
  }
  delay(100);   // beri waktu audioI2S melepas stream lama

  const String url = musicResolveUrl(musicList[i].url);
  if (musicAbortFlag) {
    StateLock lock;
    musicState = MUS_STOPPED;
    return true;
  }
  Serial.printf("[MUSIC] Putar #%d: %s\n", i, musicList[i].title.c_str());

  if (!audioI2S.connecttohost(url.c_str())) {
    Serial.println("[MUSIC] connecttohost gagal");
    return false;
  }
  musicLoadStartMs = millis();
  sfxPlay(SFX_MUSIC);
  return true;
}

static void musicAdvance(int dir) {
  if (musicCount <= 0) return;
  const int next = (musicIndex + dir + musicCount) % musicCount;
  if (!musicStartTrack(next)) musicOnTrackFail();
}

static void musicOnTrackFail() {
  musicFailStreak++;
  audioI2S.stopSong();
  if (musicFailStreak >= 3 || musicCount <= 1) {
    {
      StateLock lock;
      musicState      = MUS_STOPPED;
      musicHasStarted = false;
    }
    musicFailStreak = 0;
    sfxPlay(SFX_ERROR);
    musicSay("Maaf, lagunya gagal diputar. Coba lagi nanti ya.");
    return;
  }
  musicAdvance(+1);
}

static void musicDropStream() {
  if (musicState == MUS_STOPPED && !musicStopReq) return;
  audioI2S.stopSong();
  musicStopReq = false;
  StateLock lock;
  musicState      = MUS_STOPPED;
  musicHasStarted = false;
}

// Dipanggil dari task lain (mis. saat TALK ditahan). stopSong() sebenarnya
// dijalankan voiceTask lewat musicStopReq.
static void musicStopForVoice() {
  musicAbortFlag = true;
  if (musicState != MUS_STOPPED) {
    musicStopReq    = true;
    musicState      = MUS_STOPPED;
    musicHasStarted = false;
  }
}

static void musicTick() {
  if (musicStopReq) {
    audioI2S.stopSong();
    musicStopReq = false;
    StateLock lock;
    musicState      = MUS_STOPPED;
    musicHasStarted = false;
    return;
  }

  const uint8_t st = musicState;
  if (st == MUS_LOADING || st == MUS_PLAYING) {
    audioI2S.loop();
    
    // Catat kapan terakhir kali audio benar-benar berjalan
    if (audioI2S.isRunning()) {
      musicLastRunningMs = millis();
    }

    if (st == MUS_LOADING) {
      if (audioI2S.isRunning()) {
        StateLock lock;
        musicState         = MUS_PLAYING;
        musicHasStarted    = true;
        musicPlayStartMs   = millis();
        musicPausedAccumMs = 0;
        musicFailStreak    = 0;
        musicEof           = false;
      } else if (millis() - musicLoadStartMs > MUSIC_LOAD_TIMEOUT_MS) {
        Serial.println("[MUSIC] timeout memuat lagu");
        musicOnTrackFail();
      }
    } else {
      const uint32_t el = musicElapsedSec();
      
      // FIX: Beri waktu toleransi 15 detik jika lagu buffering sebelum dianggap selesai/gagal
      bool stuckTooLong = false;
      if (!audioI2S.isRunning() && musicLastRunningMs > 0) {
        if (millis() - musicLastRunningMs > 15000UL) { // 15 detik
          stuckTooLong = true;
        }
      }

      const bool ended =
        musicEof ||
        stuckTooLong ||
        (musicDurationSec > 0 && el > (uint32_t)musicDurationSec + 5);
        
      if (ended) {
        Serial.println("[MUSIC] lagu selesai atau terputus -> berikutnya");
        musicEof = false;
        musicLastRunningMs = 0; // Reset
        musicAdvance(+1);
      }
    }
  } else if (st == MUS_PAUSED) {
    if (millis() - musicPauseStartMs > MUSIC_SOFT_PAUSE_MAX_MS) {
      Serial.println("[MUSIC] jeda terlalu lama, stream dilepas");
      audioI2S.stopSong();
      StateLock lock;
      musicState      = MUS_STOPPED;
      musicHasStarted = false;
    }
  }
}

static void musicRequestPost(uint8_t cmd, const String& query, const String& genre) {
  {
    StateLock lock;
    musicReqCmd   = cmd;
    musicReqQuery = query;
    musicReqGenre = genre;
  }
  markUserActivity();
  if (voiceTaskHandle) xTaskNotify(voiceTaskHandle, 0x02, eSetBits);
}

// Putar satu URL audio langsung, tanpa pencarian Audius/Jamendo.
static void musicRequestUrl(const String& url) {
  {
    StateLock lock;
    musicReqCmd = MCMD_PLAY_URL;
    musicReqUrl = url;
  }
  markUserActivity();
  if (voiceTaskHandle) xTaskNotify(voiceTaskHandle, 0x02, eSetBits);
}

static void musicPostFromAct(const String& cmdStr, const String& query, const String& genre) {
  uint8_t cmd = MCMD_PLAY;
  if      (cmdStr == "pause")  cmd = MCMD_PAUSE;
  else if (cmdStr == "resume") cmd = MCMD_RESUME;
  else if (cmdStr == "next")   cmd = MCMD_NEXT;
  else if (cmdStr == "stop")   cmd = MCMD_STOP;
  String q = query;
  q.trim();
  if (q.length() > 40) q = q.substring(0, 40);
  musicRequestPost(cmd, q, musicCleanTag(genre));
}

static void musicServiceRequest() {
  uint8_t cmd;
  String  q, g, u;
  {
    StateLock lock;
    cmd = musicReqCmd;
    q   = musicReqQuery;
    g   = musicReqGenre;
    u   = musicReqUrl;
    musicReqCmd   = MCMD_NONE;
    musicReqQuery = "";
    musicReqGenre = "";
    musicReqUrl   = "";
  }
  if (cmd == MCMD_NONE) return;
  musicAbortFlag = false;

  if (cmd == MCMD_STOP) {
    musicDropStream();
    musicIndex = -1;
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    sfxPlay(SFX_ERROR);
    musicSay("Koneksi internet lagi putus, musiknya belum bisa diputar.");
    return;
  }

  if (cmd == MCMD_PLAY_URL) {
    if (u.length() < 10) {
      sfxPlay(SFX_ERROR);
      musicSay("Linknya belum ada. Kirim URL file audio ya.");
      return;
    }
    audioI2S.stopSong();
    musicCount = 1;
    musicIndex = 0;
    musicList[0].title    = "Link";
    musicList[0].artist   = "";
    musicList[0].url      = u;
    musicList[0].duration = 0;
    musicSetDisplay("Memutar link...", "", 0);
    {
      StateLock lock;
      musicState       = MUS_LOADING;
      musicHasStarted  = false;
      musicLoadStartMs = millis();
    }
    delay(100);
    // shortcut: link harus direct audio (mp3/aac), redirect tidak di-resolve
    if (!audioI2S.connecttohost(u.c_str())) {
      musicOnTrackFail();
      return;
    }
    sfxPlay(SFX_MUSIC);
    return;
  }

  if (cmd == MCMD_TOGGLE) {
    if      (musicState == MUS_PLAYING)                  cmd = MCMD_PAUSE;
    else if (musicState == MUS_PAUSED)                   cmd = MCMD_RESUME;
    else if (musicIndex >= 0 && musicIndex < musicCount) cmd = MCMD_RESUME;
    else                                                 cmd = MCMD_PLAY;
  }

  if (cmd == MCMD_PAUSE) {
    if (musicState == MUS_PLAYING) {
      audioI2S.pauseResume();
      StateLock lock;
      musicState        = MUS_PAUSED;
      musicPauseStartMs = millis();
    }
    return;
  }

  if (cmd == MCMD_RESUME) {
    if (musicState == MUS_PAUSED) {
      audioI2S.pauseResume();
      StateLock lock;
      musicPausedAccumMs += millis() - musicPauseStartMs;
      musicState = MUS_PLAYING;
      return;
    }
    if (musicState == MUS_PLAYING || musicState == MUS_LOADING) return;
    if (musicIndex >= 0 && musicIndex < musicCount) {
      if (!musicStartTrack(musicIndex)) musicOnTrackFail();
      return;
    }
    cmd = MCMD_PLAY;
  }

  if (cmd == MCMD_NEXT) {
    if (musicCount > 0 && musicIndex >= 0) {
      musicAdvance(+1);
      return;
    }
    cmd = MCMD_PLAY;
  }

  // ---- MCMD_PLAY ----
  audioI2S.stopSong();
  musicSetDisplay("Mencari lagu...", "Audius/Jamendo", 0);
  {
    StateLock lock;
    musicState       = MUS_LOADING;
    musicHasStarted  = false;
    musicLoadStartMs = millis();
  }

  int n = musicFetchPlaylist(q, g);

  // --- FIX: JIKA LAGU SPESIFIK TIDAK DITEMUKAN, JANGAN ASAL PUTAR ---
  if (n <= 0 && q.length() > 0) { 
    musicDropStream();
    sfxPlay(SFX_ERROR);
    String msg = "Maaf, lagu \"" + q + "\" tidak ada di katalog. Coba minta genre lain ya.";
    musicSay(msg);
    return;
  }

  // Jika kosong dan tidak ada query (minta putar acak), baru cari populer
  if (n <= 0 && q.length() == 0) {
    Serial.println("[MUSIC] Pencarian kosong, mencari lagu populer...");
    n = musicFetchPlaylist("", "");
  }

  if (musicAbortFlag) {
    musicDropStream();
    return;
  }
  if (n <= 0) {
    musicDropStream();
    sfxPlay(SFX_ERROR);
    musicSay("Maaf, aku belum bisa mengambil lagu sekarang.");
    return;
  }
  
  // Jika user minta genre (bukan judul spesifik), boleh putar acak
  if (q.length() == 0 && g.length() > 0) {
    musicSay("Oke, ini lagu dengan genre " + g + ".");
  }

  if (musicAbortFlag) {
    musicDropStream();
    return;
  }

  musicFailStreak = 0;
  if (!musicStartTrack(0)) musicOnTrackFail();
}

// ---------------------------------------------------------------------------
//  Tampilan OLED
// ---------------------------------------------------------------------------
static void musicDrawArt(bool playing) {
  const uint32_t now = millis();

#ifdef MUSIC_HAS_FRAMES
  static int frameIdx = 0;
  if (playing) frameIdx = (int)((now / (1000UL / MUSIC_FRAME_FPS)) % MUSIC_FRAME_COUNT);
  const int fx = MUSIC_AREA_X + (MUSIC_AREA_W - MUSIC_FRAME_W) / 2;
  const int fy = MUSIC_AREA_Y + (MUSIC_AREA_H - MUSIC_FRAME_H) / 2;
  gfxBitmap(fx, fy, musicFrames[frameIdx], MUSIC_FRAME_W, MUSIC_FRAME_H);
#else
  float raw = 0.0f;
#if MUSIC_USE_VU
  if (playing) {
    const uint32_t vu = audioI2S.getVUlevel();
    const int l = (vu >> 8) & 0xFF;
    const int r = vu & 0xFF;
    raw = (float)(l > r ? l : r) / 128.0f * MUSIC_VU_GAIN;
  }
#endif
  if (raw > 1.0f) raw = 1.0f;
  static float lvl = 0.0f;
  const float t = now * 0.001f;
  const float target = playing
      ? (raw > 0.30f ? raw : 0.30f + 0.15f * (0.5f + 0.5f * sinf(t * 6.0f)))
      : 0.0f;
  lvl = lvl * 0.7f + target * 0.3f;

  // kepala robot Abot yang goyang mengikuti musik
  const int cx  = MUSIC_AREA_X + MUSIC_AREA_W / 2;
  const int bob = playing ? (int)lroundf(sinf(t * 7.0f) * (1.0f + 1.5f * lvl)) : 0;
  const int hw = 36, hh = 20;
  const int hx = cx - hw / 2;
  const int hy = 19 + bob;
  gfxLine(cx, hy - 4, cx, hy);
  gfxFillCircle(cx, hy - 5, 2);
  gfxRoundFrame(hx, hy, hw, hh, 5);

  const int  ey     = hy + 5;
  const bool squint = playing && lvl > 0.55f && ((now / 180UL) % 2UL);
  if (!playing) {
    gfxLine(hx + 8,       ey + 4, hx + 14,      ey + 4);
    gfxLine(hx + hw - 15, ey + 4, hx + hw - 9,  ey + 4);
  } else if (squint) {
    gfxLine(hx + 8,  ey + 6, hx + 11, ey + 2);
    gfxLine(hx + 11, ey + 2, hx + 14, ey + 6);
    gfxLine(hx + hw - 15, ey + 6, hx + hw - 12, ey + 2);
    gfxLine(hx + hw - 12, ey + 2, hx + hw - 9,  ey + 6);
  } else {
    gfxFillRoundRect(hx + 8,       ey, 6, 9, 2);
    gfxFillRoundRect(hx + hw - 14, ey, 6, 9, 2);
  }

  // equalizer
  const int nBars = 8, barW = 5, gap = 2;
  const int totalW = nBars * barW + (nBars - 1) * gap;
  const int bx0 = MUSIC_AREA_X + (MUSIC_AREA_W - totalW) / 2;
  const int baseY = MUSIC_AREA_Y + MUSIC_AREA_H - 1;
  for (int i = 0; i < nBars; i++) {
    const float wave = 0.5f + 0.5f * sinf(t * (4.0f + i * 0.9f) + i * 1.7f);
    int h = playing ? 1 + (int)(lvl * 9.0f * (0.35f + 0.65f * wave)) : 1;
    if (h > 9) h = 9;
    gfxFillRect(bx0 + i * (barW + gap), baseY - h + 1, barW, h);
  }
#endif
}

static void renderMusic() {
  const uint8_t  st      = musicState;
  const bool     playing = (st == MUS_PLAYING);
  const bool     loading = (st == MUS_LOADING);
  const uint32_t now     = millis();

  beginFrame();

  // judul (berjalan kalau kepanjangan)
  const String title = String(musicTitleBuf);
  const int tw = gfxTextWidth(title);
  if (tw <= SCREEN_WIDTH) {
    gfxText((SCREEN_WIDTH - tw) / 2, 0, title);
  } else {
    const int gapPx = 30;
    const int off   = (int)((now / 35UL) % (unsigned long)(tw + gapPx));
    gfxText(-off, 0, title);
    gfxText(tw + gapPx - off, 0, title);
  }
  gfxLine(0, 10, 127, 10);

  musicDrawArt(playing);

  // kolom kanan: artis + waktu
  const int rx = 62;
  String artist = String(musicArtistBuf);
  if (artist.length() > 11) artist = artist.substring(0, 11);
  gfxText(rx, 13, artist);

  const uint32_t el  = musicElapsedSec();
  const int      dur = musicDurationSec;
  char tb[24];
  if (loading)      snprintf(tb, sizeof(tb), "MEMUAT...");
  else if (dur > 0) snprintf(tb, sizeof(tb), "%u:%02u/%d:%02d",
                             (unsigned)(el / 60), (unsigned)(el % 60), dur / 60, dur % 60);
  else              snprintf(tb, sizeof(tb), "%u:%02u", (unsigned)(el / 60), (unsigned)(el % 60));
  gfxText(rx, 25, tb);

  // ikon: prev | play/pause | next
  const int icx = 95, icy = 43;
  const int px = 70, nx = 109;
  gfxFillTriangle(px + 5,  icy - 4, px + 5,  icy + 4, px,      icy);
  gfxFillTriangle(px + 11, icy - 4, px + 11, icy + 4, px + 6,  icy);
  gfxFillTriangle(nx,      icy - 4, nx,      icy + 4, nx + 5,  icy);
  gfxFillTriangle(nx + 6,  icy - 4, nx + 6,  icy + 4, nx + 11, icy);
  if (playing) {
    gfxFillRect(icx - 4, icy - 6, 3, 12);
    gfxFillRect(icx + 2, icy - 6, 3, 12);
  } else {
    gfxFillTriangle(icx - 4, icy - 6, icx - 4, icy + 6, icx + 5, icy);
  }

  // progress bar
  const int barY = 59;
  float frac = (dur > 0) ? (float)el / (float)dur : 0.0f;
  if (frac > 1.0f) frac = 1.0f;
  for (int x = 0; x < SCREEN_WIDTH; x += 2) gfxPixel(x, barY);
  const int dotX = 3 + (int)(frac * 121.0f);
  gfxFillRect(0, barY - 1, dotX, 3);
  gfxFillCircle(dotX, barY, 3);

  presentFrame();
}