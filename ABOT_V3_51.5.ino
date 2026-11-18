/*
  ============================================================================
   OJOL ASSISTEN (ABOT) V3.51.4 — VOICE AI CHATBOT + LAYAR DINAMIS + MUSIK
  ============================================================================
   Fungsi    : Display navigasi + AI companion + voice chat + Music Player
   Target    : ESP32-S3 + OLED SSD1306 128x64 (I2C)
   Display   : U8g2 full buffer (HW I2C) + FluxGarage RoboEyes
   Link      : JSON lines via Serial / MQTT (dari HP driver)
   Audio     : Mic INMP441 (i2s_std, port 0) + Speaker MAX98357A (audioI2S, port 1)
   Voice AI  : Groq Whisper (STT) + Groq GPT-OSS (LLM) + Google Translate TTS
   Kredensial: disimpan di NVS, diisi lewat Serial 115200 (TIDAK ada di source/binary):
               SET ssid=...  |  SET pass=...  |  SET groq=gsk_...  |  SET auth=...
               CFG?  |  CFG CLEAR
   Library   : U8g2, FluxGarage RoboEyes, ArduinoJson (v7), PubSubClient,
               ESP32-audioI2S

   V3.51.4 PERUBAHAN:
     • Layar OLED menampilkan progress saat OTA (sebelumnya beku/kosong).
     • Mata siang hari (jam 11-16) sekarang MARAH (EYE_ANGRY).
     • Status OTA "pending" di NVS hanya dibersihkan kalau OTA gagal SEBELUM
       Update.end(true) (flag `flashed`), supaya tidak false "rolled_back".
     • Kredensial (Groq key, WiFi, auth token) di NVS; secrets.h tidak dipakai.
     • AUTH_TOKEN tidak lagi hardcoded di source.
     • ota_start dari MQTT (aplikasi HP) harus DIKONFIRMASI tombol di perangkat:
       ACTION = ya, TALK = batal, kedaluwarsa 45 detik. Dari Serial (kabel)
       langsung jalan. URL firmware dibatasi ke repo GitHub sendiri.
     • ota_cancel dari aplikasi benar-benar membatalkan unduhan yang berjalan.
     • Tombol dikunci selama OTA; cek heap tidak me-restart saat OTA;
       stack OtaTask 16 KB (TLS).
     • Topik MQTT bisa diganti lewat Serial: SET topic=<basis>. Default (publik
       di GitHub) tetap dipakai kalau belum diatur. Ganti juga di aplikasi HP.
     • startOTAUpdate menolak SHA256 yang bukan 64 karakter (lapis kedua).
     • Progress OTA 1% pertama dikirim ke HP supaya tidak terlihat macet.

   V3.51.0 PERUBAHAN:
     • OTA BENERAN (bukan simulasi): download firmware dari URL, verify SHA256.
     • sfxPlay() dipasang di event-event penting (boot, klik, record, reply, error).
     • Perbaikan bug penumpukan pesan di mqttCallback.
     • Cek heap dilewati saat voice pipeline sibuk (cegah restart palsu).
     • Perketat deteksi perintah suara "jam".
     • Tambah perintah serial '$' untuk tes buzzer.
  ============================================================================
*/

// ---------------------------------------------------------------------------
// Auto-prototype workaround untuk Arduino IDE
// ---------------------------------------------------------------------------
enum CompanionType : unsigned char;
enum AbotPersonality : unsigned char;
enum EyeMood : unsigned char;
enum NetworkJob : unsigned char;
enum SystemState : unsigned char;
enum CharacterEmotion : unsigned char;
enum PersonalityMode : unsigned char;
enum StartupStage : unsigned char;
enum ListeningState : unsigned char;
enum CompanionPlaybackState : unsigned char;
enum WakeIntroStage : unsigned char;
enum DestinationStage : unsigned char;

// ---------------------------------------------------------------------------
// Includes
// ---------------------------------------------------------------------------
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <time.h>
#include <U8g2lib.h>
#include <esp_task_wdt.h>
#include <Update.h>
#include <freertos/semphr.h>
#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <mbedtls/sha256.h>
#include <Audio.h>

#include <Preferences.h>
#include <esp_ota_ops.h>

// CATATAN KEAMANAN: secrets.h TIDAK di-include lagi. Key Groq, WiFi, dan auth
// token dibaca dari NVS (diisi lewat serial, lihat cfgHandleLine). Jangan
// menaruh kredensial di source atau binary.
#include <FluxGarage_RoboEyes.h>
#include "bitmaps.h"
#include "certs.h"


// CATATAN: folder sketch harus bernama "ABOT_V3_51.5" supaya semua tab .ino
// di folder ini ikut dikompilasi oleh Arduino IDE.

// ---------------------------------------------------------------------------
// Modul inti. Urutan include dipertahankan sama seperti file asli.
// ---------------------------------------------------------------------------
#include "abot_config.h"
#include "abot_audio.h"
#include "abot_types.h"
#include "abot_gfx.h"
#include "abot_helpers.h"

// ===========================================================================
//  FORWARD DECLARATIONS
// ===========================================================================
void setEyeMood(EyeMood mood);
void setCharacterEmotion(CharacterEmotion emotion);
void triggerCharacterReaction(CharacterEmotion emotion);
void resetNaturalEyeBehavior();
void triggerNaturalWink();
void markUserActivity();
void wakeFromSleep();
void startMapDisplay();
void startInfoMode();
void startStartupOnboarding();
void finishStartupOnboarding();
void startIntroduction();
void startTripIntroduction();
void startNewTrip();
void startCompanion(CompanionType type);
void finishCompanionGeneration(const String& text, CompanionType type);
void finishCompanion();
void renderCompanion();
void renderCompanionNextMenu();
void openCompanionNextMenu(unsigned long delayMs);
void closeCompanionNextMenu();
void handleArrival();
void startNavigationAI();
void startOSRM();
void startWeatherFetch();
void beginThinkingVisual();
void endThinkingVisual();
void cancelThinking();
void networkTask(void* parameter);
bool isImportantStep(const NavigationStep& step);
int nextImportantStep();
void startDestinationAsk();
void finishDestinationAsk();
void renderDestinationAsk();
void startGeocode(const String& placeName);
String buildOsrmUrl(float fromLat, float fromLon, float toLat, float toLon);
int decodePolyline(const String& encoded, RoutePoint* out, int maxPoints);
void handleCommand(char cmd);
String abotPromptHint();

void linkBegin();
void linkUpdate();
void bleBegin();
void bleSend(const String& json);
void linkDispatchLine(const String& line);
void linkReportStatus();
void mqttBegin();
void mqttUpdate();
void mqttCallback(char* topic, byte* payload, unsigned int length);
float snapToRoute(double lat, double lon, int from, int to, float& along);
void skipStartupSequence();
void syncRouteProgress();
void updateSpeedEstimate(float newDist);
void deadReckonStep();
float hudTriggerDistance();
void onLinkRoute(JsonDocument& doc);
void onLinkGps(JsonDocument& doc);
void onLinkTripStart();
void onLinkTripEnd();
void onLinkReset();
void onLinkPing();
void startOTAUpdate(const String& url, const String& requestId = "",
                    const String& targetVersion = "",
                    const String& expectedSha256 = "");
void updateDisplay();
