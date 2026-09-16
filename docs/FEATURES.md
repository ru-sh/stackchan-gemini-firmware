# Feature matrix

## Standalone firmware

Available without a gateway, when Wi-Fi/Gemini are configured:

- Gemini Live voice conversation.
- Grounding with Google Search during the voice conversation (`gemini_search_grounding`, on by default).
- Procedural face/emotions.
- Touch/screen wake and voice toggle paths.
- Camera capture smoke test and JPEG endpoint.
- Servo status, bounded moves, and gestures.
- SD-backed runtime config and prompts.
- Local memory event/dialogue/summary inspection.
- Web UI/API with optional Basic Auth and setup access point fallback.
- Sensor diagnostics page/API.

## Requires SD configuration

- Wi-Fi credentials.
- Gemini API key.
- Web UI enablement.
- Custom prompts.
- Runtime VAD/mic/voice settings.

## Optional LAN gateway

When `gateway_enabled=true`, the firmware can call a configured gateway for external tools. This is intended for private/local integrations, for example:

- Hermes assistant bridge.
- Home Assistant bridge.
- Current facts/search bridge.
- Other allowlisted local tools.

Gateway support is off by default and should be documented/deployed separately from the firmware.

## Known limitations

- Setup access point is intentionally simple: open AP, no captive DNS portal, and reboot required after saving Wi-Fi credentials.
- This is a developer/hobbyist release, not a polished consumer installer.
- A clean firmware flash does not wipe private SD state. For public handoff, clean SD secrets, memory, schedules, traces, prompts, and private skills separately.
- Web Basic Auth is not a substitute for network isolation.
