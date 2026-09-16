#include "WebConfigServer.h"
#include <M5Unified.h>
#include <M5StackChan.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <math.h>

extern volatile bool g_voice_toggle_requested;

namespace {
constexpr const char* kConfigDir = "/app/StackChan/config";
constexpr const char* kPromptDir = "/app/StackChan/prompts";
constexpr const char* kSecretDir = "/app/StackChan/secrets";
constexpr const char* kRuntimeConfigPath = "/app/StackChan/config/runtime.json";
constexpr const char* kGatewayConfigPath = "/app/StackChan/config/gateway.json";
constexpr const char* kSummaryConfigPath = "/app/StackChan/config/summary.json";
constexpr const char* kSystemPromptPath = "/app/StackChan/prompts/system.txt";
constexpr const char* kPersonaPromptPath = "/app/StackChan/prompts/persona.txt";
constexpr const char* kSecretsMetaPath = "/app/StackChan/secrets/meta.jsonl";
constexpr const char* kWebPasswordHashPath = "/app/StackChan/secrets/web_password_sha256.txt";
constexpr const char* kWebAuthUser = "stackchan";

// LTR-553ALS-WA on CoreS3 internal I2C.  This diagnostic helper deliberately
// does not drive any wake/stop behavior; it only exposes raw values so we can
// calibrate thresholds from the real robot before adding gesture semantics.
constexpr uint8_t kLtr553Addr = 0x23;
constexpr uint8_t kLtrAlsContr = 0x80;
constexpr uint8_t kLtrPsContr = 0x81;
constexpr uint8_t kLtrPsLed = 0x82;
constexpr uint8_t kLtrPsNPulses = 0x83;
constexpr uint8_t kLtrPsMeasRate = 0x84;
constexpr uint8_t kLtrAlsMeasRate = 0x85;
constexpr uint8_t kLtrAlsDataCh1 = 0x88;
constexpr uint8_t kLtrAlsDataCh0 = 0x8A;
constexpr uint8_t kLtrStatus = 0x8C;
constexpr uint8_t kLtrPsData = 0x8D;
bool g_ltr553_configured = false;
uint32_t g_ltr553_configured_ms = 0;
String g_ltr553_last_error;

bool ltrRead(uint8_t reg, uint8_t* data, size_t len) {
  return M5.In_I2C.readRegister(kLtr553Addr, reg, data, len, 100000L);
}

bool ltrWrite1(uint8_t reg, uint8_t value) {
  return M5.In_I2C.writeRegister(kLtr553Addr, reg, &value, 1, 100000L);
}

bool configureLtr553Once() {
  if (g_ltr553_configured) return true;

  // Same low-risk baseline as the official CoreS3 example: PS 50 ms, 40 kHz
  // LED pulse, ALS active.  These are sensor configuration writes only; no
  // interrupt thresholds, no wake callback, no conversation control.
  bool ok = true;
  ok = ok && ltrWrite1(kLtrPsContr, 0x00);      // PS standby while configuring.
  ok = ok && ltrWrite1(kLtrAlsContr, 0x00);     // ALS standby while configuring.
  ok = ok && ltrWrite1(kLtrPsLed, 0x27);        // 40 kHz, 100% duty, 100 mA.
  ok = ok && ltrWrite1(kLtrPsNPulses, 0x01);    // 1 pulse, official default.
  ok = ok && ltrWrite1(kLtrPsMeasRate, 0x00);   // 50 ms PS measurement.
  ok = ok && ltrWrite1(kLtrAlsContr, 0x19);     // ALS active, gain 48x.
  ok = ok && ltrWrite1(kLtrAlsMeasRate, 0x09);  // 100 ms integration/rate.
  ok = ok && ltrWrite1(kLtrPsContr, 0x02);      // PS active.

  if (ok) {
    g_ltr553_configured = true;
    g_ltr553_configured_ms = millis();
    g_ltr553_last_error = "";
  } else {
    g_ltr553_last_error = "i2c_config_failed";
  }
  return ok;
}

void fillAudioDirectionDiagnostic(JsonObject audio, bool geminiBusy) {
  audio["enabled"] = true;
  audio["diagnostic_only"] = true;
  audio["sample_rate"] = 16000;
  audio["stereo_request"] = true;
  audio["note"] = "Short stereo capture for L/R direction diagnostics only; Gemini stream remains mono.";
  auto micCfg = M5.Mic.config();
  audio["input_stereo_cfg"] = micCfg.stereo;
  audio["input_left_channel_cfg"] = micCfg.left_channel;
  if (geminiBusy || M5.Mic.isRecording() || M5.Speaker.isPlaying()) {
    audio["ok"] = false;
    audio["skipped"] = true;
    audio["reason"] = M5.Speaker.isPlaying() ? "speaker_playing" : "gemini_or_mic_busy";
    return;
  }

  bool speakerWasRunning = M5.Speaker.isRunning();
  uint8_t savedVolume = M5.Speaker.getVolume();
  // CoreS3 routes mic and speaker through the same I2S/audio codec path.  The
  // official M5CoreS3 mic example explicitly ends the speaker before recording;
  // leaving an idle speaker task running can make this diagnostic insensitive to
  // real acoustic input.  Earlier cold captures went all-zero when doing this,
  // so keep the warm-up loop below and only stop the idle speaker after we have
  // already skipped active playback.
  if (speakerWasRunning) M5.Speaker.end();
  M5.Mic.begin();

  static constexpr int kFrames = 2048;
  static constexpr int kSamples = kFrames * 2;
  static int16_t stereo[kSamples];
  bool ok = true;
  bool timedOut = false;
  int captureAttempts = 0;
  // Like the official CoreS3 mic example, treat one-shot capture as a warmed-up
  // stream: the first short buffers after Mic.begin()/I2S start can be stale or
  // zero. Queue a few short windows and analyze the last one.
  for (int attempt = 0; attempt < 4 && ok; ++attempt) {
    memset(stereo, 0, sizeof(stereo));
    ok = M5.Mic.record(stereo, kSamples, 16000, true);
    ++captureAttempts;
    if (ok) delay(45);
    uint32_t waitStart = millis();
    while (ok && M5.Mic.isRecording() && millis() - waitStart < 200) {
      delay(1);
    }
    timedOut = ok && M5.Mic.isRecording();
    if (timedOut) break;
  }
  M5.Mic.end();
  if (speakerWasRunning) {
    M5.Speaker.begin();
    M5.Speaker.setVolume(savedVolume);
    M5.Speaker.setAllChannelVolume(savedVolume);
  }
  audio["speaker_running_during_probe"] = speakerWasRunning;
  audio["capture_attempts"] = captureAttempts;
  audio["capture_timed_out"] = timedOut;
  audio["ok"] = ok;
  if (!ok) {
    audio["reason"] = "mic_record_false";
    return;
  }

  int64_t sumL = 0, sumR = 0, sumSqL = 0, sumSqR = 0;
  int peakL = 0, peakR = 0;
  for (int i = 0; i < kFrames; ++i) {
    int l = stereo[i * 2];
    int r = stereo[i * 2 + 1];
    sumL += l;
    sumR += r;
    int al = l < 0 ? -l : l;
    int ar = r < 0 ? -r : r;
    if (al > peakL) peakL = al;
    if (ar > peakR) peakR = ar;
    sumSqL += static_cast<int64_t>(l) * l;
    sumSqR += static_cast<int64_t>(r) * r;
  }
  float meanL = static_cast<float>(sumL) / kFrames;
  float meanR = static_cast<float>(sumR) / kFrames;
  double varL = static_cast<double>(sumSqL) / kFrames - static_cast<double>(meanL) * meanL;
  double varR = static_cast<double>(sumSqR) / kFrames - static_cast<double>(meanR) * meanR;
  if (varL < 0) varL = 0;
  if (varR < 0) varR = 0;
  float rmsL = sqrt(varL);
  float rmsR = sqrt(varR);
  float denom = rmsL + rmsR + 1.0f;
  float balance = (rmsR - rmsL) / denom;

  int bestLag = 0;
  double bestCorr = -1.0;
  static constexpr int kMaxLag = 8;
  for (int lag = -kMaxLag; lag <= kMaxLag; ++lag) {
    double corr = 0;
    double eL = 0;
    double eR = 0;
    int n = 0;
    for (int i = 0; i < kFrames; ++i) {
      int j = i + lag;
      if (j < 0 || j >= kFrames) continue;
      double l = static_cast<double>(stereo[i * 2]) - meanL;
      double r = static_cast<double>(stereo[j * 2 + 1]) - meanR;
      corr += l * r;
      eL += l * l;
      eR += r * r;
      ++n;
    }
    if (n > 0 && eL > 1.0 && eR > 1.0) corr /= sqrt(eL * eR);
    else corr = 0;
    if (corr > bestCorr) {
      bestCorr = corr;
      bestLag = lag;
    }
  }

  const float tdoaUs = bestLag * 1000000.0f / 16000.0f;
  const float loudness = (rmsL + rmsR) * 0.5f;
  const char* direction = "uncertain";
  if (loudness < 40) direction = "quiet";
  else if (balance > 0.12f) direction = "right_louder";
  else if (balance < -0.12f) direction = "left_louder";
  else if (bestLag >= 2) direction = "right_delayed_or_left_first";
  else if (bestLag <= -2) direction = "left_delayed_or_right_first";
  else direction = "center_or_uncertain";

  audio["frames"] = kFrames;
  audio["duration_ms"] = kFrames * 1000 / 16000;
  audio["left_rms"] = rmsL;
  audio["right_rms"] = rmsR;
  audio["left_peak"] = peakL;
  audio["right_peak"] = peakR;
  audio["balance_r_minus_l"] = balance;
  audio["tdoa_lag_samples"] = bestLag;
  audio["tdoa_us"] = tdoaUs;
  audio["corr"] = bestCorr;
  audio["direction_hint"] = direction;
}

const char kIndexHtml[] PROGMEM = R"HTML(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>StackChan Config</title>
  <style>
    :root{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;background:#111827;color:#e5e7eb}
    body{margin:0;padding:16px;max-width:920px;margin:auto}
    h1{font-size:28px;margin:8px 0 4px}.muted{color:#9ca3af}.grid{display:grid;gap:14px}.tabs{display:flex;flex-wrap:wrap;gap:8px;margin:14px 0}.tab{background:#374151;color:#e5e7eb;padding:9px 12px;margin:0}.tab.active{background:#f59e0b;color:#451a03}.card[data-tab]{display:none}.card[data-tab].active{display:block}
    .card{background:#1f2937;border:1px solid #374151;border-radius:14px;padding:14px;box-shadow:0 1px 12px #0005}
    label{display:block;margin:10px 0 4px;color:#cbd5e1}input,textarea,select{width:100%;box-sizing:border-box;border-radius:10px;border:1px solid #4b5563;background:#111827;color:#e5e7eb;padding:10px;font:inherit}textarea{min-height:120px}
    button{border:0;border-radius:10px;background:#22c55e;color:#052e16;padding:10px 14px;font-weight:700;margin:8px 8px 0 0}.secondary{background:#38bdf8;color:#082f49}.danger{background:#fb7185;color:#4c0519}
    pre{white-space:pre-wrap;background:#0b1020;border-radius:10px;padding:10px;overflow:auto}.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}@media(max-width:700px){.row{grid-template-columns:1fr}}
  </style>
</head>
<body>
  <h1>StackChan Config</h1>
  <p class="muted">Local configuration panel. Secrets are stored but never shown back.</p>
  <nav class="tabs" aria-label="Settings sections"><button class="tab active" onclick="openTab('status')">Status</button><button class="tab" onclick="openTab('runtime')">Runtime</button><button class="tab" onclick="openTab('gateway')">Gateway</button><button class="tab" onclick="openTab('secrets')">Secrets</button><button class="tab" onclick="openTab('security')">Security</button><button class="tab" onclick="openTab('prompts')">Prompts</button><button class="tab" onclick="openTab('memory')">Memory</button></nav>
  <div class="grid">
    <section class="card active" data-tab="status"><h2>Status</h2><button onclick="loadStatus()" class="secondary">Refresh</button><button onclick="voiceToggle()">Wake / sleep</button><p class="muted">Uses the same voice toggle as touch: if the dialogue is asleep it wakes it, if it is listening it stops it.</p><pre id="status">...</pre></section>
    <section class="card" data-tab="runtime"><h2>Runtime</h2><div class="row"><div><label>Robot ID</label><input id="robotId" value="stackchan"></div><div><label>Gemini model</label><input id="geminiModel" placeholder="models/gemini-3.8-live"></div></div><label>Gemini voice</label><select id="geminiVoice"><option value="Puck">Puck — upbeat</option><option value="Charon">Charon — informative</option><option value="Kore">Kore — firm</option><option value="Fenrir">Fenrir — excitable</option><option value="Aoede">Aoede — breezy</option></select><p class="muted">Voice applies to the next Gemini Live session after reboot/reconnect. Voices outside this list are not offered by the Live model and fall back to Puck.</p><label>Wi‑Fi SSID</label><input id="wifiSsid" placeholder="SSID is stored; password is stored separately in secrets"><label>Gateway Base URL</label><input id="gatewayUrl" placeholder="http://gateway.local:8811/stackchan"><label>Speaker volume: <span id="speakerVolumeLabel">200</span> / 255</label><input id="speakerVolume" type="range" min="0" max="255" step="1" value="200" oninput="speakerVolumeLabel.textContent=this.value"><div class="row"><div><label>Mic gain: <span id="micMagnificationLabel">16</span> / 24</label><input id="micMagnification" type="range" min="1" max="24" step="1" value="16" oninput="micMagnificationLabel.textContent=this.value"><p class="muted">Safe runtime gain. Baseline is 16; for tests try 20→22→24.</p></div><div><label>Mic noise filter: <span id="micNoiseFilterLabel">0</span> / 4</label><input id="micNoiseFilter" type="range" min="0" max="4" step="1" value="0" oninput="micNoiseFilterLabel.textContent=this.value"><p class="muted">Baseline is 0. Increase carefully: the filter may suppress quiet speech.</p></div></div><div class="row"><div><label>VAD prefix padding: <span id="vadPrefixPaddingLabel">800</span> ms</label><input id="vadPrefixPadding" type="range" min="0" max="2000" step="100" value="800" oninput="vadPrefixPaddingLabel.textContent=this.value"><p class="muted">Buffer before speech starts. Helps avoid losing the beginning of a phrase.</p></div><div><label>VAD silence duration: <span id="vadSilenceDurationLabel">900</span> ms</label><input id="vadSilenceDuration" type="range" min="100" max="3000" step="100" value="900" oninput="vadSilenceDurationLabel.textContent=this.value"><p class="muted">Pause before ending an utterance. Higher values reduce cut-offs but slow responses.</p></div></div><div class="row"><label><input id="vadStartSensitivityHigh" type="checkbox" checked> VAD start sensitivity high</label><label><input id="vadEndSensitivityLow" type="checkbox" checked> VAD end sensitivity low</label><label><input id="vadTurnIncludesAllInput" type="checkbox" checked> Turn includes all input</label></div><div class="row"><label><input id="wifiEnabled" type="checkbox"> Wi‑Fi enabled</label><label><input id="webEnabled" type="checkbox"> Web UI enabled</label><label><input id="geminiEnabled" type="checkbox"> Gemini enabled</label><label><input id="gatewayEnabled" type="checkbox"> Gateway enabled</label></div><button onclick="saveRuntime(false)">Save runtime</button><button onclick="saveRuntime(true)" class="danger">Save and reboot</button><button onclick="loadRuntime()" class="secondary">Load runtime</button><p class="muted">Volume and most runtime settings apply after reboot. “Save and reboot” writes settings to SD first, then reboots the robot.</p></section>
    <section class="card" data-tab="gateway"><h2>Gateway</h2><button onclick="loadTools()" class="secondary">Check tools</button><pre id="tools"></pre></section>
    <section class="card" data-tab="secrets"><h2>Secrets</h2><div class="row"><div><label>Gemini API key</label><input id="geminiKey" type="password" placeholder="enter/replace"></div><div><label>Gateway token</label><input id="gatewayToken" type="password" placeholder="optional"></div><div><label>Wi‑Fi password</label><input id="wifiPassword" type="password" placeholder="enter/replace"></div></div><button onclick="saveSecrets()" class="danger">Save secrets</button><p class="muted">The API never returns secret values, only set/missing status.</p></section>
    <section class="card" data-tab="security"><h2>Web security</h2><p class="muted">If no Web password is set, the local UI is open. After setting one, browser/API access uses HTTP Basic auth with user <code>stackchan</code>.</p><label>New Web password</label><input id="webPassword" type="password" placeholder="set or change password"><button onclick="saveWebPassword()" class="danger">Set Web password</button><p class="muted">Stored on SD as SHA-256, not returned by the API.</p></section>
    <section class="card" data-tab="prompts"><h2>Additional prompts</h2><p class="muted">This is an SD/Web overlay appended to the base StackChan firmware instructions. The base prompt with camera, search, head, and safety rules is not shown or replaced here. Changes apply to the next Gemini Live session/connection.</p><label>Additional system overlay</label><textarea id="systemPrompt"></textarea><label>Persona / style overlay</label><textarea id="personaPrompt"></textarea><button onclick="savePrompts()">Save overlay</button><button onclick="loadPrompts()" class="secondary">Load</button></section>
    <section class="card" data-tab="memory"><h2>Memory</h2><label>Summary model</label><input id="summaryModel" placeholder="gemini-flash-latest"><button onclick="saveSummaryConfig()" class="secondary">Save summary model</button><button onclick="loadMemory()" class="secondary">Context</button><button onclick="loadDialogues()" class="secondary">Dialogues</button><button onclick="loadSummaries()" class="secondary">Summary</button><label>Memory search</label><input id="memoryQuery" placeholder="who did you see today?"><button onclick="searchMemory()" class="secondary">Search memory</button><button onclick="loadMemoryStats()" class="secondary">Stats</button><button onclick="runSummarize()">Summarize</button><button onclick="runVectorize()">Vectorize</button><p class="muted">Summary v2 calls Gemini Flash/Lite using the SD API key. Ordinary numbers/dates/model IDs are preserved exactly; private PIN/code/password/address values remain markers and private-memory only.</p><pre id="memory"></pre></section>
  </div>
<script>
async function jget(u){const r=await fetch(u);return r.json()}
async function jpost(u,o){const r=await fetch(u,{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(o)});return r.json()}
function show(id,o){document.getElementById(id).textContent=typeof o==='string'?o:JSON.stringify(o,null,2)}
function openTab(name){document.querySelectorAll('[data-tab]').forEach(e=>e.classList.toggle('active',e.dataset.tab===name));document.querySelectorAll('.tab').forEach(b=>b.classList.toggle('active',(b.getAttribute('onclick')||'').indexOf("'"+name+"'")>=0))}
async function loadStatus(){show('status',await jget('/api/status'))}
async function voiceToggle(){show('status',await jpost('/api/voice/toggle',{}));setTimeout(loadStatus,700)}
async function loadRuntime(){const c=await jget('/api/runtime');robotId.value=c.robot_id||'stackchan';geminiModel.value=c.gemini_model||'';geminiVoice.value=c.gemini_voice||'Puck';wifiSsid.value='';wifiSsid.placeholder=c.wifi_ssid==='set'?'SSID is already stored; leave empty to keep it':'enter SSID';gatewayUrl.value=c.gateway_base_url||'';speakerVolume.value=c.speaker_volume||200;speakerVolumeLabel.textContent=speakerVolume.value;micMagnification.value=c.mic_magnification||16;micMagnificationLabel.textContent=micMagnification.value;micNoiseFilter.value=c.mic_noise_filter_level||0;micNoiseFilterLabel.textContent=micNoiseFilter.value;vadPrefixPadding.value=c.vad_prefix_padding_ms||800;vadPrefixPaddingLabel.textContent=vadPrefixPadding.value;vadSilenceDuration.value=c.vad_silence_duration_ms||900;vadSilenceDurationLabel.textContent=vadSilenceDuration.value;vadStartSensitivityHigh.checked=c.vad_start_sensitivity_high!==false;vadEndSensitivityLow.checked=c.vad_end_sensitivity_low!==false;vadTurnIncludesAllInput.checked=c.vad_turn_includes_all_input!==false;const firstSetup=(c.wifi_ssid!=='set'&&c.wifi_password!=='set');wifiEnabled.checked=firstSetup?true:!!c.wifi_enabled;webEnabled.checked=firstSetup?true:!!c.web_enabled;geminiEnabled.checked=!!c.gemini_enabled;gatewayEnabled.checked=!!c.gateway_enabled;show('status',c)}
async function saveRuntime(reboot){show('status',await jpost('/api/runtime',{robot_id:robotId.value,gemini_model:geminiModel.value,gemini_voice:geminiVoice.value,wifi_ssid:wifiSsid.value,gateway_base_url:gatewayUrl.value,speaker_volume:Number(speakerVolume.value),mic_magnification:Number(micMagnification.value),mic_noise_filter_level:Number(micNoiseFilter.value),vad_prefix_padding_ms:Number(vadPrefixPadding.value),vad_silence_duration_ms:Number(vadSilenceDuration.value),vad_start_sensitivity_high:vadStartSensitivityHigh.checked,vad_end_sensitivity_low:vadEndSensitivityLow.checked,vad_turn_includes_all_input:vadTurnIncludesAllInput.checked,wifi_enabled:wifiEnabled.checked,web_enabled:webEnabled.checked,gemini_enabled:geminiEnabled.checked,gateway_enabled:gatewayEnabled.checked,reboot:!!reboot}))}
async function loadConfig(){await loadRuntime()}
async function saveConfig(){await saveRuntime()}
async function saveSecrets(){show('status',await jpost('/api/secrets',{gemini_api_key:geminiKey.value,gateway_token:gatewayToken.value,wifi_password:wifiPassword.value}));geminiKey.value='';gatewayToken.value='';wifiPassword.value=''}
async function saveWebPassword(){show('status',await jpost('/api/security',{web_password:webPassword.value}));webPassword.value=''}
async function loadPrompts(){const p=await jget('/api/prompts');systemPrompt.value=p.system_prompt||'';personaPrompt.value=p.persona_prompt||''}
async function savePrompts(){show('status',await jpost('/api/prompts',{system_prompt:systemPrompt.value,persona_prompt:personaPrompt.value}))}
async function loadSummaryConfig(){const c=await jget('/api/memory/summary-config');summaryModel.value=c.model||'gemini-flash-latest'}
async function saveSummaryConfig(){show('memory',await jpost('/api/memory/summary-config',{model:summaryModel.value}))}
async function loadMemory(){const d=await jget('/api/memory/recent');const chars=(d.stats&&d.stats.active_context_chars!==undefined)?d.stats.active_context_chars:'?';show('memory','Active context chars: '+chars+'\n\n[Runtime context preview]\n'+(d.context_preview||'')+'\n\n[Active recent dialogues]\n'+(d.active_context_preview||'(empty after summarization)'))}
async function loadDialogues(){const d=await jget('/api/memory/dialogues');show('memory',d.dialogues_preview||JSON.stringify(d,null,2))}
async function loadSummaries(){const d=await jget('/api/memory/summaries');show('memory',d.summaries_preview||JSON.stringify(d,null,2))}
async function searchMemory(){show('memory',await jpost('/api/memory/search',{query:memoryQuery.value}))}
async function loadMemoryStats(){show('memory',await jget('/api/memory/stats'))}
async function runSummarize(){show('memory',await jpost('/api/memory/summarize',{model:summaryModel.value}))}
async function runVectorize(){show('memory',await jpost('/api/memory/vectorize',{}))}
async function loadTools(){show('tools',await jget('/api/gateway/tools'))}
loadStatus();loadRuntime();loadPrompts();loadSummaryConfig();
</script>
</body>
</html>
)HTML";
}

WebConfigServer::WebConfigServer(fs::FS& fs, MemoryStore& memory, ToolGatewayClient& gateway,
                                 EmotionController& emotion, ServoGestureController& servoGestures,
                                 GeminiLiveProbe& gemini, CameraCapture& camera)
    : fs_(fs), memory_(memory), gateway_(gateway), emotion_(emotion), servoGestures_(servoGestures), gemini_(gemini), camera_(camera), server_(80) {}

bool WebConfigServer::begin(const Config& config) {
  config_ = config;
  if (!config_.enabled) {
    enabled_ = false;
    return true;
  }
  // WebServer is not copy-assignable; keep the fixed constructor port for now.
  // Config.port is retained for the future Wi-Fi/config-loader step.
  const char* headers[] = {"Authorization"};
  server_.collectHeaders(headers, 1);
  registerRoutes();
  server_.begin();
  enabled_ = true;
  return true;
}

void WebConfigServer::loop() {
  if (enabled_) server_.handleClient();
}

void WebConfigServer::registerRoutes() {
  server_.on("/", HTTP_GET, [this]() { if (requireAuth()) handleIndex(); });
  server_.on("/sensors", HTTP_GET, [this]() { if (requireAuth()) handleSensorsPage(); });
  server_.on("/api/security", HTTP_GET, [this]() { handleSecurityGet(); });
  server_.on("/api/security", HTTP_POST, [this]() { handleSecuritySave(); });
  server_.on("/api/status", HTTP_GET, [this]() { if (requireAuth()) handleStatus(); });
  server_.on("/api/runtime", HTTP_GET, [this]() { if (requireAuth()) handleRuntimeGet(); });
  server_.on("/api/runtime", HTTP_POST, [this]() { if (requireAuth()) handleRuntimeSave(); });
  server_.on("/api/config", HTTP_GET, [this]() { if (requireAuth()) handleConfigGet(); });
  server_.on("/api/config", HTTP_POST, [this]() { if (requireAuth()) handleConfigSave(); });
  server_.on("/api/secrets", HTTP_POST, [this]() { if (requireAuth()) handleSecretSave(); });
  server_.on("/api/prompts", HTTP_GET, [this]() { if (requireAuth()) handlePromptsGet(); });
  server_.on("/api/prompts", HTTP_POST, [this]() { if (requireAuth()) handlePromptsSave(); });
  server_.on("/api/memory/recent", HTTP_GET, [this]() { if (requireAuth()) handleMemoryRecent(); });
  server_.on("/api/memory/dialogues", HTTP_GET, [this]() { if (requireAuth()) handleMemoryDialogues(); });
  server_.on("/api/memory/summaries", HTTP_GET, [this]() { if (requireAuth()) handleMemorySummaries(); });
  server_.on("/api/memory/search", HTTP_GET, [this]() { if (requireAuth()) handleMemorySearch(); });
  server_.on("/api/memory/search", HTTP_POST, [this]() { if (requireAuth()) handleMemorySearch(); });
  server_.on("/api/memory/stats", HTTP_GET, [this]() { if (requireAuth()) handleMemoryStats(); });
  server_.on("/api/memory/summary-config", HTTP_GET, [this]() { if (requireAuth()) handleMemorySummaryConfigGet(); });
  server_.on("/api/memory/summary-config", HTTP_POST, [this]() { if (requireAuth()) handleMemorySummaryConfigSave(); });
  server_.on("/api/memory/summarize", HTTP_POST, [this]() { if (requireAuth()) handleMemorySummarize(); });
  server_.on("/api/memory/vectorize", HTTP_POST, [this]() { if (requireAuth()) handleMemoryVectorize(); });
  server_.on("/api/gateway/tools", HTTP_GET, [this]() { if (requireAuth()) handleGatewayTools(); });
  server_.on("/api/emotion", HTTP_GET, [this]() { if (requireAuth()) handleEmotion(); });
  server_.on("/api/emotion", HTTP_POST, [this]() { if (requireAuth()) handleEmotion(); });
  server_.on("/api/servo/status", HTTP_GET, [this]() { if (requireAuth()) handleServoStatus(); });
  server_.on("/api/servo/move", HTTP_POST, [this]() { if (requireAuth()) handleServoMove(); });
  server_.on("/api/servo/gesture", HTTP_GET, [this]() { if (requireAuth()) handleServoGesture(); });
  server_.on("/api/servo/gesture", HTTP_POST, [this]() { if (requireAuth()) handleServoGesture(); });
  server_.on("/api/voice/toggle", HTTP_GET, [this]() { if (requireAuth()) handleVoiceToggle(); });
  server_.on("/api/voice/toggle", HTTP_POST, [this]() { if (requireAuth()) handleVoiceToggle(); });
  server_.on("/api/gemini/text", HTTP_GET, [this]() { if (requireAuth()) handleGeminiText(); });
  server_.on("/api/gemini/text", HTTP_POST, [this]() { if (requireAuth()) handleGeminiText(); });
  server_.on("/api/gemini/look", HTTP_GET, [this]() { if (requireAuth()) handleGeminiLook(); });
  server_.on("/api/gemini/look", HTTP_POST, [this]() { if (requireAuth()) handleGeminiLook(); });
  server_.on("/api/camera/status", HTTP_GET, [this]() { if (requireAuth()) handleCameraStatus(); });
  server_.on("/api/camera/capture", HTTP_GET, [this]() { if (requireAuth()) handleCameraCapture(); });
  server_.on("/api/camera/capture", HTTP_POST, [this]() { if (requireAuth()) handleCameraCapture(); });
  server_.on("/api/camera/jpeg", HTTP_GET, [this]() { if (requireAuth()) handleCameraJpeg(); });
  server_.on("/api/camera/latest.jpg", HTTP_GET, [this]() { if (requireAuth()) handleCameraJpeg(); });
  server_.on("/api/sensors", HTTP_GET, [this]() { if (requireAuth()) handleSensors(); });
  server_.onNotFound([this]() { if (requireAuth()) handleNotFound(); });
}


String WebConfigServer::sha256Hex(const String& value) {
  uint8_t hash[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, reinterpret_cast<const unsigned char*>(value.c_str()), value.length());
  mbedtls_sha256_finish(&ctx, hash);
  mbedtls_sha256_free(&ctx);
  char out[65];
  for (int i = 0; i < 32; ++i) snprintf(out + i * 2, 3, "%02x", hash[i]);
  out[64] = '\0';
  return String(out);
}

bool WebConfigServer::webPasswordIsSet() { return fileExists(kWebPasswordHashPath); }

bool WebConfigServer::decodeBasicCredentials(String& user, String& password) {
  String auth = server_.header("Authorization");
  if (!auth.startsWith("Basic ")) return false;
  String encoded = auth.substring(6);
  size_t outLen = 0;
  unsigned char decoded[192];
  int rc = mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &outLen,
                                 reinterpret_cast<const unsigned char*>(encoded.c_str()), encoded.length());
  if (rc != 0 || outLen == 0 || outLen >= sizeof(decoded)) return false;
  decoded[outLen] = 0;
  String pair(reinterpret_cast<char*>(decoded));
  int sep = pair.indexOf(':');
  if (sep <= 0) return false;
  user = pair.substring(0, sep);
  password = pair.substring(sep + 1);
  return true;
}

bool WebConfigServer::isAuthorized() {
  if (!webPasswordIsSet()) return true;
  String user, password;
  if (!decodeBasicCredentials(user, password)) return false;
  if (user != kWebAuthUser) return false;
  String stored = readTextFile(kWebPasswordHashPath, 128);
  stored.trim();
  return stored.length() == 64 && sha256Hex(password) == stored;
}

void WebConfigServer::sendAuthRequired() {
  server_.sendHeader("WWW-Authenticate", "Basic realm=\"StackChan\"");
  server_.send(401, "application/json", "{\"ok\":false,\"error\":\"auth_required\"}");
}

bool WebConfigServer::requireAuth() {
  if (isAuthorized()) return true;
  sendAuthRequired();
  return false;
}

void WebConfigServer::sendJson(int code, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  server_.send(code, "application/json", out);
}

String WebConfigServer::readBody() { return server_.arg("plain"); }

void WebConfigServer::handleIndex() {
  server_.send_P(200, "text/html; charset=utf-8", kIndexHtml);
}


void WebConfigServer::handleSecurityGet() {
  if (!requireAuth()) return;
  JsonDocument doc;
  doc["ok"] = true;
  doc["auth_enabled"] = webPasswordIsSet();
  doc["username"] = kWebAuthUser;
  doc["password"] = webPasswordIsSet() ? "set" : "missing";
  sendJson(200, doc);
}

void WebConfigServer::handleSecuritySave() {
  if (webPasswordIsSet() && !requireAuth()) return;
  JsonDocument in;
  DeserializationError err = deserializeJson(in, readBody());
  JsonDocument out;
  const char* password = in["web_password"] | "";
  if (err || !password || strlen(password) < 8 || strlen(password) > 96) {
    out["ok"] = false;
    out["error"] = "password_must_be_8_to_96_chars";
    sendJson(400, out);
    return;
  }
  fs_.mkdir("/app");
  fs_.mkdir("/app/StackChan");
  fs_.mkdir(kSecretDir);
  bool ok = writeTextFile(kWebPasswordHashPath, sha256Hex(String(password)) + "\n");
  out["ok"] = ok;
  out["auth_enabled"] = ok;
  out["username"] = kWebAuthUser;
  out["password"] = ok ? "set" : "save_failed";
  sendJson(ok ? 200 : 500, out);
}

void WebConfigServer::handleStatus() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["robot_id"] = config_.robotId;
  doc["gemini_voice"] = GeminiLiveProbe::supportedVoice(
      readRuntimeString("gemini_voice", GeminiLiveProbe::kDefaultVoice));
  doc["web_enabled"] = enabled_;
  doc["gateway_enabled"] = gateway_.isEnabled();
  doc["web_auth_enabled"] = webPasswordIsSet();
  doc["sd_config_dir"] = fileExists(kConfigDir) ? "present" : "missing";
  doc["gemini_api_key"] = fileExists("/app/StackChan/secrets/gemini_api_key.txt") ? "set" : "missing";
  doc["gateway_token"] = fileExists("/app/StackChan/secrets/gateway_token.txt") ? "set" : "missing";
  doc["free_heap"] = ESP.getFreeHeap();
  doc["psram_free"] = ESP.getFreePsram();
  doc["battery_mv"] = M5StackChan.getBatteryVoltage();
  doc["battery_ma"] = M5StackChan.getBatteryCurrent();
  doc["wifi_ip"] = WiFi.localIP().toString();
  doc["wifi_dns"] = WiFi.dnsIP().toString();
  doc["emotion"] = emotion_.currentEmotion();
  auto g = doc["gemini"].to<JsonObject>();
  g["ready"] = gemini_.isReady();
  g["recording"] = gemini_.isRecording();
  g["speaking"] = gemini_.isSpeaking();
  g["continuous"] = gemini_.continuousConversation();
  g["mic_ready_for_speech"] = gemini_.isMicReadyForSpeech();
  g["audio_chunks"] = gemini_.audioChunksPlayed();
  g["audio_dropped"] = gemini_.audioChunksDropped();
  g["audio_wait_ms"] = gemini_.audioBackpressureWaitMs();
  g["vad_prefix_padding_ms"] = gemini_.vadPrefixPaddingMs();
  g["vad_silence_duration_ms"] = gemini_.vadSilenceDurationMs();
  g["vad_start_sensitivity_high"] = gemini_.vadStartSensitivityHigh();
  g["vad_end_sensitivity_low"] = gemini_.vadEndSensitivityLow();
  g["vad_turn_includes_all_input"] = gemini_.vadTurnIncludesAllInput();
  auto mic = doc["mic"].to<JsonObject>();
  mic["running"] = M5.Mic.isRunning();
  mic["is_recording_queue"] = static_cast<uint32_t>(M5.Mic.isRecording());
  mic["magnification"] = M5.Mic.config().magnification;
  mic["noise_filter_level"] = M5.Mic.config().noise_filter_level;
  mic["last_rms"] = gemini_.lastMicRms();
  mic["last_peak"] = gemini_.lastMicPeak();
  mic["chunks_sent"] = gemini_.micChunksSent();
  mic["record_false_count"] = gemini_.micRecordFalseCount();
  auto cam = doc["camera"].to<JsonObject>();
  cam["enabled"] = camera_.enabled();
  cam["ready"] = camera_.ready();
  cam["last_error"] = camera_.lastError();
  cam["last_jpeg_bytes"] = camera_.lastJpegBytes();
  cam["last_base64_bytes"] = camera_.lastBase64Bytes();
  auto touch = doc["touch_sensor"].to<JsonObject>();
  auto intensities = M5StackChan.TouchSensor.getIntensities();
  touch["front"] = intensities[0];
  touch["middle"] = intensities[1];
  touch["back"] = intensities[2];
  sendJson(200, doc);
}

void WebConfigServer::handleRuntimeGet() {
  JsonDocument doc;
  doc["robot_id"] = config_.robotId;
  doc["gemini_model"] = GeminiLiveProbe::kDefaultModel;
  doc["gemini_voice"] = GeminiLiveProbe::kDefaultVoice;
  doc["wifi_enabled"] = false;
  doc["web_enabled"] = enabled_;
  doc["gemini_enabled"] = false;
  doc["gateway_enabled"] = gateway_.isEnabled();
  doc["gateway_base_url"] = "";
  doc["wifi_ssid"] = "missing";
  doc["speaker_volume"] = 200;
  doc["mic_magnification"] = M5.Mic.config().magnification;
  doc["mic_noise_filter_level"] = M5.Mic.config().noise_filter_level;
  doc["vad_prefix_padding_ms"] = gemini_.vadPrefixPaddingMs();
  doc["vad_silence_duration_ms"] = gemini_.vadSilenceDurationMs();
  doc["vad_start_sensitivity_high"] = gemini_.vadStartSensitivityHigh();
  doc["vad_end_sensitivity_low"] = gemini_.vadEndSensitivityLow();
  doc["vad_turn_includes_all_input"] = gemini_.vadTurnIncludesAllInput();

  String raw = readTextFile(kRuntimeConfigPath, 4096);
  if (raw.length()) {
    JsonDocument cfg;
    if (!deserializeJson(cfg, raw)) {
      doc["robot_id"] = cfg["robot_id"] | doc["robot_id"];
      doc["gemini_model"] = GeminiLiveProbe::upgradeLegacyModel(
          cfg["gemini_model"] | String(GeminiLiveProbe::kDefaultModel));
      doc["gemini_voice"] = GeminiLiveProbe::supportedVoice(
          cfg["gemini_voice"] | String(GeminiLiveProbe::kDefaultVoice));
      doc["wifi_enabled"] = cfg["wifi_enabled"] | false;
      doc["web_enabled"] = cfg["web_enabled"] | enabled_;
      doc["gemini_enabled"] = cfg["gemini_enabled"] | false;
      doc["gateway_enabled"] = cfg["gateway_enabled"] | gateway_.isEnabled();
      doc["gateway_base_url"] = cfg["gateway_base_url"] | "";
      doc["speaker_volume"] = cfg["speaker_volume"] | 200;
      doc["mic_magnification"] = cfg["mic_magnification"] | doc["mic_magnification"];
      doc["mic_noise_filter_level"] = cfg["mic_noise_filter_level"] | doc["mic_noise_filter_level"];
      doc["vad_prefix_padding_ms"] = cfg["vad_prefix_padding_ms"] | doc["vad_prefix_padding_ms"];
      doc["vad_silence_duration_ms"] = cfg["vad_silence_duration_ms"] | doc["vad_silence_duration_ms"];
      doc["vad_start_sensitivity_high"] = cfg["vad_start_sensitivity_high"] | doc["vad_start_sensitivity_high"];
      doc["vad_end_sensitivity_low"] = cfg["vad_end_sensitivity_low"] | doc["vad_end_sensitivity_low"];
      doc["vad_turn_includes_all_input"] = cfg["vad_turn_includes_all_input"] | doc["vad_turn_includes_all_input"];
      const char* ssid = cfg["wifi_ssid"] | "";
      doc["wifi_ssid"] = (ssid && strlen(ssid) > 0) ? "set" : "missing";
    }
  }
  doc["wifi_password"] = fileExists("/app/StackChan/secrets/wifi_password.txt") ? "set" : "missing";
  doc["gemini_api_key"] = fileExists("/app/StackChan/secrets/gemini_api_key.txt") ? "set" : "missing";
  doc["gateway_token"] = fileExists("/app/StackChan/secrets/gateway_token.txt") ? "set" : "missing";
  doc["secrets"] = "redacted";
  doc["apply"] = "reboot_required";
  sendJson(200, doc);
}

void WebConfigServer::handleRuntimeSave() {
  JsonDocument in;
  DeserializationError err = deserializeJson(in, readBody());
  JsonDocument out;
  if (err) {
    out["ok"] = false;
    out["error"] = "invalid_json";
    sendJson(400, out);
    return;
  }

  fs_.mkdir("/app");
  fs_.mkdir("/app/StackChan");
  fs_.mkdir(kConfigDir);

  JsonDocument cfg;
  String raw = readTextFile(kRuntimeConfigPath, 4096);
  if (raw.length()) deserializeJson(cfg, raw);

  cfg["robot_id"] = in["robot_id"] | (cfg["robot_id"] | config_.robotId);
  cfg["gemini_model"] = GeminiLiveProbe::upgradeLegacyModel(
      in["gemini_model"] | (cfg["gemini_model"] | String(GeminiLiveProbe::kDefaultModel)));
  cfg["gemini_voice"] = GeminiLiveProbe::supportedVoice(
      in["gemini_voice"] | (cfg["gemini_voice"] | String(GeminiLiveProbe::kDefaultVoice)));
  cfg["gateway_base_url"] = in["gateway_base_url"] | (cfg["gateway_base_url"] | "");
  cfg["wifi_enabled"] = in["wifi_enabled"] | (cfg["wifi_enabled"] | false);
  cfg["web_enabled"] = in["web_enabled"] | (cfg["web_enabled"] | enabled_);
  cfg["gemini_enabled"] = in["gemini_enabled"] | (cfg["gemini_enabled"] | false);
  cfg["gateway_enabled"] = in["gateway_enabled"] | (cfg["gateway_enabled"] | false);
  int vol = in["speaker_volume"] | (cfg["speaker_volume"] | 200);
  if (vol < 0) vol = 0;
  if (vol > 255) vol = 255;
  cfg["speaker_volume"] = vol;
  int micMag = in["mic_magnification"] | (cfg["mic_magnification"] | M5.Mic.config().magnification);
  if (micMag < 1) micMag = 1;
  if (micMag > 24) micMag = 24;
  cfg["mic_magnification"] = micMag;
  int micNoise = in["mic_noise_filter_level"] | (cfg["mic_noise_filter_level"] | M5.Mic.config().noise_filter_level);
  if (micNoise < 0) micNoise = 0;
  if (micNoise > 4) micNoise = 4;
  cfg["mic_noise_filter_level"] = micNoise;
  int vadPrefix = in["vad_prefix_padding_ms"] | (cfg["vad_prefix_padding_ms"] | gemini_.vadPrefixPaddingMs());
  if (vadPrefix < 0) vadPrefix = 0;
  if (vadPrefix > 2000) vadPrefix = 2000;
  cfg["vad_prefix_padding_ms"] = vadPrefix;
  int vadSilence = in["vad_silence_duration_ms"] | (cfg["vad_silence_duration_ms"] | gemini_.vadSilenceDurationMs());
  if (vadSilence < 100) vadSilence = 100;
  if (vadSilence > 3000) vadSilence = 3000;
  cfg["vad_silence_duration_ms"] = vadSilence;
  cfg["vad_start_sensitivity_high"] = in["vad_start_sensitivity_high"] | (cfg["vad_start_sensitivity_high"] | gemini_.vadStartSensitivityHigh());
  cfg["vad_end_sensitivity_low"] = in["vad_end_sensitivity_low"] | (cfg["vad_end_sensitivity_low"] | gemini_.vadEndSensitivityLow());
  cfg["vad_turn_includes_all_input"] = in["vad_turn_includes_all_input"] | (cfg["vad_turn_includes_all_input"] | gemini_.vadTurnIncludesAllInput());
  const char* ssid = in["wifi_ssid"] | "";
  if (ssid && strlen(ssid) > 0 && strcmp(ssid, "set") != 0 && strcmp(ssid, "missing") != 0) {
    cfg["wifi_ssid"] = ssid;
  }

  String text;
  serializeJsonPretty(cfg, text);
  bool runtimeOk = writeTextFile(kRuntimeConfigPath, text);

  JsonDocument gw;
  gw["robot_id"] = cfg["robot_id"] | config_.robotId;
  gw["gateway_base_url"] = cfg["gateway_base_url"] | "";
  String gwText;
  serializeJsonPretty(gw, gwText);
  bool gatewayOk = writeTextFile(kGatewayConfigPath, gwText);

  bool ok = runtimeOk && gatewayOk;
  out["ok"] = ok;
  out["saved"] = kRuntimeConfigPath;
  out["gateway_config_synced"] = gatewayOk;
  out["apply"] = "reboot_required";
  out["speaker_volume"] = cfg["speaker_volume"] | 200;
  out["mic_magnification"] = cfg["mic_magnification"] | 16;
  out["mic_noise_filter_level"] = cfg["mic_noise_filter_level"] | 0;
  out["vad_prefix_padding_ms"] = cfg["vad_prefix_padding_ms"] | 800;
  out["vad_silence_duration_ms"] = cfg["vad_silence_duration_ms"] | 900;
  out["vad_start_sensitivity_high"] = cfg["vad_start_sensitivity_high"] | true;
  out["vad_end_sensitivity_low"] = cfg["vad_end_sensitivity_low"] | true;
  out["vad_turn_includes_all_input"] = cfg["vad_turn_includes_all_input"] | true;
  gemini_.setVadConfig(static_cast<uint16_t>(out["vad_prefix_padding_ms"] | 800),
                       static_cast<uint16_t>(out["vad_silence_duration_ms"] | 900),
                       out["vad_start_sensitivity_high"] | true,
                       out["vad_end_sensitivity_low"] | true,
                       out["vad_turn_includes_all_input"] | true);
  out["vad_apply"] = gemini_.isReady() ? "next_gemini_session_after_reconnect" : "next_gemini_session";
  auto micCfg = M5.Mic.config();
  micCfg.magnification = static_cast<uint8_t>(out["mic_magnification"] | 16);
  micCfg.noise_filter_level = static_cast<uint8_t>(out["mic_noise_filter_level"] | 0);
  M5.Mic.config(micCfg);
  out["mic_apply"] = "immediate_and_persisted";
  out["gemini_voice"] = cfg["gemini_voice"] | GeminiLiveProbe::kDefaultVoice;
  bool reboot = in["reboot"] | false;
  out["rebooting"] = reboot;
  out["secrets"] = "redacted";
  const char* savedSsid = cfg["wifi_ssid"] | "";
  out["wifi_ssid"] = (savedSsid && strlen(savedSsid) > 0) ? "set" : "missing";
  out["wifi_password"] = fileExists("/app/StackChan/secrets/wifi_password.txt") ? "set" : "missing";
  out["gemini_api_key"] = fileExists("/app/StackChan/secrets/gemini_api_key.txt") ? "set" : "missing";
  sendJson(ok ? 200 : 500, out);
  if (ok && reboot) {
    delay(300);
    ESP.restart();
  }
}

void WebConfigServer::handleConfigGet() {
  JsonDocument doc;
  doc["robot_id"] = config_.robotId;
  doc["gateway_base_url"] = "";
  String raw = readTextFile(kGatewayConfigPath, 2048);
  if (raw.length()) {
    JsonDocument cfg;
    if (!deserializeJson(cfg, raw)) {
      doc["robot_id"] = cfg["robot_id"] | config_.robotId;
      doc["gateway_base_url"] = cfg["gateway_base_url"] | "";
    }
  }
  doc["secrets"] = "redacted";
  sendJson(200, doc);
}

void WebConfigServer::handleConfigSave() {
  JsonDocument in;
  DeserializationError err = deserializeJson(in, readBody());
  JsonDocument out;
  if (err) {
    out["ok"] = false;
    out["error"] = "invalid_json";
    sendJson(400, out);
    return;
  }
  fs_.mkdir("/app");
  fs_.mkdir("/app/StackChan");
  fs_.mkdir(kConfigDir);
  JsonDocument cfg;
  cfg["gateway_base_url"] = in["gateway_base_url"] | "";
  cfg["robot_id"] = in["robot_id"] | config_.robotId;
  String text;
  serializeJsonPretty(cfg, text);
  out["ok"] = writeTextFile(kGatewayConfigPath, text);
  out["saved"] = kGatewayConfigPath;
  sendJson(out["ok"] ? 200 : 500, out);
}

void WebConfigServer::handleSecretSave() {
  JsonDocument in;
  DeserializationError err = deserializeJson(in, readBody());
  JsonDocument out;
  if (err) {
    out["ok"] = false;
    out["error"] = "invalid_json";
    sendJson(400, out);
    return;
  }
  fs_.mkdir("/app");
  fs_.mkdir("/app/StackChan");
  fs_.mkdir(kSecretDir);
  bool wrote = false;
  const char* gemini = in["gemini_api_key"] | "";
  const char* token = in["gateway_token"] | "";
  const char* wifiPassword = in["wifi_password"] | "";
  if (gemini && strlen(gemini) > 0) {
    wrote |= writeTextFile("/app/StackChan/secrets/gemini_api_key.txt", String(gemini));
  }
  if (token && strlen(token) > 0) {
    wrote |= writeTextFile("/app/StackChan/secrets/gateway_token.txt", String(token));
  }
  if (wifiPassword && strlen(wifiPassword) > 0) {
    wrote |= writeTextFile("/app/StackChan/secrets/wifi_password.txt", String(wifiPassword));
  }
  JsonDocument meta;
  meta["ts"] = millis();
  meta["event"] = "secrets_updated";
  meta["gemini_api_key"] = (gemini && strlen(gemini) > 0) ? "set" : "unchanged";
  meta["gateway_token"] = (token && strlen(token) > 0) ? "set" : "unchanged";
  meta["wifi_password"] = (wifiPassword && strlen(wifiPassword) > 0) ? "set" : "unchanged";
  appendJsonLine(kSecretsMetaPath, meta);
  out["ok"] = true;
  out["updated"] = wrote;
  out["gemini_api_key"] = fileExists("/app/StackChan/secrets/gemini_api_key.txt") ? "set" : "missing";
  out["gateway_token"] = fileExists("/app/StackChan/secrets/gateway_token.txt") ? "set" : "missing";
  out["wifi_password"] = fileExists("/app/StackChan/secrets/wifi_password.txt") ? "set" : "missing";
  sendJson(200, out);
}

void WebConfigServer::handlePromptsGet() {
  JsonDocument doc;
  doc["system_prompt"] = readTextFile(kSystemPromptPath, 4096);
  doc["persona_prompt"] = readTextFile(kPersonaPromptPath, 4096);
  sendJson(200, doc);
}

void WebConfigServer::handlePromptsSave() {
  JsonDocument in;
  DeserializationError err = deserializeJson(in, readBody());
  JsonDocument out;
  if (err) {
    out["ok"] = false;
    out["error"] = "invalid_json";
    sendJson(400, out);
    return;
  }
  fs_.mkdir("/app");
  fs_.mkdir("/app/StackChan");
  fs_.mkdir(kPromptDir);
  String systemPrompt = String((const char*)(in["system_prompt"] | ""));
  String personaPrompt = String((const char*)(in["persona_prompt"] | ""));
  bool ok1 = writeTextFile(kSystemPromptPath, systemPrompt);
  bool ok2 = writeTextFile(kPersonaPromptPath, personaPrompt);
  if (ok1) gemini_.setSystemPrompt(systemPrompt);
  if (ok2) gemini_.setPersonaPrompt(personaPrompt);
  out["ok"] = ok1 && ok2;
  out["applies"] = gemini_.isReady() ? "next_gemini_session_after_reconnect" : "next_gemini_session";
  sendJson(out["ok"] ? 200 : 500, out);
}

void WebConfigServer::handleMemoryRecent() {
  JsonDocument doc;
  String ctx = memory_.buildContextForPrompt("", 2500);
  doc["ok"] = true;
  doc["context_preview"] = ctx;
  doc["active_context_preview"] = memory_.buildActiveDialogueContext(12000);
  doc["dialogues_preview"] = memory_.recentDialoguesPreview(6000);
  doc["note"] = "active context is injected into next Gemini Live setup; Dialogues shows raw recent log";
  memory_.addStats(doc["stats"].to<JsonObject>());
  sendJson(200, doc);
}

void WebConfigServer::handleMemoryDialogues() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["today"] = memory_.todayKey();
  doc["session"] = memory_.currentSessionId();
  doc["dialogues_preview"] = memory_.recentDialoguesPreview(10000);
  doc["note"] = "readable tail from /app/StackChan/memory/dialogues/YYYY-MM-DD.jsonl";
  sendJson(200, doc);
}

void WebConfigServer::handleMemorySummaries() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["summaries_preview"] = memory_.summariesPreview(12000);
  doc["note"] = "tail from /app/StackChan/memory/summaries.jsonl; semantic summary records contain summary_json";
  sendJson(200, doc);
}

void WebConfigServer::handleMemorySearch() {
  String query;
  if (server_.method() == HTTP_POST) {
    JsonDocument in;
    DeserializationError err = deserializeJson(in, readBody());
    if (!err) query = String((const char*)(in["query"] | ""));
  }
  if (!query.length()) query = server_.arg("q");
  query.trim();
  String result = memory_.searchMemory(query, 8, 12000);
  server_.send(200, "application/json", result);
}

void WebConfigServer::handleMemoryStats() {
  JsonDocument doc;
  doc["ok"] = true;
  memory_.addStats(doc["memory"].to<JsonObject>());
  sendJson(200, doc);
}

void WebConfigServer::handleMemorySummaryConfigGet() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["model"] = readSummaryModel();
  doc["config_path"] = kSummaryConfigPath;
  doc["gemini_api_key"] = fileExists("/app/StackChan/secrets/gemini_api_key.txt") ? "set" : "missing";
  sendJson(200, doc);
}

void WebConfigServer::handleMemorySummaryConfigSave() {
  JsonDocument in;
  JsonDocument out;
  DeserializationError err = deserializeJson(in, readBody());
  if (err) {
    out["ok"] = false;
    out["error"] = "invalid_json";
    sendJson(400, out);
    return;
  }
  String model = String((const char*)(in["model"] | "gemini-flash-latest"));
  model.trim();
  if (!model.length()) model = "gemini-flash-latest";
  fs_.mkdir("/app");
  fs_.mkdir("/app/StackChan");
  fs_.mkdir(kConfigDir);
  JsonDocument cfg;
  cfg["summary_model"] = model;
  String text;
  serializeJsonPretty(cfg, text);
  bool ok = writeTextFile(kSummaryConfigPath, text);
  out["ok"] = ok;
  out["model"] = model;
  out["saved"] = kSummaryConfigPath;
  sendJson(ok ? 200 : 500, out);
}

void WebConfigServer::handleMemorySummarize() {
  JsonDocument doc;
  String active = memory_.buildActiveDialogueContext(20000);
  active.trim();
  if (!active.length()) {
    doc["ok"] = false;
    doc["error"] = "active_context_empty";
    doc["message"] = "active context is empty";
    memory_.addStats(doc["stats"].to<JsonObject>());
    sendJson(409, doc);
    return;
  }

  JsonDocument in;
  String body = readBody();
  if (body.length()) deserializeJson(in, body);
  String model = String((const char*)(in["model"] | readSummaryModel().c_str()));
  model.trim();
  if (!model.length()) model = readSummaryModel();

  String summaryJson;
  String error;
  bool llmOk = callGeminiSummarizer(model, active, summaryJson, error);
  if (!llmOk) {
    doc["ok"] = false;
    doc["error"] = error.length() ? error : "gemini_summarizer_failed";
    doc["model"] = model;
    doc["mode"] = "semantic_summary_v2";
    doc["active_context_chars"] = active.length();
    doc["note"] = "No active_after_ts change was made because the LLM summary failed.";
    memory_.addStats(doc["stats"].to<JsonObject>());
    sendJson(502, doc);
    return;
  }

  String message;
  bool ok = memory_.commitSemanticSummary(summaryJson, model, message);
  doc["ok"] = ok;
  doc["message"] = message;
  doc["model"] = model;
  doc["mode"] = "semantic_summary_v2";
  doc["summary_json"] = summaryJson;
  doc["note"] = "Semantic summary saved only after valid JSON; private values should appear only as markers, not values.";
  memory_.addStats(doc["stats"].to<JsonObject>());
  sendJson(ok ? 200 : 500, doc);
}

void WebConfigServer::handleMemoryVectorize() {
  JsonDocument doc;
  String message;
  bool ok = memory_.queueVectorization(message);
  doc["ok"] = ok;
  doc["message"] = message;
  doc["mode"] = "manual_vectorization_queue_marker_v1";
  doc["note"] = "This queues an embedding/index marker; actual embedding API/backfill worker is the next step.";
  memory_.addStats(doc["stats"].to<JsonObject>());
  sendJson(ok ? 200 : 500, doc);
}

void WebConfigServer::handleGatewayTools() {
  JsonDocument doc;
  String tools;
  auto result = gateway_.fetchTools(&tools);
  doc["ok"] = result.ok;
  doc["gateway_enabled"] = gateway_.isEnabled();
  doc["http_code"] = result.httpCode;
  doc["error"] = result.error;
  doc["tools_json"] = tools;
  sendJson(result.ok ? 200 : 502, doc);
}

void WebConfigServer::handleVoiceToggle() {
  g_voice_toggle_requested = true;
  JsonDocument doc;
  doc["ok"] = true;
  doc["queued"] = "voice_toggle";
  sendJson(200, doc);
}

void WebConfigServer::handleGeminiText() {
  JsonDocument in;
  JsonDocument doc;
  String text;

  // Accept both the web UI JSON body and simple HTTP/manual calls such as
  // /api/gemini/text?text=hello.  Earlier query-only calls returned
  // invalid_json and could look like a wake-without-answer failure.
  if (server_.hasArg("text")) {
    text = server_.arg("text");
  } else {
    String body = readBody();
    DeserializationError err = deserializeJson(in, body);
    if (err) {
      doc["ok"] = false;
      doc["error"] = body.length() ? "invalid_json" : "missing_text";
      sendJson(400, doc);
      return;
    }
    text = String((const char*)(in["text"] | ""));
  }

  text.trim();
  if (!text.length()) {
    doc["ok"] = false;
    doc["error"] = "missing_text";
    sendJson(400, doc);
    return;
  }
  bool requested = gemini_.requestTextTurn(text);
  doc["ok"] = requested;
  doc["sent"] = gemini_.isReady();
  doc["queued"] = !gemini_.isReady();
  doc["text_len"] = text.length();
  if (!requested) doc["error"] = "gemini_request_failed";
  sendJson(requested ? (gemini_.isReady() ? 200 : 202) : 500, doc);
}

void WebConfigServer::handleCameraStatus() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["enabled"] = camera_.enabled();
  doc["ready"] = camera_.ready();
  doc["last_error"] = camera_.lastError();
  doc["last_jpeg_bytes"] = camera_.lastJpegBytes();
  doc["last_base64_bytes"] = camera_.lastBase64Bytes();
  sendJson(200, doc);
}

void WebConfigServer::handleCameraCapture() {
  JsonDocument doc;
  if (!camera_.enabled()) {
    doc["ok"] = false;
    doc["error"] = "camera_disabled_at_build";
    sendJson(501, doc);
    return;
  }
  String image;
  bool ok = camera_.captureJpegBase64(image);
  doc["ok"] = ok;
  doc["ready"] = camera_.ready();
  doc["last_error"] = camera_.lastError();
  doc["jpeg_bytes"] = camera_.lastJpegBytes();
  doc["base64_bytes"] = camera_.lastBase64Bytes();
  doc["base64_omitted"] = true;
  doc["note"] = "Smoke-test only; image bytes are not returned to avoid huge JSON responses. Use /api/camera/jpeg for direct image/jpeg retrieval.";
  sendJson(ok ? 200 : 500, doc);
}

void WebConfigServer::handleCameraJpeg() {
  if (!camera_.enabled()) {
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = "camera_disabled_at_build";
    sendJson(501, doc);
    return;
  }

  uint8_t* jpg = nullptr;
  size_t len = 0;
  bool ok = camera_.captureJpegBytes(&jpg, &len);
  if (!ok || !jpg || !len) {
    if (jpg) free(jpg);
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = camera_.lastError();
    doc["jpeg_bytes"] = camera_.lastJpegBytes();
    sendJson(500, doc);
    return;
  }

  server_.sendHeader("Cache-Control", "no-store");
  server_.sendHeader("Content-Disposition", "inline; filename=stackchan-camera.jpg");
  server_.setContentLength(len);
  server_.send(200, "image/jpeg", "");
  server_.client().write(jpg, len);
  free(jpg);
}

void WebConfigServer::handleGeminiLook() {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = "vision_temporarily_disabled";
  doc["message"] = "Direct camera-to-Gemini Live was disabled because it destabilized the voice session. /api/camera/capture remains available for smoke tests.";
  doc["camera_enabled"] = camera_.enabled();
  doc["camera_ready"] = camera_.ready();
  sendJson(503, doc);
}

void WebConfigServer::handleSensorsPage() {
  static const char kSensorsHtml[] PROGMEM = R"HTML(
<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>StackChan Sensors</title><style>
:root{font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;background:#111827;color:#e5e7eb}body{margin:0;padding:14px;max-width:920px;margin:auto}.muted{color:#9ca3af}.grid{display:grid;gap:12px}.card{background:#1f2937;border:1px solid #374151;border-radius:14px;padding:12px}.kv{display:grid;grid-template-columns:minmax(110px,1fr) minmax(90px,1fr);gap:6px 12px}.k{color:#9ca3af}.v{font:700 20px ui-monospace,SFMono-Regular,Menlo,monospace}.small{font-size:14px}.ok{color:#86efac}.bad{color:#fda4af}pre{white-space:pre-wrap;background:#0b1020;border-radius:10px;padding:10px;overflow:auto}button{border:0;border-radius:10px;background:#38bdf8;color:#082f49;padding:10px 14px;font-weight:700;margin:8px 8px 8px 0}@media(min-width:720px){.grid{grid-template-columns:1fr 1fr}.wide{grid-column:1/-1}}
</style></head><body><h1>StackChan Sensors</h1><p class="muted">Diagnostics only: does not wake, stop, or enable Vision.</p>
<button onclick="loadOnce()">Refresh</button><button onclick="toggleAuto()" id="autoBtn">Auto: off</button><button onclick="toggleAudio()" id="audioBtn">Audio: off</button> <span class="muted" id="updated"></span>
<div class="grid"><section class="card"><h2>Body touch</h2><div class="kv" id="body"></div></section><section class="card"><h2>Proximity / light</h2><div class="kv" id="ltr"></div></section><section class="card"><h2>Screen touch</h2><div class="kv" id="screen"></div></section><section class="card"><h2>IMU</h2><div class="kv" id="imu"></div></section><section class="card wide"><h2>Audio direction</h2><p class="muted">Short L/R stereo diagnostic. Does not change the Gemini mono stream; skipped if Gemini/microphone is busy.</p><div class="kv" id="audio"></div></section><section class="card wide"><h2>Gemini state</h2><div class="kv" id="state"></div></section><section class="card wide"><h2>Raw pretty JSON</h2><pre id="raw">...</pre></section></div>
<script>
let timer=null,audioOn=false;const $=id=>document.getElementById(id);function cls(v){return v===true?'ok':v===false?'bad':''}function kv(id,rows){$(id).innerHTML=rows.map(([k,v,c])=>`<div class="k">${k}</div><div class="v ${c||''}">${v??'—'}</div>`).join('')}function n(v){return typeof v==='number'?Number(v).toFixed(Math.abs(v)<10&&v!==0?3:0):v}
async function loadOnce(){const r=await fetch('/api/sensors'+(audioOn?'?audio=1':''));const d=await r.json();updated.textContent='updated '+new Date().toLocaleTimeString();kv('body',[['front',d.body_touch.front],['middle',d.body_touch.middle],['back',d.body_touch.back],['pressed',d.body_touch.pressed_raw,cls(d.body_touch.pressed_raw)]]);kv('ltr',[['ps_raw',d.ltr553.ps_raw],['als_avg',d.ltr553.als_avg],['als_ch0',d.ltr553.als_ch0],['als_ch1',d.ltr553.als_ch1],['config/read',`${d.ltr553.config_ok}/${d.ltr553.ps_ok}/${d.ltr553.als_ok}`,d.ltr553.ps_ok?'ok':'bad'],['error',d.ltr553.last_error||'—','small']]);kv('screen',[['pressed',d.screen_touch?.pressed,cls(d.screen_touch?.pressed)],['count',d.screen_touch?.count],['x',d.screen_touch?.x],['y',d.screen_touch?.y],['state',d.screen_touch?.state]]);kv('imu',[['accel_norm',n(d.imu.accel_norm)],['accel_x',n(d.imu.accel_x)],['accel_y',n(d.imu.accel_y)],['accel_z',n(d.imu.accel_z)],['gyro_x',n(d.imu.gyro_x)],['gyro_y',n(d.imu.gyro_y)],['gyro_z',n(d.imu.gyro_z)],['temp_c',n(d.imu.temp_c)]]);const a=d.audio_direction||{};kv('audio',[['enabled',audioOn,cls(audioOn)],['ok',a.ok,cls(a.ok)],['direction',a.direction_hint||a.reason||'off'],['L rms',n(a.left_rms)],['R rms',n(a.right_rms)],['balance R-L',n(a.balance_r_minus_l)],['lag samples',a.tdoa_lag_samples],['tdoa us',n(a.tdoa_us)],['corr',n(a.corr)],['L/R peak',`${a.left_peak??'—'} / ${a.right_peak??'—'}`]]);kv('state',[['speaking',d.gemini.speaking,cls(d.gemini.speaking)],['recording',d.gemini.recording,cls(d.gemini.recording)],['continuous',d.gemini.continuous,cls(d.gemini.continuous)],['emotion',d.emotion],['camera_ready',d.camera_ready,cls(d.camera_ready)]]);raw.textContent=JSON.stringify(d,null,2)}
function toggleAudio(){audioOn=!audioOn;audioBtn.textContent=audioOn?'Audio: on':'Audio: off';loadOnce()}function toggleAuto(){if(timer){clearInterval(timer);timer=null;autoBtn.textContent='Auto: off'}else{loadOnce();timer=setInterval(loadOnce,500);autoBtn.textContent='Auto: 0.5s'}}loadOnce();
</script></body></html>
)HTML";
  server_.send_P(200, "text/html; charset=utf-8", kSensorsHtml);
}

void WebConfigServer::handleSensors() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["diagnostic_only"] = true;
  doc["behavior"] = "read_values_only_no_wake_no_stop; optional_audio_diag_only_when_requested";
  doc["uptime_ms"] = millis();

  auto touch = doc["body_touch"].to<JsonObject>();
  auto intensities = M5StackChan.TouchSensor.getIntensities();
  touch["front"] = intensities[0];
  touch["middle"] = intensities[1];
  touch["back"] = intensities[2];
  touch["pressed_raw"] = (intensities[0] > 0) || (intensities[1] > 0) || (intensities[2] > 0);
  touch["note"] = "Si12T three-zone body touch; this endpoint does not call wasPressed(), so it does not consume tap edges.";

#if defined(ARDUINO_M5STACK_CORES3)
  auto screen = doc["screen_touch"].to<JsonObject>();
  auto detail = M5.Touch.getDetail();
  screen["count"] = M5.Touch.getCount();
  screen["pressed"] = (M5.Touch.getCount() > 0) || detail.isPressed();
  screen["x"] = detail.x;
  screen["y"] = detail.y;
  screen["state"] = static_cast<unsigned>(detail.state);
#endif

  auto imu = doc["imu"].to<JsonObject>();
  float ax = 0, ay = 0, az = 0, gx = 0, gy = 0, gz = 0, temp = 0;
  bool accelOk = M5.Imu.getAccel(&ax, &ay, &az);
  bool gyroOk = M5.Imu.getGyro(&gx, &gy, &gz);
  bool tempOk = M5.Imu.getTemp(&temp);
  imu["accel_ok"] = accelOk;
  imu["gyro_ok"] = gyroOk;
  imu["temp_ok"] = tempOk;
  if (accelOk) {
    imu["accel_x"] = ax;
    imu["accel_y"] = ay;
    imu["accel_z"] = az;
    imu["accel_norm"] = sqrtf(ax * ax + ay * ay + az * az);
  }
  if (gyroOk) {
    imu["gyro_x"] = gx;
    imu["gyro_y"] = gy;
    imu["gyro_z"] = gz;
  }
  if (tempOk) imu["temp_c"] = temp;

  auto ltr = doc["ltr553"].to<JsonObject>();
  ltr["addr"] = "0x23";
  ltr["configured"] = g_ltr553_configured;
  ltr["configured_ms"] = g_ltr553_configured_ms;
  bool ltrConfigOk = configureLtr553Once();
  ltr["config_ok"] = ltrConfigOk;
  ltr["configured"] = g_ltr553_configured;
  ltr["configured_ms"] = g_ltr553_configured_ms;

  uint8_t status = 0;
  uint8_t psBuf[2] = {0, 0};
  uint8_t alsBuf[4] = {0, 0, 0, 0};
  bool statusOk = ltrRead(kLtrStatus, &status, 1);
  bool psOk = ltrRead(kLtrPsData, psBuf, 2);
  bool alsOk = ltrRead(kLtrAlsDataCh1, alsBuf, 4);
  uint16_t psRaw = ((static_cast<uint16_t>(psBuf[1]) & 0x07) << 8) | psBuf[0];
  uint16_t alsCh1 = (static_cast<uint16_t>(alsBuf[1]) << 8) | alsBuf[0];
  uint16_t alsCh0 = (static_cast<uint16_t>(alsBuf[3]) << 8) | alsBuf[2];
  ltr["status_ok"] = statusOk;
  ltr["status_reg"] = status;
  ltr["ps_ok"] = psOk;
  ltr["ps_raw"] = psRaw;
  ltr["als_ok"] = alsOk;
  ltr["als_ch0"] = alsCh0;
  ltr["als_ch1"] = alsCh1;
  ltr["als_avg"] = (static_cast<uint32_t>(alsCh0) + static_cast<uint32_t>(alsCh1)) / 2;
  ltr["ps_new_data"] = statusOk ? ((status & 0x80) != 0) : false;
  ltr["als_new_data"] = statusOk ? ((status & 0x04) != 0) : false;
  ltr["last_error"] = g_ltr553_last_error;
  if (!ltrConfigOk || !statusOk || !psOk || !alsOk) {
    g_ltr553_last_error = "i2c_read_failed_or_camera_bus_busy";
    ltr["last_error"] = g_ltr553_last_error;
  }

  auto gem = doc["gemini"].to<JsonObject>();
  gem["speaking"] = gemini_.isSpeaking();
  gem["recording"] = gemini_.isRecording();
  gem["continuous"] = gemini_.continuousConversation();
  doc["emotion"] = emotion_.currentEmotion();
  doc["camera_ready"] = camera_.ready();

  if (server_.hasArg("audio") || server_.hasArg("audio_direction")) {
    bool geminiBusy = gemini_.isSpeaking() || gemini_.isRecording();
    fillAudioDirectionDiagnostic(doc["audio_direction"].to<JsonObject>(), geminiBusy);
  } else {
    auto audio = doc["audio_direction"].to<JsonObject>();
    audio["enabled"] = false;
    audio["reason"] = "add ?audio=1 or enable Audio on /sensors page";
    audio["gemini_stream"] = "mono_downmix_unchanged";
  }

  if (server_.hasArg("pretty") || server_.hasArg("pretty_json")) {
    String out;
    serializeJsonPretty(doc, out);
    server_.send(200, "application/json", out);
    return;
  }
  sendJson(200, doc);
}

void WebConfigServer::handleEmotion() {
  String requested;
  if (server_.method() == HTTP_GET) {
    requested = server_.arg("emotion");
  } else {
    JsonDocument in;
    DeserializationError err = deserializeJson(in, readBody());
    if (!err) requested = String((const char*)(in["emotion"] | ""));
  }
  if (requested.length() > 0) {
    emotion_.setEmotion(requested);
  }
  JsonDocument doc;
  doc["ok"] = true;
  doc["emotion"] = emotion_.currentEmotion();
  sendJson(200, doc);
}

void WebConfigServer::handleServoStatus() {
  JsonDocument doc;
  doc["ok"] = true;
  doc["read_only"] = true;
  auto angles = M5StackChan.Motion.getCurrentAngles();
  doc["x_angle"] = angles.x;
  doc["y_angle"] = angles.y;
  doc["moving"] = M5StackChan.Motion.isMoving();
  doc["x_moving"] = M5StackChan.Motion.isXMoving();
  doc["y_moving"] = M5StackChan.Motion.isYMoving();
  doc["gesture_active"] = servoGestures_.active();
  doc["gesture"] = servoGestures_.currentGesture();
  doc["anchor_x"] = servoGestures_.anchorX();
  doc["anchor_y"] = servoGestures_.anchorY();
  doc["note"] = "status only; movement uses POST /api/servo/move with BSP clamps: X -1280..1280, Y 0..900, speed 0..1000; anchor is the persistent look target used by relative gestures/head_motion";
  sendJson(200, doc);
}

void WebConfigServer::handleServoMove() {
  JsonDocument in;
  DeserializationError err = deserializeJson(in, readBody());
  JsonDocument out;
  if (err) {
    out["ok"] = false;
    out["error"] = "invalid_json";
    sendJson(400, out);
    return;
  }

  const char* action = in["action"] | "";
  int speed = in["speed"] | 500;
  if (speed < 0) speed = 0;
  if (speed > 1000) speed = 1000;

  auto before = M5StackChan.Motion.getCurrentAngles();
  int x = in["x"] | before.x;
  int y = in["y"] | before.y;
  if (x < -1280) x = -1280;
  if (x > 1280) x = 1280;
  if (y < 0) y = 0;
  if (y > 900) y = 900;

  bool ok = true;
  if (strcmp(action, "home") == 0) {
    M5StackChan.Motion.goHome(speed);
  } else if (strcmp(action, "move") == 0) {
    M5StackChan.Motion.move(x, y, speed);
  } else if (strcmp(action, "stop") == 0) {
    M5StackChan.Motion.stop();
  } else {
    ok = false;
    out["error"] = "unsupported_action";
    out["allowed"] = "home,move,stop";
  }

  out["ok"] = ok;
  out["action"] = action;
  out["speed"] = speed;
  out["before_x"] = before.x;
  out["before_y"] = before.y;
  out["target_x"] = x;
  out["target_y"] = y;
  out["clamped"] = true;
  sendJson(ok ? 200 : 400, out);
}

void WebConfigServer::handleServoGesture() {
  String requested;
  if (server_.method() == HTTP_GET) {
    requested = server_.arg("gesture");
  } else {
    JsonDocument in;
    DeserializationError err = deserializeJson(in, readBody());
    if (!err) requested = String((const char*)(in["gesture"] | ""));
  }
  bool queued = false;
  if (requested.length() > 0) {
    queued = servoGestures_.queueGesture(requested);
  }
  JsonDocument doc;
  doc["ok"] = queued;
  if (queued) {
    doc["queued"] = requested;
    doc["active"] = servoGestures_.currentGesture();
    doc["async"] = true;
    doc["anchor_x"] = servoGestures_.anchorX();
    doc["anchor_y"] = servoGestures_.anchorY();
  } else {
    doc["error"] = "unknown_or_missing_gesture";
    doc["allowed"] = "center_head,look_at_user,look_left_small,look_right_small,look_left,look_right,look_up,look_down,look_top_left,look_top_right,nod_yes,shake_no,curious_tilt,speaking_micro_motion,stop_motion";
  }
  sendJson(queued ? 200 : 400, doc);
}

void WebConfigServer::handleNotFound() {
  JsonDocument doc;
  doc["ok"] = false;
  doc["error"] = "not_found";
  doc["uri"] = server_.uri();
  sendJson(404, doc);
}

bool WebConfigServer::writeTextFile(const char* path, const String& value) {
  File f = fs_.open(path, FILE_WRITE);
  if (!f) return false;
  size_t n = f.print(value);
  f.close();
  return n == value.length();
}

String WebConfigServer::readTextFile(const char* path, size_t maxBytes) {
  if (!fs_.exists(path)) return String();
  File f = fs_.open(path, FILE_READ);
  if (!f) return String();
  String out;
  while (f.available() && out.length() < maxBytes) {
    out += static_cast<char>(f.read());
  }
  f.close();
  return out;
}

String WebConfigServer::readRuntimeString(const char* key, const char* fallback) {
  String raw = readTextFile(kRuntimeConfigPath, 4096);
  if (raw.length()) {
    JsonDocument doc;
    if (!deserializeJson(doc, raw)) {
      const char* value = doc[key] | fallback;
      return String(value ? value : fallback);
    }
  }
  return String(fallback ? fallback : "");
}

String WebConfigServer::readSummaryModel() {
  String raw = readTextFile(kSummaryConfigPath, 1024);
  if (raw.length()) {
    JsonDocument doc;
    if (!deserializeJson(doc, raw)) {
      const char* model = doc["summary_model"] | "gemini-flash-latest";
      return String(model ? model : "gemini-flash-latest");
    }
  }
  return "gemini-flash-latest";
}

bool WebConfigServer::callGeminiSummarizer(const String& model, const String& activeContext, String& summaryJson, String& error) {
  summaryJson = "";
  error = "";
  if (WiFi.status() != WL_CONNECTED) {
    error = "wifi_not_connected";
    return false;
  }
  String key = readTextFile("/app/StackChan/secrets/gemini_api_key.txt", 4096);
  key.trim();
  if (!key.length()) {
    error = "gemini_api_key_missing";
    return false;
  }

  String modelPath = model;
  modelPath.trim();
  if (!modelPath.length()) modelPath = "gemini-flash-latest";
  if (modelPath.startsWith("models/")) modelPath = modelPath.substring(7);
  String url = "https://generativelanguage.googleapis.com/v1beta/models/" + modelPath + ":generateContent?key=" + key;

  String prompt =
      "You are the maintenance summarizer for a small StackChan robot memory system. Return ONLY valid JSON, without markdown. "
      "Your goal is not maximum compression; preserve enough context for future recall/RAG. Write in the user's language when it is clear, otherwise use English. "
      "Create a rich episodic summary: who asked what, what the robot did, what it saw/found, where an object/person was, what it looked like, what it was wearing, counts of objects/people, user corrections, errors, and final outcomes. "
      "For visual/camera/search episodes, preserve observable details: counts, location, object/clothing colors, pose/action, search direction/sector, and user corrections. These details are not noise. "
      "In facts, add separate atomic searchable facts: one fact equals one verifiable detail. Do not limit output to two or three facts when several important details exist. "
      "Use discarded_noise only for true noise such as greetings, filler, or repeated text without new information. Do not discard observations, colors, locations, numbers, or user corrections. "
      "Preserve exact ordinary numbers, dates, times, amounts, versions, model IDs, file names, endpoint paths, IP/URL values, hashes, sizes, and technical parameters when important. Do not round or approximate them. "
      "Do not write private secrets/PIN/password/token/private code/address values verbatim into public summary/facts. For them, write only a safe marker: {kind,label,stored_elsewhere:true}. "
      "If a value is already replaced by a PRIVATE_* marker, do not try to reconstruct it. "
      "JSON schema fields: summary string, facts array of objects with text/tags/confidence, open_threads array, private_markers array, discarded_noise array. "
      "Example keys must be exactly: summary, facts, open_threads, private_markers, discarded_noise.\n\nACTIVE_CONTEXT:\n";
  prompt += activeContext;

  JsonDocument req;
  auto contents = req["contents"].to<JsonArray>();
  auto content = contents.add<JsonObject>();
  auto parts = content["parts"].to<JsonArray>();
  parts.add<JsonObject>()["text"] = prompt;
  req["generationConfig"]["temperature"] = 0.1;
  req["generationConfig"]["responseMimeType"] = "application/json";

  String payload;
  serializeJson(req, payload);

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);
  http.setReuse(false);
  if (!http.begin(client, url)) {
    error = "http_begin_failed";
    return false;
  }
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(payload);
  String body = http.getString();
  http.end();
  if (code < 200 || code >= 300) {
    error = "gemini_http_" + String(code) + ":" + body.substring(0, 240);
    return false;
  }

  JsonDocument resp;
  if (deserializeJson(resp, body) != DeserializationError::Ok) {
    error = "gemini_response_invalid_json";
    return false;
  }
  String text = String((const char*)(resp["candidates"][0]["content"]["parts"][0]["text"] | ""));
  text.trim();
  if (text.startsWith("```")) {
    int firstNl = text.indexOf('\n');
    int lastFence = text.lastIndexOf("```");
    if (firstNl >= 0 && lastFence > firstNl) text = text.substring(firstNl + 1, lastFence);
    text.trim();
  }
  if (!text.length()) {
    error = "gemini_summary_text_empty";
    return false;
  }
  JsonDocument check;
  if (deserializeJson(check, text) != DeserializationError::Ok) {
    error = "summary_text_not_valid_json";
    return false;
  }
  summaryJson = text;
  return true;
}

bool WebConfigServer::appendJsonLine(const char* path, JsonDocument& doc) {
  File f = fs_.open(path, FILE_APPEND);
  if (!f) return false;
  serializeJson(doc, f);
  f.print('\n');
  f.close();
  return true;
}

bool WebConfigServer::fileExists(const char* path) {
  return fs_.exists(path);
}
