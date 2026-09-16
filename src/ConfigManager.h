#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>
#include "ToolGatewayClient.h"
#include "WebConfigServer.h"
#include "GeminiLiveProbe.h"

// Loads StackChan option-B runtime configuration from SD without printing secrets.
// Secret values can be read by callers that need them, but status/log helpers expose only set/missing.
class ConfigManager {
 public:
  struct RuntimeConfig {
    String robotId = "stackchan";
    String geminiModel = GeminiLiveProbe::kDefaultModel;
    String geminiVoice = GeminiLiveProbe::kDefaultVoice;
    bool geminiSearchGrounding = true;
    bool wifiEnabled = false;
    bool geminiEnabled = false;
    bool gatewayEnabled = false;
    bool webEnabled = false;
    String gatewayBaseUrl;
    String wifiSsid;
    uint8_t speakerVolume = 200;
    uint8_t micMagnification = 16;
    uint8_t micNoiseFilterLevel = 0;
    uint16_t vadPrefixPaddingMs = 800;
    uint16_t vadSilenceDurationMs = 900;
    bool vadStartSensitivityHigh = true;
    bool vadEndSensitivityLow = true;
    bool vadTurnIncludesAllInput = true;
    String systemPrompt;
    String personaPrompt;
  };

  explicit ConfigManager(fs::FS& fs);

  bool begin();
  bool load();

  const RuntimeConfig& config() const { return config_; }
  bool ready() const { return ready_; }

  bool hasGeminiApiKey() const;
  bool hasGatewayToken() const;
  bool hasWifiPassword() const;
  String readGeminiApiKey() const;
  String readGatewayToken() const;
  String readWifiPassword() const;

  ToolGatewayClient::Config gatewayConfig() const;
  WebConfigServer::Config webConfig() const;

  String redactedStatusJson() const;
  void printRedactedStatus(Stream& out) const;

 private:
  fs::FS& fs_;
  RuntimeConfig config_;
  bool ready_ = false;

  bool ensureDirs();
  bool loadJsonConfig(const char* path);
  void loadPrompts();
  String readTextFile(const char* path, size_t maxBytes = 4096) const;
  bool fileExists(const char* path) const;
  static bool asBool(JsonVariantConst v, bool fallback);
};
