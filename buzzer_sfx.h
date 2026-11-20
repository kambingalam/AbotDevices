#pragma once
/*
  ============================================================================
   ABOT BUZZER SFX (buzzer AKTIF, non-blocking)  -  dipakai oleh ABOT V3.51
  ============================================================================
   - Buzzer aktif hanya punya satu nada: "suara" dibuat dari pola bunyi/diam.
   - Tanpa delay(): sfxUpdate() dipanggil dari loop().
   - sfxPlay() aman dipanggil dari task mana pun.
   - Diam otomatis kalau volumeLevel == 0 (mute).
   - sfxSelfTest() memutar semua SFX berurutan (perintah serial '$').

   Pasang   : (+) buzzer -> GPIO BUZZER_PIN, (-) -> GND.
              Buzzer 5V / pelan: transistor NPN (BC547/2N2222) + R 1k di basis.
   Opsional : #define BUZZER_PIN 12
              #define BUZZER_ACTIVE_LOW 1   (modul yang aktif saat LOW)
   Include  : setelah deklarasi volumeLevel, sebelum music_player.h
  ============================================================================
*/
#pragma once

#ifndef BUZZER_PIN
#define BUZZER_PIN 12
#endif
#ifndef BUZZER_ACTIVE_LOW
#define BUZZER_ACTIVE_LOW 0
#endif

enum SfxId : uint8_t {
  SFX_NONE = 0,
  SFX_BOOT,
  SFX_CLICK,
  SFX_REC_START,
  SFX_REC_STOP,
  SFX_REPLY,
  SFX_ERROR,
  SFX_WIFI_UP,
  SFX_WIFI_DOWN,
  SFX_QUIZ_OK,
  SFX_QUIZ_BAD,
  SFX_TURN,
  SFX_ARRIVE,
  SFX_MUSIC,
  SFX_COUNT
};

struct SfxStep {
  uint16_t onMs;
  uint16_t offMs;
};

// Setiap pola diakhiri {0, 0}
static const SfxStep SFXP_BOOT[]      = {{60, 50}, {60, 50}, {140, 0}, {0, 0}};
static const SfxStep SFXP_CLICK[]     = {{15, 0}, {0, 0}};
static const SfxStep SFXP_REC_START[] = {{45, 0}, {0, 0}};
static const SfxStep SFXP_REC_STOP[]  = {{40, 40}, {40, 0}, {0, 0}};
static const SfxStep SFXP_REPLY[]     = {{50, 60}, {50, 0}, {0, 0}};
static const SfxStep SFXP_ERROR[]     = {{160, 90}, {160, 0}, {0, 0}};
static const SfxStep SFXP_WIFI_UP[]   = {{40, 40}, {40, 0}, {0, 0}};
static const SfxStep SFXP_WIFI_DOWN[] = {{200, 0}, {0, 0}};
static const SfxStep SFXP_QUIZ_OK[]   = {{40, 40}, {40, 40}, {90, 0}, {0, 0}};
static const SfxStep SFXP_QUIZ_BAD[]  = {{260, 0}, {0, 0}};
static const SfxStep SFXP_TURN[]      = {{30, 0}, {0, 0}};
static const SfxStep SFXP_ARRIVE[]    = {{80, 60}, {80, 60}, {220, 0}, {0, 0}};
static const SfxStep SFXP_MUSIC[]     = {{25, 35}, {25, 0}, {0, 0}};

static const SfxStep* const SFX_TABLE[SFX_COUNT] = {
  nullptr,
  SFXP_BOOT, SFXP_CLICK, SFXP_REC_START, SFXP_REC_STOP, SFXP_REPLY, SFXP_ERROR,
  SFXP_WIFI_UP, SFXP_WIFI_DOWN, SFXP_QUIZ_OK, SFXP_QUIZ_BAD, SFXP_TURN,
  SFXP_ARRIVE, SFXP_MUSIC
};

// Bunyi baru boleh memotong bunyi lama kalau prioritasnya >= lama.
static const uint8_t SFX_PRIO[SFX_COUNT] = {
  0,                                   // NONE
  2, 0, 2, 2, 2, 2, 2, 2, 2, 2, 1, 2,  // BOOT..ARRIVE
  0                                    // MUSIC
};

static const SfxStep*  sfxCur     = nullptr;
static uint8_t         sfxCurId   = SFX_NONE;
static int             sfxIdx     = 0;
static bool            sfxPhaseOn = false;
static uint32_t        sfxStepAt  = 0;
static portMUX_TYPE    sfxMux     = portMUX_INITIALIZER_UNLOCKED;

static inline void sfxWrite(bool on) {
  digitalWrite(BUZZER_PIN, ((on ? 1 : 0) ^ BUZZER_ACTIVE_LOW) ? HIGH : LOW);
}

// Dipanggil di dalam mux.
static void sfxStartStep() {
  if (!sfxCur || (sfxCur[sfxIdx].onMs == 0 && sfxCur[sfxIdx].offMs == 0)) {
    sfxCur   = nullptr;
    sfxCurId = SFX_NONE;
    sfxWrite(false);
    return;
  }
  sfxPhaseOn = true;
  sfxStepAt  = millis();
  sfxWrite(true);
}

static void sfxBegin() {
  pinMode(BUZZER_PIN, OUTPUT);
  sfxWrite(false);
}

static void sfxStop() {
  portENTER_CRITICAL(&sfxMux);
  sfxCur   = nullptr;
  sfxCurId = SFX_NONE;
  sfxWrite(false);
  portEXIT_CRITICAL(&sfxMux);
}

static void sfxPlay(SfxId id) {
  if (id <= SFX_NONE || id >= SFX_COUNT) return;
  if (volumeLevel == 0) return;                       // mute -> diam
  portENTER_CRITICAL(&sfxMux);
  if (sfxCur == nullptr || SFX_PRIO[id] >= SFX_PRIO[sfxCurId]) {
    sfxCur   = SFX_TABLE[id];
    sfxCurId = id;
    sfxIdx   = 0;
    sfxStartStep();
  }
  portEXIT_CRITICAL(&sfxMux);
}

// Panggil terus-menerus dari loop()
static void sfxUpdate() {
  if (!sfxCur) return;
  portENTER_CRITICAL(&sfxMux);
  if (sfxCur) {
    const SfxStep& st = sfxCur[sfxIdx];
    const uint32_t el = millis() - sfxStepAt;
    if (sfxPhaseOn) {
      if (el >= st.onMs) {
        sfxWrite(false);
        sfxPhaseOn = false;
        sfxStepAt  = millis();
        if (st.offMs == 0) { sfxIdx++; sfxStartStep(); }
      }
    } else if (el >= st.offMs) {
      sfxIdx++;
      sfxStartStep();
    }
  }
  portEXIT_CRITICAL(&sfxMux);
}

// Tes hardware: bunyi 1 bip panjang lalu semua SFX berurutan.
// Memakai delay(), jadi hanya untuk perintah serial '$' (bukan saat dipakai).
static void sfxSelfTest() {
  Serial.printf("[SFX] Self-test di GPIO%d (active_low=%d, volume=%d)\n",
                BUZZER_PIN, BUZZER_ACTIVE_LOW, volumeLevel);
  if (volumeLevel == 0) Serial.println("[SFX] PERINGATAN: volume 0 (mute), SFX tidak akan bunyi");
  sfxWrite(true);  delay(300);  sfxWrite(false);  delay(300);   // tes pin mentah
  for (int i = SFX_BOOT; i < SFX_COUNT; i++) {
    Serial.printf("[SFX] #%d\n", i);
    sfxPlay((SfxId)i);
    const uint32_t t0 = millis();
    while (sfxCur && millis() - t0 < 1500) { sfxUpdate(); delay(1); }
    delay(250);
  }
  Serial.println("[SFX] Self-test selesai");
}