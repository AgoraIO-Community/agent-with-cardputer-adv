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

Hardware-verified status on May 9, 2026:
- protocol config fetch succeeds
- RTSA joins successfully
- `startAgent` succeeds
- fake audio frames are sent continuously
- Cardputer ADV microphone capture works on hardware
- live mic frames are sent continuously over the G.711A path
- remote agent audio is received and played through the ADV speaker path

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

## What The Firmware Does Today

1. Connects to Wi-Fi and logs IP info
2. Syncs system time for JWT expiry correctness
3. Requests runtime config from the protocol server
4. Joins the RTSA channel with the returned token and IDs
5. Starts or stops the remote agent through `/v2/startAgent` and `/v2/stopAgent`
6. Plays received remote audio through the Cardputer ADV ES8311 speaker path when playback is enabled
7. Sends fake audio frames by default, or can start the real Cardputer ADV I2S mic path when `APP_AUDIO_USE_I2S_MIC` is enabled
8. Uses G.711A as the stable codec baseline for live mic publish
9. Auto-tunes Cardputer ADV mic framing across the known slot / inversion variants if the first capture mode looks constant-valued

## Expected Serial Milestones

On a working run, the monitor should show lines like:

- `Wi-Fi connected`
- `IP address: ...`
- `Protocol config acquired: ...`
- `RTSA session started: ...`
- `Joined RTC channel successfully: ...`
- `Agent start request accepted`
- `Fake audio frames sent: 250`

For remote speaker playback, you should also see lines like:

- `Cardputer ADV speaker playback ready; waiting for remote audio`
- `Remote audio stream callback fired: codec=1 sample_rate=8000 channels=1`
- `Opened Cardputer playback codec on bck=41 ws=43 dout=42 ...`
- `First remote audio frame written to speaker: ...`

For the real mic path, you should instead see lines like:

- `Initialized Cardputer ADV mic I2S RX on bck=41 ws=43 data=46 ... slot=... ws_inv=... bclk_inv=...`
- `Cardputer mic audio publisher started`
- `Mic audio frames sent: 250 ...`
- `Mic profile: frames=250 avg_us=... max_us=... deadline_miss=0 send_fail=0 ...`

If the initial ADV framing is wrong, you may also see lines like:

- `Auto-tuning Cardputer mic mode: switching from ... to ...`
- `Cardputer mic mode appears non-constant, keeping ...`

## Known Limitations

- The display is not used yet.
- Local Wi-Fi and protocol values must be supplied through `src/app_config_local.h`.
- The stable live-mic path currently uses `APP_AUDIO_CODEC_G711A`.
- Full-duplex mic plus playback on this board remains sensitive because the ADV audio path shares hardware resources and memory is tight without PSRAM.
- The tracked example config is intentionally set to the known-good G.711A baseline without pull playback enabled by default.

## Next Step

Use the known-good Cardputer ADV mic baseline:

1. Set these in `src/app_config_local.h`:
   - `APP_AUDIO_USE_I2S_MIC 1`
   - `APP_AUDIO_CODEC APP_AUDIO_CODEC_G711A`
   - `APP_AUDIO_I2S_USE_CARDPUTER_ADV 1`
   - `APP_AUDIO_I2S_PORT I2S_NUM_1`
   - `APP_AUDIO_I2S_MIC_BCK_GPIO GPIO_NUM_41`
   - `APP_AUDIO_I2S_MIC_CLK_GPIO GPIO_NUM_43`
   - `APP_AUDIO_I2S_MIC_DATA_GPIO GPIO_NUM_46`
   - `APP_AUDIO_I2S_ADV_I2C_PORT I2C_NUM_1`
   - `APP_AUDIO_I2S_ADV_I2C_SDA_GPIO GPIO_NUM_8`
   - `APP_AUDIO_I2S_ADV_I2C_SCL_GPIO GPIO_NUM_9`
   - `APP_AUDIO_I2S_CAPTURE_RATE 16000`
2. Upload and monitor
3. Confirm logs for:
   - `Cardputer mic audio publisher started`
   - `Initialized Cardputer ADV mic I2S RX ...`
   - `Mic audio frames sent: ... avg_abs=... peak=...`
   - `Protocol config acquired: ...`
   - `Joined RTC channel successfully: ...`
   - `Remote audio stream callback fired: ...`
   - `First remote audio frame written to speaker: ...`
4. If the mic frames flow but remote audio quality still needs work, continue tuning the stable G.711A path first.
