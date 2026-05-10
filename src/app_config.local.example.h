#pragma once

#define APP_WIFI_SSID "your-wifi-ssid"
#define APP_WIFI_PASSWORD "your-wifi-password"

#define APP_PROTOCOL_BASE_URL "https://your-server.example.com"

/*
 * Hardware-verified Cardputer ADV RTSA setup without PSRAM:
 * - app-side G711A uplink encoding
 * - app-side G711A/PCMA remote audio playback
 * - Cardputer ADV ES8311 mic and speaker path
 * - controller-managed half-duplex switching for shared I2S pins
 * - keyboard-triggered agent start/stop
 */
#define APP_AUDIO_USE_I2S_MIC 1
#define APP_AUDIO_CODEC APP_AUDIO_CODEC_G711A
#define APP_AGORA_PROTOCOL_OUTPUT_AUDIO_CODEC "pcma"
#define APP_AGORA_START_ON_KEYPRESS 1

#define APP_AUDIO_ENABLE_PULL_PLAYBACK 1
#define APP_AUDIO_ENABLE_HALF_DUPLEX_GATING 1
#define APP_AUDIO_PLAYBACK_RUN_SELF_TEST 0

#define APP_AUDIO_I2S_USE_CARDPUTER_ADV 1
#define APP_AUDIO_I2S_PORT I2S_NUM_1
#define APP_AUDIO_I2S_MIC_PORT I2S_NUM_0
#define APP_AUDIO_I2S_MIC_BCK_GPIO GPIO_NUM_41
#define APP_AUDIO_I2S_MIC_CLK_GPIO GPIO_NUM_43
#define APP_AUDIO_I2S_MIC_DATA_GPIO GPIO_NUM_46
#define APP_AUDIO_I2S_CAPTURE_RATE 16000

/*
 * Mic uplink gain. 6/1 is a good starting point for normal speech on tested
 * Cardputer ADV hardware. Increase to 8/1 if remote listeners still hear you
 * quietly; reduce to 4/1 or 5/1 if your voice sounds clipped.
 */
#define APP_AUDIO_I2S_GAIN_NUM 6
#define APP_AUDIO_I2S_GAIN_DEN 1

/*
 * Speaker playback gain. The firmware caps the effective gain and applies a
 * soft limiter, so this keeps speech audible without the very thin/clipped
 * sound caused by hard saturation.
 */
#define APP_AUDIO_PLAYBACK_GAIN_NUM 24
#define APP_AUDIO_PLAYBACK_GAIN_DEN 1

#define APP_AUDIO_I2S_ADV_I2C_PORT I2C_NUM_1
#define APP_AUDIO_I2S_ADV_I2C_SDA_GPIO GPIO_NUM_8
#define APP_AUDIO_I2S_ADV_I2C_SCL_GPIO GPIO_NUM_9
