#include "app_audio_i2s.h"
#include "app_audio_fake.h"
#include "app_config.h"
#include "app_time_sync.h"
#include "app_webrtc.h"
#include "app_webrtc_probe.h"
#include "app_whip.h"
#include "app_wifi.h"

#include "esp_err.h"
#include "esp_log.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_main";

static esp_err_t app_validate_runtime_config(void)
{
    if (strcmp(APP_WIFI_SSID, "CHANGE_ME") == 0 || strcmp(APP_WIFI_PASSWORD, "CHANGE_ME") == 0) {
        ESP_LOGE(TAG, "Missing Wi-Fi config. Create src/app_config_local.h from src/app_config.local.example.h");
        return ESP_ERR_INVALID_STATE;
    }

    if (strcmp(APP_AGORA_APP_ID, "CHANGE_ME") == 0 ||
        strcmp(APP_AGORA_APP_CERTIFICATE, "CHANGE_ME") == 0 ||
        strcmp(APP_AGORA_STREAM_ID, "CHANGE_ME") == 0) {
        ESP_LOGE(TAG, "Missing Agora config. Fill APP_AGORA_APP_ID / APP_AGORA_APP_CERTIFICATE / APP_AGORA_STREAM_ID in src/app_config_local.h");
        return ESP_ERR_INVALID_STATE;
    }

    if (APP_AUDIO_USE_I2S_MIC) {
#if APP_AUDIO_CODEC == APP_AUDIO_CODEC_G711A
        ESP_LOGI(TAG, "Audio path: Cardputer I2S mic + G711A @ %d Hz", APP_AUDIO_SAMPLE_RATE);
#else
        ESP_LOGI(TAG, "Audio path: Cardputer I2S mic + Opus @ %d Hz (capture @ %d Hz)",
                 APP_AUDIO_SAMPLE_RATE, APP_AUDIO_I2S_CAPTURE_RATE);
#endif
    } else {
        ESP_LOGI(TAG, "Audio path: fake audio test publisher");
    }

    return ESP_OK;
}

void app_main(void)
{
    app_webrtc_offer_t offer = { 0 };
    app_whip_response_t whip = { 0 };
    char *normalized_offer = NULL;
    char *normalized_answer = NULL;
    char *whip_url = NULL;
    char *whip_token = NULL;

    ESP_LOGI(TAG, "Cardputer WHIP baseline starting");

    ESP_ERROR_CHECK(app_validate_runtime_config());
    ESP_ERROR_CHECK(app_webrtc_probe());
    ESP_ERROR_CHECK(app_wifi_start());
    ESP_ERROR_CHECK(app_time_sync_wait());
    ESP_ERROR_CHECK(app_webrtc_init());
    ESP_ERROR_CHECK(app_whip_build_url(&whip_url));
    ESP_ERROR_CHECK(app_whip_generate_token(&whip_token));
    ESP_ERROR_CHECK(app_webrtc_create_audio_offer(&offer));
    ESP_LOGI(TAG, "Local audio SDP offer:\n%s", offer.sdp_offer);
    ESP_ERROR_CHECK(app_whip_normalize_offer_sdp(offer.sdp_offer, &normalized_offer));
    ESP_LOGI(TAG, "Normalized WHIP SDP offer:\n%s", normalized_offer);
    ESP_LOGI(TAG, "Generated WHIP URL: %s", whip_url);
    ESP_ERROR_CHECK(app_whip_post_offer(whip_url, whip_token, normalized_offer, &whip));
    ESP_LOGI(TAG, "Remote SDP answer:\n%s", whip.sdp_answer);
    ESP_ERROR_CHECK(app_whip_normalize_answer_sdp(whip.sdp_answer, &normalized_answer));
    ESP_LOGI(TAG, "Normalized remote SDP answer:\n%s", normalized_answer);
    ESP_ERROR_CHECK(app_webrtc_set_remote_answer(normalized_answer));
    ESP_ERROR_CHECK(app_webrtc_start());
    if (APP_AUDIO_USE_I2S_MIC) {
        ESP_ERROR_CHECK(app_audio_i2s_start());
    } else {
        ESP_ERROR_CHECK(app_audio_fake_start());
    }
    app_webrtc_destroy_offer(&offer);
    app_whip_response_free(&whip);
    free(normalized_offer);
    free(normalized_answer);
    free(whip_url);
    free(whip_token);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
