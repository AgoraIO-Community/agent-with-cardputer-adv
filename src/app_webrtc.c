#include "app_webrtc.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "esp_peer_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define APP_WEBRTC_LOOP_STACK_SIZE 12288
#define APP_WEBRTC_OFFER_TIMEOUT_MS 5000

typedef struct {
    esp_peer_handle_t peer;
    TaskHandle_t loop_task;
    SemaphoreHandle_t offer_ready;
    SemaphoreHandle_t lock;
    char *pending_offer;
    esp_peer_state_t state;
    bool initialized;
    bool loop_running;
    app_webrtc_session_role_t role;
    app_webrtc_audio_info_cb_t audio_info_cb;
    app_webrtc_audio_frame_cb_t audio_frame_cb;
    void *audio_cb_ctx;
    esp_peer_rtp_transform_cb_t rtp_transform_cb;
    uint32_t recv_rtp_packets;
    uint32_t recv_rtp_bytes;
} app_webrtc_session_t;

static const char *TAG = "app_webrtc";
static app_webrtc_session_t s_sessions[2];

static const char *app_webrtc_role_name(app_webrtc_session_role_t role)
{
    return role == APP_WEBRTC_SESSION_PULL ? "pull" : "push";
}

static esp_peer_audio_codec_t app_webrtc_audio_codec(void)
{
#if APP_AUDIO_CODEC == APP_AUDIO_CODEC_G711A
    return ESP_PEER_AUDIO_CODEC_G711A;
#else
    return ESP_PEER_AUDIO_CODEC_OPUS;
#endif
}

static esp_err_t app_webrtc_peer_err(int peer_err)
{
    switch (peer_err) {
    case ESP_PEER_ERR_NONE:
        return ESP_OK;
    case ESP_PEER_ERR_INVALID_ARG:
        return ESP_ERR_INVALID_ARG;
    case ESP_PEER_ERR_NO_MEM:
        return ESP_ERR_NO_MEM;
    case ESP_PEER_ERR_WRONG_STATE:
        return ESP_ERR_INVALID_STATE;
    default:
        return ESP_FAIL;
    }
}

static app_webrtc_session_t *app_webrtc_get_session(app_webrtc_session_role_t role)
{
    return &s_sessions[(int)role];
}

static void app_webrtc_replace_offer_locked(app_webrtc_session_t *session, const uint8_t *data, int size)
{
    char *offer;

    if (session == NULL || data == NULL || size <= 0) {
        return;
    }
    offer = calloc(1, (size_t)size + 1);
    if (offer == NULL) {
        ESP_LOGE(TAG, "[%s] Failed to allocate local SDP buffer", app_webrtc_role_name(session->role));
        return;
    }
    memcpy(offer, data, (size_t)size);

    free(session->pending_offer);
    session->pending_offer = offer;
}

static int app_webrtc_on_state(esp_peer_state_t state, void *ctx)
{
    app_webrtc_session_t *session = (app_webrtc_session_t *)ctx;

    if (session == NULL) {
        return 0;
    }
    session->state = state;
    if (session->loop_task != NULL) {
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(session->loop_task);
        ESP_LOGI(TAG, "[%s] Peer state changed: %d (stack watermark=%u words)",
                 app_webrtc_role_name(session->role), (int)state, (unsigned)watermark);
    } else {
        ESP_LOGI(TAG, "[%s] Peer state changed: %d", app_webrtc_role_name(session->role), (int)state);
    }
    return 0;
}

static int app_webrtc_on_msg(esp_peer_msg_t *msg, void *ctx)
{
    app_webrtc_session_t *session = (app_webrtc_session_t *)ctx;

    if (session == NULL || msg == NULL || msg->data == NULL || msg->size <= 0) {
        return 0;
    }
    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        if (xSemaphoreTake(session->lock, portMAX_DELAY) == pdTRUE) {
            app_webrtc_replace_offer_locked(session, msg->data, msg->size);
            xSemaphoreGive(session->lock);
            xSemaphoreGive(session->offer_ready);
        }
        ESP_LOGI(TAG, "[%s] Captured local SDP offer (%d bytes)", app_webrtc_role_name(session->role), msg->size);
    } else if (msg->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
        ESP_LOGI(TAG, "[%s] Ignoring local ICE candidate during M4 (%d bytes)",
                 app_webrtc_role_name(session->role), msg->size);
    }
    return 0;
}

static int app_webrtc_on_audio_info(esp_peer_audio_stream_info_t *info, void *ctx)
{
    app_webrtc_session_t *session = (app_webrtc_session_t *)ctx;

    if (session == NULL || info == NULL) {
        return 0;
    }
    ESP_LOGI(TAG, "[%s] Remote audio info: codec=%d sample_rate=%u channels=%u",
             app_webrtc_role_name(session->role), (int)info->codec, (unsigned)info->sample_rate, (unsigned)info->channel);
    if (session->audio_info_cb != NULL) {
        session->audio_info_cb(info, session->audio_cb_ctx);
    }
    return 0;
}

static int app_webrtc_on_audio_data(esp_peer_audio_frame_t *frame, void *ctx)
{
    app_webrtc_session_t *session = (app_webrtc_session_t *)ctx;

    if (session == NULL || frame == NULL) {
        return 0;
    }
    if (session->audio_frame_cb != NULL) {
        session->audio_frame_cb(frame, session->audio_cb_ctx);
    }
    return 0;
}

static int app_webrtc_get_rtp_encoded_size(esp_peer_rtp_frame_t *frame, bool *in_place, void *ctx)
{
    (void)ctx;
    if (frame == NULL || in_place == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    *in_place = true;
    frame->encoded_size = frame->orig_size;
    return ESP_PEER_ERR_NONE;
}

static int app_webrtc_transform_incoming_rtp(esp_peer_rtp_frame_t *frame, void *ctx)
{
    app_webrtc_session_t *session = (app_webrtc_session_t *)ctx;

    if (frame == NULL || session == NULL) {
        return ESP_PEER_ERR_INVALID_ARG;
    }
    session->recv_rtp_packets++;
    session->recv_rtp_bytes += frame->orig_size;
    if (session->recv_rtp_packets == 1U || (session->recv_rtp_packets % 100U) == 0U) {
        ESP_LOGI(TAG, "[%s] Inbound RTP packets: %u bytes=%u payload_type=%u",
                 app_webrtc_role_name(session->role),
                 (unsigned)session->recv_rtp_packets,
                 (unsigned)session->recv_rtp_bytes,
                 (unsigned)frame->payload_type);
    }
    return ESP_PEER_ERR_NONE;
}

static void app_webrtc_loop_task(void *arg)
{
    app_webrtc_session_t *session = (app_webrtc_session_t *)arg;

    while (session->loop_running) {
        if (session->peer != NULL) {
            esp_peer_main_loop(session->peer);
        }
        vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_LOOP_DELAY_MS));
    }
    session->loop_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t app_webrtc_init_session(app_webrtc_session_role_t role)
{
    app_webrtc_session_t *session = app_webrtc_get_session(role);
    esp_peer_default_cfg_t default_cfg = {
        .agent_recv_timeout = APP_WEBRTC_AGENT_RECV_TIMEOUT_MS,
        .rtp_cfg = {
            .send_pool_size = APP_WEBRTC_SEND_POOL_SIZE,
            .send_queue_num = APP_WEBRTC_SEND_QUEUE_NUM,
        },
    };
    esp_peer_cfg_t cfg = {
        .role = ESP_PEER_ROLE_CONTROLLING,
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .audio_info = {
            .codec = app_webrtc_audio_codec(),
            .sample_rate = APP_AUDIO_SAMPLE_RATE,
            .channel = APP_AUDIO_CHANNELS,
        },
        .video_info = {
            .codec = ESP_PEER_VIDEO_CODEC_NONE,
        },
        .audio_dir = role == APP_WEBRTC_SESSION_PULL ? ESP_PEER_MEDIA_DIR_RECV_ONLY : ESP_PEER_MEDIA_DIR_SEND_ONLY,
        .video_dir = ESP_PEER_MEDIA_DIR_NONE,
        .enable_data_channel = false,
        .on_state = app_webrtc_on_state,
        .on_msg = app_webrtc_on_msg,
        .on_audio_info = role == APP_WEBRTC_SESSION_PULL ? app_webrtc_on_audio_info : NULL,
        .on_audio_data = role == APP_WEBRTC_SESSION_PULL ? app_webrtc_on_audio_data : NULL,
        .ctx = session,
        .extra_cfg = &default_cfg,
        .extra_size = sizeof(default_cfg),
    };
    int peer_err;

    if (session->initialized) {
        return ESP_OK;
    }

    session->role = role;
    session->offer_ready = xSemaphoreCreateBinary();
    session->lock = xSemaphoreCreateMutex();
    if (session->offer_ready == NULL || session->lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    peer_err = esp_peer_pre_generate_cert();
    if (peer_err != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "[%s] esp_peer_pre_generate_cert failed: %d", app_webrtc_role_name(role), peer_err);
        return app_webrtc_peer_err(peer_err);
    }

    peer_err = esp_peer_open(&cfg, esp_peer_get_default_impl(), &session->peer);
    if (peer_err != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "[%s] esp_peer_open failed: %d", app_webrtc_role_name(role), peer_err);
        return app_webrtc_peer_err(peer_err);
    }
    if (role == APP_WEBRTC_SESSION_PULL) {
        session->rtp_transform_cb = (esp_peer_rtp_transform_cb_t){
            .get_encoded_size = app_webrtc_get_rtp_encoded_size,
            .transform = app_webrtc_transform_incoming_rtp,
        };
        peer_err = esp_peer_set_rtp_transformer(session->peer, ESP_PEER_RTP_TRANSFORM_ROLE_RECEIVER,
                                                &session->rtp_transform_cb, session);
        if (peer_err != ESP_PEER_ERR_NONE) {
            ESP_LOGW(TAG, "[%s] Failed to set RTP receiver transformer: %d", app_webrtc_role_name(role), peer_err);
        }
    }

    session->loop_running = true;
    if (xTaskCreate(app_webrtc_loop_task, role == APP_WEBRTC_SESSION_PULL ? "app_webrtc_pull" : "app_webrtc_push",
                    APP_WEBRTC_LOOP_STACK_SIZE, session, 5, &session->loop_task) != pdPASS) {
        session->loop_running = false;
        esp_peer_close(session->peer);
        session->peer = NULL;
        return ESP_ERR_NO_MEM;
    }

    session->initialized = true;
    return ESP_OK;
}

esp_err_t app_webrtc_create_audio_offer_for_role(app_webrtc_session_role_t role, app_webrtc_offer_t *out_offer)
{
    app_webrtc_session_t *session = app_webrtc_get_session(role);
    int peer_err;
    char *offer_copy = NULL;

    if (out_offer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out_offer->sdp_offer = NULL;

    ESP_RETURN_ON_ERROR(app_webrtc_init_session(role), TAG, "Failed to initialize WebRTC session");

    if (xSemaphoreTake(session->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    free(session->pending_offer);
    session->pending_offer = NULL;
    xSemaphoreGive(session->lock);

    while (xSemaphoreTake(session->offer_ready, 0) == pdTRUE) {
    }

    peer_err = esp_peer_new_connection(session->peer);
    if (peer_err != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "[%s] esp_peer_new_connection failed: %d", app_webrtc_role_name(role), peer_err);
        return app_webrtc_peer_err(peer_err);
    }

    if (xSemaphoreTake(session->offer_ready, pdMS_TO_TICKS(APP_WEBRTC_OFFER_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(session->lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (session->pending_offer != NULL) {
        offer_copy = strdup(session->pending_offer);
    }
    xSemaphoreGive(session->lock);

    if (offer_copy == NULL) {
        return ESP_FAIL;
    }
    out_offer->sdp_offer = offer_copy;
    return ESP_OK;
}

esp_err_t app_webrtc_set_remote_answer_for_role(app_webrtc_session_role_t role, const char *sdp_answer)
{
    app_webrtc_session_t *session = app_webrtc_get_session(role);
    esp_peer_msg_t msg;
    int peer_err;

    if (sdp_answer == NULL || session->peer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    msg.type = ESP_PEER_MSG_TYPE_SDP;
    msg.data = (uint8_t *)sdp_answer;
    msg.size = (int)strlen(sdp_answer);

    peer_err = esp_peer_send_msg(session->peer, &msg);
    if (peer_err != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "[%s] esp_peer_send_msg(answer) failed: %d", app_webrtc_role_name(role), peer_err);
        return app_webrtc_peer_err(peer_err);
    }
    return ESP_OK;
}

esp_err_t app_webrtc_set_audio_receive_callbacks(app_webrtc_session_role_t role,
                                                 app_webrtc_audio_info_cb_t info_cb,
                                                 app_webrtc_audio_frame_cb_t frame_cb,
                                                 void *cb_ctx)
{
    app_webrtc_session_t *session = app_webrtc_get_session(role);

    session->audio_info_cb = info_cb;
    session->audio_frame_cb = frame_cb;
    session->audio_cb_ctx = cb_ctx;
    return ESP_OK;
}

esp_err_t app_webrtc_query_session(app_webrtc_session_role_t role)
{
    app_webrtc_session_t *session = app_webrtc_get_session(role);

    if (session->peer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "[%s] Query stats before peer query: recv_rtp_packets=%u recv_rtp_bytes=%u state=%d",
             app_webrtc_role_name(role),
             (unsigned)session->recv_rtp_packets,
             (unsigned)session->recv_rtp_bytes,
             (int)session->state);
    return app_webrtc_peer_err(esp_peer_query(session->peer));
}

esp_err_t app_webrtc_send_audio(const void *data, size_t size, uint32_t pts)
{
    app_webrtc_session_t *session = app_webrtc_get_session(APP_WEBRTC_SESSION_PUSH);
    esp_peer_audio_frame_t frame;
    int peer_err;

    if (data == NULL || size == 0 || session->peer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!app_webrtc_is_audio_send_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    frame.data = (uint8_t *)data;
    frame.size = (int)size;
    frame.pts = pts;

    peer_err = esp_peer_send_audio(session->peer, &frame);
    if (peer_err != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "[push] esp_peer_send_audio failed: %d", peer_err);
        return app_webrtc_peer_err(peer_err);
    }
    return ESP_OK;
}

bool app_webrtc_is_audio_send_ready(void)
{
    return app_webrtc_get_session(APP_WEBRTC_SESSION_PUSH)->state == ESP_PEER_STATE_CONNECTED;
}

void app_webrtc_destroy_offer(app_webrtc_offer_t *offer)
{
    if (offer == NULL) {
        return;
    }
    free(offer->sdp_offer);
    offer->sdp_offer = NULL;
}
