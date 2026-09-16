#include "GeminiLiveProbe.h"
#include <WiFiClientSecure.h>
#include <mbedtls/base64.h>
#include "libb64/cdecode.h"
#include "MemoryStore.h"

GeminiLiveProbe* GeminiLiveProbe::self_ = nullptr;

void GeminiLiveProbe::setVadConfig(uint16_t prefixPaddingMs, uint16_t silenceDurationMs,
                                    bool startSensitivityHigh, bool endSensitivityLow,
                                    bool turnIncludesAllInput) {
  if (prefixPaddingMs > 2000) prefixPaddingMs = 2000;
  if (silenceDurationMs < 100) silenceDurationMs = 100;
  if (silenceDurationMs > 3000) silenceDurationMs = 3000;
  vad_prefix_padding_ms_ = prefixPaddingMs;
  vad_silence_duration_ms_ = silenceDurationMs;
  vad_start_sensitivity_high_ = startSensitivityHigh;
  vad_end_sensitivity_low_ = endSensitivityLow;
  vad_turn_includes_all_input_ = turnIncludesAllInput;
}

bool GeminiLiveProbe::begin(const char* api_key) {
  api_key_storage_ = api_key ? api_key : "";
  api_key_ = api_key_storage_.c_str();
  self_ = this;
  for (int i = 0; i < AUDIO_RING_BUFFERS; ++i) {
    if (!audio_buf_[i]) {
      audio_buf_[i] = static_cast<uint8_t*>(ps_malloc(AUDIO_BUFFER_BYTES));
      if (!audio_buf_[i]) audio_buf_[i] = static_cast<uint8_t*>(malloc(AUDIO_BUFFER_BYTES));
      if (!audio_buf_[i]) return false;
      memset(audio_buf_[i], 0, AUDIO_BUFFER_BYTES);
    }
  }
  if (!rec_buf_) {
    rec_buf_ = static_cast<int16_t*>(ps_malloc(RT_REC_SAMPLES * sizeof(int16_t)));
    if (!rec_buf_) rec_buf_ = static_cast<int16_t*>(malloc(RT_REC_SAMPLES * sizeof(int16_t)));
    if (!rec_buf_) return false;
    memset(rec_buf_, 0, RT_REC_SAMPLES * sizeof(int16_t));
  }

  Serial.printf("GeminiLive: audio ring %d x %u bytes (%u KB); psram free=%u heap free=%u\n",
                AUDIO_RING_BUFFERS, static_cast<unsigned>(AUDIO_BUFFER_BYTES),
                static_cast<unsigned>(AUDIO_RING_BUFFERS * AUDIO_BUFFER_BYTES / 1024),
                static_cast<unsigned>(ESP.getFreePsram()),
                static_cast<unsigned>(ESP.getFreeHeap()));

  configured_ = (api_key_ && api_key_[0]);
  Serial.println(configured_ ? "GeminiLive: configured lazy" : "GeminiLive: missing api key");
  return configured_;
}

bool GeminiLiveProbe::connect() {
  if (!configured_) return false;
  if (connected_ && setup_complete_) return true;
  if (connect_requested_ && !intentional_disconnect_) return true;
  Serial.println("GeminiLive: connecting on demand");
  intentional_disconnect_ = false;
  connect_requested_ = true;
  last_activity_ms_ = millis();
  connect_started_ms_ = last_activity_ms_;
  // API key is appended to the WebSocket path but must never be printed.
  ws_.beginSSL("generativelanguage.googleapis.com", 443,
               String("/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=") + api_key_);
  ws_.onEvent(GeminiLiveProbe::wsEvent);
  ws_.setReconnectInterval(5000);
  return true;
}

void GeminiLiveProbe::disconnect(bool intentional, const char* finalEmotion) {
  intentional_disconnect_ = intentional;
  if (intentional) intentional_disconnect_emotion_ = finalEmotion ? finalEmotion : "neutral";
  pending_start_recording_ = false;
  pending_text_turn_ = false;
  resume_conversation_after_text_ = false;
  pending_text_ = "";
  if (realtime_recording_) stopRealtimeRecord();
  mic_ready_for_speech_ = false;
  realtime_recording_ = false;
  speaking_ = false;
  continuous_conversation_ = false;
  setup_complete_ = false;
  connected_ = false;
  connect_requested_ = false;
  connect_started_ms_ = 0;
  turn_in_progress_ = false;
  interaction_pending_ms_ = 0;
  finishing_turn_ = false;
  finish_started_ms_ = 0;
  ws_.disconnect();
  if (emotion_) emotion_->setEmotion(intentional ? intentional_disconnect_emotion_.c_str() : "error");
}

void GeminiLiveProbe::loop() {
  if (connect_requested_ || connected_) ws_.loop();
  if (realtime_recording_) recordAndSendAudioChunk();
  if (speaking_) drainAudioQueue();
  if (finishing_turn_) {
    // Finish only when nothing is queued locally and the speaker has fallen
    // silent. Late audio for the same turn simply extends this naturally.
    const bool drained = pending_count_ == 0 && !M5.Speaker.isPlaying();
    const bool stuck = finish_started_ms_ && millis() - finish_started_ms_ > TURN_DRAIN_TIMEOUT_MS;
    if (stuck) {
      Serial.printf("GeminiLive: turn drain timeout; %d chunks unplayed\n", pending_count_);
    }
    if (drained || stuck) finalizeResponseTurn();
  }
  if (connect_requested_ && !isReady() && connect_started_ms_ &&
      millis() - connect_started_ms_ > CONNECT_TIMEOUT_MS) {
    Serial.println("GeminiLive: connect timeout; reset to sleep");
    disconnect(true, "sleep");
    return;
  }
  if (continuous_conversation_ && isReady() && !realtime_recording_ && !speaking_ &&
      !pending_start_recording_ && stopped_recording_ms_ &&
      millis() - stopped_recording_ms_ > CONTINUOUS_RECORD_WATCHDOG_MS) {
    Serial.println("GeminiLive: continuous watchdog restarting recording");
    stopped_recording_ms_ = 0;
    startRealtimeRecord();
  }
  if (continuous_conversation_ && realtime_recording_ && record_started_ms_ &&
      millis() - record_started_ms_ > CONTINUOUS_CONVERSATION_TIMEOUT_MS) {
    Serial.println("GeminiLive: continuous conversation timeout; sleep");
    disconnect(true, "sleep");
    return;
  }
  if (isReady() && !continuous_conversation_ && !realtime_recording_ && !speaking_ &&
      last_activity_ms_ && millis() - last_activity_ms_ > IDLE_DISCONNECT_MS) {
    Serial.println("GeminiLive: idle disconnect");
    disconnect(true, "sleep");
  }
  if (turn_in_progress_ && interaction_pending_ms_ &&
      millis() - interaction_pending_ms_ > INTERACTION_STATUS_WATCHDOG_MS) {
    Serial.println("GeminiLive: interaction status watchdog; completing held turn");
    completeResponseTurn();
    return;
  }
  if (end_session_requested_ && end_session_requested_ms_ &&
      millis() - end_session_requested_ms_ > END_SESSION_GRACE_MS) {
    Serial.println("GeminiLive: end_session grace elapsed; forcing sleep disconnect");
    end_session_requested_ = false;
    speaking_ = false;
    uint32_t drainWait = 0;
    while (M5.Speaker.isPlaying() && drainWait < 3000) { delay(1); ++drainWait; }
    M5.Speaker.end();
    M5.Speaker.begin();
    M5.Speaker.setVolume(speaker_volume_);
    M5.Speaker.setAllChannelVolume(speaker_volume_);
    disconnect(true, "sleep");
  }
}

void GeminiLiveProbe::sendSetup() {
  Serial.println("GeminiLive: sending setup");
  JsonDocument doc;
  auto setup = doc["setup"].to<JsonObject>();
  setup["model"] = model_;
  auto generationConfig = setup["generationConfig"].to<JsonObject>();
  generationConfig["responseModalities"].add("AUDIO");
  generationConfig["speechConfig"]["voiceConfig"]["prebuiltVoiceConfig"]["voiceName"] = voice_name_;
  // Ask Live API to stream text transcriptions for both sides of the audio
  // conversation. These transcript chunks are logged as recent dialogues and
  // are not used for durable memory until a later batched summarizer stage.
  setup["inputAudioTranscription"].to<JsonObject>();
  setup["outputAudioTranscription"].to<JsonObject>();
  auto realtimeInputConfig = setup["realtimeInputConfig"].to<JsonObject>();
  auto automaticActivityDetection = realtimeInputConfig["automaticActivityDetection"].to<JsonObject>();
  automaticActivityDetection["disabled"] = false;
  automaticActivityDetection["startOfSpeechSensitivity"] =
      vad_start_sensitivity_high_ ? "START_SENSITIVITY_HIGH" : "START_SENSITIVITY_LOW";
  automaticActivityDetection["endOfSpeechSensitivity"] =
      vad_end_sensitivity_low_ ? "END_SENSITIVITY_LOW" : "END_SENSITIVITY_HIGH";
  automaticActivityDetection["prefixPaddingMs"] = vad_prefix_padding_ms_;
  automaticActivityDetection["silenceDurationMs"] = vad_silence_duration_ms_;
  realtimeInputConfig["turnCoverage"] =
      vad_turn_includes_all_input_ ? "TURN_INCLUDES_ALL_INPUT" : "TURN_INCLUDES_ONLY_ACTIVITY";
  Serial.printf("GeminiLive: setup model=%s voice=%s search=%s vad_prefix=%u vad_silence=%u\n",
                model_.c_str(), voice_name_.c_str(), search_grounding_ ? "on" : "off",
                static_cast<unsigned>(vad_prefix_padding_ms_),
                static_cast<unsigned>(vad_silence_duration_ms_));
  auto tools = setup["tools"].to<JsonArray>();
  // Grounding with Google Search coexists with custom functions: the Live API
  // takes several entries in one tools array. It is a separate entry, not a
  // field on the functionDeclarations entry.
  if (search_grounding_) tools.add<JsonObject>()["googleSearch"].to<JsonObject>();
  auto t0 = tools.add<JsonObject>();
  auto functionDeclarations = t0["functionDeclarations"].to<JsonArray>();
  if (tool_bridge_) {
    JsonDocument toolDoc;
    DeserializationError toolErr = deserializeJson(toolDoc, tool_bridge_->functionDeclarationsJson());
    if (!toolErr && toolDoc.is<JsonArray>()) {
      for (JsonVariant v : toolDoc.as<JsonArray>()) {
        functionDeclarations.add(v);
      }
    }
  }
  Serial.printf("GeminiLive: setup tools entries=%u declarations=%u googleSearch=%s\n",
                static_cast<unsigned>(tools.size()),
                static_cast<unsigned>(functionDeclarations.size()),
                search_grounding_ ? "yes" : "no");
  auto sys = setup["systemInstruction"].to<JsonObject>();
  sys["role"] = "user";
  String instruction =
      "You are StackChan, a compact embodied desktop robot. Use the language the user speaks unless asked otherwise. "
      "Your head has a persistent look anchor: after a deliberate look/turn, normal nods, tilts, and speaking micro-motions are relative to that anchor. "
      "Do not return to center after every answer. Use center_head only when the user asks you to rest/center/go home, says goodbye, or ends the session. "
      "Before many spoken replies, if you are not already speaking and the user is not asking for camera/search, queue at most one brief natural relative head motion "
      "with servo_gesture or head_motion; vary small nods, tilts, and glances. Then speak normally. "
      "Never call motion tools repeatedly or during speech. "
      "Camera images have corrected real-world left/right orientation. When the user asks what you see or asks you to look with the camera, call look_with_camera once. "
      "Do not speak while the camera tool is running; wait for the image turn/result, then describe what you see in the user's language. "
      "Visual search policy: if the user asks you to find, search for, look for, or locate any visible target, do a simple horizontal scan. "
      "First turn to the far-left search position with servo_gesture search_left_wide, then take exactly one photo with look_with_camera and check whether the target is there. "
      "If the target is not there, turn a little to the right and check again with exactly one photo. Continue left to right through search_left, search_center, search_right, and search_right_wide. "
      "Stop immediately when the target is found; keep looking at that sector and answer. "
      "Do not take multiple photos in the same sector, do not make a batch of photos, and do not say the target was not found until you have checked all search sectors. "
      "Use vertical up/down search only if the user explicitly asks or after the horizontal scan fails. "
      "Do not speak while the camera tool is running, and do not move the head while the camera tool itself is capturing. "
      "Web search policy: you have Grounding with Google Search. Use it for current or verifiable facts you would otherwise guess at, such as news, weather, prices, schedules, opening hours, or anything after your training data. Prefer it over guessing, keep spoken answers short, and say when an answer came from a web search. "
      "Search queries leave this device, so never put private values into one: no PINs, passwords, tokens, codes, addresses, phone numbers, or secrets, and no personal details from local memory. If answering would require searching such a value, answer locally instead or say you cannot. "
      "Memory policy: only the active post-compaction dialogue memory is provided below. It is authoritative for recent recall, but archived raw dialogues are not available to you at runtime. "
      "If the user asks whether you remember something, first use the provided active memory; if needed, call search_memory, which is limited to active memory only. "
      "If something was folded/summarized out of active context and is not explicitly present, say you do not have that detail in active memory rather than guessing. "
      "If the user explicitly asks you to remember ordinary non-sensitive information, acknowledge local memory. "
      "If the user explicitly asks you to remember a private value such as a PIN, password, token, code, address, or secret, call remember_private_memory and store it locally; do not call ask_hermes and do not forward private content. "
      "Private values must not be placed in ordinary memory/search responses. If the user later explicitly asks to recall the saved private value, call recall_private_memory locally and answer only that request. "
      "Treat PINs, passwords, tokens, codes, addresses, phone numbers, and secrets as private: never send them to Hermes/gateway/external tools, never repeat them unnecessarily, and only confirm/reveal them when the user clearly requested local recall.";
  if (system_prompt_.length()) {
    instruction += " Additional system prompt from SD/Web UI: ";
    instruction += system_prompt_;
  }
  if (persona_prompt_.length()) {
    instruction += " Persona/style prompt from SD/Web UI: ";
    instruction += persona_prompt_;
  }
  if (memory_) {
    String activeContext = memory_->buildActiveDialogueContext(12000);
    activeContext.trim();
    if (activeContext.length()) {
      instruction += "\n\nAUTHORITATIVE ACTIVE LOCAL DIALOGUE MEMORY (post-compaction only). This is the only ordinary dialogue memory currently available at runtime. It is grouped by session/date/time and private values may be replaced by PRIVATE_* markers. If a private marker is present, do not infer the value; use recall_private_memory only after an explicit user recall request. If an ordinary detail is not present here, do not retrieve it from raw archived dialogues. Never forward private lines to external tools:\n";
      instruction += activeContext;
      Serial.printf("GeminiLive: injected active dialogue context chars=%u\n", static_cast<unsigned>(activeContext.length()));
    }
  }
  sys["parts"].add<JsonObject>()["text"] = instruction;
  String out;
  serializeJson(doc, out);
  ws_.sendTXT(out);
}

void GeminiLiveProbe::sendTextTurn(const String& text) {
  // For Live API, clientContent is history/context and may not trigger a model
  // response. Realtime text input does trigger generation, including AUDIO
  // output when responseModalities contains AUDIO.
  JsonDocument doc;
  doc["realtimeInput"]["text"] = text;
  String out;
  serializeJson(doc, out);
  last_activity_ms_ = millis();
  ws_.sendTXT(out);
}

bool GeminiLiveProbe::requestTextTurn(const String& text) {
  String trimmed = text;
  trimmed.trim();
  if (!trimmed.length()) return false;
  last_activity_ms_ = millis();
  pending_text_ = trimmed;
  pending_text_turn_ = true;
  pending_start_recording_ = false;
  // Text turns can be injected while the user is in a live voice session
  // (for example async gateway/Hermes/weather callbacks). Preserve continuous
  // listening when the text interrupts an already-active dialogue; otherwise
  // StackChan speaks the injected text and falls back to neutral/sleep.
  resume_conversation_after_text_ = continuous_conversation_ || realtime_recording_;
  continuous_conversation_ = resume_conversation_after_text_;
  if (realtime_recording_) stopRealtimeRecord();
  if (!isReady()) {
    Serial.println("GeminiLive: text queued; connecting on demand");
    return connect();
  }
  Serial.println("GeminiLive: sending queued text turn");
  pending_text_turn_ = false;
  String toSend = pending_text_;
  pending_text_ = "";
  sendTextTurn(toSend);
  return true;
}

bool GeminiLiveProbe::sendImageFrame(const String& imageBase64, const String& prompt) {
  if (!isReady()) return false;
  if (imageBase64.length() == 0) return false;
  last_activity_ms_ = millis();
  if (realtime_recording_) stopRealtimeRecord();
  if (emotion_) emotion_->setEmotion("looking");

  (void)prompt;  // carried by the tool response, not by the frame

  // The frame goes in as realtime media, not as a clientContent turn ending in
  // turnComplete. A completed turn is its own generation trigger, so pairing it
  // with the toolResponse that follows made the model answer the same question
  // twice. As realtime input the frame is context only, and the toolResponse is
  // the single trigger; the prompt text rides along in that response instead.
  String out;
  out.reserve(imageBase64.length() + 120);
  out = "{\"realtimeInput\":{\"video\":{\"data\":\"";
  out += imageBase64;
  out += "\",\"mime_type\":\"image/jpeg\"}}}";
  Serial.printf("GeminiLive: sending image frame b64=%u json=%u\n",
                static_cast<unsigned>(imageBase64.length()), static_cast<unsigned>(out.length()));
  bool sent = ws_.sendTXT(out);
  return sent;
}

// Hands buffered chunks to the speaker while it has room. Never blocks: unsent
// chunks simply wait for the next call, so the websocket keeps being serviced.
void GeminiLiveProbe::drainAudioQueue() {
  if (prebuffering_) return;
  while (pending_count_ > 0 && M5.Speaker.isPlaying(1) < 2) {
    const int slot = pending_head_;
    uint8_t* buf = audio_buf_[pending_buf_[slot]];
    const int len = pending_len_[slot];
    if (!M5.Speaker.playRaw(reinterpret_cast<int16_t*>(buf), len / 2, 24000, false, 1, 1, false)) {
      ++audio_dropped_;
      Serial.println("GeminiLive: audio chunk queue failed");
      break;
    }
    pending_head_ = (pending_head_ + 1) % AUDIO_RING_BUFFERS;
    --pending_count_;
    ++audio_chunks_;
  }
}

void GeminiLiveProbe::streamAudioDeltaBase64(const String& b64) {
  uint8_t* buf = audio_buf_[next_audio_buf_];
  int len = decodeBase64(b64.c_str(), b64.length(), reinterpret_cast<char*>(buf),
                         AUDIO_BUFFER_BYTES);
  if (len <= 0) {
    ++audio_dropped_;
    return;
  }
  {
    if (!speaking_) {
      Serial.println("GeminiLive: input audio committed");
      speaking_ = true;
      if (emotion_) emotion_->setEmotion("speaking");
      stopRealtimeRecord();
      M5.Mic.end();
      M5.Speaker.begin();
      M5.Speaker.setVolume(speaker_volume_);
      M5.Speaker.setAllChannelVolume(speaker_volume_);
      audio_chunks_ = 0;
      audio_dropped_ = 0;
      audio_backpressure_wait_ms_ = 0;
      audio_underruns_ = 0;
      audio_arrival_gaps_ = 0;
      audio_arrival_gap_max_ms_ = 0;
      last_audio_rx_ms_ = 0;
      pending_head_ = 0;
      pending_count_ = 0;
      pending_peak_ = 0;
      prebuffering_ = true;
      prebuffered_ms_ = 0;
    }

    // Measured at the moment fresh audio arrives: if the channel is idle right
    // now, the DAC already ran dry and that silence was the audible gap. Checked
    // here rather than in loop() so a turn draining normally is not miscounted.
    const uint32_t now_rx = millis();
    const uint32_t arrival_gap = last_audio_rx_ms_ ? now_rx - last_audio_rx_ms_ : 0;
    // While prebuffering nothing has been handed to the speaker yet, so an idle
    // channel is expected and counting it inflated every turn by one to three.
    if (!prebuffering_ && last_audio_rx_ms_ && M5.Speaker.isPlaying(1) == 0) {
      ++audio_underruns_;
      Serial.printf("AudioUnderrun: n=%lu arrival_gap_ms=%lu chunk=%lu\n",
                    static_cast<unsigned long>(audio_underruns_),
                    static_cast<unsigned long>(arrival_gap),
                    static_cast<unsigned long>(audio_chunks_));
    } else if (arrival_gap >= AUDIO_ARRIVAL_REPORT_MS) {
      // Logging this per chunk was a line per ~200 ms of speech. Aggregate and
      // report once per turn: serial writes compete with the audio path.
      ++audio_arrival_gaps_;
      if (arrival_gap > audio_arrival_gap_max_ms_) audio_arrival_gap_max_ms_ = arrival_gap;
    }
    last_audio_rx_ms_ = now_rx;

    // Hold decoded chunks here instead of blocking on the speaker's two-slot
    // queue. Waiting inline stalled ws_.loop(), and starting playback on the
    // first chunk left no margin for the jitter that follows.
    if (pending_count_ < AUDIO_PENDING_MAX) {
      int slot = (pending_head_ + pending_count_) % AUDIO_RING_BUFFERS;
      pending_buf_[slot] = next_audio_buf_;
      pending_len_[slot] = len;
      ++pending_count_;
      if (pending_count_ > pending_peak_) pending_peak_ = pending_count_;
      next_audio_buf_ = (next_audio_buf_ + 1) % AUDIO_RING_BUFFERS;
      prebuffered_ms_ += static_cast<uint32_t>((len / 2) * 1000 / 24000);
    } else {
      ++audio_dropped_;
      Serial.println("GeminiLive: audio pending queue full");
    }

    if (prebuffering_ &&
        (prebuffered_ms_ >= AUDIO_PREBUFFER_MS || pending_count_ >= AUDIO_PENDING_MAX)) {
      Serial.printf("GeminiLive: prebuffered %lu ms in %d chunks; starting playback\n",
                    static_cast<unsigned long>(prebuffered_ms_), pending_count_);
      prebuffering_ = false;
    }
    drainAudioQueue();
  }
}

void GeminiLiveProbe::startRealtimeRecord() {
  last_activity_ms_ = millis();
  if (!isReady()) {
    Serial.println("GeminiLive: record ignored; not ready");
    return;
  }
  if (speaking_) {
    Serial.println("GeminiLive: record ignored; speaking");
    return;
  }
  if (!realtime_recording_) {
    Serial.println("GeminiLive: start realtime recording");
    // Listening again means the previous model turn is over regardless of what
    // the last interaction status said.
    turn_in_progress_ = false;
    interaction_pending_ms_ = 0;
    mic_ready_for_speech_ = false;
    // Do not show the user-facing listening cue until at least one mic chunk
    // has been successfully captured and sent to Gemini.
    if (emotion_) emotion_->setEmotion("thinking");
    // Keep the realtime listening transition silent. This path runs after every
    // response in continuous dialog, so any sound here is too frequent and delays
    // microphone availability.
    M5.Speaker.end();
    M5.Mic.begin();
    record_started_ms_ = millis();
    stopped_recording_ms_ = 0;
    realtime_recording_ = true;
  }
}

void GeminiLiveProbe::stopRealtimeRecord() {
  if (realtime_recording_) {
    Serial.println("GeminiLive: stop realtime recording");
    realtime_recording_ = false;
    mic_ready_for_speech_ = false;
    if (emotion_ && !speaking_) emotion_->setEmotion(continuous_conversation_ ? "thinking" : "neutral");
    record_started_ms_ = 0;
    stopped_recording_ms_ = millis();
    M5.Mic.end();
    M5.Speaker.begin();
    M5.Speaker.setVolume(speaker_volume_);
    M5.Speaker.setAllChannelVolume(speaker_volume_);
  }
}

void GeminiLiveProbe::playListeningChirp() {
  // Disabled for realtime listening. Future robot sounds should be attached to
  // explicit emotions/tools/boot events, use lower per-effect volume, and never
  // run in the mic-start path.
}

void GeminiLiveProbe::toggleRealtimeRecord() {
  if (realtime_recording_) stopRealtimeRecord();
  else startRealtimeRecord();
}

bool GeminiLiveProbe::requestConversationStart() {
  continuous_conversation_ = true;
  pending_start_recording_ = true;
  last_activity_ms_ = millis();
  if (!isReady()) {
    return connect();
  }
  pending_start_recording_ = false;
  startRealtimeRecord();
  return true;
}

void GeminiLiveProbe::stopConversation() {
  pending_start_recording_ = false;
  continuous_conversation_ = false;
  if (realtime_recording_) stopRealtimeRecord();
  if (!speaking_) disconnect(true);
}

void GeminiLiveProbe::requestSessionEnd(const String& reason) {
  Serial.print("GeminiLive: end_session requested");
  if (reason.length()) {
    Serial.print(" reason=");
    Serial.print(reason);
  }
  Serial.println();
  end_session_requested_ = true;
  end_session_requested_ms_ = millis();
  pending_start_recording_ = false;
  continuous_conversation_ = false;
  if (realtime_recording_) stopRealtimeRecord();
  if (emotion_) emotion_->setEmotion("sleep");
  last_activity_ms_ = millis();
}

void GeminiLiveProbe::recordAndSendAudioChunk() {
  if (!rec_buf_) return;
  if (!mic_ready_for_speech_) mic_ready_for_speech_ = false;
  if (!continuous_conversation_ && millis() - record_started_ms_ > REALTIME_RECORD_TIMEOUT_MS) {
    Serial.println("GeminiLive: realtime recording timeout");
    stopRealtimeRecord();
    return;
  }
  if (!M5.Mic.record(rec_buf_, RT_REC_SAMPLES, RT_REC_SAMPLE_RATE)) {
    ++mic_record_false_count_;
    Serial.println("GeminiLive: Mic.record false");
    delay(50);
    return;
  }
  int64_t sumSq = 0;
  int peak = 0;
  for (int i = 0; i < RT_REC_SAMPLES; ++i) {
    int v = rec_buf_[i];
    int av = v < 0 ? -v : v;
    if (av > peak) peak = av;
    sumSq += static_cast<int64_t>(v) * static_cast<int64_t>(v);
  }
  last_mic_peak_ = static_cast<uint16_t>(peak > 65535 ? 65535 : peak);
  uint32_t meanSq = static_cast<uint32_t>(sumSq / RT_REC_SAMPLES);
  // Integer sqrt keeps the realtime path lightweight and avoids extra libs.
  uint32_t x = meanSq;
  uint32_t r = 0;
  uint32_t bit = 1UL << 30;
  while (bit > x) bit >>= 2;
  while (bit != 0) {
    if (x >= r + bit) {
      x -= r + bit;
      r = (r >> 1) + bit;
    } else {
      r >>= 1;
    }
    bit >>= 2;
  }
  last_mic_rms_ = static_cast<uint16_t>(r > 65535 ? 65535 : r);
  String audio_base64 = encodeBase64(reinterpret_cast<const uint8_t*>(rec_buf_), RT_REC_SAMPLES * sizeof(int16_t));
  if (audio_base64.length()) {
    sendRealtimeAudioBase64(audio_base64);
    ++mic_chunks_sent_;
    if (!mic_ready_for_speech_) {
      mic_ready_for_speech_ = true;
      Serial.println("GeminiLive: mic ready for speech");
      if (emotion_ && realtime_recording_ && !speaking_) emotion_->setEmotion("listening");
    }
  }
}

void GeminiLiveProbe::sendRealtimeAudioBase64(const String& b64) {
  String out;
  out.reserve(b64.length() + 96);
  out = "{\"realtimeInput\":{\"audio\":{\"data\":\"";
  out += b64;
  out += "\",\"mime_type\":\"audio/pcm;rate=16000\"}}}";
  ws_.sendTXT(out);
}

String GeminiLiveProbe::encodeBase64(const uint8_t* input, size_t size) {
  size_t olen = 0;
  size_t out_len = ((size + 2) / 3) * 4 + 1;
  char* out = static_cast<char*>(malloc(out_len));
  if (!out) return String();
  int rc = mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out), out_len, &olen, input, size);
  if (rc != 0) {
    free(out);
    return String();
  }
  out[olen] = '\0';
  String encoded(out);
  free(out);
  return encoded;
}

void GeminiLiveProbe::wsEvent(WStype_t type, uint8_t* payload, size_t length) {
  if (!self_) return;
  switch (type) {
    case WStype_ERROR:
      Serial.print("GeminiLive: websocket error");
      if (payload && length) {
        Serial.print(" payload=");
        Serial.write(payload, length > 160 ? 160 : length);
      }
      Serial.println();
      break;
    case WStype_CONNECTED:
      Serial.println("GeminiLive: websocket connected");
      self_->connected_ = true;
      self_->connect_requested_ = true;
      self_->last_activity_ms_ = millis();
      self_->connect_started_ms_ = 0;
      self_->sendSetup();
      break;
    case WStype_TEXT:
      self_->handleMessage(payload, length);
      break;
    case WStype_BIN:
      self_->handleMessage(payload, length);
      break;
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
      Serial.printf("GeminiLive: websocket fragment type=%d length=%u\n", static_cast<int>(type), static_cast<unsigned>(length));
      break;
    case WStype_PING:
      Serial.println("GeminiLive: websocket ping");
      break;
    case WStype_PONG:
      Serial.println("GeminiLive: websocket pong");
      break;
    case WStype_DISCONNECTED: {
      Serial.println("GeminiLive: websocket disconnected");
      // A Live websocket may close after silence or network timeout. For the
      // robot UX, an active conversation that drops should fall back to sleep
      // instead of staring with a sticky red error face.
      bool was_active = self_->realtime_recording_ || self_->continuous_conversation_ ||
                        self_->pending_start_recording_ || self_->pending_text_turn_;
      bool should_sleep = !self_->intentional_disconnect_ && was_active;
      self_->connected_ = false;
      self_->setup_complete_ = false;
      self_->mic_ready_for_speech_ = false;
      self_->realtime_recording_ = false;
      self_->speaking_ = false;
      self_->continuous_conversation_ = false;
      self_->pending_start_recording_ = false;
      self_->pending_text_turn_ = false;
      self_->resume_conversation_after_text_ = false;
      self_->pending_text_ = "";
      self_->connect_requested_ = false;
      self_->turn_in_progress_ = false;
      self_->interaction_pending_ms_ = 0;
      self_->finishing_turn_ = false;
      self_->finish_started_ms_ = 0;
      if (self_->emotion_) self_->emotion_->setEmotion(should_sleep ? "sleep" : self_->intentional_disconnect_emotion_.c_str());
      break;
    }
    default:
      break;
  }
}

void GeminiLiveProbe::handleMessage(uint8_t* payload, size_t length) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.printf("GeminiLive: json parse error=%s length=%u\n", err.c_str(), static_cast<unsigned>(length));
    return;
  }

  if (handleServerError(doc.as<JsonVariant>())) return;

  // Last resort for locating grounding data: if the raw frame mentions it but
  // the parser above found nothing, print the top-level keys so the next
  // session can see where it actually sits instead of guessing again.
  if (!grounding_shape_logged_ && length > 8) {
    for (size_t i = 0; i + 8 <= length; ++i) {
      if (memcmp(payload + i, "rounding", 8) != 0) continue;
      grounding_shape_logged_ = true;
      Serial.print("GeminiLive: frame mentions grounding; top-level keys:");
      for (JsonPair kv : doc.as<JsonObject>()) Serial.printf(" %s", kv.key().c_str());
      JsonVariant sc = doc["serverContent"];
      if (!sc.isNull()) {
        Serial.print(" | serverContent:");
        for (JsonPair kv : sc.as<JsonObject>()) Serial.printf(" %s", kv.key().c_str());
      }
      Serial.println();
      break;
    }
  }

  JsonVariant goAway = doc["goAway"];
  if (!goAway.isNull()) {
    Serial.printf("GeminiLive: goAway timeLeft=%s\n",
                  (const char*)(goAway["timeLeft"] | "unspecified"));
  }

  JsonVariant setupComplete = doc["setupComplete"];
  if (!setupComplete.isNull()) {
    Serial.println("GeminiLive: setupComplete");
    setup_complete_ = true;
    last_activity_ms_ = millis();
    Serial.println("Status: Gemini setup ok");
    if (pending_text_turn_) {
      String toSend = pending_text_;
      pending_text_turn_ = false;
      pending_text_ = "";
      pending_start_recording_ = false;
      Serial.println("GeminiLive: setupComplete; sending pending text turn");
      sendTextTurn(toSend);
    } else if (pending_start_recording_) {
      pending_start_recording_ = false;
      startRealtimeRecord();
    }
    return;
  }

  JsonVariant serverContent = doc["serverContent"];
  handleTranscription(serverContent);
  logGroundingMetadata(doc.as<JsonVariant>());

  // Accept both spellings and both nesting levels, matching the defensive
  // parsing in the upstream raw-websocket sample.
  const char* interactionStatus = serverContent["interactionStatus"];
  if (!interactionStatus) interactionStatus = serverContent["interaction_status"];
  if (!interactionStatus) interactionStatus = doc["interactionStatus"];
  if (!interactionStatus) interactionStatus = doc["interaction_status"];
  bool requiresAction = false;
  if (interactionStatus && interactionStatus[0]) {
    saw_interaction_status_ = true;
    requiresAction = strstr(interactionStatus, "REQUIRES_ACTION") != nullptr;
    turn_in_progress_ = !requiresAction;
    interaction_pending_ms_ = requiresAction ? 0 : millis();
    Serial.printf("GeminiLive: interactionStatus=%s\n", interactionStatus);
  }

  // Support the field shape used by Gemini Live audio deltas.
  const char* data = doc["serverContent"]["modelTurn"]["parts"][0]["inlineData"]["data"];
  if (data && data[0]) streamAudioDeltaBase64(String(data));

  // A turn signal alone is not conclusive on models that reason in the
  // background: turnComplete can arrive while more audio, transcripts or tool
  // calls are still on the way. Once the server has sent an interaction
  // status, REQUIRES_ACTION is what actually ends the turn; until then the
  // firmware keeps its original turnComplete behaviour.
  bool turnSignal = !doc["serverContent"]["turnComplete"].isNull() ||
                    !doc["serverContent"]["generationComplete"].isNull();
  if (turnSignal) flushOutputTranscript();
  if (turnSignal && turn_in_progress_) {
    Serial.println("GeminiLive: turnComplete held; interaction status IN_PROGRESS");
  }
  if (requiresAction || (turnSignal && !turn_in_progress_)) completeResponseTurn();

  JsonArray functionCalls = doc["toolCall"]["functionCalls"].as<JsonArray>();
  if (!functionCalls.isNull() && tool_bridge_) {
    Serial.printf("GeminiLive: toolCall count=%u\n", static_cast<unsigned>(functionCalls.size()));
    if (emotion_) emotion_->setEmotion("thinking");
    JsonDocument responseDoc;
    // Gemini Live API docs use camelCase for tool responses:
    // {"toolResponse":{"functionResponses":[...]}}
    // The older AI_StackChan_Ex path used snake_case, but camelCase avoids the
    // model waiting indefinitely after a tool call on current Live API.
    auto functionResponses = responseDoc["toolResponse"]["functionResponses"].to<JsonArray>();
    for (JsonObject fc : functionCalls) {
      const char* name = fc["name"] | "";
      const char* id = fc["id"] | "";
      Serial.printf("GeminiLive: toolCall name=%s\n", name);
      String toolName(name);
      String result = tool_bridge_->handleFunctionCall(toolName, fc["args"].as<JsonVariantConst>(), String(id));
      JsonDocument probeDoc;
      bool resultOk = deserializeJson(probeDoc, result) == DeserializationError::Ok &&
                      probeDoc["ok"].is<bool>() && probeDoc["ok"].as<bool>();
      if (probeDoc["ok"].is<bool>() && !probeDoc["ok"].as<bool>()) {
        if (emotion_) emotion_->setEmotion("error");
      } else if (emotion_ && toolName != "set_emotion" && toolName != "end_session" && toolName != "look_with_camera") {
        emotion_->setEmotion("found");
      }
      if (toolName == "end_session" && resultOk) {
        requestSessionEnd(String((const char*)(fc["args"]["reason"] | "")));
      }
      auto fr = functionResponses.add<JsonObject>();
      if (id && id[0]) fr["id"] = id;
      fr["name"] = name;
      JsonDocument resultDoc;
      if (deserializeJson(resultDoc, result) == DeserializationError::Ok) {
        fr["response"].set(resultDoc.as<JsonVariantConst>());
      } else {
        fr["response"]["text"] = result;
      }
      // Tools declared NON_BLOCKING must say how their late result should be
      // delivered, otherwise the model has no way to schedule it.
      const char* scheduling = GeminiToolBridge::nonBlockingScheduling(toolName);
      if (scheduling && fr["response"].is<JsonObject>()) {
        fr["response"]["scheduling"] = scheduling;
      }
    }
    String out;
    serializeJson(responseDoc, out);
    ws_.sendTXT(out);
  }
}

void GeminiLiveProbe::completeResponseTurn() {
  turn_in_progress_ = false;
  interaction_pending_ms_ = 0;
  if (!finishing_turn_ && grounding_used_this_turn_) {
    Serial.printf("GeminiLive: turnGrounding used=yes sessionTurns=%lu\n",
                  static_cast<unsigned long>(grounding_turns_));
  }
  if (!finishing_turn_) grounding_used_this_turn_ = false;
  flushOutputTranscript();
  // generationComplete and turnComplete arrive as separate frames and both end
  // the turn, which logged this twice. Report once, when the turn really ends.
  if (!finishing_turn_) {
    Serial.printf("GeminiLive: responseComplete chunks=%lu dropped=%lu wait_ms=%lu underruns=%lu "
                  "gaps=%lu/%lums peakPending=%d/%d\n",
                  static_cast<unsigned long>(audio_chunks_),
                  static_cast<unsigned long>(audio_dropped_),
                  static_cast<unsigned long>(audio_backpressure_wait_ms_),
                  static_cast<unsigned long>(audio_underruns_),
                  static_cast<unsigned long>(audio_arrival_gaps_),
                  static_cast<unsigned long>(audio_arrival_gap_max_ms_),
                  pending_peak_, AUDIO_PENDING_MAX);
  }
  if (speaking_) {
    // The tail can be many seconds of audio and drains only at realtime, so
    // waiting for it here blocked the websocket and, worse, gave up after a
    // fixed guard and tore the speaker down with the rest still queued. That
    // discarded audio silently: it was never counted as dropped. Hand the
    // drain to loop() and finish the turn once the speaker is genuinely idle.
    prebuffering_ = false;
    if (!finishing_turn_) {
      finishing_turn_ = true;
      finish_started_ms_ = millis();
    }
    return;
  }
  finalizeResponseTurn();
}

void GeminiLiveProbe::finalizeResponseTurn() {
  finishing_turn_ = false;
  finish_started_ms_ = 0;
  if (speaking_) {
    speaking_ = false;
    M5.Speaker.end();
    M5.Speaker.begin();
    M5.Speaker.setVolume(speaker_volume_);
    M5.Speaker.setAllChannelVolume(speaker_volume_);
  }
  if (end_session_requested_) {
    end_session_requested_ = false;
    end_session_requested_ms_ = 0;
    if (emotion_) emotion_->setEmotion("sleep");
    Serial.println("Status: sleeping...");
    disconnect(true, "sleep");
  } else if (continuous_conversation_ && isReady()) {
    resume_conversation_after_text_ = false;
    Serial.println("Status: listening...");
    startRealtimeRecord();
  } else {
    resume_conversation_after_text_ = false;
    if (emotion_) emotion_->setEmotion("neutral");
    Serial.println("Status: Tap to talk");
  }
}

// Returns true when the frame was an error and the session was torn down.
bool GeminiLiveProbe::handleServerError(JsonVariant doc) {
  JsonVariant error = doc["error"];
  if (error.isNull()) return false;
  int code = error["code"] | 0;
  const char* status = error["status"] | "";
  const char* message = error["message"] | "unknown";
  Serial.printf("GeminiLive: server error code=%d status=%s message=%s\n", code, status, message);
  if (!setup_complete_) {
    // Almost always a rejected setup field: an unknown model id, or a voice
    // the configured model does not offer.
    Serial.printf("GeminiLive: setup rejected with model=%s voice=%s\n",
                  model_.c_str(), voice_name_.c_str());
  }
  disconnect(true, "error");
  return true;
}

// Grounding with Google Search executes on Google's side, so it never arrives
// as a toolCall and leaves no trace in the tool bridge. The attached metadata
// is the only local evidence a search ran, which otherwise makes a working
// grounding setup indistinguishable from a disabled one.
void GeminiLiveProbe::logGroundingMetadata(JsonVariant doc) {
  if (doc.isNull()) return;
  // Search ran and produced answers while this reported nothing, so the earlier
  // guess at one location was wrong. Check every level the metadata is known to
  // ride on rather than assuming; an unmatched frame is caught by the raw probe
  // in handleMessage, which prints the shape so this list can be corrected.
  JsonVariant serverContent = doc["serverContent"];
  JsonVariant candidates[] = {
      serverContent["groundingMetadata"],      serverContent["grounding_metadata"],
      serverContent["modelTurn"]["groundingMetadata"],
      serverContent["modelTurn"]["grounding_metadata"],
      doc["groundingMetadata"],                doc["grounding_metadata"],
      doc["candidates"][0]["groundingMetadata"],
      doc["candidates"][0]["grounding_metadata"],
  };
  JsonVariant grounding;
  for (JsonVariant c : candidates) {
    if (!c.isNull()) { grounding = c; break; }
  }
  if (grounding.isNull()) return;

  JsonArray queries = grounding["webSearchQueries"].as<JsonArray>();
  if (queries.isNull()) queries = grounding["web_search_queries"].as<JsonArray>();
  JsonArray chunks = grounding["groundingChunks"].as<JsonArray>();
  if (chunks.isNull()) chunks = grounding["grounding_chunks"].as<JsonArray>();

  if (!grounding_used_this_turn_) {
    grounding_used_this_turn_ = true;
    ++grounding_turns_;
  }
  Serial.printf("GeminiLive: groundingMetadata queries=%u chunks=%u\n",
                static_cast<unsigned>(queries.isNull() ? 0 : queries.size()),
                static_cast<unsigned>(chunks.isNull() ? 0 : chunks.size()));
  if (!queries.isNull()) {
    for (JsonVariant q : queries) {
      const char* text = q.as<const char*>();
      if (text && text[0]) Serial.printf("GeminiLive: searchQuery=%s\n", text);
    }
  }
}

void GeminiLiveProbe::handleTranscription(JsonVariant serverContent) {
  if (serverContent.isNull() || !memory_) return;

  const char* inputText = serverContent["inputTranscription"]["text"];
  if (!inputText) inputText = serverContent["input_transcription"]["text"];
  if (inputText && inputText[0]) {
    Serial.printf("GeminiLive: input transcript chars=%u\n", static_cast<unsigned>(strlen(inputText)));
    memory_->appendDialogue("user", String(inputText), "live_input_transcription");
  }

  const char* outputText = serverContent["outputTranscription"]["text"];
  if (!outputText) outputText = serverContent["output_transcription"]["text"];
  if (outputText && outputText[0]) {
    Serial.printf("GeminiLive: output transcript chars=%u\n", static_cast<unsigned>(strlen(outputText)));
    appendOutputTranscriptChunk(outputText);
  }
}

void GeminiLiveProbe::appendOutputTranscriptChunk(const char* text) {
  if (!text || !text[0]) return;
  String chunk(text);
  chunk.trim();
  if (!chunk.length()) return;

  if (transcript_output_buffer_.length() > 0) {
    char last = transcript_output_buffer_[transcript_output_buffer_.length() - 1];
    char first = chunk[0];
    bool firstIsPunctuation = first == '.' || first == ',' || first == '!' || first == '?' ||
                              first == ':' || first == ';' || first == ')' || first == ']';
    bool lastIsSpaceOrOpen = last == ' ' || last == '\n' || last == '(' || last == '[';
    if (!firstIsPunctuation && !lastIsSpaceOrOpen) transcript_output_buffer_ += ' ';
  }
  transcript_output_buffer_ += chunk;

  // Safety valve: avoid unbounded RAM if a very long response streams without
  // turnComplete for any reason. Normal responses flush once at responseComplete.
  if (transcript_output_buffer_.length() > 1800) flushOutputTranscript();
}

void GeminiLiveProbe::flushOutputTranscript() {
  if (!memory_) {
    transcript_output_buffer_ = "";
    return;
  }
  transcript_output_buffer_.trim();
  if (!transcript_output_buffer_.length()) return;
  Serial.printf("GeminiLive: output transcript flushed chars=%u\n",
                static_cast<unsigned>(transcript_output_buffer_.length()));
  memory_->appendDialogue("assistant", transcript_output_buffer_, "live_output_transcription");
  transcript_output_buffer_ = "";
}

int GeminiLiveProbe::decodeBase64(const char* input, int size, char* output,
                                  size_t output_capacity) {
  // base64_decode_block writes as much as the input demands and knows nothing
  // about the destination, which was survivable only while buffers were far
  // larger than any chunk. Now that they are sized to the real chunk, reject
  // anything that could not fit instead of running off the end of the buffer.
  if (size < 0) return -1;
  const size_t max_decoded = (static_cast<size_t>(size) / 4 + 1) * 3;
  if (max_decoded > output_capacity) {
    Serial.printf("GeminiLive: audio chunk too large b64=%d decoded<=%u cap=%u\n",
                  size, static_cast<unsigned>(max_decoded),
                  static_cast<unsigned>(output_capacity));
    return -1;
  }
  base64_decodestate state;
  base64_init_decodestate(&state);
  return base64_decode_block(input, size, output, &state);
}
