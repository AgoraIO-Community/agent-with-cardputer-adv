#include "app_audio_playback.h"

#include <stdbool.h>
#include <math.h>
#include <string.h>

#include "app_audio_adv_codec.h"
#include "app_codec_g711.h"
#include "app_codec_g722.h"
#include "app_config.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define APP_AUDIO_PLAYBACK_WRITE_TIMEOUT_MS 100
#define APP_AUDIO_PLAYBACK_ACTIVE_AVG_ABS_THRESHOLD 600
#define APP_AUDIO_PLAYBACK_ACTIVE_PEAK_THRESHOLD 1200

typedef struct {
    SemaphoreHandle_t lock;
    i2s_chan_handle_t tx_handle;
    app_session_audio_codec_t codec;
    uint32_t sample_rate;
    uint32_t configured_sample_rate;
    uint8_t channels;
    bool initialized;
    bool codec_stack_inited;
    bool stream_open;
    bool first_frame_logged;
    bool remote_audio_seen;
    bool remote_audio_audible_seen;
    int64_t last_frame_us;
    int64_t last_audible_frame_us;
    uint32_t write_count;
    uint32_t write_error_count;
    uint32_t decoded_log_count;
    uint32_t compressed_log_count;
    uint32_t unsupported_log_count;
} app_audio_playback_ctx_t;

static const char *TAG = "app_audio_playback";
static app_audio_playback_ctx_t s_playback;

static esp_err_t app_audio_playback_close_stream_locked(void);

static int app_audio_playback_i2s_port(void)
{
    return APP_AUDIO_I2S_SPK_PORT;
}

static void app_audio_playback_effective_gain(int32_t *num, int32_t *den)
{
    int32_t configured_num = APP_AUDIO_PLAYBACK_GAIN_NUM;
    int32_t configured_den = APP_AUDIO_PLAYBACK_GAIN_DEN;
    int32_t max_num = APP_AUDIO_PLAYBACK_MAX_EFFECTIVE_GAIN_NUM;
    int32_t max_den = APP_AUDIO_PLAYBACK_MAX_EFFECTIVE_GAIN_DEN;

    if (configured_den <= 0) {
        configured_den = 1;
    }
    if (max_den <= 0) {
        max_den = 1;
    }
    if (((int64_t)configured_num * max_den) > ((int64_t)max_num * configured_den)) {
        configured_num = max_num;
        configured_den = max_den;
    }

    *num = configured_num;
    *den = configured_den;
}

static int16_t app_audio_playback_soft_limit(int32_t sample, uint32_t *limited_count)
{
    int32_t sign = sample < 0 ? -1 : 1;
    int32_t magnitude = sample < 0 ? -sample : sample;
    const int32_t knee = APP_AUDIO_PLAYBACK_SOFT_KNEE;
    const int32_t limit = APP_AUDIO_PLAYBACK_OUTPUT_LIMIT;

    if (limit <= 0) {
        return 0;
    }
    if (magnitude <= knee || knee >= limit) {
        if (magnitude > limit) {
            magnitude = limit;
            if (limited_count != NULL) {
                (*limited_count)++;
            }
        }
        return (int16_t)(sign * magnitude);
    }

    if (limited_count != NULL) {
        (*limited_count)++;
    }

    int32_t over = magnitude - knee;
    int32_t range = limit - knee;
    magnitude = knee + (over * range) / (over + range);
    if (magnitude > limit) {
        magnitude = limit;
    }
    return (int16_t)(sign * magnitude);
}

static esp_err_t app_audio_playback_write_pcm_locked(const int16_t *mono_pcm, size_t sample_count)
{
    int16_t stereo_pcm[640];
    size_t stereo_count = 0;
    size_t bytes_written = 0;
    esp_err_t err;

    if (mono_pcm == NULL || s_playback.tx_handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_count > 320U) {
        sample_count = 320U;
    }
    for (size_t i = 0; i < sample_count; i++) {
        stereo_pcm[i * 2U] = mono_pcm[i];
        stereo_pcm[i * 2U + 1U] = mono_pcm[i];
        stereo_count += 2U;
    }
    err = i2s_channel_write(s_playback.tx_handle, stereo_pcm, stereo_count * sizeof(stereo_pcm[0]),
                            &bytes_written, pdMS_TO_TICKS(APP_AUDIO_PLAYBACK_WRITE_TIMEOUT_MS));
    return err == ESP_OK && bytes_written == (stereo_count * sizeof(stereo_pcm[0])) ? ESP_OK : ESP_FAIL;
}

static void app_audio_playback_reset_stream_state_locked(void)
{
    s_playback.codec = APP_SESSION_AUDIO_CODEC_PCM16;
    s_playback.sample_rate = 0;
    s_playback.channels = 0;
    s_playback.stream_open = false;
    s_playback.first_frame_logged = false;
    s_playback.remote_audio_seen = false;
    s_playback.remote_audio_audible_seen = false;
    s_playback.last_frame_us = 0;
    s_playback.last_audible_frame_us = 0;
    s_playback.write_count = 0;
    s_playback.write_error_count = 0;
    s_playback.decoded_log_count = 0;
    s_playback.compressed_log_count = 0;
    s_playback.unsupported_log_count = 0;
}

static esp_err_t app_audio_playback_init_codec_locked(uint32_t sample_rate)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(app_audio_playback_i2s_port(), I2S_ROLE_MASTER);
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = APP_AUDIO_I2S_SPK_BCK_GPIO,
            .ws = APP_AUDIO_I2S_SPK_WS_GPIO,
            .dout = APP_AUDIO_I2S_SPK_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    esp_err_t err;

    chan_cfg.dma_desc_num = 3;
    chan_cfg.dma_frame_num = 80;

    if (s_playback.codec_stack_inited && s_playback.tx_handle != NULL &&
        s_playback.configured_sample_rate == sample_rate) {
        return ESP_OK;
    }

    if (s_playback.codec_stack_inited) {
        (void)app_audio_playback_close_stream_locked();
    }

    err = app_audio_adv_codec_acquire();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to acquire shared ADV codec for playback: %s", esp_err_to_name(err));
        return err;
    }

    chan_cfg.id = app_audio_playback_i2s_port();
    chan_cfg.auto_clear = true;
    err = i2s_new_channel(&chan_cfg, &s_playback.tx_handle, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate playback TX channel: %s", esp_err_to_name(err));
        goto fail;
    }
    err = i2s_channel_init_std_mode(s_playback.tx_handle, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init playback TX std mode: %s", esp_err_to_name(err));
        goto fail;
    }
    err = i2s_channel_enable(s_playback.tx_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable playback TX channel: %s", esp_err_to_name(err));
        goto fail;
    }
    err = app_audio_adv_codec_enable_dac(true, APP_AUDIO_I2S_ADV_DAC_VOLUME_REG);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to enable shared ADV DAC: %s", esp_err_to_name(err));
        goto fail;
    }
    app_audio_adv_codec_log_key_regs("after-dac-enable");

    s_playback.codec_stack_inited = true;
    s_playback.configured_sample_rate = sample_rate;
    ESP_LOGI(TAG, "Opened direct Cardputer playback I2S/DAC path on bck=%d ws=%d dout=%d sample_rate=%u free=%u largest=%u",
             APP_AUDIO_I2S_SPK_BCK_GPIO, APP_AUDIO_I2S_SPK_WS_GPIO, APP_AUDIO_I2S_SPK_DOUT_GPIO,
             (unsigned)sample_rate,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    return ESP_OK;

fail:
    if (s_playback.tx_handle != NULL) {
        i2s_channel_disable(s_playback.tx_handle);
        i2s_del_channel(s_playback.tx_handle);
        s_playback.tx_handle = NULL;
    }
    s_playback.codec_stack_inited = false;
    s_playback.configured_sample_rate = 0;
    app_audio_adv_codec_enable_dac(false, APP_AUDIO_I2S_ADV_DAC_VOLUME_REG);
    app_audio_adv_codec_release();
    return err;
}

static esp_err_t app_audio_playback_close_stream_locked(void)
{
    esp_err_t ret = ESP_OK;

    if (s_playback.codec_stack_inited) {
        if (s_playback.tx_handle != NULL) {
            i2s_channel_disable(s_playback.tx_handle);
            i2s_del_channel(s_playback.tx_handle);
            s_playback.tx_handle = NULL;
        }
        if (app_audio_adv_codec_enable_dac(false, APP_AUDIO_I2S_ADV_DAC_VOLUME_REG) != ESP_OK) {
            ret = ESP_FAIL;
        }
        app_audio_adv_codec_log_key_regs("after-dac-disable");
        app_audio_adv_codec_release();
        s_playback.codec_stack_inited = false;
        s_playback.configured_sample_rate = 0;
        ESP_LOGI(TAG, "Closed direct Cardputer playback I2S/DAC path free=%u largest=%u",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    }
    app_audio_playback_reset_stream_state_locked();
    return ret;
}

static esp_err_t app_audio_playback_open_stream_locked(uint32_t sample_rate, uint8_t channels)
{
    if (s_playback.stream_open) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(app_audio_playback_init_codec_locked(sample_rate), TAG,
                        "Failed to init direct playback I2S/DAC path");

    s_playback.stream_open = true;
    ESP_LOGI(TAG,
             "Opened Cardputer direct playback on bck=%d ws=%d dout=%d sample_rate=%u channels=%u",
             APP_AUDIO_I2S_SPK_BCK_GPIO, APP_AUDIO_I2S_SPK_WS_GPIO, APP_AUDIO_I2S_SPK_DOUT_GPIO,
             (unsigned)sample_rate, (unsigned)channels);
    return ESP_OK;
}

esp_err_t app_audio_playback_init(void)
{
    if (!APP_AUDIO_ENABLE_PULL_PLAYBACK) {
        return ESP_OK;
    }
    if (s_playback.initialized) {
        return ESP_OK;
    }

    s_playback.lock = xSemaphoreCreateMutex();
    if (s_playback.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_playback.initialized = true;
    s_playback.codec = APP_SESSION_AUDIO_CODEC_PCM16;
    ESP_LOGI(TAG, "Cardputer ADV speaker playback ready; waiting for remote audio");
    return ESP_OK;
}

esp_err_t app_audio_playback_stop(void)
{
    esp_err_t ret = ESP_OK;

    if (!APP_AUDIO_ENABLE_PULL_PLAYBACK || !s_playback.initialized || s_playback.lock == NULL) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_playback.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    ret = app_audio_playback_close_stream_locked();
    xSemaphoreGive(s_playback.lock);
    return ret;
}

void app_audio_playback_handle_stream_info(app_session_audio_codec_t codec, uint32_t sample_rate, uint8_t channels)
{
    if (!APP_AUDIO_ENABLE_PULL_PLAYBACK || !s_playback.initialized) {
        return;
    }
    if (xSemaphoreTake(s_playback.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }

    s_playback.codec = codec;
    s_playback.sample_rate = sample_rate;
    s_playback.channels = channels;
    ESP_LOGI(TAG, "Remote audio stream callback fired: codec=%d sample_rate=%u channels=%u",
             (int)codec, (unsigned)sample_rate, (unsigned)channels);

    sample_rate = sample_rate ? sample_rate : 8000U;
    channels = channels ? channels : 1U;
    if (codec == APP_SESSION_AUDIO_CODEC_G711A) {
        sample_rate = 8000U;
        channels = 1U;
    } else if (codec == APP_SESSION_AUDIO_CODEC_G722) {
        sample_rate = 16000U;
        channels = 1U;
    }
    if (!s_playback.stream_open) {
        if (app_audio_playback_open_stream_locked(sample_rate, channels) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to open speaker path for remote audio");
        }
    }
    xSemaphoreGive(s_playback.lock);
}

void app_audio_playback_handle_audio_frame(app_session_audio_codec_t codec, const void *data, size_t size, uint32_t pts)
{
    int16_t decoded_pcm[320];
    int16_t stereo_pcm[640];
    size_t sample_count;
    size_t stereo_count;
    size_t bytes_written = 0;
    int32_t abs_sum = 0;
    int16_t peak = 0;
    uint32_t avg_abs = 0;
    uint32_t limited_count = 0;
    int32_t gain_num = 1;
    int32_t gain_den = 1;
    esp_err_t err;
    bool all_same_encoded = true;
    uint8_t first_encoded = 0;

    if (!APP_AUDIO_ENABLE_PULL_PLAYBACK || data == NULL || size == 0 || !s_playback.initialized) {
        return;
    }
    if (xSemaphoreTake(s_playback.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!s_playback.stream_open || s_playback.tx_handle == NULL) {
        xSemaphoreGive(s_playback.lock);
        return;
    }

    if (codec == APP_SESSION_AUDIO_CODEC_G711A) {
        const uint8_t *encoded = (const uint8_t *)data;

        first_encoded = encoded[0];
        for (size_t i = 1; i < size; i++) {
            if (encoded[i] != first_encoded) {
                all_same_encoded = false;
                break;
            }
        }
        if (s_playback.compressed_log_count < 6U) {
            s_playback.compressed_log_count++;
            ESP_LOGI(TAG, "Compressed playback frame[%u]: codec=pcma bytes=%u first=0x%02X all_same=%d",
                     (unsigned)s_playback.compressed_log_count,
                     (unsigned)size,
                     (unsigned)first_encoded,
                     all_same_encoded ? 1 : 0);
        }
        sample_count = app_codec_g711a_decode(data, size, decoded_pcm,
                                              sizeof(decoded_pcm) / sizeof(decoded_pcm[0]));
    } else if (codec == APP_SESSION_AUDIO_CODEC_G722) {
        const uint8_t *encoded = (const uint8_t *)data;

        first_encoded = encoded[0];
        for (size_t i = 1; i < size; i++) {
            if (encoded[i] != first_encoded) {
                all_same_encoded = false;
                break;
            }
        }
        if (s_playback.compressed_log_count < 6U) {
            s_playback.compressed_log_count++;
            ESP_LOGI(TAG, "Compressed playback frame[%u]: codec=g722 bytes=%u first=0x%02X all_same=%d",
                     (unsigned)s_playback.compressed_log_count,
                     (unsigned)size,
                     (unsigned)first_encoded,
                     all_same_encoded ? 1 : 0);
        }
        sample_count = app_codec_g722_decode(data, size, decoded_pcm,
                                             sizeof(decoded_pcm) / sizeof(decoded_pcm[0]));
    } else if (codec == APP_SESSION_AUDIO_CODEC_PCM16) {
        sample_count = size / sizeof(int16_t);
        if (sample_count > (sizeof(decoded_pcm) / sizeof(decoded_pcm[0]))) {
            sample_count = sizeof(decoded_pcm) / sizeof(decoded_pcm[0]);
        }
        memcpy(decoded_pcm, data, sample_count * sizeof(decoded_pcm[0]));
    } else {
        if (s_playback.unsupported_log_count < 6U) {
            s_playback.unsupported_log_count++;
            ESP_LOGW(TAG, "Dropping unsupported compressed playback frame: codec=%d bytes=%u",
                     (int)codec, (unsigned)size);
        }
        xSemaphoreGive(s_playback.lock);
        return;
    }

    app_audio_playback_effective_gain(&gain_num, &gain_den);
    stereo_count = 0;
    for (size_t i = 0; i < sample_count && (i * 2U + 1U) < (sizeof(stereo_pcm) / sizeof(stereo_pcm[0])); i++) {
        int32_t scaled = ((int32_t)decoded_pcm[i] * gain_num) / gain_den;
        int16_t sample = app_audio_playback_soft_limit(scaled, &limited_count);
        int16_t magnitude = sample >= 0 ? sample : (int16_t)(-sample);
        abs_sum += magnitude;
        if (magnitude > peak) {
            peak = magnitude;
        }
        stereo_pcm[i * 2U] = sample;
        stereo_pcm[i * 2U + 1U] = sample;
        stereo_count += 2U;
    }

    err = i2s_channel_write(s_playback.tx_handle, stereo_pcm, stereo_count * sizeof(stereo_pcm[0]),
                            &bytes_written, pdMS_TO_TICKS(APP_AUDIO_PLAYBACK_WRITE_TIMEOUT_MS));
    if (err != ESP_OK || bytes_written != (stereo_count * sizeof(stereo_pcm[0]))) {
        s_playback.write_error_count++;
        ESP_LOGW(TAG, "Speaker write failed: %s bytes=%u expected=%u",
                 esp_err_to_name(err),
                 (unsigned)bytes_written,
                 (unsigned)(stereo_count * sizeof(stereo_pcm[0])));
    } else {
        avg_abs = sample_count ? (unsigned)(abs_sum / (int32_t)sample_count) : 0U;
        s_playback.remote_audio_seen = true;
        s_playback.last_frame_us = esp_timer_get_time();
        if (avg_abs >= APP_AUDIO_PLAYBACK_ACTIVE_AVG_ABS_THRESHOLD ||
            peak >= APP_AUDIO_PLAYBACK_ACTIVE_PEAK_THRESHOLD) {
            s_playback.remote_audio_audible_seen = true;
            s_playback.last_audible_frame_us = s_playback.last_frame_us;
        }
        s_playback.write_count++;
        if (!s_playback.first_frame_logged) {
            s_playback.first_frame_logged = true;
            ESP_LOGI(TAG, "First remote audio frame written to speaker: pts=%u bytes=%u",
                     (unsigned)pts, (unsigned)bytes_written);
            ESP_LOGI(TAG, "First decoded playback frame stats: samples=%u avg_abs=%u peak=%d gain=%d/%d configured_gain=%d/%d knee=%d limit=%d limited=%u",
                     (unsigned)sample_count,
                     avg_abs,
                     (int)peak,
                     (int)gain_num,
                     (int)gain_den,
                     APP_AUDIO_PLAYBACK_GAIN_NUM,
                     APP_AUDIO_PLAYBACK_GAIN_DEN,
                     APP_AUDIO_PLAYBACK_SOFT_KNEE,
                     APP_AUDIO_PLAYBACK_OUTPUT_LIMIT,
                     (unsigned)limited_count);
        } else if ((s_playback.write_count % 250U) == 0U) {
            ESP_LOGI(TAG, "Speaker frames written: %u (errors=%u last_bytes=%u audible_recent=%d avg_abs=%u peak=%d limited=%u knee=%d limit=%d gain=%d/%d)",
                     (unsigned)s_playback.write_count, (unsigned)s_playback.write_error_count,
                     (unsigned)bytes_written,
                     (avg_abs >= APP_AUDIO_PLAYBACK_ACTIVE_AVG_ABS_THRESHOLD ||
                      peak >= APP_AUDIO_PLAYBACK_ACTIVE_PEAK_THRESHOLD) ? 1 : 0,
                     avg_abs,
                     (int)peak,
                     (unsigned)limited_count,
                     APP_AUDIO_PLAYBACK_SOFT_KNEE,
                     APP_AUDIO_PLAYBACK_OUTPUT_LIMIT,
                     (int)gain_num,
                     (int)gain_den);
        }
        if (s_playback.decoded_log_count < 4U) {
            s_playback.decoded_log_count++;
            ESP_LOGI(TAG, "Decoded playback frame stats[%u]: samples=%u avg_abs=%u peak=%d first=%d",
                     (unsigned)s_playback.decoded_log_count,
                     (unsigned)sample_count,
                     avg_abs,
                     (int)peak,
                     sample_count ? (int)stereo_pcm[0] : 0);
        }
    }
    xSemaphoreGive(s_playback.lock);
}

bool app_audio_playback_has_received_audio(void)
{
    return s_playback.remote_audio_seen;
}

bool app_audio_playback_is_recently_active(uint32_t window_ms)
{
    int64_t last_frame_us = s_playback.last_audible_frame_us;

    if (last_frame_us == 0) {
        return false;
    }
    return (esp_timer_get_time() - last_frame_us) <= ((int64_t)window_ms * 1000LL);
}

i2c_master_bus_handle_t app_audio_playback_get_i2c_bus(void)
{
    return app_audio_adv_codec_get_i2c_bus();
}

esp_err_t app_audio_playback_play_test_tone(uint32_t frequency_hz, uint32_t duration_ms)
{
    const uint32_t sample_rate = 8000U;
    const uint32_t frame_ms = 20U;
    const uint32_t samples_per_frame = (sample_rate * frame_ms) / 1000U;
    const uint32_t total_frames = duration_ms / frame_ms;
    const float phase_step = (2.0f * (float)M_PI * (float)frequency_hz) / (float)sample_rate;
    float phase = 0.0f;
    esp_err_t ret = ESP_OK;
    if (!APP_AUDIO_ENABLE_PULL_PLAYBACK || !s_playback.initialized || s_playback.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (total_frames == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_playback.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }

    if (!s_playback.stream_open) {
        ret = app_audio_playback_open_stream_locked(sample_rate, 1U);
    }
    if (ret == ESP_OK && s_playback.tx_handle != NULL) {
        ESP_LOGI(TAG, "Playing local speaker test tone: freq=%uHz duration=%ums", (unsigned)frequency_hz,
                 (unsigned)duration_ms);
        for (uint32_t frame = 0; frame < total_frames && ret == ESP_OK; frame++) {
            int16_t mono_pcm[samples_per_frame];
            for (uint32_t i = 0; i < samples_per_frame; i++) {
                int16_t sample = (int16_t)(sinf(phase) * (float)APP_AUDIO_PLAYBACK_TEST_AMPLITUDE);
                mono_pcm[i] = sample;
                phase += phase_step;
                if (phase >= (2.0f * (float)M_PI)) {
                    phase -= 2.0f * (float)M_PI;
                }
            }
            ret = app_audio_playback_write_pcm_locked(mono_pcm, samples_per_frame);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Speaker test tone write failed: %s", esp_err_to_name(ret));
                break;
            }
        }
    }

    xSemaphoreGive(s_playback.lock);
    return ret;
}

esp_err_t app_audio_playback_run_self_test_pattern(void)
{
    const uint32_t sample_rate = 8000U;
    const uint32_t frame_ms = 20U;
    const uint32_t samples_per_frame = (sample_rate * frame_ms) / 1000U;
    static const uint16_t tone_plan[][2] = {
        { 523, 300 },
        {   0, 120 },
        { 784, 300 },
        {   0, 120 },
        {1046, 450 },
        {   0, 180 },
        {1046, 200 },
        {   0, 100 },
        {1046, 200 },
    };
    int16_t mono_pcm[samples_per_frame];
    esp_err_t ret = ESP_OK;

    if (!APP_AUDIO_ENABLE_PULL_PLAYBACK || !s_playback.initialized || s_playback.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_playback.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    if (!s_playback.stream_open) {
        ret = app_audio_playback_open_stream_locked(sample_rate, 1U);
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Playing obvious speaker self-test pattern");
        for (size_t segment = 0; segment < (sizeof(tone_plan) / sizeof(tone_plan[0])) && ret == ESP_OK; segment++) {
            const uint32_t frequency_hz = tone_plan[segment][0];
            const uint32_t duration_ms = tone_plan[segment][1];
            const uint32_t total_frames = duration_ms / frame_ms;
            float phase = 0.0f;
            const float phase_step = frequency_hz
                ? ((2.0f * (float)M_PI * (float)frequency_hz) / (float)sample_rate)
                : 0.0f;

            for (uint32_t frame = 0; frame < total_frames && ret == ESP_OK; frame++) {
                for (uint32_t i = 0; i < samples_per_frame; i++) {
                    int16_t sample = 0;
                    if (frequency_hz != 0U) {
                        sample = (sinf(phase) >= 0.0f)
                            ? (int16_t)APP_AUDIO_PLAYBACK_TEST_AMPLITUDE
                            : (int16_t)(-APP_AUDIO_PLAYBACK_TEST_AMPLITUDE);
                        phase += phase_step;
                        if (phase >= (2.0f * (float)M_PI)) {
                            phase -= 2.0f * (float)M_PI;
                        }
                    }
                    mono_pcm[i] = sample;
                }
                ret = app_audio_playback_write_pcm_locked(mono_pcm, samples_per_frame);
            }
        }
    }
    xSemaphoreGive(s_playback.lock);
    return ret;
}
