# AGENT.md

## Project

- Repo: `cardputer-whip`
- Type: PlatformIO project for ESP32
- Board/env: `cardputer-whip`
- Platform: `espressif32`
- Board: `esp32-s3-devkitc-1`
- Framework: `espidf`

This repo targets an M5Stack Cardputer-class device built around ESP32-S3. The current `platformio.ini` uses ESP-IDF, 8 MB flash, `huge_app.csv`, and defines `BOARD_M5STACK_CARDPUTER`.

## Current State

- `src/` is effectively empty.
- The main implementation brief lives in `docs/cardputer_agora_whip_codex_task.md`.
- The intended direction is an audio-only Agora WHIP publisher MVP on ESP-IDF.

## Source of Truth

Read these first before making changes:

1. `platformio.ini`
2. `docs/cardputer_agora_whip_codex_task.md`

If the doc and repo differ, prefer the checked-in repo configuration unless the user explicitly asks to realign them.

## Expected Milestones

Implement in this order:

1. Buildable/flasheable ESP-IDF baseline
2. Wi-Fi connect and print IP
3. HTTPS GET baseline
4. Integrate `esp-webrtc-solution` / `esp_peer`
5. Create local audio-only SDP offer
6. WHIP POST offer and parse SDP answer
7. Reach connected WebRTC state
8. Publish fake/silent audio
9. Only then integrate the real Cardputer microphone path

Do not jump straight to I2S microphone work before fake-audio WHIP publishing is proven.

## Expected Layout

The design brief expects a structure like:

```text
src/
  main.c
  app_config.h
  app_wifi.c
  app_wifi.h
  app_https.c
  app_https.h
  app_whip.c
  app_whip.h
  app_webrtc.c
  app_webrtc.h
  app_audio_fake.c
  app_audio_fake.h

components/
  ...
```

Treat this as the preferred direction unless the user asks for a different layout.

## Build And Verification

Primary commands:

```bash
pio run
pio run -t upload
pio device monitor
```

Default serial settings from `platformio.ini`:

- Monitor speed: `115200`
- Upload speed: `115200`

When making meaningful firmware changes, prefer to at least run `pio run` before closing out. If upload or runtime verification is not possible, say so explicitly.

## Configuration Rules

- Do not commit real Wi-Fi credentials, Agora tokens, or WHIP endpoint secrets.
- Put placeholders in tracked files.
- Prefer moving secrets later to `sdkconfig`, NVS, or a local ignored header/config file.
- If adding, removing, or changing config macros that users may set locally, update `src/app_config.local.example.h` in the same change so the example stays in sync with the real configurable surface.

## Implementation Notes

- Keep the first working network/WebRTC path minimal and observable in logs.
- Favor small C modules with clear headers over one large `main.c`.
- Log milestones clearly so serial output can confirm progress on-device.
- Be cautious with memory use; TLS and WebRTC setup can be tight on ESP32-S3.

## Change Policy

- Do not touch unrelated projects outside this repo.
- Do not revert user changes unless explicitly asked.
- If adding external components, keep them isolated under `components/` or another clearly scoped location.
