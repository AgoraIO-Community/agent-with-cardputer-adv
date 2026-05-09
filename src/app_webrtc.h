#pragma once

#include "esp_err.h"
#include "esp_peer.h"

typedef enum {
    APP_WEBRTC_SESSION_PUSH = 0,
    APP_WEBRTC_SESSION_PULL = 1,
} app_webrtc_session_role_t;

typedef struct {
    char *sdp_offer;
} app_webrtc_offer_t;

typedef void (*app_webrtc_audio_info_cb_t)(const esp_peer_audio_stream_info_t *info, void *ctx);
typedef void (*app_webrtc_audio_frame_cb_t)(const esp_peer_audio_frame_t *frame, void *ctx);

esp_err_t app_webrtc_init_session(app_webrtc_session_role_t role);
esp_err_t app_webrtc_create_audio_offer_for_role(app_webrtc_session_role_t role, app_webrtc_offer_t *out_offer);
esp_err_t app_webrtc_set_remote_answer_for_role(app_webrtc_session_role_t role, const char *sdp_answer);
esp_err_t app_webrtc_set_audio_receive_callbacks(app_webrtc_session_role_t role,
                                                 app_webrtc_audio_info_cb_t info_cb,
                                                 app_webrtc_audio_frame_cb_t frame_cb,
                                                 void *cb_ctx);
esp_err_t app_webrtc_query_session(app_webrtc_session_role_t role);
esp_err_t app_webrtc_send_audio(const void *data, size_t size, uint32_t pts);
bool app_webrtc_is_audio_send_ready(void);
void app_webrtc_destroy_offer(app_webrtc_offer_t *offer);
