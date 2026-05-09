#pragma once

#define APP_WIFI_SSID "your-wifi-ssid"
#define APP_WIFI_PASSWORD "your-wifi-password"

#define APP_PROTOCOL_BASE_URL "https://your-server.example.com"

/*
 * Latest known-feasible Cardputer RTSA setup without PSRAM:
 * - app-side G711A uplink encoding
 * - Cardputer ADV ES8311 mic path
 * - pull playback left off by default; re-enable only after extra memory tuning
 */
#define APP_AUDIO_USE_I2S_MIC 1
#define APP_AUDIO_CODEC APP_AUDIO_CODEC_G711A
#define APP_AUDIO_ENABLE_PULL_PLAYBACK 0
#define APP_AUDIO_I2S_USE_CARDPUTER_ADV 1
#define APP_AUDIO_I2S_PORT I2S_NUM_1
#define APP_AUDIO_I2S_MIC_BCK_GPIO GPIO_NUM_41
#define APP_AUDIO_I2S_MIC_CLK_GPIO GPIO_NUM_43
#define APP_AUDIO_I2S_MIC_DATA_GPIO GPIO_NUM_46
#define APP_AUDIO_I2S_ADV_I2C_PORT I2C_NUM_1
#define APP_AUDIO_I2S_ADV_I2C_SDA_GPIO GPIO_NUM_8
#define APP_AUDIO_I2S_ADV_I2C_SCL_GPIO GPIO_NUM_9
