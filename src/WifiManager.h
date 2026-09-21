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
// Scanning is done ahead of time, never in the wake path. A scan takes around
// two seconds and briefly interrupts traffic, so it runs at boot and then only
// when something has actually changed: the robot is carried somewhere else, or
// the link drops. The robot is never moved during a conversation, so its
// position when a conversation ends is the position it was already scanned in
// and there is nothing to re-measure. Waking therefore costs at most a direct
// join to the remembered network, and a full scan only happens when that join
// fails.
//
// A candidate has to beat the current network by a margin before the robot
// moves, because two overlapping routers whose signals cross will otherwise
// trade the connection back and forth on ordinary signal jitter.
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
  // be noticeable, which for this firmware means any live Gemini session,
  // including the connect phase before it is fully up.
  void loop(bool busy);

  // Call on wake, before starting a session. Returns with a link up if it can.
  // Costs nothing when already connected, which is the normal case; otherwise
  // it joins the network last known to be the strongest and only falls back to
  // a full scan when that fails.
  bool ensureLinkForSession();

  // Ask for a scan at the next idle moment, e.g. after the robot was carried
  // to another room. `reason` is logged so the scan is never unexplained.
  void requestRescan(const char* reason);

  bool isConnected() const;
  const String& currentSsid() const { return current_ssid_; }
  int32_t currentRssi() const;
  uint32_t roamCount() const { return roam_count_; }
  uint32_t reconnectCount() const { return reconnect_count_; }

  // How long a link must be down before reconnecting, so a brief blip during
  // normal operation does not trigger a scan.
  static constexpr uint32_t DISCONNECT_GRACE_MS = 5000;
  // Safety net for what movement cannot explain: an access point rebooting,
  // a new one appearing, or an IMU that never reported the carry. Long,
  // because being carried is what normally triggers a rescan.
  static constexpr uint32_t IDLE_RESCAN_INTERVAL_MS = 1800000;
  // Stillness required before a due scan actually runs, so a scan cannot block
  // the loop while the audio and display work of a just-ended turn finishes.
  static constexpr uint32_t SCAN_SETTLE_MS = 3000;
  static constexpr uint32_t CONNECT_TIMEOUT_MS = 15000;

 private:
  ConfigManager* config_ = nullptr;
  String current_ssid_;
  // Strongest network seen by the most recent scan, remembered so that waking
  // can skip straight to joining it.
  String best_ssid_;
  int32_t best_rssi_ = -127;
  // When a scan last actually ran. Only ever written where one did, so the
  // backstop below measures what its name says.
  uint32_t last_scan_ms_ = 0;
  // When the robot last became idle. Separate from last_scan_ms_ because it
  // answers a different question: not "is a scan due" but "is now a good
  // moment to run one".
  uint32_t idle_since_ms_ = 0;
  uint32_t disconnected_since_ms_ = 0;
  bool was_busy_ = false;
  bool scan_pending_ = false;
  // Modem sleep costs most of the idle radio draw, but parks the radio between
  // beacons and so adds delivery jitter that a live audio session cannot
  // absorb. Tracked rather than set every loop, because each change is a call
  // into the driver.
  bool power_save_ = false;
  uint32_t roam_count_ = 0;
  uint32_t reconnect_count_ = 0;

  // Strongest visible network that is both configured and has a password,
  // or an empty ssid when none is in range.
  Candidate scanForBest(int32_t* currentRssiOut) const;
  // Scans, records the winner in best_ssid_, and roams to it when it clearly
  // beats the current network.
  void refreshBestNetwork();
  // Scanning can only change anything when there is somewhere else to go.
  bool scanningUseful() const;
  void setPowerSave(bool enabled);
  bool joinNetwork(const String& ssid);
  void applyDnsOverride() const;
};
