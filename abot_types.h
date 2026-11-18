#pragma once

// ===========================================================================
//  TIPE DATA
// ===========================================================================
enum StartupStage : unsigned char { STARTUP_NORMAL,
                                    STARTUP_USAGE,
                                    STARTUP_CLOCK,
                                    STARTUP_WEATHER };
enum ListeningState : unsigned char { LISTENING_IDLE,
                                      LISTENING_ACTIVE };
enum SystemState : unsigned char { STATE_BOOT,
                                   STATE_WIFI_CONNECTING,
                                   STATE_READY,
                                   STATE_GETTING_ROUTE,
                                   STATE_THINKING,
                                   STATE_AI_RESPONSE,
                                   STATE_ERROR };
enum EyeMood : unsigned char { EYE_DEFAULT,
                               EYE_HAPPY,
                               EYE_ANGRY,
                               EYE_TIRED };
enum CharacterEmotion : unsigned char { CHARACTER_DEFAULT,
                                        CHARACTER_LISTENING,
                                        CHARACTER_THINKING,
                                        CHARACTER_HAPPY,
                                        CHARACTER_LAUGH,
                                        CHARACTER_CONFUSED,
                                        CHARACTER_ANGRY,
                                        CHARACTER_TIRED };
enum AbotPersonality : unsigned char { ABOT_NEUTRAL,
                                       ABOT_MORNING,
                                       ABOT_DAY,
                                       ABOT_AFTERNOON,
                                       ABOT_NIGHT };
enum PersonalityMode : unsigned char { PERSONALITY_NORMAL,
                                       PERSONALITY_PLAYFUL,
                                       PERSONALITY_EXCITED };
enum CompanionType : unsigned char { COMPANION_NONE,
                                     COMPANION_QUIZ,
                                     COMPANION_TRIVIA,
                                     COMPANION_HUMOR,
                                     COMPANION_STORY,
                                     COMPANION_CARD };
enum CompanionPlaybackState : unsigned char { COMPANION_IDLE,
                                              COMPANION_TYPING,
                                              COMPANION_HOLD };
enum WakeIntroStage : unsigned char { WAKE_DARK,
                                      WAKE_ABOT_HELLO,
                                      WAKE_ABOT_PERSONALITY,
                                      WAKE_FINISHED };
enum NetworkJob : unsigned char { NET_NONE,
                                  NET_OSRM,
                                  NET_NAVIGATION_AI,
                                  NET_COMPANION,
                                  NET_WEATHER,
                                  NET_GEOCODE };
enum DestinationStage : unsigned char { DEST_ASK,
                                        DEST_GEOCODING,
                                        DEST_CONFIRMED,
                                        DEST_FAILED };

struct RoutePoint {
  double lat;
  double lon;
};

struct NavigationStep {
  String instruction;
  String road;
  String type;
  String modifier;
  float distance;
  int routeIndex;
  float routeDistance;
  bool important;
};

struct WeatherData {
  bool valid = false;
  float temperature = 0.0f;
  float tempMax = 0.0f;
  float tempMin = 0.0f;
  int weatherCode = -1;
  String condition = "-";
  unsigned long updatedAt = 0;
};

// ===========================================================================
//  KONFIGURASI JARINGAN
// ===========================================================================
#define HOME_CITY "Medan"
#define HOME_LAT "3.5952"
#define HOME_LON "98.6722"
const char* WEATHER_URL =
  "https://api.open-meteo.com/v1/forecast"
  "?latitude=" HOME_LAT "&longitude=" HOME_LON
  "&current=temperature_2m,weather_code"
  "&daily=temperature_2m_max,temperature_2m_min"
  "&timezone=Asia%2FJakarta";

const char* MQTT_HOST = "broker.emqx.io";
const uint16_t MQTT_PORT = 8883;
// Default ini ada di GitHub (publik). Ganti dengan: SET topic=<basis rahasia>
const char* MQTT_TOPIC_BASE_DEFAULT = "abot/wokwi/477375325542330369/v337";
char MQTT_TOPIC_COMMAND[112] = "abot/wokwi/477375325542330369/v337/command";
char MQTT_TOPIC_STATUS[112] = "abot/wokwi/477375325542330369/v337/status";
const unsigned long MQTT_RECONNECT_INTERVAL_MS = 5000;
const size_t MQTT_PACKET_BUFFER_SIZE = 10000;

// ===========================================================================
//  KONFIGURASI TIMING
// ===========================================================================
const unsigned long INITIAL_MAP_DURATION = 8000;
const unsigned long USER_MAP_DURATION = 8000;
const unsigned long ROUTE_MAP_DURATION = 5000;
const unsigned long HUD_SUPPRESS_DURATION = 2500;

const unsigned long COMPANION_HOLD_DURATION = 4000;
const unsigned long COMPANION_FINAL_HOLD = 6000;
const unsigned long TYPING_INTERVAL = 16;
const unsigned long COMPANION_NEXT_MENU_PAGE_MS = 2600;
const unsigned long COMPANION_NEXT_MENU_TIMEOUT = 20000;

const unsigned long STARTUP_USAGE_DURATION = 7500;
const unsigned long STARTUP_USAGE_LINK_MS = 3000;
const unsigned long STARTUP_CLOCK_DURATION = 4000;
const unsigned long STARTUP_WEATHER_DURATION = 4500;
const unsigned long INFO_CLOCK_DURATION = 3500;
const unsigned long INFO_WEATHER_DURATION = 4500;

const unsigned long SLEEP_IDLE_TIMEOUT = 20000;
const unsigned long ARRIVAL_DISPLAY_DURATION = 10000;
const unsigned long TRIP_INTRO_DURATION = 3300;
const unsigned long TRIP_INTRO_LINK_DURATION = 1200;
const unsigned long BOOT_TRANSITION_MS = 1400;

const unsigned long WAKE_DARK_MS = 250;
const unsigned long WAKE_ABOT_HELLO_MS = 1700;
const unsigned long WAKE_ABOT_PERSONALITY_MS = 1900;

const unsigned long DEST_ASK_TIMEOUT = 12000;
const unsigned long DEST_CONFIRM_DURATION = 2200;
const unsigned long DEST_FAILED_DURATION = 3000;

const unsigned long LLM_COOLDOWN = 2000;
const unsigned long WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;
const unsigned long WEATHER_RETRY_MS = 60UL * 1000UL;
const unsigned long OSRM_RETRY_MS = 10000;
const int OSRM_MAX_RETRIES = 3;
const unsigned long WIFI_CHECK_INTERVAL_MS = 2000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 5000;
const unsigned long HEAP_CHECK_INTERVAL_MS = 10000;
const uint32_t HEAP_CRITICAL_BYTES = 15000;

const unsigned long CHARACTER_REACTION_HAPPY_MS = 1200;
const unsigned long CHARACTER_REACTION_CONFUSED_MS = 1400;
const unsigned long CHARACTER_REACTION_ANGRY_MS = 1200;
const unsigned long CHARACTER_REACTION_LAUGH_MS = 1700;

const unsigned long NATURAL_EYE_FIRST_ACTION_MS = 4200;
const unsigned long NATURAL_EYE_ACTION_INTERVAL_MS = 5200;
const unsigned long NATURAL_EYE_GAZE_DURATION_MS = 850;
const unsigned long NATURAL_EYE_WINK_INTERVAL_MS = 18000;

// ===========================================================================
//  KONFIGURASI TUNING
// ===========================================================================
const float HUD_LEAD_SECONDS = 7.0f;
const float HUD_MIN_TRIGGER_M = 40.0f;
const float HUD_MAX_TRIGGER_M = 140.0f;
const unsigned long HUD_MAX_SHOW_MS = 7000;
const float HUD_PASSED_MARGIN_M = 3.0f;
const float HUD_PREEMPT_MAP_M = 60.0f;

const float GPS_SPEED_MPS = 12.0;
const float OFFROUTE_METERS = 60.0f;
const unsigned long OFFROUTE_REPORT_MS = 5000;
const unsigned long LINK_GPS_FRESH_MS = 3000;
const unsigned long LINK_STATUS_MS = 5000;
const unsigned long LINK_TIMEOUT_MS = 30000;
const unsigned long DEAD_RECKON_MAX_MS = 1500;
const unsigned long MIN_GPS_INTERVAL_MS = 100;
const unsigned long MIN_ROUTE_INTERVAL_MS = 1000;
const size_t LINK_LINE_MAX = 8192;
const bool SIM_WHEN_NO_GPS = false;
const bool NAV_AI_ON_ROUTE = false;

#define MAX_ROUTE_POINTS 120
#define MAX_NAV_STEPS 40
#define LOG_RING_SIZE 20
#define LOG_LINE_MAX 96

const int MAX_COMPANION_PAGES = 8;
const int MAX_COMPANION_LINES = 6;
const int MAX_LINE_CHARS = 20;

// ===========================================================================
//  STATE GLOBAL
// ===========================================================================
StartupStage startupStage = STARTUP_NORMAL;
unsigned long startupStageStarted = 0;
bool startupOnboardingActive = false;
bool startupOnboardingCompleted = false;
bool startupOnboardingManual = false;
bool infoModeActive = false;
unsigned long infoModeStarted = 0;
int infoPage = 0;

ListeningState listeningState = LISTENING_IDLE;
bool listeningActive = false;

bool talkLastReading = HIGH, talkStableState = HIGH, talkLongTriggered = false;
bool actionLastReading = HIGH, actionStableState = HIGH;
unsigned long talkLastChange = 0, talkPressedAt = 0;
unsigned long actionLastChange = 0, actionPressedAt = 0;

SystemState systemState = STATE_BOOT;
EyeMood currentEyeMood = EYE_DEFAULT;
CharacterEmotion currentCharacterEmotion = CHARACTER_DEFAULT;
unsigned long characterReactionUntil = 0;
bool characterReactionActive = false;

AbotPersonality currentAbotPersonality = ABOT_NEUTRAL;
AbotPersonality appliedAbotPersonality = ABOT_NEUTRAL;
unsigned long abotHoldUntil = 0;

bool naturalEyeBehaviorEnabled = true;
unsigned long naturalEyeNextActionAt = 0;
unsigned long naturalEyeReturnAt = 0;
unsigned long naturalEyeNextWinkAt = 0;
bool naturalEyeGazeActive = false;
int naturalEyeSequence = 0;

unsigned long thinkingGazeNextAt = 0;
int thinkingGazeStep = 0;
bool thinkingVisualPrepared = false;
unsigned long thinkingDemoUntil = 0;

PersonalityMode personalityMode = PERSONALITY_NORMAL;
CompanionType currentCompanion = COMPANION_NONE;
CompanionPlaybackState companionPlayback = COMPANION_IDLE;

bool quizWaitingAnswer = false;
String currentQuizQuestion = "";

bool companionNextMenuActive = false;
bool companionNextMenuPending = false;
unsigned long companionNextMenuAt = 0;
unsigned long companionNextMenuPageAt = 0;
unsigned long companionNextMenuOpenedAt = 0;
unsigned long companionNextMenuDelayMs = 350;
int companionNextMenuPage = 0;

String companionPages[MAX_COMPANION_PAGES][MAX_COMPANION_LINES];
int companionPageCount = 0;
int companionPage = 0;
int companionTypedChars = 0;
String previousCompanionText = "";
String cardTitle = "INFO";

bool conversationActive = false;
bool voiceTextActive = false;

bool mapRequested = false;
bool companionActive = false;
bool companionGenerating = false;
bool initialMapShown = false;
bool introductionFinished = false;
bool wifiConnected = false;

bool tripActive = false;
bool tripArrived = false;
bool arrivalActive = false;
bool tripMapPending = false;
unsigned long arrivalStartedAt = 0;
bool tripIntroActive = false;
unsigned long tripIntroStartedAt = 0;
bool tripIntroTriggerAsk = false;

bool sleepModeActive = false;
unsigned long lastUserActivityMs = 0;
unsigned long mapStartTime = 0;
unsigned long companionStateStart = 0;
unsigned long companionLastType = 0;
unsigned long hudSuppressUntil = 0;

WakeIntroStage wakeIntroStage = WAKE_DARK;
unsigned long wakeIntroStageStartedAt = 0;

bool bootTransitionActive = false;
unsigned long bootTransitionStartedAt = 0;

bool destinationAskActive = false;
DestinationStage destinationStage = DEST_ASK;
unsigned long destinationStageStarted = 0;
float destLat = 0.0f;
float destLon = 0.0f;
String destName = "";
String destAddress = "";
bool destinationResolved = false;
float originLat = -6.175400f;
float originLon = 106.827202f;

String activeOsrmUrl = "";
int osrmFailCount = 0;
unsigned long osrmGaveUpAt = 0;
unsigned long lastOsrmAttempt = 0;

unsigned long lastWiFiCheck = 0;
unsigned long lastWiFiAttempt = 0;
unsigned long lastHeapCheck = 0;

RoutePoint routePoints[MAX_ROUTE_POINTS];
float routeCumulativeDistance[MAX_ROUTE_POINTS];
int routePointCount = 0;
NavigationStep navSteps[MAX_NAV_STEPS];
int navStepCount = 0;
int activeStep = 0;
float osrmTotalDistance = 0;
float osrmTotalDuration = 0;
float routeGeometryDistance = 0;
float simulatedRouteDistance = 0;
int simulatedRouteIndex = 0;
unsigned long gpsLastMs = 0;

bool routeReady = false;
bool routeRequested = false;
bool navigationAIStarted = false;
bool pendingNavigationAI = false;

bool hudActive = false;
int hudLockedStep = -1;
int hudCompletedStep = -1;
unsigned long hudShownAt = 0;

unsigned long lastLlmRequest = 0;
volatile bool networkBusy = false;
volatile bool networkDone = false;
volatile bool networkSuccess = false;
volatile int networkHttpCode = -1;
volatile NetworkJob networkJob = NET_NONE;
volatile uint32_t networkRequestId = 0;
String networkPrompt = "";
String networkResponse = "";
uint32_t activeCompanionRequestId = 0;
uint32_t companionRequestCounter = 0;
unsigned long networkJobStartedAt = 0;
bool networkJobWasBusy = false;

WeatherData weatherData;
WeatherData cityWeather;
String weatherViewLabel = "";
bool weatherViewCustom = false;
bool weatherFetching = false;
unsigned long lastWeatherAttempt = 0;

WiFiClientSecure mqttNetworkClient;
PubSubClient mqttClient(mqttNetworkClient);
String mqttPendingDispatch = "";
unsigned long mqttLastReconnectAttempt = 0;
unsigned long mqttBackoffMs = MQTT_RECONNECT_INTERVAL_MS;

bool isAuthorized = false;
bool linkEnabled = true;
bool linkRouteReceived = false;
bool hpConnected = false;
// true saat linkDispatchLine dipanggil dari jalur MQTT (bukan Serial).
// Dipakai untuk menolak ota_start dari MQTT (hanya boleh via Serial).
bool dispatchFromMqtt = false;
unsigned long lastLinkMessageAt = 0;
unsigned long lastGpsMsgAt = 0;
unsigned long lastRouteMsgAt = 0;
unsigned long lastLinkGpsAt = 0;
unsigned long lastOffRouteReportAt = 0;
bool routeFromLink = false;
int routeStride = 1;
float linkDistScale = 1.0f;

bool mapForNewRoute = false;
float speedMps = 8.0f;
unsigned long speedLastAt = 0;
float speedLastDist = 0;
unsigned long drLastMs = 0;

bool otaActive = false;
// Progress OTA untuk layar OLED (ditulis OtaTask, dibaca DisplayTask)
volatile int otaProgress = 0;   // 0..100
volatile int otaStageIdx = 0;   // 0=unduh 1=verifikasi 2=pasang 3=selesai 4=gagal

// --- Konfirmasi OTA: perintah dari MQTT harus disetujui lewat tombol ---
const unsigned long OTA_CONFIRM_TIMEOUT_MS = 45000UL;
// Hanya URL firmware di repo milik sendiri yang diterima (huruf besar/kecil bebas).
const char* const OTA_URL_PREFIXES[] = {
  "https://github.com/kambingalam/AbotDevices/releases/download/",
  "https://github.com/kambingalam/AbotDevices/releases/latest/download/",
  "https://raw.githubusercontent.com/kambingalam/AbotDevices/"
};
bool otaConfirmActive = false;
unsigned long otaConfirmUntil = 0;
unsigned long otaConfirmStartedAt = 0;
String otaPendUrl = "";
String otaPendId = "";
String otaPendVer = "";
String otaPendSha = "";
volatile bool otaLaunched = false;         // task OTA sudah dibuat (tutup race ota_start ganda)
volatile bool otaCancelRequested = false;  // diset ota_cancel, dicek di loop unduh
static inline bool otaUiLocked() {
  return otaConfirmActive || otaActive || otaLaunched;
}

Preferences otaPrefs;
bool pendingOtaVerification = false;
String otaTargetVersion = "";
unsigned long bootTimeMs = 0;

String otaReportedVersion = FW_VERSION;

TaskHandle_t displayTaskHandle = NULL;
TaskHandle_t voiceTaskHandle = NULL;

char logRing[LOG_RING_SIZE][LOG_LINE_MAX];
uint8_t logRingHead = 0;
uint8_t logRingCount = 0;

