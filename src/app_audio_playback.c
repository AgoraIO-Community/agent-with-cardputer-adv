#include "app_audio_playback.h"

#include <stdbool.h>
#include <math.h>
#include <string.h>

#include "app_audio_adv_codec.h"
#include "app_audio_i2s.h"
#include "app_codec_g711.h"
#include "app_codec_g722.h"
#include "app_config.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "es8311_codec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define APP_AUDIO_PLAYBACK_WRITE_TIMEOUT_MS 100
#define APP_AUDIO_PLAYBACK_ACTIVE_AVG_ABS_THRESHOLD 600
#define APP_AUDIO_PLAYBACK_ACTIVE_PEAK_THRESHOLD 1200

typedef struct {
    SemaphoreHandle_t lock;
    esp_codec_dev_handle_t codec_dev;
    const audio_codec_data_if_t *data_if;
    const audio_codec_ctrl_if_t *ctrl_if;
    const audio_codec_gpio_if_t *gpio_if;
    const audio_codec_if_t *codec_if;
    i2c_master_bus_handle_t i2c_bus;
    i2s_chan_handle_t tx_handle;
    app_session_audio_codec_t codec;
    uint32_t sample_rate;
    uint8_t channels;
    bool initialized;
    bool codec_stack_inited;
    bool owns_i2c_bus;
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
} app_audio_playback_ctx_t;

static const char *TAG = "app_audio_playback";
static app_audio_playback_ctx_t s_playback;

static i2c_port_num_t app_audio_playback_i2c_port(void)
{
    return APP_AUDIO_I2S_ADV_I2C_PORT;
}

static int app_audio_playback_i2s_port(void)
{
    return APP_AUDIO_I2S_SPK_PORT;
}

static esp_err_t app_audio_playback_write_pcm_locked(const int16_t *mono_pcm, size_t sample_count)
{
    int16_t stereo_pcm[640];
    size_t stereo_count = 0;
    int err;

    if (mono_pcm == NULL || s_playback.codec_dev == NULL) {
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
    err = esp_codec_dev_write(s_playback.codec_dev, stereo_pcm, stereo_count * sizeof(stereo_pcm[0]));
    return err == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
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
}

static esp_err_t app_audio_playback_init_codec_locked(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = app_audio_playback_i2c_port(),
        .sda_io_num = APP_AUDIO_I2S_ADV_I2C_SDA_GPIO,
        .scl_io_num = APP_AUDIO_I2S_ADV_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(app_audio_playback_i2s_port(), I2S_ROLE_MASTER);
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(8000),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(32, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = APP_AUDIO_I2S_SPK_BCK_GPIO,
            .ws = APP_AUDIO_I2S_SPK_WS_GPIO,
            .dout = APP_AUDIO_I2S_SPK_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };
    audio_codec_i2s_cfg_t i2s_cfg = { 0 };
    audio_codec_i2c_cfg_t i2c_cfg = { 0 };
    es8311_codec_cfg_t es8311_cfg = { 0 };
    esp_codec_dev_cfg_t dev_cfg = { 0 };
    esp_err_t err;

    if (s_playback.codec_stack_inited && s_playback.codec_dev != NULL) {
        return ESP_OK;
    }

    s_playback.i2c_bus = app_audio_adv_codec_get_i2c_bus();
    if (s_playback.i2c_bus == NULL) {
        err = i2c_new_master_bus(&bus_cfg, &s_playback.i2c_bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create playback I2C bus: %s", esp_err_to_name(err));
            return err;
        }
        s_playback.owns_i2c_bus = true;
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

    i2s_cfg.port = app_audio_playback_i2s_port();
    i2s_cfg.tx_handle = s_playback.tx_handle;
    s_playback.data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (s_playback.data_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        goto fail;
    }

    i2c_cfg.port = app_audio_playback_i2c_port();
    i2c_cfg.addr = ES8311_CODEC_DEFAULT_ADDR;
    i2c_cfg.bus_handle = s_playback.i2c_bus;
    s_playback.ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (s_playback.ctrl_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed");
        goto fail;
    }

    s_playback.gpio_if = audio_codec_new_gpio();
    if (s_playback.gpio_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_gpio failed");
        goto fail;
    }

    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.ctrl_if = s_playback.ctrl_if;
    es8311_cfg.gpio_if = s_playback.gpio_if;
    es8311_cfg.pa_pin = -1;
    es8311_cfg.use_mclk = false;
    es8311_cfg.hw_gain.pa_gain = 6.0f;
    s_playback.codec_if = es8311_codec_new(&es8311_cfg);
    if (s_playback.codec_if == NULL) {
        ESP_LOGE(TAG, "es8311_codec_new failed");
        goto fail;
    }

    dev_cfg.codec_if = s_playback.codec_if;
    dev_cfg.data_if = s_playback.data_if;
    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT;
    s_playback.codec_dev = esp_codec_dev_new(&dev_cfg);
    if (s_playback.codec_dev == NULL) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        goto fail;
    }

    app_audio_adv_codec_set_external_dac_active(true);
    if (esp_codec_dev_set_out_vol(s_playback.codec_dev, 100) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "Failed to set playback volume to max");
    }
    s_playback.codec_stack_inited = true;
    return ESP_OK;

fail:
    if (s_playback.codec_dev != NULL) {
        esp_codec_dev_delete(s_playback.codec_dev);
        s_playback.codec_dev = NULL;
    }
    if (s_playback.codec_if != NULL) {
        audio_codec_delete_codec_if(s_playback.codec_if);
        s_playback.codec_if = NULL;
    }
    if (s_playback.gpio_if != NULL) {
        audio_codec_delete_gpio_if(s_playback.gpio_if);
        s_playback.gpio_if = NULL;
    }
    if (s_playback.ctrl_if != NULL) {
        audio_codec_delete_ctrl_if(s_playback.ctrl_if);
        s_playback.ctrl_if = NULL;
    }
    if (s_playback.data_if != NULL) {
        audio_codec_delete_data_if(s_playback.data_if);
        s_playback.data_if = NULL;
    }
    if (s_playback.tx_handle != NULL) {
        i2s_channel_disable(s_playback.tx_handle);
        i2s_del_channel(s_playback.tx_handle);
        s_playback.tx_handle = NULL;
    }
    if (s_playback.owns_i2c_bus && s_playback.i2c_bus != NULL) {
        i2c_del_master_bus(s_playback.i2c_bus);
        s_playback.i2c_bus = NULL;
        s_playback.owns_i2c_bus = false;
    }
    if (!s_playback.owns_i2c_bus) {
        s_playback.i2c_bus = NULL;
    }
    s_playback.codec_stack_inited = false;
    app_audio_adv_codec_set_external_dac_active(false);
    return ESP_FAIL;
}

static esp_err_t app_audio_playback_close_stream_locked(void)
{
    esp_err_t ret = ESP_OK;

    if (s_playback.codec_dev != NULL && s_playback.stream_open) {
        int err = esp_codec_dev_close(s_playback.codec_dev);
        if (err != ESP_CODEC_DEV_OK) {
            ret = ESP_FAIL;
        }
    }
    if (s_playback.codec_stack_inited) {
        esp_codec_dev_delete(s_playback.codec_dev);
        s_playback.codec_dev = NULL;
        audio_codec_delete_codec_if(s_playback.codec_if);
        s_playback.codec_if = NULL;
        audio_codec_delete_gpio_if(s_playback.gpio_if);
        s_playback.gpio_if = NULL;
        audio_codec_delete_ctrl_if(s_playback.ctrl_if);
        s_playback.ctrl_if = NULL;
        audio_codec_delete_data_if(s_playback.data_if);
        s_playback.data_if = NULL;
        if (s_playback.tx_handle != NULL) {
            i2s_channel_disable(s_playback.tx_handle);
            i2s_del_channel(s_playback.tx_handle);
            s_playback.tx_handle = NULL;
        }
        if (s_playback.owns_i2c_bus && s_playback.i2c_bus != NULL) {
            i2c_del_master_bus(s_playback.i2c_bus);
        }
        s_playback.i2c_bus = NULL;
        s_playback.owns_i2c_bus = false;
        s_playback.codec_stack_inited = false;
        app_audio_adv_codec_set_external_dac_active(false);
    }
    app_audio_playback_reset_stream_state_locked();
    return ret;
}

static esp_err_t app_audio_playback_open_stream_locked(uint32_t sample_rate, uint8_t channels)
{
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = sample_rate,
        .mclk_multiple = 0,
    };
    int err;

    if (s_playback.stream_open) {
        return ESP_OK;
    }

    if (APP_AUDIO_USE_I2S_MIC && APP_AUDIO_ENABLE_HALF_DUPLEX_GATING) {
        ESP_LOGI(TAG, "Stopping mic before opening shared speaker path");
        ESP_RETURN_ON_ERROR(app_audio_i2s_stop(), TAG, "Failed to stop mic before playback");
    }
    ESP_RETURN_ON_ERROR(app_audio_playback_init_codec_locked(), TAG, "Failed to init playback codec stack");
    err = esp_codec_dev_open(s_playback.codec_dev, &fs);
    if (err != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed: %d", err);
        return ESP_FAIL;
    }

    s_playback.stream_open = true;
    ESP_LOGI(TAG,
             "Opened Cardputer playback codec on bck=%d ws=%d dout=%d sample_rate=%u channels=%u",
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
    esp_err_t err;
    bool all_same_encoded = true;
    uint8_t first_encoded = 0;

    if (!APP_AUDIO_ENABLE_PULL_PLAYBACK || data == NULL || size == 0 || !s_playback.initialized) {
        return;
    }
    if (xSemaphoreTake(s_playback.lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    if (!s_playback.stream_open || s_playback.codec_dev == NULL) {
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
    } else {
        sample_count = size / sizeof(int16_t);
        if (sample_count > (sizeof(decoded_pcm) / sizeof(decoded_pcm[0]))) {
            sample_count = sizeof(decoded_pcm) / sizeof(decoded_pcm[0]);
        }
        memcpy(decoded_pcm, data, sample_count * sizeof(decoded_pcm[0]));
    }

    stereo_count = 0;
    for (size_t i = 0; i < sample_count && (i * 2U + 1U) < (sizeof(stereo_pcm) / sizeof(stereo_pcm[0])); i++) {
        int32_t scaled = ((int32_t)decoded_pcm[i] * APP_AUDIO_PLAYBACK_GAIN_NUM) / APP_AUDIO_PLAYBACK_GAIN_DEN;
        if (scaled > INT16_MAX) {
            scaled = INT16_MAX;
        } else if (scaled < INT16_MIN) {
            scaled = INT16_MIN;
        }
        int16_t sample = (int16_t)scaled;
        int16_t magnitude = sample >= 0 ? sample : (int16_t)(-sample);
        abs_sum += magnitude;
        if (magnitude > peak) {
            peak = magnitude;
        }
        stereo_pcm[i * 2U] = sample;
        stereo_pcm[i * 2U + 1U] = sample;
        stereo_count += 2U;
    }

    err = esp_codec_dev_write(s_playback.codec_dev, stereo_pcm, stereo_count * sizeof(stereo_pcm[0]));
    if (err != ESP_CODEC_DEV_OK) {
        s_playback.write_error_count++;
        ESP_LOGW(TAG, "Speaker write failed: %d", err);
    } else {
        bytes_written = stereo_count * sizeof(stereo_pcm[0]);
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
            ESP_LOGI(TAG, "First decoded playback frame stats: samples=%u avg_abs=%u peak=%d gain=%d/%d",
                     (unsigned)sample_count,
                     avg_abs,
                     (int)peak,
                     APP_AUDIO_PLAYBACK_GAIN_NUM,
                     APP_AUDIO_PLAYBACK_GAIN_DEN);
        } else if ((s_playback.write_count % 250U) == 0U) {
            ESP_LOGI(TAG, "Speaker frames written: %u (errors=%u last_bytes=%u audible_recent=%d avg_abs=%u peak=%d)",
                     (unsigned)s_playback.write_count, (unsigned)s_playback.write_error_count,
                     (unsigned)bytes_written,
                     (avg_abs >= APP_AUDIO_PLAYBACK_ACTIVE_AVG_ABS_THRESHOLD ||
                      peak >= APP_AUDIO_PLAYBACK_ACTIVE_PEAK_THRESHOLD) ? 1 : 0,
                     avg_abs,
                     (int)peak);
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
    return s_playback.i2c_bus;
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
    if (ret == ESP_OK && s_playback.codec_dev != NULL) {
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
