# SD card layout

The firmware stores mutable runtime state on the SD card under `/app/StackChan/`.

## Directory tree

```text
/app/StackChan/
  config/
    runtime.json
    gateway.json
    summary.json
  prompts/
    system.txt
    persona.txt
  secrets/
    gemini_api_key.txt
    wifi_password.txt
    wifi_networks.json
    gateway_token.txt
    web_password_sha256.txt
    meta.jsonl
  memory/
    events/YYYY-MM-DD.jsonl
    dialogues/YYYY-MM-DD.jsonl
    summaries.jsonl
    memories.jsonl
    profile.json
  camera/
    latest.jpg
```

The firmware creates some directories automatically after SD mount. For first configuration, either prepare the files manually or use the setup access point Web UI after flashing.

## Minimal Wi-Fi + Web UI setup

Create:

```text
/app/StackChan/config/runtime.json
/app/StackChan/secrets/wifi_password.txt
```

Example `runtime.json`:

```json
{
  "robot_id": "stackchan",
  "wifi_enabled": true,
  "wifi_ssid": "YOUR_WIFI_SSID",
  "web_enabled": true,
  "gemini_enabled": false,
  "gateway_enabled": false
}
```

Example `wifi_password.txt`:

```text
YOUR_WIFI_PASSWORD
```

## Multiple Wi-Fi networks

List every network the robot may join in `wifi_networks`. At connect time it
scans and joins whichever of them is strongest, so the same SD card works in
more than one place without being edited.

```json
{
  "wifi_ssid": "YOUR_WIFI_SSID",
  "wifi_networks": ["YOUR_WIFI_SSID", "YOUR_SECOND_WIFI_SSID"],
  "wifi_roam_margin_db": 8
}
```

`wifi_ssid` remains the primary network and is always included, so a card
written before this feature keeps working with no changes.

Passwords stay out of `runtime.json`. The primary network continues to use
`wifi_password.txt`; every other network takes its password from
`wifi_networks.json`, keyed by SSID:

```json
{
  "YOUR_SECOND_WIFI_SSID": "SECOND_WIFI_PASSWORD"
}
```

A network with no password is skipped rather than attempted.

After connecting, the robot re-checks roughly every two minutes and moves only
when another configured network is stronger by at least `wifi_roam_margin_db`
(default 8, clamped to 3-30). The margin is what stops two overlapping routers
trading the connection back and forth as signals waver. Because a scan briefly
interrupts traffic, it runs only while no Gemini session is active, so a
conversation is never cut short to look for a better router. A dropped link is
reconnected regardless of that, after a short grace period.

Boot the robot. If station Wi-Fi connects, read the IP from serial logs and open `http://ROBOT_IP/`. If credentials are missing or connection fails, join the open `<robot_id>-setup` access point and open `http://192.168.4.1/`. The Web UI can save Wi-Fi SSID/password and other settings; reboot after saving network changes.

## Gemini setup

Add:

```text
/app/StackChan/secrets/gemini_api_key.txt
```

and enable Gemini in `runtime.json`:

```json
{
  "gemini_enabled": true,
  "gemini_model": "models/gemini-3.8-live",
  "gemini_voice": "Puck",
  "gemini_search_grounding": true
}
```

`gemini_search_grounding` turns on Grounding with Google Search, which the
Live API runs alongside the robot's own tools rather than instead of them.
It defaults to `true`; set it to `false` to keep the session offline apart
from Gemini itself. Search queries leave the device, so the system prompt
forbids putting private values or local-memory details into one.

`gemini_model` written by older firmware (`models/gemini-3.1-flash-live-preview`)
is upgraded to `models/gemini-3.8-live` when the config is read, so an existing
SD card needs no manual edit. Any other value you set is left untouched.

`gemini_voice` accepts the prebuilt voices this model offers: `Puck`, `Charon`,
`Kore`, `Fenrir`, `Aoede`. Anything else falls back to `Puck` instead of being
rejected by the API at session setup.

You can also edit prompts:

```text
/app/StackChan/prompts/system.txt
/app/StackChan/prompts/persona.txt
```

## Optional gateway setup

Gateway integration is optional. Add `/app/StackChan/config/gateway.json` or set the equivalent fields in `runtime.json`:

```json
{
  "gateway_enabled": true,
  "gateway_base_url": "http://YOUR_GATEWAY_HOST:8811/stackchan",
  "robot_id": "stackchan"
}
```

If the gateway requires a token, store it in:

```text
/app/StackChan/secrets/gateway_token.txt
```

## Secrets policy

Never commit real files from `/app/StackChan/secrets/` or private runtime SD dumps. The Web/API status endpoints redact secret values as `set` or `missing`.
