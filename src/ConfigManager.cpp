#include "ConfigManager.h"

namespace {
constexpr const char* kConfigDir = "/app/StackChan/config";
constexpr const char* kPromptDir = "/app/StackChan/prompts";
constexpr const char* kSecretDir = "/app/StackChan/secrets";
constexpr const char* kRuntimeConfigPath = "/app/StackChan/config/runtime.json";
constexpr const char* kGatewayConfigPath = "/app/StackChan/config/gateway.json";
constexpr const char* kSystemPromptPath = "/app/StackChan/prompts/system.txt";
constexpr const char* kPersonaPromptPath = "/app/StackChan/prompts/persona.txt";
constexpr const char* kGeminiKeyPath = "/app/StackChan/secrets/gemini_api_key.txt";
constexpr const char* kGatewayTokenPath = "/app/StackChan/secrets/gateway_token.txt";
constexpr const char* kWifiPasswordPath = "/app/StackChan/secrets/wifi_password.txt";
// Per-network passwords, {"ssid": "password"}. The single-network file above is
// still honoured for the primary ssid so existing cards keep working untouched.
constexpr const char* kWifiNetworksSecretPath = "/app/StackChan/secrets/wifi_networks.json";
constexpr size_t kMaxWifiNetworks = 8;
}

ConfigManager::ConfigManager(fs::FS& fs) : fs_(fs) {}

bool ConfigManager::begin() {
  ready_ = ensureDirs();
  if (!ready_) return false;
  return load();
}

bool ConfigManager::load() {
  RuntimeConfig defaults;
  config_ = defaults;
  if (!loadJsonConfig(kRuntimeConfigPath)) {
    // runtime.json is optional; defaults are safe and offline.
  }
  if (!loadJsonConfig(kGatewayConfigPath)) {
    // gateway.json is optional; web UI may create it later.
  }
  loadPrompts();
  loadWifiNetworks();
  ready_ = true;
  return true;
}

bool ConfigManager::ensureDirs() {
  fs_.mkdir("/app");
  fs_.mkdir("/app/StackChan");
  fs_.mkdir(kConfigDir);
  fs_.mkdir(kPromptDir);
  fs_.mkdir(kSecretDir);
  return true;
}

bool ConfigManager::loadJsonConfig(const char* path) {
  String raw = readTextFile(path, 4096);
  if (!raw.length()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, raw)) return false;

  config_.robotId = doc["robot_id"] | config_.robotId;
  config_.geminiModel =
      GeminiLiveProbe::upgradeLegacyModel(doc["gemini_model"] | config_.geminiModel);
  config_.geminiVoice =
      GeminiLiveProbe::supportedVoice(doc["gemini_voice"] | config_.geminiVoice);
  config_.geminiSearchGrounding = doc["gemini_search_grounding"] | config_.geminiSearchGrounding;
  config_.gatewayBaseUrl = doc["gateway_base_url"] | config_.gatewayBaseUrl;
  config_.wifiSsid = doc["wifi_ssid"] | config_.wifiSsid;
  int roamMargin = doc["wifi_roam_margin_db"] | config_.wifiRoamMarginDb;
  // Below ~3 dB the margin is inside normal signal jitter and stops preventing
  // flapping; above ~30 dB nothing would ever clear it.
  if (roamMargin < 3) roamMargin = 3;
  if (roamMargin > 30) roamMargin = 30;
  config_.wifiRoamMarginDb = static_cast<uint8_t>(roamMargin);
  int vol = doc["speaker_volume"] | config_.speakerVolume;
  if (vol < 0) vol = 0;
  if (vol > 255) vol = 255;
  config_.speakerVolume = static_cast<uint8_t>(vol);

  int micMag = doc["mic_magnification"] | config_.micMagnification;
  if (micMag < 1) micMag = 1;
  // Keep the runtime knob conservative: close-range tests already hit int16 peak.
  if (micMag > 24) micMag = 24;
  config_.micMagnification = static_cast<uint8_t>(micMag);

  int micNoise = doc["mic_noise_filter_level"] | config_.micNoiseFilterLevel;
  if (micNoise < 0) micNoise = 0;
  if (micNoise > 4) micNoise = 4;
  config_.micNoiseFilterLevel = static_cast<uint8_t>(micNoise);

  int vadPrefix = doc["vad_prefix_padding_ms"] | config_.vadPrefixPaddingMs;
  if (vadPrefix < 0) vadPrefix = 0;
  if (vadPrefix > 2000) vadPrefix = 2000;
  config_.vadPrefixPaddingMs = static_cast<uint16_t>(vadPrefix);

  int vadSilence = doc["vad_silence_duration_ms"] | config_.vadSilenceDurationMs;
  if (vadSilence < 100) vadSilence = 100;
  if (vadSilence > 3000) vadSilence = 3000;
  config_.vadSilenceDurationMs = static_cast<uint16_t>(vadSilence);

  config_.vadStartSensitivityHigh = asBool(doc["vad_start_sensitivity_high"], config_.vadStartSensitivityHigh);
  config_.vadEndSensitivityLow = asBool(doc["vad_end_sensitivity_low"], config_.vadEndSensitivityLow);
  config_.vadTurnIncludesAllInput = asBool(doc["vad_turn_includes_all_input"], config_.vadTurnIncludesAllInput);

  config_.wifiEnabled = asBool(doc["wifi_enabled"], config_.wifiEnabled);
  config_.geminiEnabled = asBool(doc["gemini_enabled"], config_.geminiEnabled);
  config_.gatewayEnabled = asBool(doc["gateway_enabled"], config_.gatewayEnabled);
  config_.webEnabled = asBool(doc["web_enabled"], config_.webEnabled);
  return true;
}

void ConfigManager::loadPrompts() {
  config_.systemPrompt = readTextFile(kSystemPromptPath, 4096);
  config_.personaPrompt = readTextFile(kPersonaPromptPath, 4096);
}

bool ConfigManager::hasGeminiApiKey() const { return fileExists(kGeminiKeyPath); }
bool ConfigManager::hasGatewayToken() const { return fileExists(kGatewayTokenPath); }
bool ConfigManager::hasWifiPassword() const { return fileExists(kWifiPasswordPath); }

String ConfigManager::readGeminiApiKey() const { return readTextFile(kGeminiKeyPath, 4096); }
String ConfigManager::readGatewayToken() const { return readTextFile(kGatewayTokenPath, 2048); }
String ConfigManager::readWifiPassword() const { return readTextFile(kWifiPasswordPath, 2048); }

bool ConfigManager::loadWifiNetworks() {
  config_.wifiSsids.clear();
  // The primary ssid from runtime.json stays first, so a card written before
  // multi-network support keeps its exact behaviour with no secrets file.
  if (config_.wifiSsid.length()) config_.wifiSsids.push_back(config_.wifiSsid);

  String raw = readTextFile(kWifiNetworksSecretPath, 4096);
  if (!raw.length()) return !config_.wifiSsids.empty();
  JsonDocument doc;
  if (deserializeJson(doc, raw) != DeserializationError::Ok) {
    Serial.println("WiFi: wifi_networks.json is not valid JSON; ignoring it");
    return !config_.wifiSsids.empty();
  }
  if (!doc.is<JsonArrayConst>()) {
    Serial.println("WiFi: wifi_networks.json must be an array of {ssid, password}");
    return !config_.wifiSsids.empty();
  }

  for (JsonVariantConst entry : doc.as<JsonArrayConst>()) {
    if (config_.wifiSsids.size() >= kMaxWifiNetworks) break;
    String ssid = entry["ssid"] | "";
    ssid.trim();
    if (!ssid.length()) continue;
    bool duplicate = false;
    for (const String& seen : config_.wifiSsids) {
      if (seen == ssid) { duplicate = true; break; }
    }
    if (!duplicate) config_.wifiSsids.push_back(ssid);
  }
  return !config_.wifiSsids.empty();
}

String ConfigManager::readWifiPasswordFor(const String& ssid) const {
  if (!ssid.length()) return String();
  String raw = readTextFile(kWifiNetworksSecretPath, 4096);
  if (raw.length()) {
    JsonDocument doc;
    if (deserializeJson(doc, raw) == DeserializationError::Ok && doc.is<JsonArrayConst>()) {
      for (JsonVariantConst entry : doc.as<JsonArrayConst>()) {
        const char* entrySsid = entry["ssid"] | "";
        if (!entrySsid || ssid != entrySsid) continue;
        const char* password = entry["password"] | "";
        if (password && password[0]) return String(password);
        // Named with an empty password: deliberately unusable, not a lookup miss.
        return String();
      }
    }
  }
  // wifi_password.txt is the single-network format from before this file
  // existed, and only ever belonged to the primary ssid.
  if (ssid == config_.wifiSsid) return readWifiPassword();
  return String();
}

bool ConfigManager::hasWifiPasswordFor(const String& ssid) const {
  return readWifiPasswordFor(ssid).length() > 0;
}

std::vector<String> ConfigManager::configuredWifiSsids() const {
  std::vector<String> usable;
  for (const String& ssid : config_.wifiSsids) {
    if (hasWifiPasswordFor(ssid)) usable.push_back(ssid);
  }
  return usable;
}

ToolGatewayClient::Config ConfigManager::gatewayConfig() const {
  ToolGatewayClient::Config cfg;
  cfg.enabled = config_.gatewayEnabled;
  cfg.baseUrl = config_.gatewayBaseUrl;
  cfg.robotId = config_.robotId;
  cfg.timeoutMs = 8000;
  cfg.maxResponseBytes = 8192;
  return cfg;
}

WebConfigServer::Config ConfigManager::webConfig() const {
  WebConfigServer::Config cfg;
  cfg.enabled = config_.webEnabled;
  cfg.port = 80;
  cfg.robotId = config_.robotId.c_str();
  return cfg;
}

String ConfigManager::redactedStatusJson() const {
  JsonDocument doc;
  doc["ready"] = ready_;
  doc["robot_id"] = config_.robotId;
  doc["wifi_enabled"] = config_.wifiEnabled;
  doc["wifi_ssid"] = config_.wifiSsid.length() ? "set" : "missing";
  doc["wifi_password"] = hasWifiPassword() ? "set" : "missing";
  doc["wifi_networks"] = config_.wifiSsids.size();
  doc["wifi_networks_usable"] = configuredWifiSsids().size();
  doc["wifi_roam_margin_db"] = config_.wifiRoamMarginDb;
  doc["gemini_enabled"] = config_.geminiEnabled;
  doc["gemini_model"] = config_.geminiModel;
  doc["gemini_voice"] = config_.geminiVoice;
  doc["gemini_search_grounding"] = config_.geminiSearchGrounding;
  doc["gemini_api_key"] = hasGeminiApiKey() ? "set" : "missing";
  doc["gateway_enabled"] = config_.gatewayEnabled;
  doc["gateway_base_url"] = config_.gatewayBaseUrl;
  doc["gateway_token"] = hasGatewayToken() ? "set" : "missing";
  doc["web_enabled"] = config_.webEnabled;
  doc["speaker_volume"] = config_.speakerVolume;
  doc["mic_magnification"] = config_.micMagnification;
  doc["mic_noise_filter_level"] = config_.micNoiseFilterLevel;
  doc["vad_prefix_padding_ms"] = config_.vadPrefixPaddingMs;
  doc["vad_silence_duration_ms"] = config_.vadSilenceDurationMs;
  doc["vad_start_sensitivity_high"] = config_.vadStartSensitivityHigh;
  doc["vad_end_sensitivity_low"] = config_.vadEndSensitivityLow;
  doc["vad_turn_includes_all_input"] = config_.vadTurnIncludesAllInput;
  doc["system_prompt"] = config_.systemPrompt.length() ? "set" : "missing";
  doc["persona_prompt"] = config_.personaPrompt.length() ? "set" : "missing";
  String out;
  serializeJson(doc, out);
  return out;
}

void ConfigManager::printRedactedStatus(Stream& out) const {
  out.print("ConfigManager status: ");
  out.println(redactedStatusJson());
}

String ConfigManager::readTextFile(const char* path, size_t maxBytes) const {
  if (!fs_.exists(path)) return String();
  File f = fs_.open(path, FILE_READ);
  if (!f) return String();
  String out;
  while (f.available() && out.length() < maxBytes) {
    out += static_cast<char>(f.read());
  }
  f.close();
  out.trim();
  return out;
}

bool ConfigManager::fileExists(const char* path) const {
  return fs_.exists(path);
}

bool ConfigManager::asBool(JsonVariantConst v, bool fallback) {
  if (v.isNull()) return fallback;
  if (v.is<bool>()) return v.as<bool>();
  if (v.is<int>()) return v.as<int>() != 0;
  if (v.is<const char*>()) {
    String s = v.as<const char*>();
    s.toLowerCase();
    return s == "1" || s == "true" || s == "yes" || s == "on" || s == "enabled";
  }
  return fallback;
}
