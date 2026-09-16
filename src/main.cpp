#include <Arduino.h>
#include <M5Unified.h>
#include <M5StackChan.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include "GeminiLiveProbe.h"
#include "MemoryStore.h"
#include "ToolGatewayClient.h"
#include "GeminiToolBridge.h"
#include "WebConfigServer.h"
#include "ConfigManager.h"
#include "EmotionController.h"
#include "ServoGestureController.h"
#include "CameraCapture.h"

GeminiLiveProbe gemini;
MemoryStore memory(SD);
ToolGatewayClient toolGateway;
EmotionController emotion;
ServoGestureController servoGestures;
CameraCapture camera;
GeminiToolBridge toolBridge(toolGateway, emotion, servoGestures, camera);
WebConfigServer webConfig(SD, memory, toolGateway, emotion, servoGestures, gemini, camera);
ConfigManager configManager(SD);
volatile bool g_voice_toggle_requested = false;
static uint8_t g_speaker_volume = 200;
static constexpr uint32_t kBootNeutralSleepMs = 45000;
static constexpr uint32_t kPowerButtonLedOffHoldMs = 700;
static uint32_t g_last_human_activity_ms = 0;
static uint32_t g_power_button_pressed_ms = 0;
static bool g_power_button_leds_off = false;

static void applySpeakerVolume() {
  M5.Speaker.setVolume(g_speaker_volume);
  M5.Speaker.setAllChannelVolume(g_speaker_volume);
  gemini.setSpeakerVolume(g_speaker_volume);
}

static void playBootDroidWhistle() {
  static constexpr int kBootSoundSampleRate = 24000;
  static constexpr float kPi = 3.14159265358979f;
  static constexpr float kTwoPi = 2.0f * kPi;
  static int16_t bootSound[14000];  // greeting phrase is ~0.55s at 24 kHz.
  size_t pos = 0;

  auto appendSweep = [&](float f1, float f2, float dur, float amp = 0.50f) {
    int n = static_cast<int>(kBootSoundSampleRate * dur);
    float phase = 0.0f;
    for (int i = 0; i < n && pos < (sizeof(bootSound) / sizeof(bootSound[0])); ++i) {
      float t = static_cast<float>(i) / static_cast<float>(n);
      float f = f1 + (f2 - f1) * t;
      float env = sinf(t * kPi);  // bell envelope: removes clicks/harsh edges.
      float s = amp * env * sinf(phase);
      bootSound[pos++] = static_cast<int16_t>(32767.0f * s);
      phase += kTwoPi * f / static_cast<float>(kBootSoundSampleRate);
      if (phase > kTwoPi) phase -= kTwoPi;
    }
  };

  // User-provided "greeting" phrase, rendered as PCM instead of square-wave
  // tone(): smoother R2-D2-style whistle with exact sweep timings. Use the
  // original amplitudes from the reference because the first quiet cap was
  // physically inaudible on the CoreS3 speaker/amp.
  appendSweep(800.0f, 2400.0f, 0.15f, 0.50f);   // rising whistle
  appendSweep(1800.0f, 600.0f, 0.10f, 0.48f);   // quick fall
  appendSweep(1200.0f, 2800.0f, 0.12f, 0.50f);  // happy beep
  appendSweep(2400.0f, 1600.0f, 0.18f, 0.46f);  // soft resolution

  const uint8_t savedVolume = g_speaker_volume;
  const uint8_t effectVolume = savedVolume > 180 ? 96 : (savedVolume > 80 ? savedVolume / 2 : 48);
  M5.Speaker.begin();
  delay(30);
  M5.Speaker.setVolume(effectVolume);
  M5.Speaker.setAllChannelVolume(effectVolume);
  // Keep robot sound effects off Gemini's voice channel (1).  Vision replies
  // stream voice audio on channel 1; stopping that channel from an effect path
  // can leave a later camera/voice handoff looking like it speaks silently.
  static constexpr uint8_t kEffectChannel = 0;
  bool queued = M5.Speaker.playRaw(bootSound, pos, kBootSoundSampleRate, false, 1, kEffectChannel, false);
  Serial.printf("Boot droid whistle: samples=%u volume=%u channel=%u queued=%s\n",
                static_cast<unsigned>(pos), static_cast<unsigned>(effectVolume),
                static_cast<unsigned>(kEffectChannel), queued ? "yes" : "no");
  uint32_t start = millis();
  while (M5.Speaker.isPlaying(kEffectChannel) && millis() - start < 1200) {
    M5StackChan.update();
    delay(10);
  }
  M5.Speaker.stop(kEffectChannel);
  M5.Speaker.setVolume(savedVolume);
  M5.Speaker.setAllChannelVolume(savedVolume);
}

static void playWakeDroidChirp() {
  static constexpr int kWakeSoundSampleRate = 24000;
  static constexpr float kPi = 3.14159265358979f;
  static constexpr float kTwoPi = 2.0f * kPi;
  static int16_t wakeSound[8000];  // ~0.30s max at 24 kHz.
  static uint32_t lastWakeChirpMs = 0;

  uint32_t now = millis();
  if (now - lastWakeChirpMs < 1500) return;  // debounce future proximity/touch bursts.
  lastWakeChirpMs = now;

  size_t pos = 0;
  auto appendSweep = [&](float f1, float f2, float dur, float amp) {
    int n = static_cast<int>(kWakeSoundSampleRate * dur);
    float phase = 0.0f;
    for (int i = 0; i < n && pos < (sizeof(wakeSound) / sizeof(wakeSound[0])); ++i) {
      float t = static_cast<float>(i) / static_cast<float>(n);
      float f = f1 + (f2 - f1) * t;
      float env = sinf(t * kPi);
      float s = amp * env * sinf(phase);
      wakeSound[pos++] = static_cast<int16_t>(32767.0f * s);
      phase += kTwoPi * f / static_cast<float>(kWakeSoundSampleRate);
      if (phase > kTwoPi) phase -= kTwoPi;
    }
  };

  // Short wake acknowledgement: related to the boot greeting, but much shorter
  // so it confirms the tap/wake without delaying mic availability.
  appendSweep(800.0f, 1600.0f, 0.08f, 0.46f);
  appendSweep(1800.0f, 2400.0f, 0.10f, 0.44f);
  appendSweep(1700.0f, 1100.0f, 0.08f, 0.38f);

  const uint8_t savedVolume = g_speaker_volume;
  // User-confirmed good wake sound; raise only this acknowledgement cue by an
  // additional ~10% without changing voice volume or the boot greeting.
  const uint8_t effectVolume = savedVolume > 180 ? 121 : (savedVolume > 80 ? (savedVolume * 11) / 16 : 61);
  M5.Speaker.begin();
  delay(15);
  M5.Speaker.setVolume(effectVolume);
  M5.Speaker.setAllChannelVolume(effectVolume);
  // Keep wake effects isolated from Gemini voice channel (1).  The camera tool
  // can be followed immediately by streamed voice; never stop/reuse that voice
  // channel from this acknowledgement cue.
  static constexpr uint8_t kEffectChannel = 0;
  bool queued = M5.Speaker.playRaw(wakeSound, pos, kWakeSoundSampleRate, false, 1, kEffectChannel, false);
  Serial.printf("Wake droid chirp: samples=%u volume=%u channel=%u queued=%s\n",
                static_cast<unsigned>(pos), static_cast<unsigned>(effectVolume),
                static_cast<unsigned>(kEffectChannel), queued ? "yes" : "no");
  uint32_t start = millis();
  while (M5.Speaker.isPlaying(kEffectChannel) && millis() - start < 450) {
    M5StackChan.update();
    delay(5);
  }
  M5.Speaker.stop(kEffectChannel);
  M5.Speaker.setVolume(savedVolume);
  M5.Speaker.setAllChannelVolume(savedVolume);
}

static void configureAudio(uint8_t micMagnification = 16, uint8_t micNoiseFilterLevel = 0) {
  auto mic = M5.Mic.config();
  mic.sample_rate = 16000;
  // CoreS3/M5Unified software input gain. Default is 16. Keep conservative:
  // too much gain also amplifies room noise/echo and can hurt Gemini VAD.
  if (micMagnification < 1) micMagnification = 1;
  if (micMagnification > 24) micMagnification = 24;
  if (micNoiseFilterLevel > 4) micNoiseFilterLevel = 4;
  mic.magnification = micMagnification;
  mic.noise_filter_level = micNoiseFilterLevel;
  M5.Mic.config(mic);

  auto spk = M5.Speaker.config();
  spk.sample_rate = 96000;
  spk.task_pinned_core = APP_CPU_NUM;
  spk.magnification = 16;
  M5.Speaker.config(spk);
}

static void drawStatus(const char* line) {
  // Keep the LCD owned by EmotionController's procedural face renderer.
  // Older builds wrote status text directly to the screen here, which briefly
  // replaced the face until the next face frame. Status now goes to serial/API.
  Serial.printf("Status: %s\n", line ? line : "");
}

static void blackoutExternalLightsForPowerOff() {
  if (!g_power_button_leds_off) {
    Serial.println("Power: button hold -> RGB off latch");
    g_power_button_leds_off = true;
  }
  // The StackChan RGB LEDs live on the BSP IO expander and can remain latched
  // even when the CoreS3 display/PMIC turns the screen off. Keep writing black
  // while the firmware still runs so EmotionController cannot relight them.
  M5StackChan.showRgbColor(0, 0, 0);
}

static bool connectWifiFromConfig(const ConfigManager& cfg) {
  const auto& c = cfg.config();
  if (!c.wifiEnabled) {
    Serial.println("WiFi: disabled");
    return false;
  }
  String password = cfg.readWifiPassword();
  if (!c.wifiSsid.length() || !password.length()) {
    Serial.println("WiFi: missing ssid or password");
    return false;
  }
  Serial.println("WiFi: starting station mode");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  // DHCP on some routers/boots can leave DNS unusable even though the ESP is
  // reachable on the LAN. Gemini Live connect then fails at hostByName() and
  // the robot drops into error before audio starts. Keep DHCP addressing, but
  // pin reliable resolvers for outbound Gemini/gateway connections.
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE,
              IPAddress(1, 1, 1, 1), IPAddress(8, 8, 8, 8));
  WiFi.begin(c.wifiSsid.c_str(), password.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    M5StackChan.update();
  }
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(),
                IPAddress(1, 1, 1, 1), IPAddress(8, 8, 8, 8));
    Serial.print("WiFi: connected ip=");
    Serial.print(WiFi.localIP());
    Serial.print(" dns=");
    Serial.println(WiFi.dnsIP());
    drawStatus(WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("WiFi: connect timeout");
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  return false;
}

static bool shouldStartSetupAccessPoint(const ConfigManager& cfg, bool sdConfigExists) {
  const auto& c = cfg.config();
  if (!sdConfigExists) return true;
  if (c.wifiEnabled) return true;
  return false;
}

static String setupApSuffix() {
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%04X", static_cast<unsigned>(ESP.getEfuseMac() & 0xFFFF));
  return String(suffix);
}

static bool startSetupAccessPoint(const ConfigManager& cfg) {
  String suffix = setupApSuffix();
  String apName = cfg.config().robotId.length() ? cfg.config().robotId : "stackchan";
  apName += "-setup-";
  apName += suffix;
  apName.replace(" ", "-");
  String apPassword = "stackchan" + suffix;
  Serial.print("WiFi: starting setup access point ssid=");
  Serial.println(apName);
  Serial.print("WiFi: setup access point password=");
  Serial.println(apPassword);
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  bool ok = WiFi.softAP(apName.c_str(), apPassword.c_str());
  if (!ok) {
    Serial.println("WiFi: setup access point failed");
    return false;
  }
  Serial.print("WiFi: setup access point ip=");
  Serial.println(WiFi.softAPIP());
  drawStatus(WiFi.softAPIP().toString().c_str());
  return true;
}

void setup() {
  Serial.begin(115200);
  M5StackChan.begin();
  configureAudio();
  M5.Speaker.begin();
  applySpeakerVolume();
  playBootDroidWhistle();
  emotion.begin();
  servoGestures.begin();
  camera.begin();
  M5StackChan.Motion.goHome();
  // Safe defaults before SD config is loaded. ConfigManager may override after SD mount.
  ToolGatewayClient::Config gatewayConfig;
  gatewayConfig.enabled = false;
  gatewayConfig.baseUrl = "";
  gatewayConfig.robotId = "stackchan";
  toolGateway.begin(gatewayConfig);
  gemini.setToolBridge(&toolBridge);
  toolBridge.setGemini(&gemini);
  toolBridge.setMemoryStore(&memory);
  gemini.setEmotionController(&emotion);
  gemini.setMemoryStore(&memory);
  WebConfigServer::Config webConfigOptions;
  webConfigOptions.enabled = false;
  webConfigOptions.port = 80;
  webConfigOptions.robotId = "stackchan";
  webConfig.begin(webConfigOptions);
  drawStatus("BSP + config compile");

  // Compile-only: do not print or embed secrets. Tomorrow load key from SD/NVS.
  // Example SD mount path is preserved for compatibility investigation.
  bool sd_ok = SD.begin(GPIO_NUM_4, SPI, 25000000);
  Serial.printf("SD status: %s\n", sd_ok ? "ok" : "missing");
  if (sd_ok) {
    MemoryStore::Policy policy;
    policy.keepRawSessions = 3;
    policy.keepRawDays = 3;
    policy.maxEventFileBytes = 256 * 1024;
    policy.maxRetrievedChars = 3000;
    bool memory_ok = memory.begin(policy);
    Serial.printf("MemoryStore status: %s session=%s day=%s\n",
                  memory_ok ? "ok" : "failed",
                  memory.currentSessionId(),
                  memory.todayKey());
    if (memory_ok) {
      memory.compactIfNeeded();
    }
    bool cfg_ok = configManager.begin();
    bool runtime_config_exists = SD.exists("/app/StackChan/config/runtime.json");
    Serial.printf("ConfigManager init: %s\n", cfg_ok ? "ok" : "failed");
    configManager.printRedactedStatus(Serial);
    g_speaker_volume = configManager.config().speakerVolume;
    configureAudio(configManager.config().micMagnification,
                   configManager.config().micNoiseFilterLevel);
    applySpeakerVolume();
    Serial.printf("Audio: speaker_volume=%u mic_magnification=%u mic_noise_filter=%u\n",
                  static_cast<unsigned>(g_speaker_volume),
                  static_cast<unsigned>(configManager.config().micMagnification),
                  static_cast<unsigned>(configManager.config().micNoiseFilterLevel));
    toolGateway.begin(configManager.gatewayConfig());
    bool wifi_ok = connectWifiFromConfig(configManager);
    bool setup_ap_ok = false;
    if (!wifi_ok && shouldStartSetupAccessPoint(configManager, runtime_config_exists)) {
      setup_ap_ok = startSetupAccessPoint(configManager);
    }
    if (wifi_ok || setup_ap_ok) {
      if (wifi_ok) {
        webConfig.begin(configManager.webConfig());
      } else {
        WebConfigServer::Config setupWeb;
        setupWeb.enabled = true;
        setupWeb.port = 80;
        setupWeb.robotId = configManager.config().robotId.c_str();
        webConfig.begin(setupWeb);
      }
      const auto& cfg = configManager.config();
      if (wifi_ok && cfg.geminiEnabled) {
        if (configManager.hasGeminiApiKey()) {
          Serial.println("Gemini: configured lazy");
          gemini.setModel(cfg.geminiModel);
          gemini.setVoiceName(cfg.geminiVoice);
          gemini.setSearchGrounding(cfg.geminiSearchGrounding);
          gemini.setVadConfig(cfg.vadPrefixPaddingMs, cfg.vadSilenceDurationMs,
                              cfg.vadStartSensitivityHigh, cfg.vadEndSensitivityLow,
                              cfg.vadTurnIncludesAllInput);
          gemini.setSystemPrompt(cfg.systemPrompt);
          gemini.setPersonaPrompt(cfg.personaPrompt);
          bool gemini_ok = gemini.begin(configManager.readGeminiApiKey().c_str());
          Serial.printf("Gemini: %s\n", gemini_ok ? "ready for on-demand connect" : "failed");
        } else {
          Serial.println("Gemini: enabled but api key missing");
        }
      } else if (wifi_ok) {
        Serial.println("Gemini: disabled");
      } else {
        Serial.println("Gemini: skipped in setup access point mode");
      }
    } else {
      WebConfigServer::Config webOff;
      webOff.enabled = false;
      webOff.robotId = configManager.config().robotId.c_str();
      webConfig.begin(webOff);
      Serial.println("Gemini: skipped because WiFi is not connected");
    }
  }

  // Gemini API key is loaded by ConfigManager as a secret and must never be printed.
  g_last_human_activity_ms = millis();
}

void loop() {
  M5StackChan.update();

  uint32_t now = millis();
  if (M5.BtnPWR.isPressed()) {
    if (g_power_button_pressed_ms == 0) g_power_button_pressed_ms = now;
    if (now - g_power_button_pressed_ms >= kPowerButtonLedOffHoldMs) {
      blackoutExternalLightsForPowerOff();
    }
  } else {
    g_power_button_pressed_ms = 0;
  }
  if (g_power_button_leds_off) {
    blackoutExternalLightsForPowerOff();
    delay(50);
    return;
  }

  gemini.loop();
  emotion.loop();
  servoGestures.loop();
  webConfig.loop();
  bool touched = M5StackChan.TouchSensor.wasPressed();
  if (g_voice_toggle_requested) {
    g_voice_toggle_requested = false;
    Serial.println("HTTP voice toggle requested");
    touched = true;
  }
#if defined(ARDUINO_M5STACK_CORES3)
  static bool last_screen_pressed = false;
  auto t = M5.Touch.getDetail();
  bool screen_pressed = (M5.Touch.getCount() > 0) || t.isPressed();
  if (screen_pressed && !last_screen_pressed) {
    Serial.printf("Display touch x=%d y=%d count=%u state=%u\n", t.x, t.y, M5.Touch.getCount(), static_cast<unsigned>(t.state));
    touched = true;
  }
  last_screen_pressed = screen_pressed;
#endif
  if (touched) {
    g_last_human_activity_ms = millis();
    memory.appendEvent("user", "touch pressed", "touch");
    if (!gemini.continuousConversation()) {
      // Audible wake acknowledgement is safe here because this is an explicit
      // user/proximity/HTTP wake event, not the automatic continuous-dialog
      // mic resume path. Play it *before* requestConversationStart(), because
      // if Gemini is already connected that call immediately starts the mic and
      // the speaker must stay out of the recording path.
      playWakeDroidChirp();
      bool queued = gemini.requestConversationStart();
      if (queued) {
        // Keep user-facing listening cue honest: GeminiLiveProbe will switch to
        // "listening" only after the first mic chunk is actually sent.
        emotion.setEmotion("thinking");
        drawStatus(gemini.isReady() ? "dialog: preparing mic..." : "Gemini connecting...");
      } else {
        emotion.setEmotion("error");
        drawStatus("Gemini not configured");
      }
    } else {
      gemini.stopConversation();
      emotion.setEmotion("neutral");
      drawStatus("dialog off");
    }
  }
  now = millis();
  if (!gemini.isReady() && !gemini.isRecording() && !gemini.isSpeaking() &&
      emotion.currentEmotion() == "neutral" && g_last_human_activity_ms &&
      now - g_last_human_activity_ms > kBootNeutralSleepMs) {
    Serial.println("Status: boot idle -> sleep");
    emotion.setEmotion("sleep");
    servoGestures.queueGesture("center_head");
  }
  delay(10);
}
