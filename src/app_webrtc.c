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
} app_webrtc_ctx_t;

static const char *TAG = "app_webrtc";
static app_webrtc_ctx_t s_webrtc;

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

static void app_webrtc_replace_offer_locked(const uint8_t *data, int size)
{
    char *offer;

    if (data == NULL || size <= 0) {
        return;
    }
    offer = calloc(1, (size_t)size + 1);
    if (offer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate local SDP buffer");
        return;
    }
    memcpy(offer, data, (size_t)size);

    free(s_webrtc.pending_offer);
    s_webrtc.pending_offer = offer;
}

static int app_webrtc_on_state(esp_peer_state_t state, void *ctx)
{
    (void)ctx;
    s_webrtc.state = state;
    if (s_webrtc.loop_task != NULL) {
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(s_webrtc.loop_task);
        ESP_LOGI(TAG, "Peer state changed: %d (stack watermark=%u words)", (int)state, (unsigned)watermark);
    } else {
        ESP_LOGI(TAG, "Peer state changed: %d", (int)state);
    }
    return 0;
}

static int app_webrtc_on_msg(esp_peer_msg_t *msg, void *ctx)
{
    (void)ctx;

    if (msg == NULL || msg->data == NULL || msg->size <= 0) {
        return 0;
    }
    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        if (xSemaphoreTake(s_webrtc.lock, portMAX_DELAY) == pdTRUE) {
            app_webrtc_replace_offer_locked(msg->data, msg->size);
            xSemaphoreGive(s_webrtc.lock);
            xSemaphoreGive(s_webrtc.offer_ready);
        }
        ESP_LOGI(TAG, "Captured local SDP offer (%d bytes)", msg->size);
    } else if (msg->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
        ESP_LOGI(TAG, "Ignoring local ICE candidate during M4 (%d bytes)", msg->size);
    }
    return 0;
}

static void app_webrtc_loop_task(void *arg)
{
    app_webrtc_ctx_t *ctx = (app_webrtc_ctx_t *)arg;

    while (ctx->loop_running) {
        if (ctx->peer != NULL) {
            esp_peer_main_loop(ctx->peer);
        }
        vTaskDelay(pdMS_TO_TICKS(APP_WEBRTC_LOOP_DELAY_MS));
    }
    ctx->loop_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t app_webrtc_init(void)
{
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
        .audio_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
        .video_dir = ESP_PEER_MEDIA_DIR_NONE,
        .enable_data_channel = false,
        .on_state = app_webrtc_on_state,
        .on_msg = app_webrtc_on_msg,
        .extra_cfg = &default_cfg,
        .extra_size = sizeof(default_cfg),
    };
    int peer_err;

    if (s_webrtc.initialized) {
        return ESP_OK;
    }

    s_webrtc.offer_ready = xSemaphoreCreateBinary();
    s_webrtc.lock = xSemaphoreCreateMutex();
    if (s_webrtc.offer_ready == NULL || s_webrtc.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    peer_err = esp_peer_pre_generate_cert();
    if (peer_err != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_pre_generate_cert failed: %d", peer_err);
        return app_webrtc_peer_err(peer_err);
    }

    peer_err = esp_peer_open(&cfg, esp_peer_get_default_impl(), &s_webrtc.peer);
    if (peer_err != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_open failed: %d", peer_err);
        return app_webrtc_peer_err(peer_err);
    }

    s_webrtc.loop_running = true;
    if (xTaskCreate(app_webrtc_loop_task, "app_webrtc", APP_WEBRTC_LOOP_STACK_SIZE, &s_webrtc, 5,
                    &s_webrtc.loop_task) != pdPASS) {
        s_webrtc.loop_running = false;
        esp_peer_close(s_webrtc.peer);
        s_webrtc.peer = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_webrtc.initialized = true;
    return ESP_OK;
}

esp_err_t app_webrtc_create_audio_offer(app_webrtc_offer_t *out_offer)
{
    int peer_err;
    char *offer_copy = NULL;

    if (out_offer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    out_offer->sdp_offer = NULL;

    ESP_RETURN_ON_ERROR(app_webrtc_init(), TAG, "Failed to initialize WebRTC");

    if (xSemaphoreTake(s_webrtc.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    free(s_webrtc.pending_offer);
    s_webrtc.pending_offer = NULL;
    xSemaphoreGive(s_webrtc.lock);

    while (xSemaphoreTake(s_webrtc.offer_ready, 0) == pdTRUE) {
    }

    peer_err = esp_peer_new_connection(s_webrtc.peer);
    if (peer_err != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_new_connection failed: %d", peer_err);
        return app_webrtc_peer_err(peer_err);
    }

    if (xSemaphoreTake(s_webrtc.offer_ready, pdMS_TO_TICKS(APP_WEBRTC_OFFER_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (xSemaphoreTake(s_webrtc.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (s_webrtc.pending_offer != NULL) {
        offer_copy = strdup(s_webrtc.pending_offer);
    }
    xSemaphoreGive(s_webrtc.lock);

    if (offer_copy == NULL) {
        return ESP_FAIL;
    }
    out_offer->sdp_offer = offer_copy;
    return ESP_OK;
}

esp_err_t app_webrtc_set_remote_answer(const char *sdp_answer)
{
    esp_peer_msg_t msg;
    int peer_err;

    if (sdp_answer == NULL || s_webrtc.peer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    msg.type = ESP_PEER_MSG_TYPE_SDP;
    msg.data = (uint8_t *)sdp_answer;
    msg.size = (int)strlen(sdp_answer);

    peer_err = esp_peer_send_msg(s_webrtc.peer, &msg);
    if (peer_err != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_send_msg(answer) failed: %d", peer_err);
        return app_webrtc_peer_err(peer_err);
    }
    return ESP_OK;
}

esp_err_t app_webrtc_start(void)
{
    return app_webrtc_init();
}

esp_err_t app_webrtc_send_audio(const void *data, size_t size, uint32_t pts)
{
    esp_peer_audio_frame_t frame;
    int peer_err;

    if (data == NULL || size == 0 || s_webrtc.peer == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!app_webrtc_is_audio_send_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    frame.data = (uint8_t *)data;
    frame.size = (int)size;
    frame.pts = pts;

    peer_err = esp_peer_send_audio(s_webrtc.peer, &frame);
    if (peer_err != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "esp_peer_send_audio failed: %d", peer_err);
        return app_webrtc_peer_err(peer_err);
    }
    return ESP_OK;
}

bool app_webrtc_is_audio_send_ready(void)
{
    return s_webrtc.state == ESP_PEER_STATE_CONNECTED;
}

void app_webrtc_destroy_offer(app_webrtc_offer_t *offer)
{
    if (offer == NULL) {
        return;
    }
    free(offer->sdp_offer);
    offer->sdp_offer = NULL;
}
