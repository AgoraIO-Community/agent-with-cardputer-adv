#include "app_audio_controller.h"

#include <string.h>

#include "app_audio_fake.h"
#include "app_audio_i2s.h"
#include "app_audio_playback.h"
#include "app_codec_g711.h"
#include "app_codec_g722.h"
#include "app_config.h"
#include "app_session.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define APP_AUDIO_CONTROLLER_STACK_SIZE 4096

typedef struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    bool initialized;
    bool running;
    app_audio_mode_t mode;
    int64_t last_mode_switch_us;
    int64_t last_remote_active_frame_us;
    int64_t playback_rearm_after_us;
    uint32_t silence_pts;
    uint32_t silence_frame_count;
    bool playback_stream_info_opened;
    bool remote_audio_seen;
    app_session_audio_codec_t remote_codec;
    uint32_t remote_sample_rate;
    uint8_t remote_channels;
} app_audio_controller_ctx_t;

static const char *TAG = "app_audio_ctrl";
static app_audio_controller_ctx_t s_audio_controller;

static bool app_audio_controller_is_half_duplex_managed(void)
{
    return APP_AUDIO_USE_I2S_MIC && APP_AUDIO_ENABLE_PULL_PLAYBACK && APP_AUDIO_ENABLE_HALF_DUPLEX_GATING;
}

static bool app_audio_controller_can_return_to_recording(int64_t now_us)
{
    if (s_audio_controller.last_mode_switch_us == 0) {
        return true;
    }
    return (now_us - s_audio_controller.last_mode_switch_us) >= ((int64_t)APP_AUDIO_MODE_MIN_HOLD_MS * 1000LL);
}

static bool app_audio_controller_can_reenter_playback(int64_t now_us, bool strong_frame)
{
    if (strong_frame) {
        return true;
    }
    return s_audio_controller.playback_rearm_after_us == 0 ||
        now_us >= s_audio_controller.playback_rearm_after_us;
}

static esp_err_t app_audio_controller_enter_recording_locked(int64_t now_us, bool allow_hold_bypass)
{
    esp_err_t err;
    bool from_playback = s_audio_controller.mode == APP_AUDIO_MODE_PLAYBACK;

    if (s_audio_controller.mode == APP_AUDIO_MODE_RECORDING) {
        return ESP_OK;
    }
    if (!allow_hold_bypass && !app_audio_controller_can_return_to_recording(now_us)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (APP_AUDIO_ENABLE_PULL_PLAYBACK) {
        err = app_audio_playback_stop();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop speaker path: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (APP_AUDIO_USE_I2S_MIC) {
        if (from_playback && !app_audio_controller_is_half_duplex_managed()) {
            app_audio_i2s_mark_post_playback_recovery();
        }
        err = app_audio_i2s_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start mic path: %s", esp_err_to_name(err));
            return err;
        }
    } else {
        err = app_audio_fake_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start fake audio path: %s", esp_err_to_name(err));
            return err;
        }
    }

    s_audio_controller.mode = APP_AUDIO_MODE_RECORDING;
    s_audio_controller.last_mode_switch_us = now_us;
    if (from_playback) {
        s_audio_controller.playback_rearm_after_us =
            now_us + ((int64_t)APP_AUDIO_PLAYBACK_REARM_COOLDOWN_MS * 1000LL);
        ESP_LOGI(TAG, "Audio mode -> RECORDING reason=playback_tail_expired rearm_cooldown_ms=%u",
                 (unsigned)APP_AUDIO_PLAYBACK_REARM_COOLDOWN_MS);
    } else {
        ESP_LOGI(TAG, "Audio mode -> RECORDING reason=start");
    }
    return ESP_OK;
}

static esp_err_t app_audio_controller_enter_playback_locked(int64_t now_us)
{
    esp_err_t err;

    if (s_audio_controller.mode == APP_AUDIO_MODE_PLAYBACK) {
        return ESP_OK;
    }

    if (APP_AUDIO_USE_I2S_MIC) {
        err = app_audio_controller_is_half_duplex_managed()
            ? app_audio_i2s_pause_for_playback()
            : app_audio_i2s_stop();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop mic path: %s", esp_err_to_name(err));
            return err;
        }
    } else {
        err = app_audio_fake_stop();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop fake audio path: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (APP_AUDIO_ENABLE_PULL_PLAYBACK) {
        err = app_audio_playback_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to init speaker path: %s", esp_err_to_name(err));
            return err;
        }
    }

    s_audio_controller.mode = APP_AUDIO_MODE_PLAYBACK;
    s_audio_controller.last_mode_switch_us = now_us;
    s_audio_controller.silence_frame_count = 0;
    s_audio_controller.playback_rearm_after_us = 0;
    ESP_LOGI(TAG, "Audio mode -> PLAYBACK reason=remote_active_audio");
    return ESP_OK;
}

static esp_err_t app_audio_controller_send_silence_frame(void)
{
    const uint32_t samples_per_frame = (APP_AUDIO_SAMPLE_RATE * APP_AUDIO_FRAME_MS) / 1000U;
    int16_t pcm_frame[samples_per_frame];
    uint8_t encoded_frame[samples_per_frame];
    const void *frame_data = pcm_frame;
    size_t frame_size = sizeof(pcm_frame);

    memset(pcm_frame, 0, sizeof(pcm_frame));
    memset(encoded_frame, 0, sizeof(encoded_frame));

#if APP_AUDIO_CODEC == APP_AUDIO_CODEC_G711A
    frame_size = app_codec_g711a_encode(pcm_frame, samples_per_frame, encoded_frame, sizeof(encoded_frame));
    frame_data = encoded_frame;
#elif APP_AUDIO_CODEC == APP_AUDIO_CODEC_G722
    frame_size = app_codec_g722_encode(pcm_frame, samples_per_frame, encoded_frame, sizeof(encoded_frame));
    frame_data = encoded_frame;
#endif

    if (app_session_send_audio(frame_data, frame_size, s_audio_controller.silence_pts) != ESP_OK) {
        return ESP_FAIL;
    }

    s_audio_controller.silence_pts += samples_per_frame;
    s_audio_controller.silence_frame_count++;
    if ((s_audio_controller.silence_frame_count % (5000U / APP_AUDIO_FRAME_MS)) == 0U) {
        ESP_LOGI(TAG, "Silence uplink frames sent during playback: %u",
                 (unsigned)s_audio_controller.silence_frame_count);
    }
    return ESP_OK;
}

static bool app_audio_controller_remote_frame_get_activity(app_session_audio_codec_t codec,
                                                           const void *data,
                                                           size_t size,
                                                           bool *strong_frame)
{
    int16_t decoded_pcm[320];
    size_t sample_count = 0;
    int64_t abs_sum = 0;
    int32_t peak = 0;
    bool all_same_encoded = false;

    if (data == NULL || size == 0) {
        if (strong_frame != NULL) {
            *strong_frame = false;
        }
        return false;
    }

    if (codec == APP_SESSION_AUDIO_CODEC_G711A) {
        const uint8_t *encoded = (const uint8_t *)data;
        all_same_encoded = true;
        for (size_t i = 1; i < size; i++) {
            if (encoded[i] != encoded[0]) {
                all_same_encoded = false;
                break;
            }
        }
        sample_count = app_codec_g711a_decode(data, size, decoded_pcm,
                                              sizeof(decoded_pcm) / sizeof(decoded_pcm[0]));
    } else if (codec == APP_SESSION_AUDIO_CODEC_G722) {
        sample_count = app_codec_g722_decode(data, size, decoded_pcm,
                                             sizeof(decoded_pcm) / sizeof(decoded_pcm[0]));
    } else {
        sample_count = size / sizeof(decoded_pcm[0]);
        if (sample_count > (sizeof(decoded_pcm) / sizeof(decoded_pcm[0]))) {
            sample_count = sizeof(decoded_pcm) / sizeof(decoded_pcm[0]);
        }
        memcpy(decoded_pcm, data, sample_count * sizeof(decoded_pcm[0]));
    }

    for (size_t i = 0; i < sample_count; i++) {
        int32_t magnitude = decoded_pcm[i] >= 0 ? decoded_pcm[i] : -(int32_t)decoded_pcm[i];
        abs_sum += magnitude;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }

    if (codec == APP_SESSION_AUDIO_CODEC_G711A && all_same_encoded && peak <= 32) {
        if (strong_frame != NULL) {
            *strong_frame = false;
        }
        return false;
    }

    if (strong_frame != NULL) {
        *strong_frame = sample_count > 0 &&
            ((abs_sum / (int64_t)sample_count) >= APP_AUDIO_REMOTE_REARM_AVG_ABS_THRESHOLD ||
             peak >= APP_AUDIO_REMOTE_REARM_PEAK_THRESHOLD);
    }

    return sample_count > 0 &&
        ((abs_sum / (int64_t)sample_count) >= APP_AUDIO_REMOTE_ACTIVE_AVG_ABS_THRESHOLD ||
         peak >= APP_AUDIO_REMOTE_ACTIVE_PEAK_THRESHOLD);
}

static void app_audio_controller_task(void *arg)
{
    const TickType_t frame_ticks = pdMS_TO_TICKS(APP_AUDIO_FRAME_MS);
    TickType_t last_wake_time = xTaskGetTickCount();

    (void)arg;

    while (s_audio_controller.running) {
        int64_t now_us = esp_timer_get_time();
        bool should_send_silence = false;
        bool should_return_to_recording = false;

        if (xSemaphoreTake(s_audio_controller.lock, portMAX_DELAY) == pdTRUE) {
            if (s_audio_controller.mode == APP_AUDIO_MODE_PLAYBACK) {
                should_send_silence = APP_AUDIO_SEND_SILENCE_DURING_PLAYBACK != 0;
                if (s_audio_controller.last_remote_active_frame_us != 0 &&
                    (now_us - s_audio_controller.last_remote_active_frame_us) >
                        ((int64_t)APP_AUDIO_REMOTE_TAIL_MS * 1000LL) &&
                    app_audio_controller_can_return_to_recording(now_us)) {
                    should_return_to_recording = true;
                }
            }
            xSemaphoreGive(s_audio_controller.lock);
        }

        if (should_send_silence && app_session_is_audio_send_ready()) {
            if (app_audio_controller_send_silence_frame() != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send silence uplink frame");
            }
        }

        if (should_return_to_recording) {
            if (xSemaphoreTake(s_audio_controller.lock, portMAX_DELAY) == pdTRUE) {
                now_us = esp_timer_get_time();
                if (s_audio_controller.mode == APP_AUDIO_MODE_PLAYBACK &&
                    s_audio_controller.last_remote_active_frame_us != 0 &&
                    (now_us - s_audio_controller.last_remote_active_frame_us) >
                        ((int64_t)APP_AUDIO_REMOTE_TAIL_MS * 1000LL)) {
                    if (app_audio_controller_enter_recording_locked(now_us, false) != ESP_OK) {
                        ESP_LOGW(TAG, "Playback tail expired, but recording switch deferred");
                    } else {
                        s_audio_controller.last_remote_active_frame_us = 0;
                        s_audio_controller.playback_stream_info_opened = false;
                    }
                }
                xSemaphoreGive(s_audio_controller.lock);
            }
        }

        vTaskDelayUntil(&last_wake_time, frame_ticks);
    }

    s_audio_controller.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t app_audio_controller_init(void)
{
    if (s_audio_controller.initialized) {
        return ESP_OK;
    }

    memset(&s_audio_controller, 0, sizeof(s_audio_controller));
    s_audio_controller.lock = xSemaphoreCreateMutex();
    if (s_audio_controller.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (APP_AUDIO_ENABLE_PULL_PLAYBACK) {
        ESP_RETURN_ON_ERROR(app_audio_playback_init(), TAG, "Failed to initialize playback module");
    }

    s_audio_controller.initialized = true;
    s_audio_controller.mode = APP_AUDIO_MODE_IDLE;
    return ESP_OK;
}

esp_err_t app_audio_controller_start(void)
{
    int64_t now_us = esp_timer_get_time();

    if (!s_audio_controller.initialized) {
        ESP_RETURN_ON_ERROR(app_audio_controller_init(), TAG, "Failed to initialize controller");
    }
    if (s_audio_controller.running) {
        return ESP_OK;
    }

    s_audio_controller.running = true;
    s_audio_controller.last_remote_active_frame_us = 0;
    s_audio_controller.silence_pts = 0;
    s_audio_controller.silence_frame_count = 0;
    s_audio_controller.playback_stream_info_opened = false;
    s_audio_controller.remote_audio_seen = false;
    s_audio_controller.playback_rearm_after_us = 0;

    if (xSemaphoreTake(s_audio_controller.lock, portMAX_DELAY) != pdTRUE) {
        s_audio_controller.running = false;
        return ESP_FAIL;
    }
    if (app_audio_controller_enter_recording_locked(now_us, true) != ESP_OK) {
        xSemaphoreGive(s_audio_controller.lock);
        s_audio_controller.running = false;
        return ESP_FAIL;
    }
    xSemaphoreGive(s_audio_controller.lock);

    if (app_audio_controller_is_half_duplex_managed()) {
        if (xTaskCreate(app_audio_controller_task, "app_audio_ctrl", APP_AUDIO_CONTROLLER_STACK_SIZE,
                        NULL, 4, &s_audio_controller.task) != pdPASS) {
            s_audio_controller.running = false;
            if (xSemaphoreTake(s_audio_controller.lock, portMAX_DELAY) == pdTRUE) {
                (void)app_audio_i2s_stop();
                (void)app_audio_fake_stop();
                (void)app_audio_playback_stop();
                s_audio_controller.mode = APP_AUDIO_MODE_IDLE;
                xSemaphoreGive(s_audio_controller.lock);
            }
            return ESP_ERR_NO_MEM;
        }
    }

    return ESP_OK;
}

esp_err_t app_audio_controller_stop(void)
{
    esp_err_t ret = ESP_OK;

    if (!s_audio_controller.initialized) {
        return ESP_OK;
    }

    s_audio_controller.running = false;
    if (xSemaphoreTake(s_audio_controller.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (APP_AUDIO_USE_I2S_MIC) {
        ret = app_audio_i2s_stop();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop mic path: %s", esp_err_to_name(ret));
        }
    } else {
        ret = app_audio_fake_stop();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop fake audio path: %s", esp_err_to_name(ret));
        }
    }
    if (APP_AUDIO_ENABLE_PULL_PLAYBACK) {
        esp_err_t playback_ret = app_audio_playback_stop();
        if (playback_ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to stop playback path: %s", esp_err_to_name(playback_ret));
            if (ret == ESP_OK) {
                ret = playback_ret;
            }
        }
    }

    s_audio_controller.mode = APP_AUDIO_MODE_IDLE;
    s_audio_controller.last_mode_switch_us = 0;
    s_audio_controller.last_remote_active_frame_us = 0;
    s_audio_controller.playback_stream_info_opened = false;
    s_audio_controller.playback_rearm_after_us = 0;
    xSemaphoreGive(s_audio_controller.lock);
    return ret;
}

void app_audio_controller_handle_remote_stream_info(app_session_audio_codec_t codec,
                                                    uint32_t sample_rate,
                                                    uint8_t channels)
{
    if (!APP_AUDIO_ENABLE_PULL_PLAYBACK || !s_audio_controller.initialized || !s_audio_controller.running) {
        return;
    }

    if (xSemaphoreTake(s_audio_controller.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    s_audio_controller.remote_codec = codec;
    s_audio_controller.remote_sample_rate = sample_rate;
    s_audio_controller.remote_channels = channels;
    if (!app_audio_controller_is_half_duplex_managed()) {
        s_audio_controller.playback_stream_info_opened = true;
    }
    xSemaphoreGive(s_audio_controller.lock);

    if (!app_audio_controller_is_half_duplex_managed()) {
        app_audio_playback_handle_stream_info(codec, sample_rate, channels);
    }
}

void app_audio_controller_handle_remote_audio_frame(app_session_audio_codec_t codec,
                                                    const void *data,
                                                    size_t size,
                                                    uint32_t pts)
{
    bool half_duplex = app_audio_controller_is_half_duplex_managed();
    bool active_frame;
    bool strong_frame = false;
    bool open_stream_info = false;
    int64_t now_us;

    if (!APP_AUDIO_ENABLE_PULL_PLAYBACK || !s_audio_controller.initialized || !s_audio_controller.running) {
        return;
    }

    active_frame = app_audio_controller_remote_frame_get_activity(codec, data, size, &strong_frame);

    if (xSemaphoreTake(s_audio_controller.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    now_us = esp_timer_get_time();
    if (active_frame) {
        s_audio_controller.last_remote_active_frame_us = now_us;
    }
    if (half_duplex) {
        if (s_audio_controller.mode != APP_AUDIO_MODE_PLAYBACK && !active_frame) {
            xSemaphoreGive(s_audio_controller.lock);
            return;
        }
        if (s_audio_controller.mode != APP_AUDIO_MODE_PLAYBACK) {
            if (!app_audio_controller_can_reenter_playback(now_us, strong_frame)) {
                xSemaphoreGive(s_audio_controller.lock);
                return;
            }
            if (app_audio_controller_enter_playback_locked(s_audio_controller.last_remote_active_frame_us) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to switch into playback mode on remote audio frame");
                xSemaphoreGive(s_audio_controller.lock);
                return;
            }
            open_stream_info = !s_audio_controller.playback_stream_info_opened;
            s_audio_controller.playback_stream_info_opened = true;
        }
    }
    xSemaphoreGive(s_audio_controller.lock);

    if (open_stream_info) {
        app_audio_playback_handle_stream_info(s_audio_controller.remote_codec,
                                              s_audio_controller.remote_sample_rate,
                                              s_audio_controller.remote_channels);
    }
    s_audio_controller.remote_audio_seen = true;
    app_audio_playback_handle_audio_frame(codec, data, size, pts);
}

bool app_audio_controller_is_in_playback(void)
{
    return s_audio_controller.mode == APP_AUDIO_MODE_PLAYBACK;
}

app_audio_mode_t app_audio_controller_get_mode(void)
{
    return s_audio_controller.mode;
}

bool app_audio_controller_has_received_remote_audio(void)
{
    return s_audio_controller.remote_audio_seen;
}
