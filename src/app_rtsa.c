#include "app_rtsa.h"

#include <stdlib.h>
#include <string.h>

#include "agora_rtc_api.h"
#include "app_config.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"

typedef struct {
    bool initialized;
    bool joined;
    connection_id_t conn_id;
    app_protocol_config_t config;
    app_session_audio_info_cb_t info_cb;
    app_session_audio_frame_cb_t frame_cb;
    void *cb_ctx;
    audio_data_type_e rx_data_type;
    bool rx_stream_info_emitted;
    uint32_t rx_frame_count;
    bool rx_first_frame_logged;
} app_rtsa_ctx_t;

static const char *TAG = "app_rtsa";
static app_rtsa_ctx_t s_rtsa;

static audio_codec_type_e app_rtsa_audio_codec_type(void)
{
#if APP_AUDIO_CODEC == APP_AUDIO_CODEC_G711A
    return AUDIO_CODEC_DISABLED;
#elif APP_AUDIO_CODEC == APP_AUDIO_CODEC_G722
    return AUDIO_CODEC_TYPE_G722;
#else
    return AUDIO_CODEC_TYPE_OPUS;
#endif
}

static audio_data_type_e app_rtsa_rx_type_from_name(void)
{
    if (strcmp(APP_AGORA_PROTOCOL_OUTPUT_AUDIO_CODEC, "g722") == 0) {
        return AUDIO_DATA_TYPE_G722;
    }
    if (strcmp(APP_AGORA_PROTOCOL_OUTPUT_AUDIO_CODEC, "g711a") == 0 || strcmp(APP_AGORA_PROTOCOL_OUTPUT_AUDIO_CODEC, "pcma") == 0) {
        return AUDIO_DATA_TYPE_PCMA;
    }
    if (strcmp(APP_AGORA_PROTOCOL_OUTPUT_AUDIO_CODEC, "opus") == 0) {
        return AUDIO_DATA_TYPE_OPUS;
    }
    return AUDIO_DATA_TYPE_G722;
}

static app_session_audio_codec_t app_rtsa_map_codec(audio_data_type_e data_type)
{
    switch (data_type) {
    case AUDIO_DATA_TYPE_PCMA:
        return APP_SESSION_AUDIO_CODEC_G711A;
    case AUDIO_DATA_TYPE_G722:
        return APP_SESSION_AUDIO_CODEC_G722;
    case AUDIO_DATA_TYPE_OPUS:
    case AUDIO_DATA_TYPE_OPUSFB:
        return APP_SESSION_AUDIO_CODEC_OPUS;
    case AUDIO_DATA_TYPE_PCM:
        return APP_SESSION_AUDIO_CODEC_PCM16;
    default:
        return APP_SESSION_AUDIO_CODEC_PCM16;
    }
}

static uint32_t app_rtsa_codec_sample_rate(audio_data_type_e data_type)
{
    switch (data_type) {
    case AUDIO_DATA_TYPE_PCMA:
    case AUDIO_DATA_TYPE_PCMU:
        return 8000U;
    case AUDIO_DATA_TYPE_G722:
    case AUDIO_DATA_TYPE_OPUS:
        return 16000U;
    case AUDIO_DATA_TYPE_OPUSFB:
        return 48000U;
    case AUDIO_DATA_TYPE_PCM:
    default:
        return APP_AUDIO_SAMPLE_RATE;
    }
}

static void app_rtsa_on_join_channel_success(connection_id_t conn_id, uint32_t uid, int elapsed_ms)
{
    s_rtsa.joined = true;
    ESP_LOGI(TAG, "Joined RTC channel successfully: conn=%u uid=%u elapsed=%dms", conn_id, uid, elapsed_ms);
}

static void app_rtsa_on_reconnecting(connection_id_t conn_id)
{
    s_rtsa.joined = false;
    ESP_LOGW(TAG, "RTC reconnecting: conn=%u", conn_id);
}

static void app_rtsa_on_connection_lost(connection_id_t conn_id)
{
    s_rtsa.joined = false;
    ESP_LOGW(TAG, "RTC connection lost: conn=%u", conn_id);
}

static void app_rtsa_on_rejoin_channel_success(connection_id_t conn_id, uint32_t uid, int elapsed_ms)
{
    s_rtsa.joined = true;
    ESP_LOGI(TAG, "RTC rejoined channel successfully: conn=%u uid=%u elapsed=%dms", conn_id, uid, elapsed_ms);
}

static void app_rtsa_on_error(connection_id_t conn_id, int code, const char *msg)
{
    ESP_LOGE(TAG, "RTC error conn=%u code=%d msg=%s", conn_id, code, msg ? msg : "<null>");
}

static void app_rtsa_on_token_privilege_will_expire(connection_id_t conn_id, const char *token)
{
    (void)token;
    ESP_LOGW(TAG, "RTC token will expire soon on conn=%u; renewal path not implemented yet", conn_id);
}

static void app_rtsa_on_user_joined(connection_id_t conn_id, uint32_t uid, int elapsed_ms)
{
    ESP_LOGI(TAG, "Remote user joined: conn=%u uid=%u elapsed=%dms expected_agent_uid=%s",
             conn_id, uid, elapsed_ms, s_rtsa.config.agent_uid);
}

static void app_rtsa_on_user_offline(connection_id_t conn_id, uint32_t uid, int reason)
{
    ESP_LOGI(TAG, "Remote user offline: conn=%u uid=%u reason=%d expected_agent_uid=%s",
             conn_id, uid, reason, s_rtsa.config.agent_uid);
}

static void app_rtsa_on_user_mute_audio(connection_id_t conn_id, uint32_t uid, bool muted)
{
    ESP_LOGI(TAG, "Remote user audio mute changed: conn=%u uid=%u muted=%d expected_agent_uid=%s",
             conn_id, uid, muted ? 1 : 0, s_rtsa.config.agent_uid);
}

static void app_rtsa_emit_audio_info(audio_data_type_e data_type)
{
    if (s_rtsa.info_cb != NULL) {
        s_rtsa.info_cb(app_rtsa_map_codec(data_type), app_rtsa_codec_sample_rate(data_type), 1U, s_rtsa.cb_ctx);
    }
}

static void app_rtsa_on_audio_data(connection_id_t conn_id,
                                   uint32_t uid,
                                   uint16_t sent_ts,
                                   const void *data_ptr,
                                   size_t data_len,
                                   const audio_frame_info_t *info_ptr)
{
    audio_data_type_e data_type = AUDIO_DATA_TYPE_PCM;

    if (info_ptr != NULL) {
        data_type = info_ptr->data_type;
    }
    if (data_ptr != NULL && data_len > 0) {
        s_rtsa.rx_frame_count++;
        if (!s_rtsa.rx_first_frame_logged) {
            s_rtsa.rx_first_frame_logged = true;
            ESP_LOGI(TAG,
                     "First remote audio frame: conn=%u uid=%u data_type=%d bytes=%u sent_ts=%u",
                     conn_id, uid, (int)data_type, (unsigned)data_len, (unsigned)sent_ts);
        } else if ((s_rtsa.rx_frame_count % 250U) == 0U) {
            ESP_LOGI(TAG,
                     "Remote audio frames received: %u (conn=%u uid=%u data_type=%d last_bytes=%u sent_ts=%u)",
                     (unsigned)s_rtsa.rx_frame_count,
                     conn_id,
                     uid,
                     (int)data_type,
                     (unsigned)data_len,
                     (unsigned)sent_ts);
        }
    }
    if (!s_rtsa.rx_stream_info_emitted || s_rtsa.rx_data_type != data_type) {
        s_rtsa.rx_data_type = data_type;
        s_rtsa.rx_stream_info_emitted = true;
        ESP_LOGI(TAG, "Remote audio stream callback fired: conn=%u uid=%u data_type=%d sample_rate=%u",
                 conn_id, uid, (int)data_type, (unsigned)app_rtsa_codec_sample_rate(data_type));
        app_rtsa_emit_audio_info(data_type);
    }
    if (s_rtsa.frame_cb != NULL && data_ptr != NULL && data_len > 0) {
        s_rtsa.frame_cb(app_rtsa_map_codec(data_type), data_ptr, data_len, (uint32_t)sent_ts, s_rtsa.cb_ctx);
    }
}

static void app_rtsa_on_mixed_audio_data(connection_id_t conn_id,
                                         const void *data_ptr,
                                         size_t data_len,
                                         const audio_frame_info_t *info_ptr)
{
    (void)conn_id;
    app_rtsa_on_audio_data(conn_id, 0U, 0U, data_ptr, data_len, info_ptr);
}

esp_err_t app_rtsa_set_audio_receive_callbacks(app_session_audio_info_cb_t info_cb,
                                               app_session_audio_frame_cb_t frame_cb,
                                               void *ctx)
{
    s_rtsa.info_cb = info_cb;
    s_rtsa.frame_cb = frame_cb;
    s_rtsa.cb_ctx = ctx;
    return ESP_OK;
}

esp_err_t app_rtsa_start(const app_protocol_config_t *config)
{
    agora_rtc_event_handler_t handler = { 0 };
    rtc_service_option_t service_opt = { 0 };
    rtc_channel_options_t channel_options = { 0 };
    app_session_audio_info_cb_t info_cb = s_rtsa.info_cb;
    app_session_audio_frame_cb_t frame_cb = s_rtsa.frame_cb;
    void *cb_ctx = s_rtsa.cb_ctx;
    int err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&s_rtsa, 0, sizeof(s_rtsa));
    s_rtsa.info_cb = info_cb;
    s_rtsa.frame_cb = frame_cb;
    s_rtsa.cb_ctx = cb_ctx;
    s_rtsa.config = *config;
    s_rtsa.rx_data_type = AUDIO_DATA_TYPE_UNKNOW;
    s_rtsa.rx_stream_info_emitted = false;

    handler.on_join_channel_success = app_rtsa_on_join_channel_success;
    handler.on_reconnecting = app_rtsa_on_reconnecting;
    handler.on_connection_lost = app_rtsa_on_connection_lost;
    handler.on_rejoin_channel_success = app_rtsa_on_rejoin_channel_success;
    handler.on_error = app_rtsa_on_error;
    handler.on_token_privilege_will_expire = app_rtsa_on_token_privilege_will_expire;
    handler.on_user_joined = app_rtsa_on_user_joined;
    handler.on_user_offline = app_rtsa_on_user_offline;
    handler.on_user_mute_audio = app_rtsa_on_user_mute_audio;
    handler.on_audio_data = app_rtsa_on_audio_data;
    handler.on_mixed_audio_data = app_rtsa_on_mixed_audio_data;

    service_opt.area_code = APP_AGORA_AREA_CODE;
    service_opt.log_cfg.log_level = RTC_LOG_ERROR;
    service_opt.log_cfg.log_disable = true;
    service_opt.log_cfg.log_path = "io.agora.rtsa";

    ESP_LOGI(TAG, "Free heap before agora_rtc_init: %u", (unsigned)esp_get_free_heap_size());

    err = agora_rtc_init(config->app_id, &handler, &service_opt);
    if (err < 0) {
        ESP_LOGE(TAG, "agora_rtc_init failed: %d", err);
        return ESP_FAIL;
    }

    err = agora_rtc_create_connection(&s_rtsa.conn_id);
    if (err < 0) {
        ESP_LOGE(TAG, "agora_rtc_create_connection failed: %d", err);
        return ESP_FAIL;
    }

    channel_options.auto_subscribe_audio = APP_AUDIO_ENABLE_PULL_PLAYBACK;
    channel_options.auto_subscribe_video = false;
    channel_options.enable_audio_jitter_buffer = false;
    channel_options.enable_audio_mixer = false;
    channel_options.audio_codec_opt.audio_codec_type = app_rtsa_audio_codec_type();
    channel_options.audio_codec_opt.pcm_sample_rate = APP_AUDIO_SAMPLE_RATE;
    channel_options.audio_codec_opt.pcm_channel_num = 1;
    channel_options.audio_codec_opt.pcm_duration = APP_AUDIO_FRAME_MS;

#if APP_AUDIO_CODEC == APP_AUDIO_CODEC_G711A
    ESP_LOGI(TAG, "RTSA audio codec mode: external G711A encode, SDK codec disabled");
#else
    ESP_LOGI(TAG, "RTSA audio codec mode: SDK encoder=%d", (int)channel_options.audio_codec_opt.audio_codec_type);
#endif

    err = agora_rtc_join_channel(s_rtsa.conn_id,
                                 config->channel_name,
                                 (uint32_t)strtoul(config->uid, NULL, 10),
                                 config->token,
                                 &channel_options);
    if (err < 0) {
        ESP_LOGE(TAG, "agora_rtc_join_channel failed: %d", err);
        return ESP_FAIL;
    }

    s_rtsa.initialized = true;
    ESP_LOGI(TAG, "Free heap after agora_rtc_join_channel: %u", (unsigned)esp_get_free_heap_size());
    ESP_LOGI(TAG, "RTSA session started: channel=%s uid=%s agent_uid=%s",
             config->channel_name, config->uid, config->agent_uid);
    return ESP_OK;
}

bool app_rtsa_is_audio_send_ready(void)
{
    return s_rtsa.initialized && s_rtsa.joined;
}

esp_err_t app_rtsa_send_audio(const void *data, size_t size, uint32_t pts)
{
    audio_frame_info_t info = { 0 };
    int err;

    if (data == NULL || size == 0 || !app_rtsa_is_audio_send_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

#if APP_AUDIO_CODEC == APP_AUDIO_CODEC_G711A
    info.data_type = AUDIO_DATA_TYPE_PCMA;
#elif APP_AUDIO_CODEC == APP_AUDIO_CODEC_G722
    info.data_type = AUDIO_DATA_TYPE_PCM;
#else
    info.data_type = AUDIO_DATA_TYPE_PCM;
#endif

    (void)pts;
    err = agora_rtc_send_audio_data(s_rtsa.conn_id, data, size, &info);
    if (err < 0) {
        ESP_LOGW(TAG, "agora_rtc_send_audio_data failed: %d", err);
        return ESP_FAIL;
    }
    return ESP_OK;
}
