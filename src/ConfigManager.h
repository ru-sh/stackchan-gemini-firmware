#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
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
    // Primary network, kept for configs written before multi-network support.
    String wifiSsid;
    // Every network the robot may join, strongest-first at connect time. The
    // legacy wifiSsid is folded in as the first entry when it is not repeated.
    std::vector<String> wifiSsids;
    // A candidate must beat the current network by this many dB before the
    // robot moves. Without a margin two overlapping routers trade the
    // connection back and forth every time the signal wavers.
    uint8_t wifiRoamMarginDb = 8;
    uint8_t speakerVolume = 200;
    uint8_t micMagnification = 16;
    uint8_t micNoiseFilterLevel = 1;
    uint16_t vadPrefixPaddingMs = 800;
    uint16_t vadSilenceDurationMs = 900;
    bool vadStartSensitivityHigh = false;
    bool vadEndSensitivityLow = true;
    bool vadTurnIncludesAllInput = false;
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
  bool hasWifiPasswordFor(const String& ssid) const;
  String readGeminiApiKey() const;
  String readGatewayToken() const;
  String readWifiPassword() const;
  // Password for one network: the per-network secrets file first, then the
  // single-network file when the ssid is the legacy primary one.
  String readWifiPasswordFor(const String& ssid) const;
  // Networks that have both an ssid and a usable password.
  std::vector<String> configuredWifiSsids() const;

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
  bool loadWifiNetworks(JsonVariantConst networks);
  bool fileExists(const char* path) const;
  static bool asBool(JsonVariantConst v, bool fallback);
};
