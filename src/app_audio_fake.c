#include "app_audio_fake.h"

#include <string.h>

#include "app_codec_g711.h"
#include "app_config.h"
#include "app_webrtc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define APP_AUDIO_FAKE_STACK_SIZE 4096

typedef struct {
    TaskHandle_t task;
    bool running;
    uint32_t frame_count;
    uint32_t pts;
} app_audio_fake_ctx_t;

static const char *TAG = "app_audio_fake";
static app_audio_fake_ctx_t s_audio_fake;

static void app_audio_fake_task(void *arg)
{
    const TickType_t frame_ticks = pdMS_TO_TICKS(APP_AUDIO_FRAME_MS);
    const uint32_t samples_per_frame = (APP_AUDIO_SAMPLE_RATE * APP_AUDIO_FRAME_MS) / 1000U;
    int16_t pcm_frame[samples_per_frame];
    uint8_t encoded_frame[samples_per_frame];
    TickType_t last_wake_time = xTaskGetTickCount();
    bool waiting_logged = false;

    (void)arg;
    memset(pcm_frame, 0, sizeof(pcm_frame));
    memset(encoded_frame, 0, sizeof(encoded_frame));

    while (s_audio_fake.running) {
        if (!app_webrtc_is_audio_send_ready()) {
            if (!waiting_logged) {
                ESP_LOGI(TAG, "Waiting for WebRTC connected state before sending fake audio");
                waiting_logged = true;
            }
            vTaskDelayUntil(&last_wake_time, frame_ticks);
            continue;
        }
        waiting_logged = false;

        const void *frame_data = pcm_frame;
        size_t frame_size = sizeof(pcm_frame);
#if APP_AUDIO_CODEC == APP_AUDIO_CODEC_G711A
        frame_size = app_codec_g711a_encode(pcm_frame, samples_per_frame, encoded_frame, sizeof(encoded_frame));
        frame_data = encoded_frame;
#endif
        esp_err_t err = app_webrtc_send_audio(frame_data, frame_size, s_audio_fake.pts);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send fake audio frame: %s", esp_err_to_name(err));
            vTaskDelayUntil(&last_wake_time, frame_ticks);
            continue;
        }
        s_audio_fake.frame_count++;
        s_audio_fake.pts += samples_per_frame;

        if ((s_audio_fake.frame_count % (5000U / APP_AUDIO_FRAME_MS)) == 0U) {
            ESP_LOGI(TAG, "Fake audio frames sent: %u", (unsigned)s_audio_fake.frame_count);
        }
        vTaskDelayUntil(&last_wake_time, frame_ticks);
    }

    s_audio_fake.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t app_audio_fake_start(void)
{
    if (s_audio_fake.running) {
        return ESP_OK;
    }

    s_audio_fake.running = true;
    s_audio_fake.frame_count = 0;
    s_audio_fake.pts = 0;

    if (xTaskCreate(app_audio_fake_task, "app_audio_fake", APP_AUDIO_FAKE_STACK_SIZE, NULL, 4,
                    &s_audio_fake.task) != pdPASS) {
        s_audio_fake.running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Fake audio publisher started");
    return ESP_OK;
}

esp_err_t app_audio_fake_stop(void)
{
    if (!s_audio_fake.running) {
        return ESP_OK;
    }
    s_audio_fake.running = false;
    return ESP_OK;
}
