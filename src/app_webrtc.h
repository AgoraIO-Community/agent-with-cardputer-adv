#pragma once

#include "esp_err.h"

typedef struct {
    char *sdp_offer;
} app_webrtc_offer_t;

esp_err_t app_webrtc_init(void);
esp_err_t app_webrtc_create_audio_offer(app_webrtc_offer_t *out_offer);
esp_err_t app_webrtc_set_remote_answer(const char *sdp_answer);
esp_err_t app_webrtc_start(void);
esp_err_t app_webrtc_send_audio(const void *data, size_t size, uint32_t pts);
bool app_webrtc_is_audio_send_ready(void);
void app_webrtc_destroy_offer(app_webrtc_offer_t *offer);
