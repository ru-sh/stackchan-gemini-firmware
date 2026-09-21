#include "MemoryStore.h"

#include <ArduinoJson.h>
#include <SD.h>
#include <time.h>

MemoryStore::MemoryStore(fs::FS& fs, const char* root) : _fs(fs), _root(root) {}

bool MemoryStore::begin() {
  Policy policy;
  return begin(policy);
}

namespace {
// 2023-11-14. Anything earlier means the clock was never set: the ESP32 starts
// its epoch in 1970 and getLocalTime has its own, looser, threshold.
constexpr time_t kClockSanityEpoch = 1700000000;
}  // namespace

bool MemoryStore::adoptClock() {
  if (_todayKey != "undated") return false;
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 10)) return false;

  char day[16];
  char session[32];
  strftime(day, sizeof(day), "%Y-%m-%d", &timeInfo);
  strftime(session, sizeof(session), "%Y%m%d-%H%M%S", &timeInfo);
  // Whatever was written before this point keeps the undated name and the
  // boot- id. Splitting one boot across two sessions is worth it: the
  // alternative is everything after it being undated too.
  Serial.printf("MemoryStore: clock arrived; day %s -> %s, session %s -> %s\n",
                _todayKey.c_str(), day, _sessionId.c_str(), session);
  _todayKey = day;
  _sessionId = session;
  return true;
}

bool MemoryStore::begin(const Policy& policy) {
  _policy = policy;

  struct tm timeInfo;
  if (getLocalTime(&timeInfo, 10)) {
    char day[16];
    char session[32];
    strftime(day, sizeof(day), "%Y-%m-%d", &timeInfo);
    strftime(session, sizeof(session), "%Y%m%d-%H%M%S", &timeInfo);
    _todayKey = day;
    _sessionId = session;
  } else {
    _todayKey = "undated";
    _sessionId = String("boot-") + String((uint32_t)millis());
  }

  _ready = ensureDir(_root) &&
           ensureDir(_root + "/events") &&
           ensureDir(_root + "/dialogues") &&
           ensureDir(_root + "/index");

  if (_ready) {
    compactDialoguesFile();
    cleanupScaffoldMemoryRecords();
    appendEvent("system", String("memory_store_ready session=") + _sessionId, "state");
  }
  return _ready;
}

bool MemoryStore::appendEvent(const char* role, const String& text, const char* type) {
  if (!_ready) return false;
  String line = "{";
  line += "\"ts\":\"" + nowIso() + "\",";
  line += "\"session\":\"" + jsonEscape(_sessionId) + "\",";
  line += "\"type\":\"" + jsonEscape(type) + "\",";
  line += "\"role\":\"" + jsonEscape(role) + "\",";
  line += "\"text\":\"" + jsonEscape(text) + "\"";
  line += "}";
  return appendJsonLine(eventsPath(), line);
}

bool MemoryStore::appendDialogue(const char* role, const String& text, const char* source) {
  if (!_ready) return false;
  (void)source;  // Dialogues are already separated from events/facts; keep records compact.
  String trimmed = text;
  trimmed.trim();
  if (!trimmed.length()) return false;
  return appendJsonLine(dialoguesPath(), dialogueJsonLine(nowIso(), _sessionId, String(role), trimmed));
}

bool MemoryStore::appendFact(const String& fact, const char* source, float confidence) {
  if (!_ready) return false;
  String cleanFact = fact;
  cleanFact.trim();
  if (!cleanFact.length()) return false;
  if (String(source) == "firmware_boot") return false;
  String line = "{";
  line += "\"ts\":\"" + nowIso() + "\",";
  line += "\"source\":\"" + jsonEscape(source) + "\",";
  line += "\"confidence\":" + String(confidence, 2) + ",";
  line += "\"fact\":\"" + jsonEscape(cleanFact) + "\"";
  line += "}";
  return appendJsonLine(factsPath(), line);
}

bool MemoryStore::appendSummary(const String& summary, const char* scope) {
  if (!_ready) return false;
  String cleanSummary = summary;
  cleanSummary.trim();
  if (!cleanSummary.length()) return false;
  if (String(scope) == "boot") return false;
  String line = "{";
  line += "\"ts\":\"" + nowIso() + "\",";
  line += "\"scope\":\"" + jsonEscape(scope) + "\",";
  line += "\"session\":\"" + jsonEscape(_sessionId) + "\",";
  line += "\"summary\":\"" + jsonEscape(cleanSummary) + "\"";
  line += "}";
  return appendJsonLine(summariesPath(), line);
}

bool MemoryStore::rememberPrivateMemory(const String& kind, const String& label, const String& value) {
  if (!_ready) return false;
  String cleanKind = kind;
  String cleanLabel = label;
  String cleanValue = value;
  cleanKind.trim();
  cleanLabel.trim();
  cleanValue.trim();
  if (!cleanValue.length()) return false;
  if (!cleanKind.length()) cleanKind = "private_note";
  if (!cleanLabel.length()) cleanLabel = cleanKind;

  String line = "{";
  line += "\"ts\":\"" + nowIso() + "\",";
  line += "\"session\":\"" + jsonEscape(_sessionId) + "\",";
  line += "\"kind\":\"" + jsonEscape(cleanKind) + "\",";
  line += "\"label\":\"" + jsonEscape(cleanLabel) + "\",";
  line += "\"value\":\"" + jsonEscape(cleanValue) + "\",";
  line += "\"recall_policy\":\"explicit_user_request_only_local\"";
  line += "}";
  return appendJsonLine(privateMemoryPath(), line);
}

bool MemoryStore::recallPrivateMemory(const String& query, String& kind, String& label, String& value, String& ts) {
  kind = "";
  label = "";
  value = "";
  ts = "";
  if (!_ready) return false;
  File f = _fs.open(privateMemoryPath(), FILE_READ);
  if (!f) return false;

  String q = query;
  q.trim();
  q.toLowerCase();
  bool found = false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;
    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
    String candKind = String((const char*)(doc["kind"] | ""));
    String candLabel = String((const char*)(doc["label"] | ""));
    String candValue = String((const char*)(doc["value"] | ""));
    String candTs = String((const char*)(doc["ts"] | ""));
    if (!candValue.length()) continue;
    String kindLower = candKind;
    String labelLower = candLabel;
    kindLower.toLowerCase();
    labelLower.toLowerCase();
    String hay = kindLower + " " + labelLower;
    bool match = !q.length() || hay.indexOf(q) >= 0 || q.indexOf(kindLower) >= 0 || q.indexOf(labelLower) >= 0;
    if (!match) {
      if ((q.indexOf("pin") >= 0 || q.indexOf("\xD0\xBF\xD0\xB8\xD0\xBD") >= 0) && kindLower.indexOf("pin") >= 0) match = true;
      if ((q.indexOf("code") >= 0 || q.indexOf("\xD0\xBA\xD0\xBE\xD0\xB4") >= 0) && kindLower.indexOf("code") >= 0) match = true;
    }
    if (!match) continue;
    kind = candKind;
    label = candLabel;
    value = candValue;
    ts = candTs;
    found = true;  // Keep scanning so the latest matching record wins.
  }
  f.close();
  return found;
}

bool MemoryStore::activeDialogueBounds(String& startTs, String& endTs, String& firstSession) const {
  startTs = "";
  endTs = "";
  firstSession = "";
  if (!_ready) return false;
  String after = activeAfterTs();
  File f = _fs.open(dialoguesPath(), FILE_READ);
  if (!f) return false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    String ts, session, role, text;
    if (!readDialogueLine(line, ts, session, role, text)) continue;
    if (after.length() && ts.length() && ts <= after) continue;
    if (!startTs.length()) {
      startTs = ts;
      firstSession = session;
    }
    if (ts.length()) endTs = ts;
  }
  f.close();
  return startTs.length() || endTs.length();
}

bool MemoryStore::appendSemanticFact(JsonVariant factVar, const String& observedStart, const String& observedEnd, const String& sourceSession, const String& summaryTs) {
  if (!_ready) return false;
  String factText;
  float confidence = 0.8f;
  JsonArray tags;
  if (factVar.is<const char*>()) {
    factText = String((const char*)factVar);
  } else if (factVar.is<JsonObject>()) {
    JsonObject obj = factVar.as<JsonObject>();
    factText = String((const char*)(obj["text"] | ""));
    confidence = obj["confidence"] | 0.8f;
    tags = obj["tags"].as<JsonArray>();
  }
  factText.trim();
  if (!factText.length()) return false;

  String line = "{";
  line += "\"ts\":\"" + jsonEscape(summaryTs.length() ? summaryTs : nowIso()) + "\",";
  line += "\"source\":\"semantic_summary_v2\",";
  line += "\"source_session\":\"" + jsonEscape(sourceSession) + "\",";
  line += "\"observed_start_ts\":\"" + jsonEscape(observedStart) + "\",";
  line += "\"observed_end_ts\":\"" + jsonEscape(observedEnd) + "\",";
  line += "\"confidence\":" + String(confidence, 2) + ",";
  line += "\"tags\":[";
  bool first = true;
  for (JsonVariant t : tags) {
    String tag = String((const char*)t);
    tag.trim();
    if (!tag.length()) continue;
    if (!first) line += ",";
    line += "\"" + jsonEscape(tag) + "\"";
    first = false;
  }
  line += "],";
  line += "\"fact\":\"" + jsonEscape(factText) + "\"";
  line += "}";
  return appendJsonLine(factsPath(), line);
}

bool MemoryStore::compactIfNeeded() {
  if (!_ready) return false;
  File f = _fs.open(eventsPath(), FILE_READ);
  if (!f) return false;
  size_t size = f.size();
  f.close();

  if (size <= _policy.maxEventFileBytes) return true;

  // Do not summarize locally with heuristics yet; mark for Gemini/Hermes compaction.
  // This avoids bad lossy compression. Tomorrow's ContextBuilder can detect this marker.
  return appendSummary(String("COMPACTION_REQUESTED events_file=") + eventsPath() +
                       " bytes=" + String(size), "maintenance");
}

String MemoryStore::buildContextForPrompt(const String& query, size_t maxChars) {
  if (!_ready) return "";
  (void)query;  // Future: lexical/entity scoring + optional Hermes vector retrieval.
  if (maxChars == 0) maxChars = _policy.maxRetrievedChars;

  String context;
  context.reserve(maxChars + 128);
  context += "Relevant local memory from SD:\n";
  size_t sectionBudget = maxChars / 4;
  context += "[Recent summaries]\n";
  context += readTail(summariesPath(), sectionBudget);
  context += "\n[Durable facts]\n";
  context += readTail(factsPath(), sectionBudget);
  context += "\n[Active recent dialogues]\n";
  context += buildActiveDialogueContext(sectionBudget);
  context += "\n[Recent raw events]\n";
  context += readTail(eventsPath(), sectionBudget);

  if (context.length() > maxChars) {
    context = context.substring(context.length() - maxChars);
  }
  return context;
}

String MemoryStore::buildActiveDialogueContext(size_t maxChars) {
  if (!_ready) return "";
  if (maxChars == 0) maxChars = 12000;
  String after = activeAfterTs();
  File f = _fs.open(dialoguesPath(), FILE_READ);
  if (!f) return "";

  String out;
  String lastSession;
  String lastDate;
  out.reserve(maxChars + 256);
  while (f.available()) {
    String line = f.readStringUntil('\n');
    String ts, session, role, text;
    if (!readDialogueLine(line, ts, session, role, text)) continue;
    if (after.length() && ts.length() && ts <= after) continue;

    String date = ts.length() >= 10 ? ts.substring(0, 10) : _todayKey;
    String time = ts.length() >= 19 && ts.indexOf('T') == 10 ? ts.substring(11, 19) : ts;
    if (session != lastSession || date != lastDate) {
      if (out.length()) out += "\n";
      out += "[Session ";
      out += session.length() ? session : "unknown";
      out += ", date ";
      out += date;
      out += "]\n";
      lastSession = session;
      lastDate = date;
    }
    String safeText = redactSensitiveForRuntime(text);
    out += time;
    out += " ";
    out += (role == "user") ? "User" : "Assistant";
    out += ": ";
    out += safeText;
    out += "\n";
    if (out.length() > maxChars) {
      out = out.substring(out.length() - maxChars);
      int firstHeader = out.indexOf("[Session ");
      if (firstHeader > 0) out = out.substring(firstHeader);
      else {
        int firstNl = out.indexOf('\n');
        if (firstNl >= 0 && firstNl + 1 < (int)out.length()) out = out.substring(firstNl + 1);
      }
    }
  }
  f.close();
  return out;
}

String MemoryStore::summariesPreview(size_t maxChars) {
  if (!_ready) return "";
  if (maxChars == 0) maxChars = 8000;
  return readTail(summariesPath(), maxChars);
}

String MemoryStore::dateForRelativeQuery(const String& queryLower) const {
  if (queryLower.indexOf("today") >= 0 || queryLower.indexOf("\xD1\x81\xD0\xB5\xD0\xB3\xD0\xBE\xD0\xB4\xD0\xBD\xD1\x8F") >= 0) return _todayKey;
  if (queryLower.indexOf("yesterday") >= 0 || queryLower.indexOf("\xD0\xB2\xD1\x87\xD0\xB5\xD1\x80\xD0\xB0") >= 0) {
    struct tm timeInfo;
    if (getLocalTime(&timeInfo, 10)) {
      time_t t = mktime(&timeInfo) - 24 * 60 * 60;
      struct tm* y = localtime(&t);
      if (y) {
        char buf[16];
        strftime(buf, sizeof(buf), "%Y-%m-%d", y);
        return String(buf);
      }
    }
  }
  // Explicit ISO-like date in query, e.g. 2026-05-07.
  for (int i = 0; i <= (int)queryLower.length() - 10; ++i) {
    if (isDigit(queryLower[i]) && isDigit(queryLower[i+1]) && isDigit(queryLower[i+2]) && isDigit(queryLower[i+3]) &&
        queryLower[i+4] == '-' && isDigit(queryLower[i+5]) && isDigit(queryLower[i+6]) &&
        queryLower[i+7] == '-' && isDigit(queryLower[i+8]) && isDigit(queryLower[i+9])) {
      return queryLower.substring(i, i + 10);
    }
  }
  return "";
}

int MemoryStore::scoreMemoryText(const String& queryLower, const String& textLower, const String& tagsLower, const String& observedDate) const {
  if (!queryLower.length()) return 1;
  int score = 0;
  if (observedDate.length() && queryLower.indexOf(observedDate) >= 0) score += 6;
  int start = 0;
  while (start < (int)queryLower.length()) {
    while (start < (int)queryLower.length()) {
      char c = queryLower[start];
      bool sep = c == ' ' || c == '\n' || c == '\t' || c == ',' || c == '.' || c == '?' || c == '!' || c == ':' || c == ';' || c == '(' || c == ')' || c == '[' || c == ']';
      if (!sep) break;
      ++start;
    }
    int end = start;
    while (end < (int)queryLower.length()) {
      char c = queryLower[end];
      bool sep = c == ' ' || c == '\n' || c == '\t' || c == ',' || c == '.' || c == '?' || c == '!' || c == ':' || c == ';' || c == '(' || c == ')' || c == '[' || c == ']';
      if (sep) break;
      ++end;
    }
    if (end > start) {
      String tok = queryLower.substring(start, end);
      if (tok.length() >= 4) {
        if (textLower.indexOf(tok) >= 0) score += 3;
        if (tagsLower.indexOf(tok) >= 0) score += 5;
      }
    }
    start = end + 1;
  }
  if ((queryLower.indexOf("people") >= 0 || queryLower.indexOf("person") >= 0 ||
       queryLower.indexOf("\xD0\xBB\xD1\x8E\xD0\xB4") >= 0 || queryLower.indexOf("\xD1\x87\xD0\xB5\xD0\xBB\xD0\xBE\xD0\xB2\xD0\xB5\xD0\xBA") >= 0) &&
      (tagsLower.indexOf("people") >= 0 || textLower.indexOf("person") >= 0 || textLower.indexOf("people") >= 0 ||
       textLower.indexOf("\xD1\x87\xD0\xB5\xD0\xBB\xD0\xBE\xD0\xB2\xD0\xB5\xD0\xBA") >= 0 || textLower.indexOf("\xD0\xBB\xD1\x8E\xD0\xB4") >= 0)) score += 6;
  if ((queryLower.indexOf("room") >= 0 || queryLower.indexOf("saw") >= 0 || queryLower.indexOf("look") >= 0 ||
       queryLower.indexOf("\xD0\xBA\xD0\xBE\xD0\xBC\xD0\xBD\xD0\xB0\xD1\x82") >= 0 || queryLower.indexOf("\xD0\xB2\xD0\xB8\xD0\xB4\xD0\xB5\xD0\xBB") >= 0 || queryLower.indexOf("\xD1\x81\xD0\xBC\xD0\xBE\xD1\x82\xD1\x80") >= 0) &&
      (tagsLower.indexOf("vision") >= 0 || tagsLower.indexOf("room") >= 0 || tagsLower.indexOf("search") >= 0)) score += 5;
  if ((queryLower.indexOf("color") >= 0 || queryLower.indexOf("clothes") >= 0 || queryLower.indexOf("shirt") >= 0 ||
       queryLower.indexOf("\xD1\x86\xD0\xB2\xD0\xB5\xD1\x82") >= 0 || queryLower.indexOf("\xD0\xBE\xD0\xB4\xD0\xB5\xD0\xB6") >= 0 || queryLower.indexOf("\xD1\x84\xD1\x83\xD1\x82\xD0\xB1\xD0\xBE\xD0\xBB") >= 0) &&
      (textLower.indexOf("red") >= 0 || textLower.indexOf("white") >= 0 || textLower.indexOf("shirt") >= 0 ||
       textLower.indexOf("\xD0\xBA\xD1\x80\xD0\xB0\xD1\x81\xD0\xBD") >= 0 || textLower.indexOf("\xD0\xB1\xD0\xB5\xD0\xBB") >= 0 || textLower.indexOf("\xD1\x84\xD1\x83\xD1\x82\xD0\xB1\xD0\xBE\xD0\xBB") >= 0)) score += 4;
  if ((queryLower.indexOf("weather") >= 0 || queryLower.indexOf("\xD0\xBF\xD0\xBE\xD0\xB3\xD0\xBE\xD0\xB4") >= 0) &&
      (textLower.indexOf("weather") >= 0 || textLower.indexOf("\xD0\xBF\xD0\xBE\xD0\xB3\xD0\xBE\xD0\xB4") >= 0)) score += 6;
  if (queryLower.indexOf("\xD0\xB2\xD0\xB0\xD1\x80\xD1\x88\xD0\xB0\xD0\xB2") >= 0 && textLower.indexOf("\xD0\xB2\xD0\xB0\xD1\x80\xD1\x88\xD0\xB0\xD0\xB2") >= 0) score += 6;
  return score;
}

String MemoryStore::searchMemory(const String& query, size_t maxResults, size_t maxChars) {
  JsonDocument out;
  out["ok"] = true;
  out["query"] = query;
  out["raw_dialogues_exposed"] = false;
  out["policy"] = "time-aware lexical/tag retrieval over active context, facts, and semantic summaries; private values remain redacted/marker-only";
  String q = query;
  q.trim();
  String qLower = q;
  qLower.toLowerCase();
  String dateFilter = dateForRelativeQuery(qLower);
  if (dateFilter.length()) out["date_filter"] = dateFilter;
  out["default_time_policy"] = dateFilter.length() ? "filter_to_requested_date" : "prefer_recent_and_group_by_observed_time";
  auto results = out["results"].to<JsonArray>();

  String active = buildActiveDialogueContext(3000);
  active.trim();
  if (active.length()) {
    String lower = active;
    lower.toLowerCase();
    int score = scoreMemoryText(qLower, lower, "active dialogue recent", "");
    if (score > 0 || !qLower.length()) {
      auto r = results.add<JsonObject>();
      r["layer"] = "active_context";
      r["score"] = score + 10;
      r["recency"] = "fresh";
      r["text"] = active;
    }
  }

  struct Hit { int score; String json; };
  Hit hits[12];
  size_t hitCount = 0;
  auto addHit = [&](int score, const String& json) {
    if (score <= 0) return;
    size_t pos = hitCount;
    if (hitCount < 12) ++hitCount;
    else {
      pos = 0;
      for (size_t i = 1; i < hitCount; ++i) if (hits[i].score < hits[pos].score) pos = i;
      if (score <= hits[pos].score) return;
    }
    hits[pos].score = score;
    hits[pos].json = json;
  };

  File f = _fs.open(factsPath(), FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (!line.length()) continue;
      JsonDocument doc;
      if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
      String text = String((const char*)(doc["fact"] | doc["text"] | ""));
      String observed = String((const char*)(doc["observed_start_ts"] | doc["ts"] | ""));
      String observedDate = observed.length() >= 10 ? observed.substring(0, 10) : "";
      if (dateFilter.length() && observedDate != dateFilter) continue;
      String tags;
      if (doc["tags"].is<JsonArray>()) {
        for (JsonVariant t : doc["tags"].as<JsonArray>()) { tags += String((const char*)t); tags += " "; }
      }
      String lower = text; lower.toLowerCase();
      String tagsLower = tags; tagsLower.toLowerCase();
      int score = scoreMemoryText(qLower, lower, tagsLower, observedDate);
      // Recency bias only after an actual lexical/tag/date match.
      if (score > 0) score += 2;
      String j = "{\"layer\":\"fact\",\"score\":" + String(score) +
                 ",\"observed_start_ts\":\"" + jsonEscape(observed) + "\"" +
                 ",\"observed_end_ts\":\"" + jsonEscape(String((const char*)(doc["observed_end_ts"] | ""))) + "\"" +
                 ",\"source_session\":\"" + jsonEscape(String((const char*)(doc["source_session"] | doc["session"] | ""))) + "\"" +
                 ",\"tags\":\"" + jsonEscape(tags) + "\"" +
                 ",\"text\":\"" + jsonEscape(text) + "\"}";
      addHit(score, j);
    }
    f.close();
  }

  f = _fs.open(summariesPath(), FILE_READ);
  if (f) {
    while (f.available()) {
      String line = f.readStringUntil('\n');
      line.trim();
      if (!line.length()) continue;
      JsonDocument doc;
      if (deserializeJson(doc, line) != DeserializationError::Ok) continue;
      String scope = String((const char*)(doc["scope"] | ""));
      if (scope != "semantic_summary_v2" && scope != "manual_active_context") continue;
      String observed = String((const char*)(doc["observed_start_ts"] | doc["ts"] | ""));
      String observedDate = observed.length() >= 10 ? observed.substring(0, 10) : "";
      if (dateFilter.length() && observedDate != dateFilter) continue;
      String text;
      if (doc["summary_json"].is<JsonObject>()) text = String((const char*)(doc["summary_json"]["summary"] | ""));
      if (!text.length()) text = String((const char*)(doc["summary"] | ""));
      String lower = text; lower.toLowerCase();
      int score = scoreMemoryText(qLower, lower, scope, observedDate);
      if (score > 0) score += 1;
      String j = "{\"layer\":\"summary\",\"score\":" + String(score) +
                 ",\"scope\":\"" + jsonEscape(scope) + "\"" +
                 ",\"observed_start_ts\":\"" + jsonEscape(observed) + "\"" +
                 ",\"observed_end_ts\":\"" + jsonEscape(String((const char*)(doc["observed_end_ts"] | ""))) + "\"" +
                 ",\"source_session\":\"" + jsonEscape(String((const char*)(doc["source_session"] | doc["session"] | ""))) + "\"" +
                 ",\"text\":\"" + jsonEscape(text) + "\"}";
      addHit(score, j);
    }
    f.close();
  }

  // Emit hits sorted by score descending.
  for (size_t emitted = 0; emitted < maxResults && emitted < hitCount; ++emitted) {
    int best = -1;
    for (size_t i = 0; i < hitCount; ++i) if (hits[i].score >= 0 && (best < 0 || hits[i].score > hits[best].score)) best = i;
    if (best < 0) break;
    JsonDocument h;
    if (deserializeJson(h, hits[best].json) == DeserializationError::Ok) results.add(h.as<JsonObject>());
    hits[best].score = -1;
  }

  out["result_count"] = results.size();
  String serialized;
  serializeJsonPretty(out, serialized);
  // Do not truncate serialized JSON here: cutting UTF-8/JSON mid-string makes the
  // HTTP endpoint invalid. Individual result snippets are bounded above instead.
  return serialized;
}

bool MemoryStore::commitSemanticSummary(const String& summaryJson, const String& model, String& message) {
  if (!_ready) {
    message = "memory store not ready";
    return false;
  }
  String clean = summaryJson;
  clean.trim();
  if (!clean.length()) {
    message = "summary json is empty";
    return false;
  }

  JsonDocument check;
  if (deserializeJson(check, clean) != DeserializationError::Ok) {
    message = "summary json is not valid JSON";
    return false;
  }
  String compact;
  serializeJson(check, compact);

  String active = buildActiveDialogueContext(20000);
  active.trim();
  if (!active.length()) {
    message = "active context is empty";
    return false;
  }

  String observedStart, observedEnd, sourceSession;
  activeDialogueBounds(observedStart, observedEnd, sourceSession);
  String summaryTs = nowIso();

  String record = "{";
  record += "\"ts\":\"" + summaryTs + "\",";
  record += "\"scope\":\"semantic_summary_v2\",";
  record += "\"session\":\"" + jsonEscape(_sessionId) + "\",";
  record += "\"source_session\":\"" + jsonEscape(sourceSession) + "\",";
  record += "\"observed_start_ts\":\"" + jsonEscape(observedStart) + "\",";
  record += "\"observed_end_ts\":\"" + jsonEscape(observedEnd) + "\",";
  record += "\"model\":\"" + jsonEscape(model) + "\",";
  record += "\"active_chars\":" + String(active.length()) + ",";
  record += "\"summary_json\":" + compact;
  record += "}";
  if (!appendJsonLine(summariesPath(), record)) {
    message = "failed to append semantic summary";
    return false;
  }

  JsonArray facts = check["facts"].as<JsonArray>();
  for (JsonVariant factVar : facts) {
    String factText;
    if (factVar.is<const char*>()) {
      factText = String((const char*)factVar);
    } else if (factVar.is<JsonObject>()) {
      JsonObject obj = factVar.as<JsonObject>();
      factText = String((const char*)(obj["text"] | ""));
    }
    factText.trim();
    if (factText.length()) appendSemanticFact(factVar, observedStart, observedEnd, sourceSession, summaryTs);
  }

  String cutoff = nowIso();
  if (!writeActiveAfterTs(cutoff)) {
    message = "summary saved, but failed to advance active context marker";
    return false;
  }
  message = "semantic summary saved chars=" + String(compact.length()) + ", active_after_ts=" + cutoff;
  return true;
}

String MemoryStore::recentDialoguesPreview(size_t maxChars) {
  if (!_ready) return "";
  if (maxChars == 0) maxChars = 6000;

  String raw = readTail(dialoguesPath(), maxChars);
  String out;
  String lastSession;
  String lastDate;
  out.reserve(raw.length() + 256);
  int start = 0;
  while (start < (int)raw.length()) {
    int end = raw.indexOf('\n', start);
    String line = (end < 0) ? raw.substring(start) : raw.substring(start, end);
    line.trim();
    if (line.length()) {
      String ts, session, role, text;
      if (readDialogueLine(line, ts, session, role, text)) {
        String date = ts.length() >= 10 ? ts.substring(0, 10) : _todayKey;
        String time = ts.length() >= 19 && ts.indexOf('T') == 10 ? ts.substring(11, 19) : ts;
        if (session != lastSession || date != lastDate) {
          if (out.length()) out += "\n";
          out += "[Session ";
          out += session.length() ? session : "unknown";
          out += ", date ";
          out += date;
          out += "]\n";
          lastSession = session;
          lastDate = date;
        }
        out += time;
        out += " ";
        out += role;
        out += ": ";
        out += text;
        out += "\n";
      } else {
        // Keep malformed tail lines visible for debugging instead of hiding data.
        out += line;
        out += "\n";
      }
    }
    if (end < 0) break;
    start = end + 1;
  }
  return out;
}

void MemoryStore::addStats(JsonObject obj) {
  obj["ready"] = _ready;
  obj["today"] = _todayKey;
  obj["session"] = _sessionId;
  obj["active_after_ts"] = activeAfterTs();
  obj["dialogues_bytes"] = fileSize(dialoguesPath());
  obj["dialogues_lines"] = countLines(dialoguesPath());
  obj["events_bytes"] = fileSize(eventsPath());
  obj["summaries_bytes"] = fileSize(summariesPath());
  obj["facts_bytes"] = fileSize(factsPath());
  obj["private_memory_bytes"] = fileSize(privateMemoryPath());
  obj["private_memory_lines"] = countLines(privateMemoryPath());
  obj["vector_index_bytes"] = fileSize(vectorIndexPath());
  String active = buildActiveDialogueContext(20000);
  obj["active_context_chars"] = active.length();
  obj["active_context_soft_limit_chars"] = 12000;
  obj["active_context_hard_limit_chars"] = 20000;
  obj["active_context_over_soft_limit"] = active.length() > 12000;
}

bool MemoryStore::summarizeActiveContext(String& message) {
  if (!_ready) {
    message = "memory store not ready";
    return false;
  }
  String active = buildActiveDialogueContext(20000);
  active.trim();
  if (!active.length()) {
    message = "active context is empty";
    return false;
  }

  // v1 safe manual fold: archive the exact active dialogue text as a summary
  // record, then advance active_after_ts. This does not yet call an external
  // LLM summarizer, so it is non-lossy and suitable for testing buffer rollover.
  String summary = "MANUAL_ACTIVE_CONTEXT_ARCHIVE chars=" + String(active.length()) + "\n" + active;
  if (!appendSummary(summary, "manual_active_context")) {
    message = "failed to append summary archive";
    return false;
  }
  String cutoff = nowIso();
  if (!writeActiveAfterTs(cutoff)) {
    message = "summary archived, but failed to advance active context marker";
    return false;
  }
  message = "archived active context chars=" + String(active.length()) + ", active_after_ts=" + cutoff;
  return true;
}

bool MemoryStore::queueVectorization(String& message) {
  if (!_ready) {
    message = "memory store not ready";
    return false;
  }
  String line = "{";
  line += "\"ts\":\"" + jsonEscape(nowIso()) + "\",";
  line += "\"status\":\"pending_external_embedding\",";
  line += "\"source\":\"summaries.jsonl\",";
  line += "\"model\":\"not_configured_yet\",";
  line += "\"dim\":0";
  line += "}";
  if (!appendJsonLine(vectorIndexPath(), line)) {
    message = "failed to append vectorization request";
    return false;
  }
  message = "queued vectorization marker in index/vector_index.jsonl";
  return true;
}

bool MemoryStore::compactDialoguesFile() {
  String path = dialoguesPath();
  if (!_fs.exists(path)) return true;

  File in = _fs.open(path, FILE_READ);
  if (!in) return false;
  String tmpPath = path + ".tmp";
  String bakPath = path + ".bak";
  File out = _fs.open(tmpPath, FILE_WRITE);
  if (!out) {
    in.close();
    return false;
  }

  String curTs;
  String curSession;
  String curRole;
  String curText;
  bool changed = false;
  bool hadRecord = false;
  bool ok = true;

  auto flushCurrent = [&]() {
    if (!curText.length()) return;
    out.println(dialogueJsonLine(curTs, curSession, curRole, curText));
    curText = "";
  };

  while (in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;

    JsonDocument doc;
    if (deserializeJson(doc, line) != DeserializationError::Ok) {
      flushCurrent();
      out.println(line);
      ok = false;
      continue;
    }

    String ts = String((const char*)(doc["ts"] | ""));
    String session = String((const char*)(doc["session"] | ""));
    String role = String((const char*)(doc["role"] | ""));
    String text = String((const char*)(doc["text"] | ""));
    text.trim();
    if (!role.length() || !text.length()) continue;
    if (!ts.length()) ts = nowIso();
    if (!session.length()) session = _sessionId;

    bool canMerge = hadRecord && role == curRole && session == curSession;
    if (!canMerge) {
      flushCurrent();
      curTs = ts;
      curSession = session;
      curRole = role;
      curText = text;
      hadRecord = true;
    } else {
      appendDialogueChunk(curText, text);
      changed = true;
    }

    // Old noisy records included source per tiny chunk. Even if no merge happens,
    // rewriting without source removes transport metadata from dialogue storage.
    if (!doc["source"].isNull()) changed = true;
  }
  flushCurrent();
  in.close();
  out.close();

  if (!ok || !changed) {
    _fs.remove(tmpPath);
    return ok;
  }

  _fs.remove(bakPath);
  _fs.rename(path, bakPath);
  if (!_fs.rename(tmpPath, path)) {
    _fs.rename(bakPath, path);
    _fs.remove(tmpPath);
    return false;
  }
  _fs.remove(bakPath);
  Serial.println("MemoryStore: compacted dialogues jsonl");
  return true;
}

String MemoryStore::dialogueJsonLine(const String& ts, const String& session, const String& role, const String& text) const {
  String line = "{";
  line += "\"ts\":\"" + jsonEscape(ts) + "\",";
  line += "\"session\":\"" + jsonEscape(session) + "\",";
  line += "\"role\":\"" + jsonEscape(role) + "\",";
  line += "\"text\":\"" + jsonEscape(text) + "\"";
  line += "}";
  return line;
}

void MemoryStore::appendDialogueChunk(String& buffer, const String& chunk) const {
  String part = chunk;
  part.trim();
  if (!part.length()) return;
  if (buffer.length()) {
    char last = buffer[buffer.length() - 1];
    char first = part[0];
    bool firstIsPunctuation = first == '.' || first == ',' || first == '!' || first == '?' ||
                              first == ':' || first == ';' || first == ')' || first == ']';
    bool lastIsSpaceOrOpen = last == ' ' || last == '\n' || last == '(' || last == '[';
    if (!firstIsPunctuation && !lastIsSpaceOrOpen) buffer += ' ';
  }
  buffer += part;
}

String MemoryStore::activeAfterTs() const {
  File f = _fs.open(contextStatePath(), FILE_READ);
  if (!f) return "";
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return "";
  return String((const char*)(doc["active_after_ts"] | ""));
}

bool MemoryStore::writeActiveAfterTs(const String& ts) {
  JsonDocument doc;
  doc["active_after_ts"] = ts;
  doc["updated_at"] = nowIso();
  String tmp = contextStatePath() + ".tmp";
  File f = _fs.open(tmp, FILE_WRITE);
  if (!f) return false;
  serializeJson(doc, f);
  f.println();
  f.close();
  _fs.remove(contextStatePath());
  return _fs.rename(tmp, contextStatePath());
}

bool MemoryStore::readDialogueLine(const String& line, String& ts, String& session, String& role, String& text) const {
  String trimmed = line;
  trimmed.trim();
  if (!trimmed.length()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, trimmed) != DeserializationError::Ok) return false;
  ts = String((const char*)(doc["ts"] | ""));
  session = String((const char*)(doc["session"] | ""));
  role = String((const char*)(doc["role"] | ""));
  text = String((const char*)(doc["text"] | ""));
  text.trim();
  return role.length() && text.length();
}

String MemoryStore::redactSensitiveForRuntime(const String& text) const {
  String lower = text;
  lower.toLowerCase();
  bool sensitive = lower.indexOf("pin") >= 0 || lower.indexOf("\xD0\xBF\xD0\xB8\xD0\xBD") >= 0 ||
                   lower.indexOf("password") >= 0 || lower.indexOf("\xD0\xBF\xD0\xB0\xD1\x80\xD0\xBE\xD0\xBB") >= 0 ||
                   lower.indexOf("token") >= 0 || lower.indexOf("secret") >= 0 || lower.indexOf("\xD1\x81\xD0\xB5\xD0\xBA\xD1\x80\xD0\xB5\xD1\x82") >= 0 ||
                   lower.indexOf("code") >= 0 || lower.indexOf("\xD0\xBA\xD0\xBE\xD0\xB4") >= 0 ||
                   lower.indexOf("address") >= 0 || lower.indexOf("\xD0\xB0\xD0\xB4\xD1\x80\xD0\xB5\xD1\x81") >= 0 ||
                   lower.indexOf("apartment") >= 0 || lower.indexOf("\xD0\xBA\xD0\xB2\xD0\xB0\xD1\x80\xD1\x82\xD0\xB8\xD1\x80") >= 0;
  if (!sensitive) return text;

  if (lower.indexOf("pin") >= 0 || lower.indexOf("\xD0\xBF\xD0\xB8\xD0\xBD") >= 0 ||
      lower.indexOf("password") >= 0 || lower.indexOf("\xD0\xBF\xD0\xB0\xD1\x80\xD0\xBE\xD0\xBB") >= 0 ||
      lower.indexOf("token") >= 0 || lower.indexOf("secret") >= 0 || lower.indexOf("\xD1\x81\xD0\xB5\xD0\xBA\xD1\x80\xD0\xB5\xD1\x82") >= 0 ||
      lower.indexOf("code") >= 0 || lower.indexOf("\xD0\xBA\xD0\xBE\xD0\xB4") >= 0) {
    return "[PRIVATE_CODE_REDACTED: user discussed a private code/PIN/password; do not repeat it aloud or expose it to tools]";
  }
  return "[PRIVATE_ADDRESS_REDACTED: user discussed a private address/location; do not repeat it aloud or expose it to tools]";
}

size_t MemoryStore::fileSize(const String& path) const {
  File f = _fs.open(path, FILE_READ);
  if (!f) return 0;
  size_t size = f.size();
  f.close();
  return size;
}

size_t MemoryStore::countLines(const String& path) const {
  File f = _fs.open(path, FILE_READ);
  if (!f) return 0;
  size_t lines = 0;
  while (f.available()) {
    f.readStringUntil('\n');
    ++lines;
  }
  f.close();
  return lines;
}

bool MemoryStore::ensureDir(const String& path) {
  if (_fs.exists(path)) return true;

  String partial;
  int start = (path[0] == '/') ? 1 : 0;
  if (start == 1) partial = "/";

  while (start < (int)path.length()) {
    int slash = path.indexOf('/', start);
    String part = (slash < 0) ? path.substring(start) : path.substring(start, slash);
    if (part.length() > 0) {
      if (partial.length() > 1 && !partial.endsWith("/")) partial += "/";
      partial += part;
      if (!_fs.exists(partial) && !_fs.mkdir(partial)) {
        return false;
      }
    }
    if (slash < 0) break;
    start = slash + 1;
  }
  return true;
}

bool MemoryStore::appendJsonLine(const String& path, const String& line) {
  File f = _fs.open(path, FILE_APPEND);
  if (!f) return false;
  f.println(line);
  f.close();
  return true;
}

bool MemoryStore::cleanupScaffoldMemoryRecords() {
  bool ok = true;
  ok = pruneJsonlFieldEquals(summariesPath(), "scope", "boot") && ok;
  ok = pruneJsonlFieldEquals(factsPath(), "source", "firmware_boot") && ok;
  return ok;
}

bool MemoryStore::pruneJsonlFieldEquals(const String& path, const char* field, const char* value) {
  if (!_fs.exists(path)) return true;

  File in = _fs.open(path, FILE_READ);
  if (!in) return false;

  String tmp = path + ".tmp";
  String bak = path + ".bak";
  _fs.remove(tmp);
  _fs.remove(bak);
  File out = _fs.open(tmp, FILE_WRITE);
  if (!out) {
    in.close();
    return false;
  }

  size_t removed = 0;
  while (in.available()) {
    String line = in.readStringUntil('\n');
    String trimmed = line;
    trimmed.trim();
    bool drop = false;
    if (trimmed.length()) {
      JsonDocument doc;
      if (deserializeJson(doc, trimmed) == DeserializationError::Ok) {
        String current = String((const char*)(doc[field] | ""));
        drop = (current == value);
      }
    }
    if (drop) {
      ++removed;
    } else {
      out.println(line);
    }
  }
  in.close();
  out.close();

  if (removed == 0) {
    _fs.remove(tmp);
    return true;
  }

  if (!_fs.rename(path, bak)) {
    _fs.remove(tmp);
    return false;
  }
  if (!_fs.rename(tmp, path)) {
    _fs.rename(bak, path);
    _fs.remove(tmp);
    return false;
  }
  _fs.remove(bak);
  return true;
}

String MemoryStore::eventsPath() const {
  return _root + "/events/" + _todayKey + ".jsonl";
}

String MemoryStore::dialoguesPath() const {
  return _root + "/dialogues/" + _todayKey + ".jsonl";
}

String MemoryStore::factsPath() const {
  return _root + "/facts.jsonl";
}

String MemoryStore::summariesPath() const {
  return _root + "/summaries.jsonl";
}

String MemoryStore::contextStatePath() const {
  return _root + "/context_state.json";
}

String MemoryStore::vectorIndexPath() const {
  return _root + "/index/vector_index.jsonl";
}

String MemoryStore::privateMemoryPath() const {
  return _root + "/private_memory.jsonl";
}

String MemoryStore::nowIso() const {
  // UTC, and said so with the Z. These used to be local time with nothing to
  // say which zone, so changing the timezone setting or crossing a DST
  // boundary left old records that could not be compared against new ones.
  // Filenames stay on local dates, because "today" has to mean the user's
  // today and resolve to one file.
  const time_t now = time(nullptr);
  if (now > kClockSanityEpoch) {
    struct tm utc;
    gmtime_r(&now, &utc);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return String(buf);
  }
  return String("millis:") + String((uint32_t)millis());
}

String MemoryStore::jsonEscape(const String& value) const {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value[i];
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if ((uint8_t)c < 0x20) out += ' ';
        else out += c;
        break;
    }
  }
  return out;
}

String MemoryStore::readTail(const String& path, size_t maxChars) {
  File f = _fs.open(path, FILE_READ);
  if (!f) return "";
  size_t size = f.size();
  size_t start = (size > maxChars) ? (size - maxChars) : 0;
  f.seek(start);
  if (start > 0) {
    // Drop partial first line.
    f.readStringUntil('\n');
  }
  String out = f.readString();
  f.close();
  return out;
}
