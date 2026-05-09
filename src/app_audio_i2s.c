#include "app_audio_i2s.h"

#include <stdint.h>
#include <string.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

#include "app_codec_g711.h"
#include "app_config.h"
#include "app_audio_adv_codec.h"
#include "app_audio_playback.h"
#include "app_session.h"
#include "driver/i2s_std.h"
#include "driver/i2s_pdm.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/i2s_struct.h"

#if __has_include(<hal/i2s_ll.h>)
#include <hal/i2s_ll.h>
#endif

#if __has_include(<soc/pcr_struct.h>)
#include <soc/pcr_struct.h>
#endif

#define APP_AUDIO_I2S_STACK_SIZE 6144
#define APP_AUDIO_I2S_READ_TIMEOUT_MS 100
#define APP_AUDIO_I2S_UPLINK_GATE_HOLD_MS 1000U
#define APP_AUDIO_I2S_PLL_D2_CLK 120000000U
#define APP_AUDIO_I2S_OVERSAMPLING 2
#if APP_AUDIO_I2S_USE_CARDPUTER_ADV
#define APP_AUDIO_I2S_READ_SAMPLES_PER_FRAME ((APP_AUDIO_I2S_CAPTURE_RATE * APP_AUDIO_FRAME_MS) / 1000U)
#else
#define APP_AUDIO_I2S_READ_SAMPLES_PER_FRAME (((APP_AUDIO_I2S_CAPTURE_RATE * APP_AUDIO_FRAME_MS) / 1000U) * APP_AUDIO_I2S_OVERSAMPLING * 2)
#endif

typedef struct {
    TaskHandle_t task;
    i2s_chan_handle_t rx_handle;
    bool running;
    uint32_t processed_frame_count;
    uint32_t sent_frame_count;
    uint32_t pts;
    uint8_t mode_index;
    uint8_t constant_frame_reports;
    uint8_t tuned_mode_mask;
    bool mode_locked;
#if APP_AUDIO_I2S_PROFILE_ENABLE
    uint32_t profile_window_frames;
    uint32_t profile_send_failures;
    uint32_t profile_deadline_misses;
    uint32_t profile_max_frame_time_us;
    uint64_t profile_total_frame_time_us;
#endif
} app_audio_i2s_ctx_t;

static const char *TAG = "app_audio_i2s";
static app_audio_i2s_ctx_t s_audio_i2s;

typedef struct {
#if APP_AUDIO_I2S_USE_CARDPUTER_ADV
    bool left_slot;
    bool ws_inv;
#else
    bool raw_fmt;
#endif
    bool clk_inv;
    const char *name;
} app_audio_i2s_mode_t;

#if APP_AUDIO_I2S_USE_CARDPUTER_ADV
static const app_audio_i2s_mode_t s_audio_i2s_modes[] = {
    { false, false, false, "right ws_inv=0 bclk_inv=0" },
    { true,  false, false, "left ws_inv=0 bclk_inv=0" },
    { false, true,  false, "right ws_inv=1 bclk_inv=0" },
    { true,  true,  false, "left ws_inv=1 bclk_inv=0" },
    { false, false, true,  "right ws_inv=0 bclk_inv=1" },
    { true,  false, true,  "left ws_inv=0 bclk_inv=1" },
    { false, true,  true,  "right ws_inv=1 bclk_inv=1" },
    { true,  true,  true,  "left ws_inv=1 bclk_inv=1" },
};
#else
static const app_audio_i2s_mode_t s_audio_i2s_modes[] = {
    { false, false, "pcm clk_inv=0" },
    { true,  false, "raw clk_inv=0" },
    { false, true,  "pcm clk_inv=1" },
    { true,  true,  "raw clk_inv=1" },
};
#endif

static bool app_audio_i2s_mode_is_allowed(uint8_t mode_index)
{
#if APP_AUDIO_I2S_USE_CARDPUTER_ADV
    (void)mode_index;
    return true;
#else
    const app_audio_i2s_mode_t *mode = &s_audio_i2s_modes[mode_index % (sizeof(s_audio_i2s_modes) / sizeof(s_audio_i2s_modes[0]))];

    if (!APP_AUDIO_I2S_ALLOW_RAW_TUNE && mode->raw_fmt) {
        return false;
    }
    return true;
#endif
}

static uint8_t app_audio_i2s_find_next_allowed_mode(uint8_t current_mode_index)
{
    const uint8_t mode_count = sizeof(s_audio_i2s_modes) / sizeof(s_audio_i2s_modes[0]);

    for (uint8_t offset = 1; offset <= mode_count; offset++) {
        uint8_t candidate = (uint8_t)((current_mode_index + offset) % mode_count);
        if (app_audio_i2s_mode_is_allowed(candidate)) {
            return candidate;
        }
    }
    return current_mode_index;
}

static uint8_t app_audio_i2s_initial_mode_index(void)
{
#if APP_AUDIO_I2S_USE_CARDPUTER_ADV
    return 0;
#else
    for (uint8_t i = 0; i < sizeof(s_audio_i2s_modes) / sizeof(s_audio_i2s_modes[0]); i++) {
        if (s_audio_i2s_modes[i].raw_fmt == (APP_AUDIO_I2S_PDM_DATA_FMT == APP_AUDIO_I2S_PDM_FMT_RAW) &&
            s_audio_i2s_modes[i].clk_inv == (APP_AUDIO_I2S_PDM_CLK_INVERT != 0)) {
            return i;
        }
    }
    return 0;
#endif
}

static void app_audio_i2s_calc_clock_div(uint32_t *div_a, uint32_t *div_b, uint32_t *div_n,
                                         uint32_t base_clock, uint32_t target_freq)
{
    if (base_clock <= (target_freq << 1)) {
        *div_n = 2;
        *div_a = 1;
        *div_b = 0;
        return;
    }

    uint32_t save_n = 255;
    uint32_t save_a = 63;
    uint32_t save_b = 62;
    if (target_freq != 0) {
        float fdiv = (float)base_clock / (float)target_freq;
        uint32_t n = (uint32_t)fdiv;
        if (n < 256) {
            fdiv -= n;

            float check_base = (float)base_clock;
            while ((int32_t)target_freq >= 0) {
                target_freq <<= 1;
                check_base *= 2.0f;
            }
            float check_target = (float)target_freq;

            uint32_t save_diff = UINT32_MAX;
            if (n < 255) {
                save_a = 1;
                save_b = 0;
                save_n = n + 1;
                save_diff = abs((int)(check_target - check_base / (float)save_n));
            }

            for (uint32_t a = 1; a < 64; ++a) {
                uint32_t b = (uint32_t)lroundf(a * fdiv);
                if (a <= b) {
                    continue;
                }
                uint32_t diff = abs((int)(check_target - ((check_base * a) / (n * a + b))));
                if (save_diff <= diff) {
                    continue;
                }
                save_diff = diff;
                save_a = a;
                save_b = b;
                save_n = n;
                if (diff == 0) {
                    break;
                }
            }
        }
    }

    *div_n = save_n;
    *div_a = save_a;
    *div_b = save_b;
}

static void app_audio_i2s_apply_cardputer_pdm_clock(uint32_t sample_rate)
{
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    const bool use_pdm = true;
    const uint32_t bits = 64;
    const uint32_t div_m = 2;
    uint32_t div_a = 0;
    uint32_t div_b = 0;
    uint32_t div_n = 0;

    app_audio_i2s_calc_clock_div(&div_a, &div_b, &div_n,
                                 APP_AUDIO_I2S_PLL_D2_CLK / (bits * div_m),
                                 sample_rate * APP_AUDIO_I2S_OVERSAMPLING);

    i2s_dev_t *dev = &I2S0;
#if SOC_I2S_NUM >= 2
    if (APP_AUDIO_I2S_PORT == I2S_NUM_1) {
        dev = &I2S1;
    }
#endif

    dev->rx_conf.rx_pdm_en = use_pdm;
    dev->rx_conf.rx_tdm_en = !use_pdm;
#if defined(I2S_RX_PDM2PCM_CONF_REG)
    dev->rx_pdm2pcm_conf.rx_pdm2pcm_en = use_pdm;
    dev->rx_pdm2pcm_conf.rx_pdm_sinc_dsr_16_en = 1;
#elif defined(I2S_RX_PDM2PCM_EN)
    dev->rx_conf.rx_pdm2pcm_en = use_pdm;
    dev->rx_conf.rx_pdm_sinc_dsr_16_en = 1;
#endif
    dev->rx_conf.rx_update = 1;
    dev->rx_conf1.rx_bck_div_num = div_m - 1;

    bool yn1 = (div_b > (div_a >> 1));
    if (yn1) {
        div_b = div_a - div_b;
    }
    int div_y = 1;
    int div_x = 0;
    if (div_b != 0) {
        div_x = (int)(div_a / div_b) - 1;
        div_y = (int)(div_a % div_b);
        if (div_y == 0) {
            div_y = 1;
            div_b = 511;
        }
    }

#if __has_include(<hal/i2s_ll.h>)
    i2s_ll_rx_set_raw_clk_div(dev, div_n, div_x, div_y, div_b, yn1);
#endif

#if defined(I2S_RX_CLKM_DIV_X)
    dev->rx_clkm_div_conf.rx_clkm_div_x = div_x;
    dev->rx_clkm_div_conf.rx_clkm_div_y = div_y;
    dev->rx_clkm_div_conf.rx_clkm_div_z = div_b;
    dev->rx_clkm_div_conf.rx_clkm_div_yn1 = yn1;
    dev->rx_clkm_conf.rx_clkm_div_num = div_n;
    dev->rx_clkm_conf.rx_clk_sel = 1;
    dev->tx_clkm_conf.clk_en = 1;
    dev->rx_clkm_conf.rx_clk_active = 1;
    dev->rx_conf.rx_update = 1;
    dev->rx_conf.rx_update = 0;
#endif
#else
    (void)sample_rate;
#endif
}

static esp_err_t app_audio_i2s_init_channel_for_mode(const app_audio_i2s_mode_t *mode)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(APP_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t err;

    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 64;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_audio_i2s.rx_handle), TAG,
                        "Failed to allocate I2S RX channel");

#if APP_AUDIO_I2S_USE_CARDPUTER_ADV
    {
        i2s_std_config_t std_rx_cfg = { 0 };

        ESP_RETURN_ON_ERROR(app_audio_adv_codec_acquire(), TAG, "Failed to acquire ADV codec");
        ESP_RETURN_ON_ERROR(app_audio_adv_codec_enable_adc(true), TAG, "Failed to enable ADV codec ADC");
        std_rx_cfg.clk_cfg.sample_rate_hz = APP_AUDIO_I2S_CAPTURE_RATE;
        std_rx_cfg.clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;
        std_rx_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
        std_rx_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
        std_rx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
        std_rx_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
        std_rx_cfg.slot_cfg.slot_mask = mode->left_slot ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_RIGHT;
        std_rx_cfg.slot_cfg.ws_width = 16;
        std_rx_cfg.slot_cfg.bit_shift = true;
        std_rx_cfg.slot_cfg.left_align = true;
        std_rx_cfg.slot_cfg.big_endian = false;
        std_rx_cfg.slot_cfg.bit_order_lsb = false;
        std_rx_cfg.gpio_cfg.bclk = APP_AUDIO_I2S_MIC_BCK_GPIO;
        std_rx_cfg.gpio_cfg.ws = APP_AUDIO_I2S_MIC_CLK_GPIO;
        std_rx_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
        std_rx_cfg.gpio_cfg.din = APP_AUDIO_I2S_MIC_DATA_GPIO;
        std_rx_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
        std_rx_cfg.gpio_cfg.invert_flags.bclk_inv = mode->clk_inv;
        std_rx_cfg.gpio_cfg.invert_flags.ws_inv = mode->ws_inv;

        err = i2s_channel_init_std_mode(s_audio_i2s.rx_handle, &std_rx_cfg);
        if (err != ESP_OK) {
            app_audio_adv_codec_enable_adc(false);
            app_audio_adv_codec_release();
            return err;
        }
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_audio_i2s.rx_handle), TAG, "Failed to enable ADV I2S RX channel");
        vTaskDelay(pdMS_TO_TICKS(APP_AUDIO_I2S_ADV_STARTUP_DELAY_MS));
        ESP_LOGI(TAG, "Initialized Cardputer ADV mic I2S RX on bck=%d ws=%d data=%d capture_rate=%d send_rate=%d slot=%s ws_inv=%d bclk_inv=%d (%s)",
                 APP_AUDIO_I2S_MIC_BCK_GPIO, APP_AUDIO_I2S_MIC_CLK_GPIO, APP_AUDIO_I2S_MIC_DATA_GPIO,
                 APP_AUDIO_I2S_CAPTURE_RATE, APP_AUDIO_SAMPLE_RATE,
                 mode->left_slot ? "left" : "right",
                 mode->ws_inv ? 1 : 0,
                 mode->clk_inv ? 1 : 0,
                 mode->name);
    }
#else
    {
        i2s_pdm_rx_config_t pdm_rx_cfg = { 0 };

        pdm_rx_cfg.clk_cfg.clk_src = I2S_CLK_SRC_PLL_160M;
        pdm_rx_cfg.clk_cfg.sample_rate_hz = 48000;
        pdm_rx_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_128;
        pdm_rx_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_16BIT;
        pdm_rx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;
        pdm_rx_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_MONO;
        pdm_rx_cfg.slot_cfg.slot_mask = I2S_PDM_SLOT_RIGHT;
        pdm_rx_cfg.slot_cfg.data_fmt = mode->raw_fmt ? I2S_PDM_DATA_FMT_RAW : I2S_PDM_DATA_FMT_PCM;
        pdm_rx_cfg.gpio_cfg.clk = APP_AUDIO_I2S_MIC_CLK_GPIO;
        pdm_rx_cfg.gpio_cfg.din = APP_AUDIO_I2S_MIC_DATA_GPIO;
        pdm_rx_cfg.gpio_cfg.invert_flags.clk_inv = mode->clk_inv;

        ESP_RETURN_ON_ERROR(i2s_channel_init_pdm_rx_mode(s_audio_i2s.rx_handle, &pdm_rx_cfg), TAG,
                            "Failed to init I2S PDM RX mode");
        app_audio_i2s_apply_cardputer_pdm_clock(APP_AUDIO_I2S_CAPTURE_RATE);
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_audio_i2s.rx_handle), TAG, "Failed to enable I2S RX channel");
        ESP_LOGI(TAG, "Initialized Cardputer mic I2S RX on clk=%d data=%d capture_rate=%d send_rate=%d fmt=%s clk_inv=%d (%s)",
                 APP_AUDIO_I2S_MIC_CLK_GPIO, APP_AUDIO_I2S_MIC_DATA_GPIO, APP_AUDIO_I2S_CAPTURE_RATE, APP_AUDIO_SAMPLE_RATE,
                 mode->raw_fmt ? "raw" : "pcm",
                 mode->clk_inv ? 1 : 0,
                 mode->name);
    }
#endif
    return ESP_OK;
}

static void app_audio_i2s_deinit_channel(void)
{
    if (s_audio_i2s.rx_handle == NULL) {
        return;
    }
    i2s_channel_disable(s_audio_i2s.rx_handle);
    i2s_del_channel(s_audio_i2s.rx_handle);
    s_audio_i2s.rx_handle = NULL;
#if APP_AUDIO_I2S_USE_CARDPUTER_ADV
    app_audio_adv_codec_enable_adc(false);
    app_audio_adv_codec_release();
#endif
}

static esp_err_t app_audio_i2s_switch_mode(uint8_t next_mode_index)
{
    const uint8_t previous_mode_index = s_audio_i2s.mode_index;
    const app_audio_i2s_mode_t *previous_mode = &s_audio_i2s_modes[previous_mode_index];
    const app_audio_i2s_mode_t *mode = &s_audio_i2s_modes[next_mode_index % (sizeof(s_audio_i2s_modes) / sizeof(s_audio_i2s_modes[0]))];
    esp_err_t err;

    app_audio_i2s_deinit_channel();
    s_audio_i2s.mode_index = next_mode_index % (sizeof(s_audio_i2s_modes) / sizeof(s_audio_i2s_modes[0]));
    s_audio_i2s.tuned_mode_mask |= (uint8_t)(1U << s_audio_i2s.mode_index);
    s_audio_i2s.constant_frame_reports = 0;
    err = app_audio_i2s_init_channel_for_mode(mode);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Mode switch to %s failed, restoring %s", mode->name, previous_mode->name);
    s_audio_i2s.mode_index = previous_mode_index;
    s_audio_i2s.constant_frame_reports = 0;
    return app_audio_i2s_init_channel_for_mode(previous_mode);
}

static size_t app_audio_i2s_resample_frame(const int16_t *input,
                                           size_t input_count,
                                           uint32_t input_rate,
                                           int16_t *output,
                                           size_t output_capacity,
                                           uint32_t output_rate)
{
    if (input == NULL || output == NULL || input_count == 0 || output_capacity == 0 ||
        input_rate == 0 || output_rate == 0) {
        return 0;
    }

    if (input_rate == output_rate) {
        size_t copy_count = input_count < output_capacity ? input_count : output_capacity;
        memcpy(output, input, copy_count * sizeof(output[0]));
        return copy_count;
    }

    if (input_rate > output_rate && (input_rate % output_rate) == 0U) {
        size_t output_count = 0;
        size_t ratio = input_rate / output_rate;

        for (size_t input_index = 0;
             (input_index + ratio - 1U) < input_count && output_count < output_capacity;
             input_index += ratio) {
            int32_t sum = 0;
            for (size_t i = 0; i < ratio; i++) {
                sum += input[input_index + i];
            }
            output[output_count++] = (int16_t)(sum / (int32_t)ratio);
        }
        return output_count;
    }

    if (output_rate > input_rate && (output_rate % input_rate) == 0U) {
        size_t output_count = 0;
        size_t ratio = output_rate / input_rate;

        for (size_t input_index = 0; input_index < input_count && output_count < output_capacity; input_index++) {
            for (size_t i = 0; i < ratio && output_count < output_capacity; i++) {
                output[output_count++] = input[input_index];
            }
        }
        return output_count;
    }

    return 0;
}

static size_t app_audio_i2s_expand_mono_to_stereo(const int16_t *input,
                                                  size_t input_count,
                                                  int16_t *output,
                                                  size_t output_capacity)
{
    if (input == NULL || output == NULL || output_capacity < (input_count * 2U)) {
        return 0;
    }

    for (size_t i = 0; i < input_count; i++) {
        output[i * 2U] = input[i];
        output[i * 2U + 1U] = input[i];
    }
    return input_count * 2U;
}

static void app_audio_i2s_task(void *arg)
{
    const uint32_t samples_per_frame = (APP_AUDIO_SAMPLE_RATE * APP_AUDIO_FRAME_MS) / 1000U;
    const uint32_t capture_samples_per_frame = (APP_AUDIO_I2S_CAPTURE_RATE * APP_AUDIO_FRAME_MS) / 1000U;
    static int16_t raw_frame_buffer[APP_AUDIO_I2S_READ_SAMPLES_PER_FRAME];
    static int16_t capture_frame_buffer[(APP_AUDIO_I2S_CAPTURE_RATE * APP_AUDIO_FRAME_MS) / 1000U];
    static int16_t frame_buffer[(APP_AUDIO_SAMPLE_RATE * APP_AUDIO_FRAME_MS) / 1000U];
#if APP_AUDIO_CODEC != APP_AUDIO_CODEC_G711A
    static int16_t stereo_frame_buffer[((APP_AUDIO_SAMPLE_RATE * APP_AUDIO_FRAME_MS) / 1000U) * 2U];
#endif
    bool waiting_logged = false;
    bool uplink_gated = false;

    (void)arg;
    memset(raw_frame_buffer, 0, sizeof(raw_frame_buffer));
    memset(capture_frame_buffer, 0, sizeof(capture_frame_buffer));
    memset(frame_buffer, 0, sizeof(frame_buffer));
#if APP_AUDIO_CODEC != APP_AUDIO_CODEC_G711A
    memset(stereo_frame_buffer, 0, sizeof(stereo_frame_buffer));
#endif

    size_t warmup_bytes = 0;
    i2s_channel_read(s_audio_i2s.rx_handle, raw_frame_buffer, sizeof(raw_frame_buffer), &warmup_bytes, pdMS_TO_TICKS(1));
    i2s_channel_read(s_audio_i2s.rx_handle, raw_frame_buffer, sizeof(raw_frame_buffer), &warmup_bytes, pdMS_TO_TICKS(1));

    while (s_audio_i2s.running) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(s_audio_i2s.rx_handle, raw_frame_buffer, sizeof(raw_frame_buffer),
                                         &bytes_read, pdMS_TO_TICKS(APP_AUDIO_I2S_READ_TIMEOUT_MS));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S read failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(APP_AUDIO_FRAME_MS));
            continue;
        }
        if (bytes_read == 0) {
            ESP_LOGW(TAG, "I2S read returned no data");
            continue;
        }

        const size_t raw_sample_count = bytes_read / sizeof(raw_frame_buffer[0]);
        size_t capture_sample_count = 0;
#if APP_AUDIO_I2S_USE_CARDPUTER_ADV
        capture_sample_count = raw_sample_count < capture_samples_per_frame ? raw_sample_count : capture_samples_per_frame;
        memcpy(capture_frame_buffer, raw_frame_buffer, capture_sample_count * sizeof(capture_frame_buffer[0]));
#else
        size_t capture_index = 0;
        while ((capture_index + (2 * APP_AUDIO_I2S_OVERSAMPLING) - 1) < raw_sample_count &&
               capture_sample_count < capture_samples_per_frame) {
            int32_t oversampled_sum = 0;
            size_t oversampled_used = 0;
            for (size_t j = 0; j < (2 * APP_AUDIO_I2S_OVERSAMPLING); j += 2) {
                size_t idx = capture_index + j;
                if (idx < raw_sample_count) {
                    oversampled_sum += raw_frame_buffer[idx];
                    oversampled_used++;
                }
            }
            capture_frame_buffer[capture_sample_count++] = oversampled_used
                ? (int16_t)(oversampled_sum / (int32_t)oversampled_used)
                : 0;
            capture_index += 2 * APP_AUDIO_I2S_OVERSAMPLING;
        }
#endif

        size_t sample_count = app_audio_i2s_resample_frame(capture_frame_buffer,
                                                           capture_sample_count,
                                                           APP_AUDIO_I2S_CAPTURE_RATE,
                                                           frame_buffer,
                                                           samples_per_frame,
                                                           APP_AUDIO_SAMPLE_RATE);
        if (sample_count == 0) {
            ESP_LOGW(TAG, "Unsupported audio rate conversion: capture=%u send=%u",
                     (unsigned)APP_AUDIO_I2S_CAPTURE_RATE,
                     (unsigned)APP_AUDIO_SAMPLE_RATE);
            vTaskDelay(pdMS_TO_TICKS(APP_AUDIO_FRAME_MS));
            continue;
        }

        int32_t raw_peak = 0;
        int64_t raw_abs_sum = 0;
        bool all_same = sample_count > 0;
        int16_t first_sample = sample_count > 0 ? frame_buffer[0] : 0;
        uint32_t zero_crossings = 0;
        for (size_t i = 0; i < sample_count; i++) {
            int32_t sample = frame_buffer[i];
            int32_t magnitude = sample < 0 ? -sample : sample;
            if (magnitude > raw_peak) {
                raw_peak = magnitude;
            }
            raw_abs_sum += magnitude;
            if (frame_buffer[i] != first_sample) {
                all_same = false;
            }
            if (i > 0) {
                bool prev_non_negative = frame_buffer[i - 1] >= 0;
                bool cur_non_negative = frame_buffer[i] >= 0;
                if (prev_non_negative != cur_non_negative) {
                    zero_crossings++;
                }
            }
        }

        int32_t peak = 0;
        int64_t abs_sum = 0;
        for (size_t i = 0; i < sample_count; i++) {
            int32_t amplified = ((int32_t)frame_buffer[i] * APP_AUDIO_I2S_GAIN_NUM) / APP_AUDIO_I2S_GAIN_DEN;
            if (amplified > INT16_MAX) {
                amplified = INT16_MAX;
            } else if (amplified < INT16_MIN) {
                amplified = INT16_MIN;
            }
            frame_buffer[i] = (int16_t)amplified;

            int32_t magnitude = amplified < 0 ? -amplified : amplified;
            if (magnitude > peak) {
                peak = magnitude;
            }
            abs_sum += magnitude;
        }

        int64_t frame_start_us = 0;
#if APP_AUDIO_I2S_PROFILE_ENABLE
        frame_start_us = esp_timer_get_time();
#endif
        s_audio_i2s.processed_frame_count++;

        if (!app_session_is_audio_send_ready()) {
            if (!waiting_logged) {
                ESP_LOGI(TAG, "Waiting for RTC connected state before sending mic audio");
                waiting_logged = true;
            }
            continue;
        }
        waiting_logged = false;

        bool uplink_gated_this_frame = app_audio_playback_is_recently_active(APP_AUDIO_I2S_UPLINK_GATE_HOLD_MS);
        if (uplink_gated_this_frame) {
            if (!uplink_gated) {
                uplink_gated = true;
                ESP_LOGI(TAG, "Remote audio active, muting mic uplink with silence frames");
            }
        } else if (uplink_gated) {
            uplink_gated = false;
            ESP_LOGI(TAG, "Remote audio inactive for %u ms, resuming live mic uplink",
                     (unsigned)APP_AUDIO_I2S_UPLINK_GATE_HOLD_MS);
        }

        if (uplink_gated_this_frame) {
            memset(frame_buffer, 0, sample_count * sizeof(frame_buffer[0]));
        }

        if (!s_audio_i2s.running) {
            break;
        }

        {
            const void *frame_data = frame_buffer;
            size_t frame_size = sample_count * sizeof(frame_buffer[0]);
#if APP_AUDIO_CODEC == APP_AUDIO_CODEC_G711A
            uint8_t encoded_frame[samples_per_frame];
            frame_size = app_codec_g711a_encode(frame_buffer, sample_count, encoded_frame, sizeof(encoded_frame));
            frame_data = encoded_frame;
            if (frame_size != samples_per_frame) {
                ESP_LOGE(TAG,
                         "Unexpected G711A uplink frame size: encoded=%u expected=%u sample_count=%u bytes_read=%u gated=%d",
                         (unsigned)frame_size,
                         (unsigned)samples_per_frame,
                         (unsigned)sample_count,
                         (unsigned)bytes_read,
                         uplink_gated ? 1 : 0);
                continue;
            }
#else
            {
                size_t stereo_sample_count = app_audio_i2s_expand_mono_to_stereo(frame_buffer, sample_count,
                                                                                  stereo_frame_buffer,
                                                                                  samples_per_frame * 2U);
                if (stereo_sample_count == 0) {
                    ESP_LOGW(TAG, "Failed to expand Opus PCM frame to stereo");
                    continue;
                }
                frame_data = stereo_frame_buffer;
                frame_size = stereo_sample_count * sizeof(stereo_frame_buffer[0]);
            }
#endif

            err = app_session_send_audio(frame_data, frame_size, s_audio_i2s.pts);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send mic audio frame: %s", esp_err_to_name(err));
#if APP_AUDIO_I2S_PROFILE_ENABLE
                s_audio_i2s.profile_send_failures++;
#endif
                continue;
            }

            s_audio_i2s.sent_frame_count++;
            s_audio_i2s.pts += samples_per_frame;
#if APP_AUDIO_I2S_PROFILE_ENABLE
            {
                uint32_t frame_time_us = (uint32_t)(esp_timer_get_time() - frame_start_us);
                uint32_t frame_budget_us = APP_AUDIO_FRAME_MS * 1000U;
                s_audio_i2s.profile_window_frames++;
                s_audio_i2s.profile_total_frame_time_us += frame_time_us;
                if (frame_time_us > s_audio_i2s.profile_max_frame_time_us) {
                    s_audio_i2s.profile_max_frame_time_us = frame_time_us;
                }
                if (frame_time_us > frame_budget_us) {
                    s_audio_i2s.profile_deadline_misses++;
                }
            }
#endif
        }

        if ((s_audio_i2s.processed_frame_count % (5000U / APP_AUDIO_FRAME_MS)) == 0U) {
            uint32_t raw_avg_abs = sample_count ? (uint32_t)(raw_abs_sum / (int64_t)sample_count) : 0U;
            uint32_t avg_abs = sample_count ? (uint32_t)(abs_sum / (int64_t)sample_count) : 0U;
            ESP_LOGI(TAG, "Mic frames processed=%u sent=%u uplink_gated=%d (%u bytes last read, raw_avg_abs=%u raw_peak=%u avg_abs=%u peak=%u zero_crossings=%u first_sample=%d gain=%d/%d)",
                     (unsigned)s_audio_i2s.processed_frame_count,
                     (unsigned)s_audio_i2s.sent_frame_count,
                     uplink_gated ? 1 : 0,
                     (unsigned)bytes_read,
                     (unsigned)raw_avg_abs, (unsigned)raw_peak,
                     (unsigned)avg_abs, (unsigned)peak, (unsigned)zero_crossings, (int)first_sample,
                     APP_AUDIO_I2S_GAIN_NUM, APP_AUDIO_I2S_GAIN_DEN);
#if APP_AUDIO_I2S_PROFILE_ENABLE
            {
                uint32_t avg_frame_time_us = s_audio_i2s.profile_window_frames
                    ? (uint32_t)(s_audio_i2s.profile_total_frame_time_us / s_audio_i2s.profile_window_frames)
                    : 0U;
                ESP_LOGI(TAG, "Mic profile: frames=%u avg_us=%u max_us=%u deadline_miss=%u send_fail=%u budget_us=%u",
                         (unsigned)s_audio_i2s.profile_window_frames,
                         (unsigned)avg_frame_time_us,
                         (unsigned)s_audio_i2s.profile_max_frame_time_us,
                         (unsigned)s_audio_i2s.profile_deadline_misses,
                         (unsigned)s_audio_i2s.profile_send_failures,
                         (unsigned)(APP_AUDIO_FRAME_MS * 1000U));
                s_audio_i2s.profile_window_frames = 0;
                s_audio_i2s.profile_send_failures = 0;
                s_audio_i2s.profile_deadline_misses = 0;
                s_audio_i2s.profile_max_frame_time_us = 0;
                s_audio_i2s.profile_total_frame_time_us = 0;
            }
#endif
            if (all_same) {
                s_audio_i2s.constant_frame_reports++;
                ESP_LOGW(TAG, "Mic frame appears constant-valued (%u reports)", (unsigned)s_audio_i2s.constant_frame_reports);
#if APP_AUDIO_I2S_AUTO_TUNE
                if (!s_audio_i2s.mode_locked && s_audio_i2s.constant_frame_reports >= 2) {
                    uint8_t next_mode = app_audio_i2s_find_next_allowed_mode(s_audio_i2s.mode_index);
                    if (next_mode == s_audio_i2s.mode_index ||
                        (s_audio_i2s.tuned_mode_mask & (uint8_t)(1U << next_mode)) != 0U) {
                        s_audio_i2s.mode_locked = true;
                        ESP_LOGW(TAG, "Cardputer mic auto-tune exhausted; keeping %s and reporting constant capture",
                                 s_audio_i2s_modes[s_audio_i2s.mode_index].name);
                    } else {
                        ESP_LOGW(TAG, "Auto-tuning Cardputer mic mode: switching from %s to %s",
                                 s_audio_i2s_modes[s_audio_i2s.mode_index].name,
                                 s_audio_i2s_modes[next_mode].name);
                        if (app_audio_i2s_switch_mode(next_mode) != ESP_OK) {
                            s_audio_i2s.mode_locked = true;
                            ESP_LOGE(TAG, "Failed to switch Cardputer mic mode, keeping %s",
                                     s_audio_i2s_modes[s_audio_i2s.mode_index].name);
                        }
                    }
                }
#else
                ESP_LOGW(TAG, "Try APP_AUDIO_I2S_PDM_DATA_FMT raw or APP_AUDIO_I2S_PDM_CLK_INVERT 1");
#endif
            } else if (!s_audio_i2s.mode_locked) {
                s_audio_i2s.mode_locked = true;
                ESP_LOGI(TAG, "Cardputer mic mode appears non-constant, keeping %s",
                         s_audio_i2s_modes[s_audio_i2s.mode_index].name);
            }
        }
    }

    s_audio_i2s.task = NULL;
    vTaskDelete(NULL);
}

esp_err_t app_audio_i2s_start(void)
{
    esp_err_t err;

    if (s_audio_i2s.running) {
        return ESP_OK;
    }

    s_audio_i2s.mode_index = app_audio_i2s_initial_mode_index();
    s_audio_i2s.constant_frame_reports = 0;
    s_audio_i2s.tuned_mode_mask = (uint8_t)(1U << s_audio_i2s.mode_index);
    s_audio_i2s.mode_locked = false;

    err = app_audio_i2s_init_channel_for_mode(&s_audio_i2s_modes[s_audio_i2s.mode_index]);
    if (err != ESP_OK) {
        return err;
    }

    s_audio_i2s.running = true;
    s_audio_i2s.processed_frame_count = 0;
    s_audio_i2s.sent_frame_count = 0;
    s_audio_i2s.pts = 0;

    ESP_LOGI(TAG, "Free heap before mic task create: %u", (unsigned)esp_get_free_heap_size());

    if (xTaskCreate(app_audio_i2s_task, "app_audio_i2s", APP_AUDIO_I2S_STACK_SIZE, NULL, 4,
                    &s_audio_i2s.task) != pdPASS) {
        s_audio_i2s.running = false;
        app_audio_i2s_deinit_channel();
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Cardputer mic audio publisher started");
    return ESP_OK;
}

esp_err_t app_audio_i2s_stop(void)
{
    if (!s_audio_i2s.running) {
        return ESP_OK;
    }
    s_audio_i2s.running = false;
    app_audio_i2s_deinit_channel();
    return ESP_OK;
}
