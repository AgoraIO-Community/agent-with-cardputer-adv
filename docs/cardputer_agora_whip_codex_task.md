# Codex Task: Build Cardputer Agora WHIP Publisher MVP

## Background

We are using an **M5Stack Cardputer**, based on **ESP32-S3 / M5StampS3**. The hardware flashing path has already been verified with PlatformIO using Arduino framework and a simple Hello World screen demo.

Now create a separate **PlatformIO + ESP-IDF** project for a direct-to-Agora WHIP publisher.

Target architecture:

```text
M5Stack Cardputer / ESP32-S3
→ PlatformIO + ESP-IDF
→ esp-webrtc-solution / esp_peer
→ custom WHIP signaling
→ audio-only WebRTC publisher
→ Agora WHIP endpoint
```

Use `esp-webrtc-solution` / `esp_peer` as the WebRTC stack.

WHIP is only the WebRTC signaling layer. Media still flows over WebRTC ICE / DTLS / SRTP.

---

## Important Constraints

Do **not** start by implementing the Cardputer microphone.

First build a working staged MVP:

```text
M0: ESP-IDF project builds and flashes
M1: Wi-Fi connects and prints IP
M2: HTTPS request works
M3: esp_peer / esp-webrtc-solution compiles into project
M4: create audio-only SDP offer
M5: implement WHIP POST offer → receive SDP answer
M6: set remote answer and reach WebRTC connected state
M7: publish fake/silent audio
M8: only then integrate Cardputer I2S microphone
```

The first working demo may use fake audio / silence. The purpose is to prove WHIP + WebRTC compatibility before debugging I2S audio.

---

## Project Name

Create a new project:

```text
cardputer-whip
```

Do not modify the previous Arduino Hello World project.

---

## PlatformIO Setup

Create `platformio.ini`:

```ini
[env:cardputer-whip]
platform = espressif32
board = esp32-s3-devkitc-1
framework = espidf

monitor_speed = 115200
upload_speed = 115200

board_build.flash_size = 8MB
board_build.partitions = partitions.csv

build_flags =
  -D BOARD_M5STACK_CARDPUTER
  -D CONFIG_LOG_DEFAULT_LEVEL=3
```

Create `partitions.csv`:

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 0x700000,
```

Create `sdkconfig.defaults`:

```ini
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096
CONFIG_FREERTOS_HZ=1000
CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN=16384
CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN=4096
CONFIG_LOG_DEFAULT_LEVEL_INFO=y
```

---

## Directory Structure

Create this structure:

```text
cardputer-whip/
  platformio.ini
  partitions.csv
  sdkconfig.defaults

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
    README.md
```

Later, `components/` should contain `esp-webrtc-solution` or the required subset/components.

---

## Configuration

Create `src/app_config.h`:

```c
#pragma once

#define APP_WIFI_SSID       "CHANGE_ME"
#define APP_WIFI_PASSWORD   "CHANGE_ME"

#define APP_AGORA_WHIP_URL  "https://CHANGE_ME"
#define APP_AGORA_TOKEN     "CHANGE_ME"

#define APP_AUDIO_SAMPLE_RATE 48000
#define APP_AUDIO_CHANNELS    1
#define APP_AUDIO_FRAME_MS    20
```

Do not hardcode real secrets in committed files. Add a comment that these should later move to `sdkconfig`, NVS, or a local ignored header.

---

## M0 / M1: ESP-IDF Wi-Fi Baseline

Implement:

```text
src/app_wifi.c
src/app_wifi.h
```

Requirements:

1. Initialize NVS.
2. Initialize TCP/IP network interface.
3. Connect to Wi-Fi station mode.
4. Wait until connected.
5. Print local IP.
6. Reconnect on disconnect.

Expose:

```c
esp_err_t app_wifi_start(void);
```

`main.c` should initially be:

```c
void app_main(void) {
    app_wifi_start();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

Acceptance criteria:

```bash
pio run
pio run -t upload
pio device monitor
```

Serial monitor shows:

```text
Wi-Fi connected
IP address: x.x.x.x
```

---

## M2: HTTPS Baseline

Implement:

```text
src/app_https.c
src/app_https.h
```

Expose:

```c
esp_err_t app_https_get_test(const char *url);
```

Use `esp_http_client`.

Purpose:

1. Verify TLS works.
2. Verify DNS works.
3. Verify heap is sufficient.

Use a simple GET to a public HTTPS endpoint first, then make this function reusable for WHIP POST.

Acceptance criteria:

```text
HTTPS GET completed with 2xx/3xx response
No TLS allocation crash
```

---

## M3: Add esp-webrtc-solution / esp_peer

Add Espressif WebRTC components. Preferred approach:

```bash
git submodule add https://github.com/espressif/esp-webrtc-solution.git components/esp-webrtc-solution
```

Then inspect the repository and include only what is needed if full integration is too heavy.

Relevant components:

```text
esp_peer
esp_webrtc
esp_capture only if needed later
codec-related components only if needed
```

Do not enable video.

Do not enable rendering.

Do not enable data channel unless required by the component.

Acceptance criteria:

```text
Project still builds with WebRTC components included.
No unused video/camera/display dependency is required.
```

---

## M4: Create Audio-only SDP Offer

Implement:

```text
src/app_webrtc.c
src/app_webrtc.h
```

Expose:

```c
typedef struct {
    char *sdp_offer;
} app_webrtc_offer_t;

esp_err_t app_webrtc_init(void);
esp_err_t app_webrtc_create_audio_offer(app_webrtc_offer_t *out_offer);
esp_err_t app_webrtc_set_remote_answer(const char *sdp_answer);
esp_err_t app_webrtc_start(void);
void app_webrtc_destroy_offer(app_webrtc_offer_t *offer);
```

Requirements:

1. Initialize WebRTC/peer connection.
2. Create one audio sender/track only.
3. Direction should be `sendonly`.
4. Prefer Opus if supported.
5. If Opus support is not available yet, document the gap and temporarily use the codec supported by the `esp_peer` demo to get the PeerConnection flow compiling.

Target SDP shape should be audio-only, roughly:

```sdp
m=audio 9 UDP/TLS/RTP/SAVPF 111
a=sendonly
a=rtcp-mux
a=rtpmap:111 opus/48000/2
a=ptime:20
```

Acceptance criteria:

```text
Serial monitor prints a valid-looking audio-only SDP offer.
No video m-line.
No datachannel requirement unless unavoidable.
```

---

## M5: Implement WHIP Signaling

Implement:

```text
src/app_whip.c
src/app_whip.h
```

Expose:

```c
typedef struct {
    char *sdp_answer;
    char *location;
    char *etag;
    int status_code;
} app_whip_response_t;

esp_err_t app_whip_post_offer(
    const char *whip_url,
    const char *bearer_token,
    const char *sdp_offer,
    app_whip_response_t *out
);

esp_err_t app_whip_delete_session(
    const char *location,
    const char *bearer_token
);

void app_whip_response_free(app_whip_response_t *resp);
```

WHIP POST behavior:

```http
POST <WHIP_URL>
Content-Type: application/sdp
Authorization: Bearer <token>

<v=0 SDP offer>
```

Expected success:

```http
201 Created
Content-Type: application/sdp
Location: <session resource URL>

<v=0 SDP answer>
```

For MVP, support non-trickle ICE first.

Also parse and store:

```text
Location
ETag, if present
response body as SDP answer
```

Implement `DELETE` for cleanup:

```http
DELETE <Location>
Authorization: Bearer <token>
```

Do not implement PATCH/trickle ICE in the first version unless Agora requires it.

Acceptance criteria:

```text
POST sends SDP offer.
Response status/body/Location are logged.
SDP answer is captured into memory.
```

---

## M6: Connect PeerConnection

Update `main.c` flow:

```c
void app_main(void) {
    ESP_ERROR_CHECK(app_wifi_start());
    ESP_ERROR_CHECK(app_webrtc_init());

    app_webrtc_offer_t offer = {0};
    ESP_ERROR_CHECK(app_webrtc_create_audio_offer(&offer));

    app_whip_response_t whip = {0};
    ESP_ERROR_CHECK(app_whip_post_offer(
        APP_AGORA_WHIP_URL,
        APP_AGORA_TOKEN,
        offer.sdp_offer,
        &whip
    ));

    ESP_ERROR_CHECK(app_webrtc_set_remote_answer(whip.sdp_answer));
    ESP_ERROR_CHECK(app_webrtc_start());

    app_webrtc_destroy_offer(&offer);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

Add logging for WebRTC state changes:

```text
ICE state
DTLS state
PeerConnection state
selected candidate pair if available
```

Acceptance criteria:

```text
State reaches connected/completed, or failure reason is clearly logged.
```

---

## M7: Fake Audio Publisher

Implement:

```text
src/app_audio_fake.c
src/app_audio_fake.h
```

Expose:

```c
esp_err_t app_audio_fake_start(void);
esp_err_t app_audio_fake_stop(void);
```

Behavior:

1. Generate 20 ms silent PCM frames or encoded silent frames depending on esp_peer API.
2. Feed frames into audio sender at real-time cadence.
3. Use `esp_timer` or FreeRTOS task with `vTaskDelayUntil`.
4. Do not block WebRTC task.

Audio assumptions:

```text
sample rate: 48000
channels: 1
frame duration: 20 ms
samples per frame: 960
```

If the codec path requires encoded frames, provide a codec abstraction but leave Opus encoder as TODO if unavailable.

Acceptance criteria:

```text
Fake audio task runs.
No heap leak over 5 minutes.
Agora side detects publisher if WHIP compatibility is correct.
```

---

## M8: Real Cardputer Microphone

Do not implement until M0-M7 work.

Add later:

```text
src/app_audio_i2s.c
src/app_audio_i2s.h
src/app_codec_opus.c
src/app_codec_opus.h
```

Requirements later:

```text
Cardputer I2S mic
→ PCM normalize
→ resample if needed
→ Opus encode
→ WebRTC audio sender
```

---

## Debugging Commands

Use:

```bash
pio run
pio run -t upload
pio device monitor
```

For Cardputer upload mode:

```text
1. Unplug USB
2. Power switch OFF
3. Hold G0
4. Plug USB
5. Run Upload
6. Release G0 after Writing starts
```

If upload fails with boot mode error, hold G0 until upload completes.

---

## Logging Requirements

Use ESP-IDF logging:

```c
#include "esp_log.h"
```

Each module should have its own tag:

```c
static const char *TAG = "app_whip";
```

Log at least:

```text
Wi-Fi state
IP address
free heap before/after WebRTC init
SDP offer length
WHIP HTTP status
WHIP Location header
SDP answer length
PeerConnection / ICE / DTLS state
audio fake frame counter every 5 seconds
```

---

## Error Handling Requirements

Do not ignore return codes.

Every public function should return `esp_err_t`.

On failure:

```text
log exact stage
log HTTP status if available
log response body if safe
free allocated memory
do not reboot-loop silently
```

---

## Deliverables

Create or update these files:

```text
platformio.ini
partitions.csv
sdkconfig.defaults

src/main.c
src/app_config.h
src/app_wifi.c
src/app_wifi.h
src/app_https.c
src/app_https.h
src/app_whip.c
src/app_whip.h
src/app_webrtc.c
src/app_webrtc.h
src/app_audio_fake.c
src/app_audio_fake.h
```

Also create:

```text
README.md
```

README should include:

```text
How to build
How to upload to Cardputer
How to configure Wi-Fi and WHIP URL/token
Current MVP stage
Known limitations
Next steps for Opus and I2S microphone
```

---

## Initial README Content

Use this structure:

```md
# Cardputer Agora WHIP Publisher

## Goal

ESP32-S3 / M5Stack Cardputer audio-only WebRTC publisher using WHIP.

## Current Stage

- [ ] ESP-IDF project builds
- [ ] Wi-Fi connects
- [ ] HTTPS works
- [ ] esp_peer compiles
- [ ] audio-only SDP offer generated
- [ ] WHIP POST offer implemented
- [ ] SDP answer applied
- [ ] fake audio publisher
- [ ] Cardputer mic integration

## Build

```bash
pio run
```

## Upload

```bash
pio run -t upload
```

Cardputer upload mode:

1. Unplug USB
2. Switch power OFF
3. Hold G0
4. Plug USB
5. Upload
6. Release G0 after Writing starts

## Monitor

```bash
pio device monitor
```

## Configuration

Edit `src/app_config.h` for local development:

```c
#define APP_WIFI_SSID       "..."
#define APP_WIFI_PASSWORD   "..."
#define APP_AGORA_WHIP_URL  "..."
#define APP_AGORA_TOKEN     "..."
```

Do not commit real secrets.

## Architecture

```text
Wi-Fi
→ WebRTC PeerConnection via esp_peer
→ audio-only SDP offer
→ WHIP POST
→ SDP answer
→ fake audio sender
→ later Cardputer I2S mic
```

## Notes

WHIP is only the WebRTC signaling layer. The media still flows over WebRTC ICE/DTLS/SRTP.
```

---

## Definition of Done for First Codex Pass

Stop after M2 unless M2 is completely working.

The first pass is successful when:

```text
pio run succeeds
pio run -t upload succeeds
serial monitor shows Wi-Fi connected + IP address
HTTPS test succeeds
README is created
```

Do not attempt full WebRTC in the first pass if Wi-Fi/HTTPS baseline is not stable.
