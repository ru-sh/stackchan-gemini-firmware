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
  last_roam_check_ms_ = millis();
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
  if (best.ssid.length()) {
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
  // Roaming is an optimisation, never worth interrupting a live conversation.
  if (busy) return;

  const uint32_t now = millis();
  if (!last_roam_check_ms_) last_roam_check_ms_ = now;
  if (now - last_roam_check_ms_ < ROAM_CHECK_INTERVAL_MS) return;
  last_roam_check_ms_ = now;

  if (config_->configuredWifiSsids().size() < 2) return;

  // Start from the live link reading; the scan replaces it if it sees the
  // current network more strongly through another access point.
  int32_t currentRssi = WiFi.RSSI();
  Candidate best = scanForBest(&currentRssi);
  if (!best.ssid.length() || best.ssid == current_ssid_) return;

  const int32_t margin = static_cast<int32_t>(config_->config().wifiRoamMarginDb);
  if (best.rssi < currentRssi + margin) {
    Serial.printf("WiFi: staying put; best alternative %d vs current %d, margin %d\n",
                  static_cast<int>(best.rssi), static_cast<int>(currentRssi),
                  static_cast<int>(margin));
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
