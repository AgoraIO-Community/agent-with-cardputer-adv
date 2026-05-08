#pragma once

#define APP_WIFI_SSID "your-wifi-ssid"
#define APP_WIFI_PASSWORD "your-wifi-password"

#define APP_HTTPS_TEST_URL "https://www.google.com/generate_204"

#define APP_AGORA_WHIP_SERVER "ap-webrtc-whip.ap.sd-rtn.com"
#define APP_AGORA_APP_ID "your-agora-app-id"
#define APP_AGORA_APP_CERTIFICATE "your-agora-app-certificate"
#define APP_AGORA_STREAM_ID "your-stream-id"
#define APP_AGORA_UID "10010"
#define APP_AGORA_WHIP_TOKEN_TTL_SEC 600

/*
 * Opus trial:
 * - Keep capture at 16 kHz on Cardputer ADV.
 * - The firmware upsamples mic PCM to 48 kHz, then duplicates mono to stereo
 *   before handing it to WebRTC because Agora WHIP accepts Opus as /2 here.
 *
 * Known-good baseline:
 * - Use G.711A for stable publishing on current esp_peer / Agora WHIP path.
 */
#define APP_AUDIO_CODEC APP_AUDIO_CODEC_G711A
#define APP_AUDIO_FRAME_MS 20
#define APP_AUDIO_USE_I2S_MIC 1
#define APP_AUDIO_I2S_USE_CARDPUTER_ADV 1
#define APP_AUDIO_I2S_PORT I2S_NUM_1
#define APP_AUDIO_I2S_MIC_BCK_GPIO GPIO_NUM_41
#define APP_AUDIO_I2S_MIC_CLK_GPIO GPIO_NUM_43
#define APP_AUDIO_I2S_MIC_DATA_GPIO GPIO_NUM_46
#define APP_AUDIO_I2S_ADV_I2C_PORT I2C_NUM_1
#define APP_AUDIO_I2S_ADV_I2C_SDA_GPIO GPIO_NUM_8
#define APP_AUDIO_I2S_ADV_I2C_SCL_GPIO GPIO_NUM_9
#define APP_AUDIO_I2S_CAPTURE_RATE 16000
/* For G.711 fallback keep 20 ms. For Opus trials, 20 ms is the safe starting point. */
#define APP_AUDIO_I2S_ADV_PGA_REG 0x1E
#define APP_AUDIO_I2S_ADV_ADC_VOLUME_REG 0xFF
#define APP_AUDIO_I2S_GAIN_NUM 8
#define APP_AUDIO_I2S_GAIN_DEN 1
#define APP_AUDIO_I2S_PROFILE_ENABLE 1
#define APP_AUDIO_I2S_PDM_DATA_FMT APP_AUDIO_I2S_PDM_FMT_PCM
#define APP_AUDIO_I2S_PDM_CLK_INVERT 0
#define APP_AUDIO_I2S_ALLOW_RAW_TUNE 0

#define APP_WEBRTC_LOOP_DELAY_MS 10
#define APP_WEBRTC_AGENT_RECV_TIMEOUT_MS 100
#define APP_WEBRTC_SEND_POOL_SIZE 16384
#define APP_WEBRTC_SEND_QUEUE_NUM 64
