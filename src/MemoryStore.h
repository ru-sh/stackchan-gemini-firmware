#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>

// SD-backed memory scaffold for StackChan option-B firmware.
// Goal: keep only a short live context in RAM, append raw events to SD,
// and compact older history into summaries/facts before feeding Gemini.
class MemoryStore {
 public:
  struct Policy {
    uint8_t keepRawSessions = 3;       // Keep the latest N sessions as raw event streams.
    uint8_t keepRawDays = 3;           // Or keep raw files for the latest N days when date is known.
    size_t maxEventFileBytes = 256 * 1024;
    size_t maxRetrievedChars = 3000;   // Hard cap for snippets injected into Gemini setup/context.
  };

  explicit MemoryStore(fs::FS& fs, const char* root = "/app/StackChan/memory");

  bool begin();
  bool begin(const Policy& policy);
  // Takes the date and session id from the clock if it arrived after begin().
  // Returns true the one time it does. A boot that misses the sync would
  // otherwise stay undated until the next reboot.
  bool adoptClock();
  bool appendEvent(const char* role, const String& text, const char* type = "utterance");
  bool appendDialogue(const char* role, const String& text, const char* source = "live_transcription");
  bool appendFact(const String& fact, const char* source = "assistant", float confidence = 0.7f);
  bool appendSummary(const String& summary, const char* scope = "session");
  bool rememberPrivateMemory(const String& kind, const String& label, const String& value);
  bool recallPrivateMemory(const String& query, String& kind, String& label, String& value, String& ts);

  // Compile-stage placeholder: records that compaction should happen.
  // Later this will call Gemini/Hermes summarizer, then rewrite/mark old raw files.
  bool compactIfNeeded();

  // Small context builder for tomorrow's Gemini setup prompt.
  // Later: score by query, entities, time, and optional vector/Hermes retrieval.
  String buildContextForPrompt(const String& query, size_t maxChars = 0);
  String buildActiveDialogueContext(size_t maxChars = 12000);
  String recentDialoguesPreview(size_t maxChars = 6000);
  String summariesPreview(size_t maxChars = 8000);
  String searchMemory(const String& query, size_t maxResults = 8, size_t maxChars = 8000);
  bool commitSemanticSummary(const String& summaryJson, const String& model, String& message);
  void addStats(JsonObject obj);
  bool summarizeActiveContext(String& message);
  bool queueVectorization(String& message);

  const char* currentSessionId() const { return _sessionId.c_str(); }
  const char* todayKey() const { return _todayKey.c_str(); }

 private:
  fs::FS& _fs;
  String _root;
  String _sessionId;
  String _todayKey;
  Policy _policy;
  bool _ready = false;

  bool ensureDir(const String& path);
  bool appendJsonLine(const String& path, const String& line);
  bool compactDialoguesFile();
  bool cleanupScaffoldMemoryRecords();
  bool pruneJsonlFieldEquals(const String& path, const char* field, const char* value);
  bool activeDialogueBounds(String& startTs, String& endTs, String& firstSession) const;
  bool appendSemanticFact(JsonVariant factVar, const String& observedStart, const String& observedEnd, const String& sourceSession, const String& summaryTs);
  int scoreMemoryText(const String& queryLower, const String& textLower, const String& tagsLower, const String& observedDate) const;
  String dateForRelativeQuery(const String& queryLower) const;
  String activeAfterTs() const;
  bool writeActiveAfterTs(const String& ts);
  bool readDialogueLine(const String& line, String& ts, String& session, String& role, String& text) const;
  String redactSensitiveForRuntime(const String& text) const;
  String privateMemoryPath() const;
  size_t fileSize(const String& path) const;
  size_t countLines(const String& path) const;
  String dialogueJsonLine(const String& ts, const String& session, const String& role, const String& text) const;
  void appendDialogueChunk(String& buffer, const String& chunk) const;
  String eventsPath() const;
  String dialoguesPath() const;
  String factsPath() const;
  String summariesPath() const;
  String contextStatePath() const;
  String vectorIndexPath() const;
  String nowIso() const;
  String jsonEscape(const String& value) const;
  String readTail(const String& path, size_t maxChars);
};
