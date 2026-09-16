#pragma once

#include <Arduino.h>
#include <vector>

class ConfigManager;

// Chooses which of the configured networks the robot is on.
//
// The firmware previously connected once at boot to a single ssid and never
// looked again, so a network going weak was permanent until a reboot and a
// dropped link was never re-established. This picks the strongest configured
// network at connect time, reconnects when the link drops, and may move to a
// better network later.
//
// Two rules keep roaming from costing more than it gains. A scan takes around
// two seconds and briefly interrupts traffic, so it only runs while no Gemini
// session is active: a conversation is never cut short to look for a better
// router. And a candidate has to beat the current network by a margin before
// the robot moves, because two overlapping routers whose signals cross will
// otherwise trade the connection back and forth on ordinary signal jitter.
class WifiManager {
 public:
  struct Candidate {
    String ssid;
    int32_t rssi = -127;
  };

  bool begin(ConfigManager* config);

  // Scans and joins the strongest configured network. Blocking, boot-time.
  bool connectBest();

  // Call every loop. `busy` must be true whenever interrupting the link would
  // be noticeable, which for this firmware means any live Gemini session.
  void loop(bool busy);

  bool isConnected() const;
  const String& currentSsid() const { return current_ssid_; }
  int32_t currentRssi() const;
  uint32_t roamCount() const { return roam_count_; }
  uint32_t reconnectCount() const { return reconnect_count_; }

  // How long a link must be down before reconnecting, so a brief blip during
  // normal operation does not trigger a scan.
  static constexpr uint32_t DISCONNECT_GRACE_MS = 5000;
  // Gap between roam checks. Also bounds how often a switch can happen, since
  // the margin alone does not rate-limit anything.
  static constexpr uint32_t ROAM_CHECK_INTERVAL_MS = 120000;
  static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

 private:
  ConfigManager* config_ = nullptr;
  String current_ssid_;
  uint32_t last_roam_check_ms_ = 0;
  uint32_t disconnected_since_ms_ = 0;
  uint32_t roam_count_ = 0;
  uint32_t reconnect_count_ = 0;

  // Strongest visible network that is both configured and has a password,
  // or an empty ssid when none is in range.
  Candidate scanForBest(int32_t* currentRssiOut) const;
  bool joinNetwork(const String& ssid);
  void applyDnsOverride() const;
};
