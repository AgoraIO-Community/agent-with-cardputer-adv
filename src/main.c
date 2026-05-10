#include <ctype.h>

#include "app_audio_controller.h"
#include "app_audio_playback.h"
#include "app_audio_adv_codec.h"
#include "app_config.h"
#include "app_display.h"
#include "app_https.h"
#include "app_protocol.h"
#include "app_rtsa.h"
#include "app_session.h"
#include "app_time_sync.h"
#include "app_wifi.h"

#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_main";

#define APP_CARDPUTER_ADV_KB_I2C_ADDR 0x34
#define APP_CARDPUTER_ADV_KB_INT_GPIO GPIO_NUM_11
#define APP_CARDPUTER_ADV_KB_ROWS 7
#define APP_CARDPUTER_ADV_KB_COLS 8

#define TCA8418_REG_CFG 0x01
#define TCA8418_REG_INT_STAT 0x02
#define TCA8418_REG_KEY_LCK_EC 0x03
#define TCA8418_REG_KEY_EVENT_A 0x04
#define TCA8418_REG_GPIO_INT_STAT_1 0x11
#define TCA8418_REG_GPIO_INT_STAT_2 0x12
#define TCA8418_REG_GPIO_INT_STAT_3 0x13
#define TCA8418_REG_GPIO_INT_EN_1 0x1A
#define TCA8418_REG_GPIO_INT_EN_2 0x1B
#define TCA8418_REG_GPIO_INT_EN_3 0x1C
#define TCA8418_REG_KP_GPIO_1 0x1D
#define TCA8418_REG_KP_GPIO_2 0x1E
#define TCA8418_REG_KP_GPIO_3 0x1F
#define TCA8418_REG_GPI_EM_1 0x20
#define TCA8418_REG_GPI_EM_2 0x21
#define TCA8418_REG_GPI_EM_3 0x22
#define TCA8418_REG_GPIO_DIR_1 0x23
#define TCA8418_REG_GPIO_DIR_2 0x24
#define TCA8418_REG_GPIO_DIR_3 0x25
#define TCA8418_REG_GPIO_INT_LVL_1 0x26
#define TCA8418_REG_GPIO_INT_LVL_2 0x27
#define TCA8418_REG_GPIO_INT_LVL_3 0x28

typedef struct {
    bool pressed;
    uint8_t row;
    uint8_t col;
} app_keyboard_event_t;

typedef struct {
    i2c_master_dev_handle_t dev;
    bool using_adv_codec_bus;
} app_keyboard_ctx_t;

static const char s_app_keyboard_map[4][14] = {
    { '`', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b' },
    { '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\\' },
    { 0, 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '\n' },
    { 0, 0, 0, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', ' ' },
};

static void app_main_on_pull_audio_info(app_session_audio_codec_t codec,
                                        uint32_t sample_rate,
                                        uint8_t channels,
                                        void *ctx)
{
    (void)ctx;
    app_audio_controller_handle_remote_stream_info(codec, sample_rate, channels);
}

static void app_main_on_pull_audio_frame(app_session_audio_codec_t codec,
                                         const void *data,
                                         size_t size,
                                         uint32_t pts,
                                         void *ctx)
{
    (void)ctx;
    app_audio_controller_handle_remote_audio_frame(codec, data, size, pts);
}

static esp_err_t app_validate_runtime_config(void)
{
    if (strcmp(APP_WIFI_SSID, "CHANGE_ME") == 0 || strcmp(APP_WIFI_PASSWORD, "CHANGE_ME") == 0) {
        ESP_LOGE(TAG, "Missing Wi-Fi config. Create src/app_config_local.h from src/app_config.local.example.h");
        return ESP_ERR_INVALID_STATE;
    }

    if (strcmp(APP_PROTOCOL_BASE_URL, "https://CHANGE_ME") == 0) {
        ESP_LOGE(TAG, "Missing protocol config. Fill APP_PROTOCOL_BASE_URL in src/app_config_local.h");
        return ESP_ERR_INVALID_STATE;
    }

    if (APP_AUDIO_USE_I2S_MIC) {
#if APP_AUDIO_CODEC == APP_AUDIO_CODEC_G711A
        ESP_LOGI(TAG, "Audio path: Cardputer I2S mic + G711A @ %d Hz", APP_AUDIO_SAMPLE_RATE);
#elif APP_AUDIO_CODEC == APP_AUDIO_CODEC_G722
        ESP_LOGI(TAG, "Audio path: Cardputer I2S mic + G722 @ %d Hz", APP_AUDIO_SAMPLE_RATE);
#else
        ESP_LOGI(TAG, "Audio path: Cardputer I2S mic + Opus @ %d Hz (capture @ %d Hz)",
                 APP_AUDIO_SAMPLE_RATE, APP_AUDIO_I2S_CAPTURE_RATE);
#endif
    } else {
        ESP_LOGI(TAG, "Audio path: fake audio test publisher");
    }

    return ESP_OK;
}

static esp_err_t app_keyboard_write_reg(app_keyboard_ctx_t *ctx, uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = { reg, value };

    return i2c_master_transmit(ctx->dev, payload, sizeof(payload), 100);
}

static esp_err_t app_keyboard_read_reg(app_keyboard_ctx_t *ctx, uint8_t reg, uint8_t *value)
{
    esp_err_t err;

    err = i2c_master_transmit(ctx->dev, &reg, 1, 100);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_master_receive(ctx->dev, value, 1, 100);
}

static esp_err_t app_keyboard_init(app_keyboard_ctx_t *ctx)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = APP_CARDPUTER_ADV_KB_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    i2c_master_bus_handle_t bus;
    uint8_t row_mask = 0;
    uint8_t col_mask = 0;
    esp_err_t err;

    memset(ctx, 0, sizeof(*ctx));

    for (int i = 0; i < APP_CARDPUTER_ADV_KB_ROWS; i++) {
        row_mask = (uint8_t)((row_mask << 1) | 1U);
    }
    for (int i = 0; i < APP_CARDPUTER_ADV_KB_COLS; i++) {
        col_mask = (uint8_t)((col_mask << 1) | 1U);
    }

    bus = app_audio_playback_get_i2c_bus();
    if (bus != NULL) {
        ESP_LOGI(TAG, "Keyboard reusing playback codec I2C bus");
    } else {
        ESP_RETURN_ON_ERROR(app_audio_adv_codec_acquire(), TAG, "Keyboard I2C acquire failed");
        ctx->using_adv_codec_bus = true;
        bus = app_audio_adv_codec_get_i2c_bus();
        if (bus == NULL) {
            ESP_LOGE(TAG, "Keyboard I2C bus unavailable");
            app_audio_adv_codec_release();
            ctx->using_adv_codec_bus = false;
            return ESP_ERR_INVALID_STATE;
        }
    }
    err = i2c_master_bus_add_device(bus, &dev_cfg, &ctx->dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Keyboard I2C dev init failed: %s", esp_err_to_name(err));
        if (ctx->using_adv_codec_bus) {
            app_audio_adv_codec_release();
            ctx->using_adv_codec_bus = false;
        }
        return err;
    }

    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_GPIO_DIR_1, 0x00), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_GPIO_DIR_2, 0x00), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_GPIO_DIR_3, 0x00), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_GPI_EM_1, 0xFF), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_GPI_EM_2, 0xFF), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_GPI_EM_3, 0xFF), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_GPIO_INT_LVL_1, 0x00), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_GPIO_INT_LVL_2, 0x00), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_GPIO_INT_LVL_3, 0x00), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_GPIO_INT_EN_1, 0xFF), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_GPIO_INT_EN_2, 0xFF), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_GPIO_INT_EN_3, 0xFF), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_KP_GPIO_1, row_mask), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_KP_GPIO_2, col_mask), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_KP_GPIO_3, 0x00), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_INT_STAT, 0x03), TAG, "KB reg write failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_CFG, 0x01), TAG, "KB cfg write failed");

    return ESP_OK;
}

static void app_keyboard_deinit(app_keyboard_ctx_t *ctx)
{
    if (ctx->dev != NULL) {
        i2c_master_bus_rm_device(ctx->dev);
        ctx->dev = NULL;
    }
    if (ctx->using_adv_codec_bus) {
        app_audio_adv_codec_release();
        ctx->using_adv_codec_bus = false;
    }
}

static esp_err_t app_keyboard_get_event(app_keyboard_ctx_t *ctx, app_keyboard_event_t *event)
{
    uint8_t count = 0;
    uint8_t raw = 0;
    uint8_t int1 = 0;
    uint8_t int2 = 0;
    uint8_t int3 = 0;

    memset(event, 0, sizeof(*event));

    ESP_RETURN_ON_ERROR(app_keyboard_read_reg(ctx, TCA8418_REG_KEY_LCK_EC, &count), TAG, "KB count read failed");
    if ((count & 0x0F) == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(app_keyboard_read_reg(ctx, TCA8418_REG_KEY_EVENT_A, &raw), TAG, "KB event read failed");
    ESP_RETURN_ON_ERROR(app_keyboard_read_reg(ctx, TCA8418_REG_GPIO_INT_STAT_1, &int1), TAG, "KB stat read failed");
    ESP_RETURN_ON_ERROR(app_keyboard_read_reg(ctx, TCA8418_REG_GPIO_INT_STAT_2, &int2), TAG, "KB stat read failed");
    ESP_RETURN_ON_ERROR(app_keyboard_read_reg(ctx, TCA8418_REG_GPIO_INT_STAT_3, &int3), TAG, "KB stat read failed");
    ESP_RETURN_ON_ERROR(app_keyboard_write_reg(ctx, TCA8418_REG_INT_STAT, 0x03), TAG, "KB int clear failed");

    if (raw == 0) {
        return ESP_ERR_NOT_FOUND;
    }

    event->pressed = (raw & 0x80U) == 0;
    raw = (uint8_t)((raw & 0x7FU) - 1U);
    event->row = raw / 10U;
    event->col = raw % 10U;
    return ESP_OK;
}

static char app_keyboard_event_to_char(const app_keyboard_event_t *event)
{
    uint8_t row;
    uint8_t col;

    if (event == NULL) {
        return 0;
    }

    col = (uint8_t)(event->row * 2U);
    if (event->col > 3U) {
        col++;
    }
    row = (uint8_t)((event->col + 4U) % 4U);

    if (row >= 4U || col >= 14U) {
        return 0;
    }
    return s_app_keyboard_map[row][col];
}

static esp_err_t app_wait_for_start_key(void)
{
    app_keyboard_ctx_t kb = { 0 };
    esp_err_t err;

    err = app_keyboard_init(&kb);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << APP_CARDPUTER_ADV_KB_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Press '%c' on the keyboard to start the agent", APP_AGORA_START_KEY);

    while (true) {
        app_keyboard_event_t event = { 0 };
        if (app_keyboard_get_event(&kb, &event) == ESP_OK && event.pressed) {
            char key = app_keyboard_event_to_char(&event);
            if (key != 0) {
                ESP_LOGI(TAG, "Keyboard key pressed: '%c'", key);
            }
            if (key == APP_AGORA_START_KEY || key == (char)toupper((unsigned char)APP_AGORA_START_KEY)) {
                app_keyboard_deinit(&kb);
                return ESP_OK;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static esp_err_t app_wait_for_toggle_key_press(void)
{
    app_keyboard_ctx_t kb = { 0 };
    esp_err_t err;

    err = app_keyboard_init(&kb);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << APP_CARDPUTER_ADV_KB_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    while (true) {
        app_keyboard_event_t event = { 0 };
        if (app_keyboard_get_event(&kb, &event) == ESP_OK && event.pressed) {
            char key = app_keyboard_event_to_char(&event);
            if (key != 0) {
                ESP_LOGI(TAG, "Keyboard key pressed: '%c'", key);
            }
            if (key == APP_AGORA_START_KEY || key == (char)toupper((unsigned char)APP_AGORA_START_KEY)) {
                app_keyboard_deinit(&kb);
                return ESP_OK;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    app_protocol_config_t protocol_cfg = { 0 };
    esp_err_t err;
    bool audio_started = false;
    bool agent_started = false;
    bool display_started = false;

    ESP_LOGI(TAG, "Cardputer RTSA protocol client starting");

    ESP_ERROR_CHECK(app_validate_runtime_config());
    ESP_ERROR_CHECK(app_wifi_start());
    ESP_ERROR_CHECK(app_time_sync_wait());
#if APP_HTTPS_TEST_BEFORE_PROTOCOL
    ESP_LOGI(TAG, "Running HTTPS probe before protocol request");
    if (app_https_get_test(APP_HTTPS_TEST_URL) != ESP_OK) {
        ESP_LOGW(TAG, "HTTPS probe failed, continuing to protocol test");
    }
#endif
    err = app_protocol_get_config(&protocol_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "get_config failed: %s", esp_err_to_name(err));
        return;
    }
    if (APP_AUDIO_ENABLE_PULL_PLAYBACK) {
        ESP_ERROR_CHECK(app_session_set_audio_receive_callbacks(app_main_on_pull_audio_info,
                                                                app_main_on_pull_audio_frame,
                                                                NULL));
    }
    ESP_ERROR_CHECK(app_audio_controller_init());
    ESP_ERROR_CHECK(app_rtsa_start(&protocol_cfg));
    if (!display_started) {
        ESP_ERROR_CHECK(app_display_start());
        display_started = true;
    }
    if (APP_AGORA_AUTO_START) {
#if APP_AGORA_START_ON_KEYPRESS
        ESP_ERROR_CHECK(app_wait_for_start_key());
#endif
        ESP_ERROR_CHECK(app_protocol_start_agent(&protocol_cfg));
        agent_started = true;
    }

    ESP_ERROR_CHECK(app_audio_controller_start());
    audio_started = true;

#if APP_AUDIO_ENABLE_PULL_PLAYBACK
    if (agent_started && audio_started) {
        ESP_LOGI(TAG, "Playback armed; speak into the mic to elicit agent audio");
        vTaskDelay(pdMS_TO_TICKS(10000));
        if (!app_audio_controller_has_received_remote_audio() &&
            !app_audio_controller_is_in_playback()) {
            ESP_LOGW(TAG, "No remote audio received within 10s after local audio start");
        }
    }
#endif

    while (true) {
#if APP_AGORA_START_ON_KEYPRESS
        if (agent_started) {
            ESP_LOGI(TAG, "Press '%c' again to stop the agent", APP_AGORA_START_KEY);
            ESP_ERROR_CHECK(app_wait_for_toggle_key_press());
            ESP_ERROR_CHECK(app_audio_controller_stop());
            audio_started = false;
            ESP_ERROR_CHECK(app_protocol_stop_agent(&protocol_cfg));
            agent_started = false;
            ESP_LOGI(TAG, "Agent stopped");
            continue;
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
