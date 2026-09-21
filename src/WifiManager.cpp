#include "WifiManager.h"

#include <WiFi.h>
#include <M5StackChan.h>

#include "ConfigManager.h"

bool WifiManager::begin(ConfigManager* config) {
  config_ = config;
  return config_ != nullptr;
}

bool WifiManager::isConnected() const { return WiFi.status() == WL_CONNECTED; }

int32_t WifiManager::currentRssi() const { return isConnected() ? WiFi.RSSI() : -127; }

void WifiManager::applyDnsOverride() const {
  // DHCP on some routers leaves DNS unusable even though the ESP is reachable
  // on the LAN, and Gemini Live then fails at hostByName() before any audio.
  // Keep DHCP addressing but pin reliable resolvers for outbound connections.
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
              IPAddress(1, 1, 1, 1), IPAddress(8, 8, 8, 8));
}

WifiManager::Candidate WifiManager::scanForBest(int32_t* currentRssiOut) const {
  Candidate best;
  if (currentRssiOut) *currentRssiOut = -127;
  if (!config_) return best;

  std::vector<String> configured = config_->configuredWifiSsids();
  if (configured.empty()) return best;

  const int found = WiFi.scanNetworks(false, false);
  if (found <= 0) {
    WiFi.scanDelete();
    return best;
  }

  for (int i = 0; i < found; ++i) {
    const String ssid = WiFi.SSID(i);
    const int32_t rssi = WiFi.RSSI(i);
    // A network can appear several times when more than one access point
    // serves it; the strongest sighting is the one that matters.
    if (ssid == current_ssid_ && currentRssiOut && rssi > *currentRssiOut) {
      *currentRssiOut = rssi;
    }
    bool known = false;
    for (const String& candidate : configured) {
      if (candidate == ssid) { known = true; break; }
    }
    if (!known) continue;
    if (rssi > best.rssi) {
      best.ssid = ssid;
      best.rssi = rssi;
    }
  }
  WiFi.scanDelete();
  return best;
}

bool WifiManager::joinNetwork(const String& ssid) {
  if (!config_ || !ssid.length()) return false;
  const String password = config_->readWifiPasswordFor(ssid);
  if (!password.length()) {
    // Never print the ssid's password, and do not imply one was tried.
    Serial.println("WiFi: no password for selected network");
    return false;
  }

  WiFi.mode(WIFI_STA);
  // Joining and the session that follows both want the radio fully awake.
  power_save_ = false;
  WiFi.setSleep(false);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE,
              IPAddress(1, 1, 1, 1), IPAddress(8, 8, 8, 8));
  WiFi.begin(ssid.c_str(), password.c_str());

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < CONNECT_TIMEOUT_MS) {
    delay(250);
    M5StackChan.update();
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("WiFi: connect timeout ssid=%s\n", ssid.c_str());
    return false;
  }

  applyDnsOverride();
  current_ssid_ = ssid;
  disconnected_since_ms_ = 0;
  // Whatever we just joined is, as far as we know, the one to go back to.
  best_ssid_ = ssid;
  best_rssi_ = WiFi.RSSI();
  Serial.printf("WiFi: connected ssid=%s rssi=%d ip=%s dns=%s\n", ssid.c_str(),
                static_cast<int>(WiFi.RSSI()), WiFi.localIP().toString().c_str(),
                WiFi.dnsIP().toString().c_str());
  return true;
}

bool WifiManager::connectBest() {
  if (!config_) return false;
  const auto& c = config_->config();
  if (!c.wifiEnabled) {
    Serial.println("WiFi: disabled");
    return false;
  }

  std::vector<String> configured = config_->configuredWifiSsids();
  if (configured.empty()) {
    Serial.println("WiFi: missing ssid or password");
    return false;
  }
  Serial.printf("WiFi: %u configured network(s); scanning\n",
                static_cast<unsigned>(configured.size()));

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  Candidate best = scanForBest(nullptr);
  last_scan_ms_ = millis();
  scan_pending_ = false;
  if (best.ssid.length()) {
    best_ssid_ = best.ssid;
    best_rssi_ = best.rssi;
    Serial.printf("WiFi: strongest configured network rssi=%d\n", static_cast<int>(best.rssi));
    if (joinNetwork(best.ssid)) return true;
  } else {
    Serial.println("WiFi: no configured network in range");
  }

  // The scan can miss a hidden or briefly absent network, so fall back to
  // trying each configured network in order rather than giving up on a scan.
  for (const String& ssid : configured) {
    if (ssid == best.ssid) continue;
    if (joinNetwork(ssid)) return true;
  }

  Serial.println("WiFi: no network joined");
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  return false;
}

void WifiManager::loop(bool busy) {
  if (!config_ || !config_->config().wifiEnabled) return;

  if (!isConnected()) {
    const uint32_t now = millis();
    if (!disconnected_since_ms_) {
      disconnected_since_ms_ = now;
      return;
    }
    // Reconnect even while busy: without a link there is nothing to protect,
    // and the session is already broken.
    if (now - disconnected_since_ms_ < DISCONNECT_GRACE_MS) return;
    Serial.println("WiFi: link down; reconnecting");
    ++reconnect_count_;
    disconnected_since_ms_ = 0;
    connectBest();
    return;
  }

  disconnected_since_ms_ = 0;
  const uint32_t now = millis();

  // Scanning is never worth interrupting a conversation for, and the whole
  // point of scanning early is to keep it out of the wake path.
  if (busy) {
    was_busy_ = true;
    idle_since_ms_ = 0;
    setPowerSave(false);
    return;
  }

  if (was_busy_) {
    // The robot cannot have moved during the conversation, so there is no
    // scan to do here; only the radio to let idle down again.
    was_busy_ = false;
  }
  if (!idle_since_ms_) idle_since_ms_ = now;
  setPowerSave(true);

  if (!scan_pending_ && now - last_scan_ms_ >= IDLE_RESCAN_INTERVAL_MS &&
      scanningUseful()) {
    Serial.println("WiFi: periodic rescan");
    scan_pending_ = true;
  }
  if (!scan_pending_) return;
  if (!scanningUseful()) {
    scan_pending_ = false;
    return;
  }
  // A scan that is due right as a conversation ends waits for the tail of it
  // rather than being pushed out to the next interval.
  if (now - idle_since_ms_ < SCAN_SETTLE_MS) return;
  scan_pending_ = false;

  refreshBestNetwork();
}

bool WifiManager::scanningUseful() const {
  return config_ && config_->configuredWifiSsids().size() >= 2;
}

void WifiManager::requestRescan(const char* reason) {
  if (!config_ || !config_->config().wifiEnabled) return;
  Serial.printf("WiFi: rescan requested (%s)\n", reason ? reason : "unspecified");
  scan_pending_ = true;
}

void WifiManager::setPowerSave(bool enabled) {
  if (power_save_ == enabled) return;
  power_save_ = enabled;
  // Idle is nearly all of the robot's uptime, and a listening radio is the
  // largest draw in it; waking from modem sleep costs far less than the
  // handshake that follows a wake, so it is not felt at the start of a
  // conversation.
  WiFi.setSleep(enabled);
  Serial.printf("WiFi: modem sleep %s\n", enabled ? "on" : "off");
}

void WifiManager::refreshBestNetwork() {
  // Start from the live link reading; the scan replaces it if it sees the
  // current network more strongly through another access point.
  int32_t currentRssi = WiFi.RSSI();
  Candidate best = scanForBest(&currentRssi);
  last_scan_ms_ = millis();
  if (!best.ssid.length()) {
    Serial.println("WiFi: rescan found no configured network; keeping current");
    return;
  }

  best_ssid_ = best.ssid;
  best_rssi_ = best.rssi;

  if (best.ssid == current_ssid_) {
    Serial.printf("WiFi: rescan; staying on current network rssi=%d, no better one in range\n",
                  static_cast<int>(currentRssi));
    return;
  }

  const int32_t margin = static_cast<int32_t>(config_->config().wifiRoamMarginDb);
  if (best.rssi < currentRssi + margin) {
    Serial.printf("WiFi: staying put; best alternative %d vs current %d, margin %d\n",
                  static_cast<int>(best.rssi), static_cast<int>(currentRssi),
                  static_cast<int>(margin));
    // The alternative is visible but not worth moving to, so the network to
    // rejoin on a dropped link is still the one we are on.
    best_ssid_ = current_ssid_;
    best_rssi_ = currentRssi;
    return;
  }

  Serial.printf("WiFi: roaming; alternative %d beats current %d by at least %d\n",
                static_cast<int>(best.rssi), static_cast<int>(currentRssi),
                static_cast<int>(margin));
  const String previous = current_ssid_;
  if (joinNetwork(best.ssid)) {
    ++roam_count_;
    return;
  }
  // The better network refused us; go back rather than sit disconnected.
  Serial.println("WiFi: roam failed; restoring previous network");
  if (!joinNetwork(previous)) connectBest();
}

bool WifiManager::ensureLinkForSession() {
  if (!config_ || !config_->config().wifiEnabled) return false;
  // The link normally survives between conversations, so this is the path
  // taken almost every time and it must cost nothing beyond waking the radio.
  setPowerSave(false);
  if (isConnected()) return true;

  disconnected_since_ms_ = 0;
  if (best_ssid_.length()) {
    Serial.printf("WiFi: wake with link down; rejoining remembered network rssi_last=%d\n",
                  static_cast<int>(best_rssi_));
    if (joinNetwork(best_ssid_)) return true;
    Serial.println("WiFi: remembered network unavailable; scanning");
  }
  return connectBest();
}
