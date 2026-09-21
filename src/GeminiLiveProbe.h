#pragma once
#include <Arduino.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <M5Unified.h>
#include <M5StackChan.h>
#include "GeminiToolBridge.h"
#include "EmotionController.h"

class MemoryStore;

// Compile-only Gemini Live skeleton for StackChan-BSP-based firmware.
// Secrets are intentionally not embedded here. Tomorrow this should load
// Wi-Fi/API key/system prompt from SD/NVS, with redacted serial logs.
class GeminiLiveProbe {
 public:
  // Capacity that matters here is chunks, not bytes. Gemini delivers a long
  // answer faster than realtime while the speaker drains at exactly realtime,
  // so the backlog grows for the whole answer and a ring that cannot hold all
  // of it drops audio mid-sentence. Enlarging byte-wise did not help: sixteen
  // 100 KB buffers still overflowed at 42 chunks, having overflowed at 23 with
  // eight. Chunks measure ~8-20 KB, so 100 KB per buffer wasted most of the
  // ring. The same ~2 MB re-cut as 64 x 32 KB holds a complete long answer.
  // Chunks were assumed to be ~19 KB from the prebuffer timings; the guard in
  // decodeBase64 caught one at 36.5 KB (760 ms), so 32 KB was too small and a
  // chunk was rejected mid-answer. 64 KB is about twice the largest measured.
  // Buffer count comes from measurement, not estimate: peakPending reached 30
  // of 37 on a 103-chunk answer with 5.4 MB of PSRAM still free, so 64 buffers
  // (61 pending) is a ~2x margin costing headroom that nothing else needed.
  static constexpr int AUDIO_RING_BUFFERS = 64;
  static constexpr size_t AUDIO_BUFFER_BYTES = 64 * 1024;

  // Live API model shipped as the default. Runtime configs written by older
  // firmware still name the retired preview model, so loaders upgrade that
  // exact string in place and leave any other user choice untouched.
  static constexpr const char* kDefaultModel = "models/gemini-3.8-live";
  static constexpr const char* kLegacyModel = "models/gemini-3.1-flash-live-preview";
  static String upgradeLegacyModel(const String& model) {
    return model == kLegacyModel ? String(kDefaultModel) : model;
  }

  // Prebuilt voices confirmed available on the default Live model. Older
  // configs may name one of the wider catalogue that this model does not
  // offer, which the API rejects at setup, so unknown names fall back to the
  // default rather than failing the session.
  static constexpr const char* kDefaultVoice = "Puck";
  static String supportedVoice(const String& voice) {
    if (voice == "Puck" || voice == "Charon" || voice == "Kore" ||
        voice == "Fenrir" || voice == "Aoede") {
      return voice;
    }
    return String(kDefaultVoice);
  }

  bool begin(const char* api_key);
  bool connect();
  void disconnect(bool intentional = true, const char* finalEmotion = nullptr);
  void setToolBridge(GeminiToolBridge* bridge) { tool_bridge_ = bridge; }
  void setEmotionController(EmotionController* emotion) { emotion_ = emotion; }
  void setMemoryStore(MemoryStore* memory) { memory_ = memory; }
  void setSpeakerVolume(uint8_t volume) { speaker_volume_ = volume; }
  void setVadConfig(uint16_t prefixPaddingMs, uint16_t silenceDurationMs,
                    bool startSensitivityHigh, bool endSensitivityLow,
                    bool turnIncludesAllInput);
  uint16_t vadPrefixPaddingMs() const { return vad_prefix_padding_ms_; }
  uint16_t vadSilenceDurationMs() const { return vad_silence_duration_ms_; }
  bool vadStartSensitivityHigh() const { return vad_start_sensitivity_high_; }
  bool vadEndSensitivityLow() const { return vad_end_sensitivity_low_; }
  bool vadTurnIncludesAllInput() const { return vad_turn_includes_all_input_; }
  void setModel(const String& model) { if (model.length()) model_ = model; }
  void setVoiceName(const String& voiceName) { if (voiceName.length()) voice_name_ = voiceName; }
  void setSearchGrounding(bool enabled) { search_grounding_ = enabled; }
  // Comma separated BCP-47 codes, e.g. "ru-RU,en-US". Empty means no hint.
  void setTranscriptionLanguageCodes(const String& codes) {
    transcription_language_codes_ = codes;
    transcription_language_codes_.trim();
  }
  bool searchGrounding() const { return search_grounding_; }
  void setSystemPrompt(const String& systemPrompt) { system_prompt_ = systemPrompt; system_prompt_.trim(); }
  void setPersonaPrompt(const String& personaPrompt) { persona_prompt_ = personaPrompt; persona_prompt_.trim(); }
  void loop();
  void sendSetup();
  void sendTextTurn(const String& text);
  bool requestTextTurn(const String& text);
  bool sendImageFrame(const String& imageBase64, const String& prompt);
  void streamAudioDeltaBase64(const String& b64);
  void drainAudioQueue();
  bool isReady() const { return connected_ && setup_complete_; }
  bool isRecording() const { return realtime_recording_; }
  bool isSpeaking() const { return speaking_; }
  bool continuousConversation() const { return continuous_conversation_; }
  bool isMicReadyForSpeech() const { return mic_ready_for_speech_; }
  uint16_t lastMicRms() const { return last_mic_rms_; }
  uint16_t lastMicPeak() const { return last_mic_peak_; }
  uint32_t micChunksSent() const { return mic_chunks_sent_; }
  uint32_t micRecordFalseCount() const { return mic_record_false_count_; }
  uint32_t audioChunksPlayed() const { return audio_chunks_; }
  uint32_t audioChunksDropped() const { return audio_dropped_; }
  uint32_t audioBackpressureWaitMs() const { return audio_backpressure_wait_ms_; }
  uint32_t audioUnderruns() const { return audio_underruns_; }
  void setContinuousConversation(bool enabled) { continuous_conversation_ = enabled; }
  bool requestConversationStart();
  void stopConversation();
  void requestSessionEnd(const String& reason = "");
  void startRealtimeRecord();
  void stopRealtimeRecord();
  void toggleRealtimeRecord();

 private:
  static constexpr int RT_REC_SAMPLE_RATE = 16000;
  static constexpr int RT_REC_SAMPLES = 2000;  // 0.125s at 16 kHz, copied from working realtime path.
  static constexpr uint32_t REALTIME_RECORD_TIMEOUT_MS = 30000;
  static constexpr uint32_t IDLE_DISCONNECT_MS = 120000;
  static constexpr uint32_t CONNECT_TIMEOUT_MS = 20000;
  static constexpr uint32_t CONTINUOUS_CONVERSATION_TIMEOUT_MS = 150000;
  static constexpr uint32_t CONTINUOUS_RECORD_WATCHDOG_MS = 2500;
  static constexpr uint32_t END_SESSION_GRACE_MS = 12000;
  // How long to keep a turn open after the last IN_PROGRESS interaction
  // status before giving up and completing it anyway.
  static constexpr uint32_t INTERACTION_STATUS_WATCHDOG_MS = 20000;
  // Upper bound on playing out a turn's tail. Only a stuck speaker should ever
  // reach it: a backlog drains at realtime, so this must exceed the longest
  // answer the ring can hold, not the few seconds a blocking drain could spare.
  static constexpr uint32_t TURN_DRAIN_TIMEOUT_MS = 45000;
  // Report arrival gaps big enough to matter against the speaker buffer depth.
  static constexpr uint32_t AUDIO_ARRIVAL_REPORT_MS = 150;
  // Playback used to start on the first chunk with nothing behind it, so any
  // jitter in the opening moments ran the DAC dry. Hold this much audio before
  // the first chunk is queued; after that the stream over-delivers and keeps
  // itself ahead. Kept below the ring capacity so decoding never overtakes it.
  static constexpr uint32_t AUDIO_PREBUFFER_MS = 400;
  // Reserve three: up to two handed to the speaker whose DMA still reads them,
  // plus the one currently being decoded. Overwriting either is an audible tear.
  static constexpr int AUDIO_PENDING_MAX = AUDIO_RING_BUFFERS - 3;

  WebSocketsClient ws_;
  uint8_t* audio_buf_[AUDIO_RING_BUFFERS] = {nullptr};
  int16_t* rec_buf_ = nullptr;
  int next_audio_buf_ = 0;
  bool connected_ = false;
  bool setup_complete_ = false;
  bool realtime_recording_ = false;
  bool speaking_ = false;
  bool continuous_conversation_ = false;
  bool configured_ = false;
  bool connect_requested_ = false;
  bool intentional_disconnect_ = false;
  bool pending_start_recording_ = false;
  bool pending_text_turn_ = false;
  bool resume_conversation_after_text_ = false;
  bool mic_ready_for_speech_ = false;
  // Whether this conversation has reached "listening" even once. Before it
  // has, the robot shows the connecting spinner; after it has, a pause for the
  // mic is just part of the back-and-forth and showing it would only flicker.
  bool session_listened_ = false;
  bool end_session_requested_ = false;
  // Models with background reasoning keep working after turnComplete, so the
  // interaction status drives end-of-turn once the server has sent one.
  bool saw_interaction_status_ = false;
  bool turn_in_progress_ = false;
  uint32_t record_started_ms_ = 0;
  uint32_t last_activity_ms_ = 0;
  uint32_t connect_started_ms_ = 0;
  uint32_t stopped_recording_ms_ = 0;
  uint32_t end_session_requested_ms_ = 0;
  uint32_t interaction_pending_ms_ = 0;
  uint32_t audio_chunks_ = 0;
  uint32_t audio_dropped_ = 0;
  uint32_t audio_backpressure_wait_ms_ = 0;
  // A loop stall is not an audio gap: the backpressure wait happens because the
  // speaker queue is full, which means playback is healthy. The gap is the
  // opposite condition, the queue running dry with more audio still to come.
  uint32_t audio_underruns_ = 0;
  // Arrival gaps are aggregated per turn rather than logged per chunk.
  uint32_t audio_arrival_gaps_ = 0;
  uint32_t audio_arrival_gap_max_ms_ = 0;
  uint32_t last_audio_rx_ms_ = 0;
  // Chunks decoded but not yet handed to the speaker, oldest first.
  int pending_buf_[AUDIO_RING_BUFFERS] = {0};
  int pending_len_[AUDIO_RING_BUFFERS] = {0};
  int pending_head_ = 0;
  int pending_count_ = 0;
  // Peak backlog per turn: the number that actually sizes the ring.
  int pending_peak_ = 0;
  bool prebuffering_ = false;
  // A turn whose audio is still playing out. The teardown waits for loop() to
  // drain it instead of blocking, which previously discarded the tail.
  bool finishing_turn_ = false;
  uint32_t finish_started_ms_ = 0;
  uint32_t prebuffered_ms_ = 0;
  uint16_t last_mic_rms_ = 0;
  uint16_t last_mic_peak_ = 0;
  uint32_t mic_chunks_sent_ = 0;
  uint32_t mic_record_false_count_ = 0;
  uint8_t speaker_volume_ = 200;
  uint16_t vad_prefix_padding_ms_ = 800;
  uint16_t vad_silence_duration_ms_ = 900;
  bool vad_start_sensitivity_high_ = false;
  bool vad_end_sensitivity_low_ = true;
  bool vad_turn_includes_all_input_ = false;
  bool search_grounding_ = true;
  String transcription_language_codes_;
  // Search runs server-side, so these only record what the metadata reported.
  bool grounding_used_this_turn_ = false;
  uint32_t grounding_turns_ = 0;
  bool grounding_shape_logged_ = false;
  String api_key_storage_;
  String model_ = kDefaultModel;
  String voice_name_ = kDefaultVoice;
  String system_prompt_;
  String persona_prompt_;
  String intentional_disconnect_emotion_ = "neutral";
  String pending_text_;
  String transcript_output_buffer_;
  const char* api_key_ = nullptr;
  GeminiToolBridge* tool_bridge_ = nullptr;
  EmotionController* emotion_ = nullptr;
  MemoryStore* memory_ = nullptr;

  static GeminiLiveProbe* self_;
  static void wsEvent(WStype_t type, uint8_t* payload, size_t length);
  void handleMessage(uint8_t* payload, size_t length);
  void handleTranscription(JsonVariant serverContent);
  void logGroundingMetadata(JsonVariant doc);
  bool handleServerError(JsonVariant doc);
  void completeResponseTurn();
  void finalizeResponseTurn();
  void appendOutputTranscriptChunk(const char* text);
  void flushOutputTranscript();
  void recordAndSendAudioChunk();
  void playListeningChirp();
  void sendRealtimeAudioBase64(const String& b64);
  String encodeBase64(const uint8_t* input, size_t size);
  int decodeBase64(const char* input, int size, char* output, size_t output_capacity);
};
