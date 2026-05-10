# Cardputer RTSA Audio Client

ESP32-S3 / M5Stack Cardputer audio client using Agora RTSA plus a local protocol server for config and agent control.

## Current Stage

- [x] M0: ESP-IDF PlatformIO project builds and uploads
- [x] M1: Wi-Fi connects and prints IP
- [x] M2: HTTPS request works
- [x] M3: protocol `get_config` works
- [x] M4: RTSA join works
- [x] M5: `startAgent` / `stopAgent` control works
- [x] M6: fake audio publisher sends frames over the RTSA path
- [x] M7: real Cardputer microphone integration on the stable G.711A path
- [x] M8: pull playback works on the Cardputer ADV speaker path
- [x] M9: official half-duplex audio switching for shared mic/speaker hardware

Hardware-verified status on May 9, 2026:
- protocol config fetch succeeds
- RTSA joins successfully
- `startAgent` succeeds
- fake audio frames are sent continuously
- Cardputer ADV microphone capture works on hardware
- live mic frames are sent continuously over the G.711A path
- remote agent audio is received and played through the ADV speaker path
- remote playback now switches mic/speaker ownership instead of keeping both active

## Build

```bash
~/.platformio/penv/bin/pio run
```

## Upload

```bash
~/.platformio/penv/bin/pio run -t upload
```

Cardputer upload mode:

1. Unplug USB
2. Switch power OFF
3. Hold `G0`
4. Plug USB
5. Start upload
6. Release `G0` after writing begins

If upload fails with a boot mode error, keep holding `G0` until upload completes.

## Monitor

```bash
~/.platformio/penv/bin/pio device monitor
```

## Local Config

Tracked files contain placeholders only.

For local development:

1. Copy `src/app_config.local.example.h` to `src/app_config_local.h`
2. Fill in Wi-Fi and protocol values
3. Build and upload normally

`src/app_config_local.h` is git-ignored and overrides the placeholder defaults in `src/app_config.h`.

For a new Cardputer ADV setup, start from the checked-in template:

```bash
cp src/app_config.local.example.h src/app_config_local.h
```

Then edit:

- `APP_WIFI_SSID` / `APP_WIFI_PASSWORD`
- `APP_PROTOCOL_BASE_URL`, for example `http://192.168.0.101:8000`

The template enables the hardware-verified path:

- G.711A / PCMA app-side encode/decode
- keyboard-triggered `startAgent` / `stopAgent`
- pull playback through the Cardputer ADV speaker
- half-duplex mic/speaker switching on shared I2S pins
- Cardputer ADV mic capture on BCK `41`, WS `43`, DATA `46`
- ES8311 I2C control on SDA `8`, SCL `9`

Mic gain defaults to `APP_AUDIO_I2S_GAIN_NUM 6` in the template. If remote listeners still hear you quietly, try `8`; if your voice clips, try `4` or `5`. Speaker gain defaults to `APP_AUDIO_PLAYBACK_GAIN_NUM 24`, while firmware caps the effective gain and applies soft limiting to keep playback from sounding harsh.

## What The Firmware Does Today

1. Connects to Wi-Fi and logs IP info
2. Syncs system time for JWT expiry correctness
3. Requests runtime config from the protocol server
4. Joins the RTSA channel with the returned token and IDs
5. Starts or stops the remote agent through `/v2/startAgent` and `/v2/stopAgent`
6. Plays received remote audio through the Cardputer ADV ES8311 speaker path when playback is enabled
7. Uses controller-managed half-duplex switching on shared audio hardware: mic active in record mode, speaker active in playback mode
8. Sends fake audio frames by default, or can start the real Cardputer ADV I2S mic path when `APP_AUDIO_USE_I2S_MIC` is enabled
9. Uses G.711A as the stable codec baseline for live mic publish
10. Auto-tunes Cardputer ADV mic framing across the known slot / inversion variants if the first capture mode looks constant-valued

## Expected Serial Milestones

On a working run, the monitor should show lines like:

- `Wi-Fi connected`
- `IP address: ...`
- `Protocol config acquired: ...`
- `RTSA session started: ...`
- `Joined RTC channel successfully: ...`
- `Agent start request accepted`
- `Audio mode -> RECORDING reason=start`

For remote speaker playback, you should also see lines like:

- `Cardputer ADV speaker playback ready; waiting for remote audio`
- `Remote audio stream callback fired: codec=1 sample_rate=8000 channels=1`
- `Opened direct Cardputer playback I2S/DAC path on bck=41 ws=43 dout=42 ...`
- `First remote audio frame written to speaker: ...`
- `Audio mode -> PLAYBACK`
- `Silence uplink frames sent during playback: ...`
- `Audio mode -> RECORDING`

For the real mic path, you should instead see lines like:

- `Initialized Cardputer ADV mic I2S RX on bck=41 ws=43 data=46 ... slot=... ws_inv=... bclk_inv=...`
- `Cardputer mic audio publisher started`
- `Mic frames processed=250 sent=250 ... raw_avg_abs=... avg_abs=... gain=6/1 knee=9000 limit=22000 ...`
- `Mic profile: frames=250 avg_us=... max_us=... deadline_miss=0 send_fail=0 ...`

If the initial ADV framing is wrong, you may also see lines like:

- `Auto-tuning Cardputer mic mode: switching from ... to ...`
- `Cardputer mic mode appears non-constant, keeping ...`

## Known Limitations

- The display is not used yet.
- Local Wi-Fi and protocol values must be supplied through `src/app_config_local.h`.
- The stable live-mic path currently uses `APP_AUDIO_CODEC_G711A`.
- The board now runs in logical half-duplex for shared mic/speaker hardware. During playback, the firmware sends silence uplink frames instead of keeping the mic hardware active.
- Half-duplex gating and pull playback are enabled in the tracked local example config for Cardputer ADV.

## New User Checklist

Use the known-good Cardputer ADV baseline:

1. Copy the example config:
   ```bash
   cp src/app_config.local.example.h src/app_config_local.h
   ```
2. Set these required values in `src/app_config_local.h`:
   - `APP_WIFI_SSID`
   - `APP_WIFI_PASSWORD`
   - `APP_PROTOCOL_BASE_URL`
3. Keep these Cardputer ADV audio settings unless you are intentionally experimenting:
   - `APP_AUDIO_USE_I2S_MIC 1`
   - `APP_AUDIO_CODEC APP_AUDIO_CODEC_G711A`
   - `APP_AGORA_PROTOCOL_OUTPUT_AUDIO_CODEC "pcma"`
   - `APP_AGORA_START_ON_KEYPRESS 1`
   - `APP_AUDIO_ENABLE_PULL_PLAYBACK 1`
   - `APP_AUDIO_ENABLE_HALF_DUPLEX_GATING 1`
   - `APP_AUDIO_I2S_USE_CARDPUTER_ADV 1`
   - `APP_AUDIO_I2S_PORT I2S_NUM_1`
   - `APP_AUDIO_I2S_MIC_PORT I2S_NUM_0`
   - `APP_AUDIO_I2S_MIC_BCK_GPIO GPIO_NUM_41`
   - `APP_AUDIO_I2S_MIC_CLK_GPIO GPIO_NUM_43`
   - `APP_AUDIO_I2S_MIC_DATA_GPIO GPIO_NUM_46`
   - `APP_AUDIO_I2S_ADV_I2C_PORT I2C_NUM_1`
   - `APP_AUDIO_I2S_ADV_I2C_SDA_GPIO GPIO_NUM_8`
   - `APP_AUDIO_I2S_ADV_I2C_SCL_GPIO GPIO_NUM_9`
   - `APP_AUDIO_I2S_CAPTURE_RATE 16000`
   - `APP_AUDIO_I2S_GAIN_NUM 6`
   - `APP_AUDIO_PLAYBACK_GAIN_NUM 24`
4. Build, upload, and monitor
5. Press `k` on the Cardputer keyboard to start the agent
6. Confirm logs for:
   - `Cardputer mic audio publisher started`
   - `Initialized Cardputer ADV mic I2S RX ...`
   - `Mic frames processed=... raw_avg_abs=... avg_abs=... gain=6/1 ...`
   - `Protocol config acquired: ...`
   - `Joined RTC channel successfully: ...`
   - `Remote audio stream callback fired: ...`
   - `First remote audio frame written to speaker: ...`
7. If remote listeners hear you too quietly, raise `APP_AUDIO_I2S_GAIN_NUM` to `8`. If they hear clipping, lower it to `4` or `5`.
