#include <Arduino.h>
// ===========================================================================
//  VOICE TASK
// ===========================================================================
static void voiceReact(CharacterEmotion e) {
  StateLock lock;
  triggerCharacterReaction(e);
}
static void voiceThinkOn() {
  StateLock lock;
  setCharacterEmotion(CHARACTER_THINKING);
  beginThinkingVisual();
}
static void voiceThinkOff() {
  StateLock lock;
  endThinkingVisual();
}
static void voiceEmotion(CharacterEmotion e) {
  StateLock lock;
  setCharacterEmotion(e);
}

static String quizKeywordFallback(const String& transcript) {
  String low = transcript;
  low.toLowerCase();
  if (low.indexOf("benar") >= 0 || low.indexOf("betul") >= 0) {
    handleQuizAnswer('J');
    return "Wah hebat, jawabanmu benar!";
  }
  if (low.indexOf("salah") >= 0) {
    handleQuizAnswer('K');
    return "Yah, sayang sekali salah.";
  }
  if (low.indexOf("tidak tahu") >= 0 || low.indexOf("gak tahu") >= 0 || low.indexOf("bingung") >= 0) {
    handleQuizAnswer('L');
    return "Tidak apa-apa, tetap semangat ya.";
  }
  handleQuizAnswer('L');
  return "Aku belum bisa menilai jawaban itu, anggap tidak tahu ya.";
}

constexpr int STORY_MAX_SEGMENTS = 24;

static void storyContinue(const String& request, const VoiceAct& first) {
  String told = first.say;
  bool more = first.more;
  for (int seg = 1; seg < STORY_MAX_SEGMENTS && more; seg++) {
    if (ttsStop || speakAborted || volumeLevel == 0) break;
    String tail = (told.length() > 500) ? told.substring(told.length() - 500) : told;
    String extra = " Kamu sedang menceritakan cerita bertahap untuk permintaan pengguna: \"" + request + "\". Ini bagian ke-" + String(seg + 1) + ". Bagian terakhir yang sudah kamu ceritakan: \"" + tail + "\". Lanjutkan 4-5 kalimat berikutnya tanpa mengulang. Isi more=true kalau cerita masih "
                                                                                                                                                                                                             "berlanjut, atau more=false kalau ini bagian penutup. show harus none.";
    VoiceAct nxt;
    if (!groqAsk("Lanjutkan ceritanya.", extra, nxt) || nxt.say.length() == 0) {
      voiceTextMode(true);
      speakText("Maaf, ceritanya terputus karena koneksi. Coba minta lagi nanti ya.");
      break;
    }
    if (ttsStop || speakAborted) break;
    told += " ";
    told += nxt.say;

    if (histCount > 0) {
      histBot[histCount - 1] = (told.length() > 1500) ? told.substring(told.length() - 1500) : told;
    }

    more = nxt.more;

    voiceEmotion(moodFromString(nxt.mood));
    voiceTextMode(true);
    speakText(nxt.say);
  }
  ttsStop = false;
}

static void voiceProcess() {
  musicDropStream();
  voiceTextMode(false);
  if (WiFi.status() != WL_CONNECTED) {
    voiceReact(CHARACTER_CONFUSED);
    voiceTextMode(true);
    speakText("Koneksi internet lagi putus, coba lagi nanti ya.");
    return;
  }

  size_t samples = 0;
  int16_t* snapshot = nullptr;
  {
    StateLock lock;
    samples = recLength;
    if (samples * sizeof(int16_t) < STT_MIN_BYTES) {
      Serial.println("[VOICE] rekaman terlalu pendek, skip");
      linkSend("{\"t\":\"stt_skip\",\"why\":\"too_short\"}");
      return;
    }
    if (recPeak < 400) {
      Serial.println("[VOICE] rekaman terlalu pelan, skip");
      linkSend("{\"t\":\"stt_skip\",\"why\":\"too_quiet\"}");
      triggerCharacterReaction(CHARACTER_CONFUSED);
      return;
    }
    snapshot = (int16_t*)heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!snapshot) snapshot = (int16_t*)malloc(samples * sizeof(int16_t));
    if (!snapshot) {
      Serial.println("[VOICE] OOM snapshot");
      return;
    }
    memcpy(snapshot, recBuf, samples * sizeof(int16_t));
  }

  voiceThinkOn();
  String transcript = groqTranscribe(snapshot, samples);
  free(snapshot);

  if (transcript.length() == 0 || isWhisperHallucination(transcript)) {
    voiceThinkOff();
    Serial.println("[VOICE] STT kosong/halusinasi, diabaikan");
    voiceReact(CHARACTER_CONFUSED);
    linkSend("{\"t\":\"stt_result\",\"ok\":0}");
    if (sttLastCode != 200 && sttLastCode != 0) speakText("Aku kurang jelas dengar, coba ulangi ya.");
    sfxPlay(SFX_ERROR);
    return;
  }

  linkSend("{\"t\":\"stt_result\",\"ok\":1,\"text\":\"" + jsonEscape(transcript) + "\"}");
  Serial.printf("[VOICE] STT OK: %s\n", transcript.c_str());

  String say;
  if (!quizWaitingAnswer && wordCount(transcript) <= 4 && voiceLocalCommand(transcript, say, false)) {
    voiceThinkOff();
    if (say.length() > 0) {
      voiceEmotion(CHARACTER_HAPPY);
      speakText(say);
      voiceEmotion(CHARACTER_DEFAULT);
    }
    return;
  }

  groqEnsureModel();
  VoiceAct act;
  String quizExtra = "";
  if (quizWaitingAnswer) {
    quizExtra = " Kuis aktif di layar dengan pertanyaan: \"" + currentQuizQuestion + "\". Jika pesan pengguna adalah jawaban kuis, nilai jawabannya, isi verdict "
                                                                                     "(correct, wrong, atau unknown bila pengguna menyerah), dan sebutkan jawaban yang benar. "
                                                                                     "Jika pesan bukan jawaban kuis, abaikan kuis dan kosongkan verdict.";
  }
  bool ok = groqAsk(transcript, quizExtra, act);

  if (ok && act.show == "weather" && act.city.length() > 0) {
    String resolved, summary;
    if (fetchCityWeather(act.city, resolved, summary)) {
      String extra = " Data cuaca terbaru untuk " + resolved + ": " + summary + ". Jawab pakai data ini dan set show=weather.";
      VoiceAct act2;
      if (groqAsk(transcript, extra, act2)) {
        act = act2;
        act.show = "weather";
      } else {
        act.say = "Cuaca di " + resolved + " sekarang " + summary + ".";
        act.show = "weather";
      }
      act.customWeather = true;
    } else {
      act.say = "Maaf, aku tidak menemukan data cuaca untuk " + act.city + ".";
      act.show = "none";
    }
  }

  voiceThinkOff();

  if (!ok) {
    Serial.printf("[VOICE] LLM gagal (HTTP %d)\n", lastLlmHttp);
    sfxPlay(SFX_ERROR);
    if (quizWaitingAnswer) {
      say = quizKeywordFallback(transcript);
      voiceTextMode(true);
      speakText(say);
      return;
    }
    if (voiceLocalCommand(transcript, say, true)) {
      if (say.length() > 0) {
        voiceEmotion(CHARACTER_HAPPY);
        speakText(say);
        voiceEmotion(CHARACTER_DEFAULT);
      }
      return;
    }
    voiceReact(CHARACTER_CONFUSED);
    voiceTextMode(true);
    speakText(llmErrorText());
    return;
  }

  histAdd(transcript, act.say);
  linkSend("{\"t\":\"llm_reply\",\"ok\":1,\"show\":\"" + jsonEscape(act.show) + "\",\"text\":\"" + jsonEscape(act.say) + "\"}");
  Serial.printf("[VOICE] LLM OK (show=%s mood=%s): %s\n",
                act.show.c_str(), act.mood.c_str(), act.say.c_str());

  sfxPlay(SFX_REPLY);
  voiceApply(act);
  speakText(act.say);
  if (act.more) storyContinue(transcript, act);
  voiceEmotion(CHARACTER_DEFAULT);
}

static void voiceTask(void* pv) {
  unsigned long lastModelCheck = 0;
  for (;;) {
    uint32_t bits = 0;
    const bool fast = (musicState == MUS_PLAYING || musicState == MUS_LOADING || musicStopReq);
    const TickType_t wait = fast ? pdMS_TO_TICKS(2) : pdMS_TO_TICKS(500);

    if (xTaskNotifyWait(0, 0xFFFFFFFF, &bits, wait) == pdFALSE) {
      musicTick();
      if (!fast && !modelChecked && millis() > 6000 && millis() - lastModelCheck > 3000) {
        lastModelCheck = millis();
        groqEnsureModel();
      }
      continue;
    }
    if (bits & 0x01) {
      voicePipelineActive = true;
      ttsStop = false;
      voiceProcess();
      voicePipelineActive = false;
    }
    if (bits & 0x02) musicServiceRequest();
  }
}

