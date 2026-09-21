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

Every network the robot may join is defined in
`secrets/wifi_networks.json`, one entry per network carrying both the SSID and
its password, so adding a network means editing one file:

```json
[
  {"ssid": "HOME", "password": "HOME_PASSWORD"},
  {"ssid": "OFFICE", "password": "OFFICE_PASSWORD"}
]
```

At connect time the robot scans and joins whichever of them is strongest, so
the same card works in more than one place without being edited.

Up to 8 networks. An entry with an empty password is skipped rather than
attempted. No SSID or password is ever written to the logs, and the API
reports SSIDs only, never the passwords beside them.

`runtime.json` holds no passwords and no network list, only the roam margin:

```json
{
  "wifi_roam_margin_db": 8
}
```

## Spoken language

`transcription_language_codes` is a comma separated list of BCP-47 hints
telling speech recognition which languages to expect:

```json
{
  "transcription_language_codes": "ru-RU,en-US"
}
```

Leave it empty and the language is guessed from the audio alone. On a
one-word reply there is very little to guess from, and a mis-identified
language is transcribed as that language: a short Russian phrase has come back
as Spanish, and the model then answers in Spanish, because that is faithfully
what it was handed. These are hints rather than a restriction, so other
languages still transcribe; pin the reply language in `prompts/system.txt` as
well if it matters.

## Clock

Nothing else in the firmware sets the clock. Without it `getLocalTime` fails
and the memory store names every file `undated` and every session
`boot-<millis>`, and those ids collide because the value is the same few
hundred milliseconds on every boot.

```json
{
  "timezone": "UTC0",
  "ntp_server": "pool.ntp.org"
}
```

`timezone` is a POSIX TZ string, not an IANA name like `Asia/Yerevan`. POSIX
counts west as positive, so the offset looks inverted: UTC+4 is written `-4`,
giving `<+04>-4`. A zone with DST needs its rules spelled out, as in
`CET-1CEST,M3.5.0,M10.5.0/3`. The Web UI derives the right string from the
browser, which is easier than writing one by hand.

Getting the timezone wrong only shifts when the memory files roll over to a
new day; record timestamps are stored in UTC regardless. Setting `ntp_server`
to an empty string disables the sync.

After connecting, the robot re-checks roughly every two minutes and moves only
when another configured network is stronger by at least `wifi_roam_margin_db`
(default 8, clamped to 3-30). The margin is what stops two overlapping routers
trading the connection back and forth as signals waver. Because a scan briefly
interrupts traffic, it runs only while no Gemini session is active, so a
conversation is never cut short to look for a better router. A dropped link is
reconnected regardless of that, after a short grace period.

### Cards written before this

`wifi_ssid` in `runtime.json` and `secrets/wifi_password.txt` still work and
need no migration. That SSID is always included as the primary network, and
its password is read from `wifi_password.txt` when the SSID is absent from
`wifi_networks.json`. New setups should use `wifi_networks.json` alone.
