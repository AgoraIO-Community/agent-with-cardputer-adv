#include "app_audio_adv_codec.h"

#include <string.h>

#include "app_config.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

typedef struct {
    SemaphoreHandle_t lock;
    i2c_master_bus_handle_t i2c_bus;
    i2c_master_dev_handle_t i2c_dev;
    uint32_t ref_count;
    bool adc_enabled;
    bool dac_enabled;
    bool external_dac_active;
    bool owns_i2c_bus;
} app_audio_adv_codec_ctx_t;

static const char *TAG = "app_audio_adv_codec";
static const uint8_t APP_AUDIO_I2S_ADV_ES8311_ADDR = 0x18;
static app_audio_adv_codec_ctx_t s_adv_codec;

static esp_err_t app_audio_adv_codec_write_reg_locked(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = { reg, value };

    ESP_RETURN_ON_FALSE(s_adv_codec.i2c_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "Codec I2C device not ready");
    return i2c_master_transmit(s_adv_codec.i2c_dev, data, sizeof(data), 100);
}

static esp_err_t app_audio_adv_codec_read_reg_locked(uint8_t reg, uint8_t *value)
{
    ESP_RETURN_ON_FALSE(value != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid codec readback pointer");
    ESP_RETURN_ON_FALSE(s_adv_codec.i2c_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "Codec I2C device not ready");
    return i2c_master_transmit_receive(s_adv_codec.i2c_dev, &reg, sizeof(reg), value, 1, 100);
}

static esp_err_t app_audio_adv_codec_attach_device_locked(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = APP_AUDIO_I2S_ADV_ES8311_ADDR,
        .scl_speed_hz = 100000,
    };
    static const uint8_t init_bulk_data[] = {
        2, 0x00, 0x80,
        2, 0x01, 0xBA,
        2, 0x02, 0x18,
        0
    };
    esp_err_t err;

    s_adv_codec.i2c_bus = bus;
    err = i2c_master_bus_add_device(bus, &dev_cfg, &s_adv_codec.i2c_dev);
    if (err != ESP_OK) {
        s_adv_codec.i2c_bus = NULL;
        return err;
    }
    err = app_audio_adv_codec_write_bulk(init_bulk_data);
    if (err != ESP_OK) {
        i2c_master_bus_rm_device(s_adv_codec.i2c_dev);
        s_adv_codec.i2c_dev = NULL;
        s_adv_codec.i2c_bus = NULL;
        return err;
    }
    s_adv_codec.adc_enabled = false;
    s_adv_codec.dac_enabled = false;
    return ESP_OK;
}

static esp_err_t app_audio_adv_codec_lock(void)
{
    if (s_adv_codec.lock == NULL) {
        s_adv_codec.lock = xSemaphoreCreateMutex();
        if (s_adv_codec.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (xSemaphoreTake(s_adv_codec.lock, portMAX_DELAY) != pdTRUE) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static void app_audio_adv_codec_unlock(void)
{
    if (s_adv_codec.lock != NULL) {
        xSemaphoreGive(s_adv_codec.lock);
    }
}

static esp_err_t app_audio_adv_codec_apply_power_state_locked(void)
{
    uint8_t power_seq[20];
    size_t power_len = 0;

    if (!s_adv_codec.adc_enabled && !s_adv_codec.dac_enabled && !s_adv_codec.external_dac_active) {
        power_seq[power_len++] = 2;
        power_seq[power_len++] = 0x0D;
        power_seq[power_len++] = 0xFC;
        power_seq[power_len++] = 2;
        power_seq[power_len++] = 0x0E;
        power_seq[power_len++] = 0x6A;
        power_seq[power_len++] = 2;
        power_seq[power_len++] = 0x00;
        power_seq[power_len++] = 0x00;
        power_seq[power_len++] = 0;
        return app_audio_adv_codec_write_bulk(power_seq);
    }

    power_seq[power_len++] = 2;
    power_seq[power_len++] = 0x0D;
    if (s_adv_codec.adc_enabled && (s_adv_codec.dac_enabled || s_adv_codec.external_dac_active)) {
        power_seq[power_len++] = 0x00;
    } else if (s_adv_codec.adc_enabled) {
        power_seq[power_len++] = 0x01;
    } else {
        power_seq[power_len++] = 0x02;
    }
    power_seq[power_len++] = 2;
    power_seq[power_len++] = 0x0E;
    power_seq[power_len++] = 0x02;
    power_seq[power_len++] = 0;
    return app_audio_adv_codec_write_bulk(power_seq);
}

esp_err_t app_audio_adv_codec_write_bulk(const uint8_t *bulk_data)
{
    ESP_RETURN_ON_FALSE(bulk_data != NULL, ESP_ERR_INVALID_ARG, TAG, "Invalid codec bulk write");
    ESP_RETURN_ON_FALSE(s_adv_codec.i2c_dev != NULL, ESP_ERR_INVALID_STATE, TAG, "Codec I2C device not ready");

    while (*bulk_data != 0) {
        uint8_t len = *bulk_data++;
        ESP_RETURN_ON_FALSE(len >= 2, ESP_ERR_INVALID_ARG, TAG, "Invalid codec bulk write length");
        ESP_RETURN_ON_ERROR(i2c_master_transmit(
                                s_adv_codec.i2c_dev,
                                bulk_data,
                                len,
                                100),
                            TAG, "Failed to write ADV codec registers");
        bulk_data += len;
    }
    return ESP_OK;
}

esp_err_t app_audio_adv_codec_acquire(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = APP_AUDIO_I2S_ADV_I2C_PORT,
        .sda_io_num = APP_AUDIO_I2S_ADV_I2C_SDA_GPIO,
        .scl_io_num = APP_AUDIO_I2S_ADV_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t existing_bus = NULL;
    esp_err_t err;

    ESP_RETURN_ON_ERROR(app_audio_adv_codec_lock(), TAG, "Failed to lock ADV codec");
    if (s_adv_codec.ref_count > 0) {
        s_adv_codec.ref_count++;
        app_audio_adv_codec_unlock();
        return ESP_OK;
    }

    (void)i2c_master_get_bus_handle(APP_AUDIO_I2S_ADV_I2C_PORT, &existing_bus);
    if (existing_bus != NULL) {
        err = app_audio_adv_codec_attach_device_locked(existing_bus);
        if (err == ESP_OK) {
            s_adv_codec.owns_i2c_bus = false;
        }
    } else {
        err = i2c_new_master_bus(&bus_cfg, &s_adv_codec.i2c_bus);
        if (err == ESP_OK) {
            s_adv_codec.owns_i2c_bus = true;
            err = app_audio_adv_codec_attach_device_locked(s_adv_codec.i2c_bus);
        }
    }

    if (err == ESP_OK) {
        s_adv_codec.ref_count = 1;
        ESP_LOGI(TAG, "Initialized Cardputer ADV ES8311 control on i2c_port=%d", APP_AUDIO_I2S_ADV_I2C_PORT);
    } else {
        if (s_adv_codec.i2c_dev != NULL) {
            i2c_master_bus_rm_device(s_adv_codec.i2c_dev);
            s_adv_codec.i2c_dev = NULL;
        }
        if (s_adv_codec.i2c_bus != NULL && s_adv_codec.owns_i2c_bus) {
            i2c_del_master_bus(s_adv_codec.i2c_bus);
        }
        s_adv_codec.i2c_bus = NULL;
        s_adv_codec.owns_i2c_bus = false;
    }
    app_audio_adv_codec_unlock();
    return err;
}

void app_audio_adv_codec_release(void)
{
    if (app_audio_adv_codec_lock() != ESP_OK) {
        return;
    }
    if (s_adv_codec.ref_count == 0) {
        app_audio_adv_codec_unlock();
        return;
    }
    s_adv_codec.ref_count--;
    if (s_adv_codec.ref_count == 0) {
        s_adv_codec.adc_enabled = false;
        s_adv_codec.dac_enabled = false;
        if (s_adv_codec.i2c_dev != NULL) {
            i2c_master_bus_rm_device(s_adv_codec.i2c_dev);
            s_adv_codec.i2c_dev = NULL;
        }
        if (s_adv_codec.i2c_bus != NULL && s_adv_codec.owns_i2c_bus) {
            i2c_del_master_bus(s_adv_codec.i2c_bus);
        }
        s_adv_codec.i2c_bus = NULL;
        s_adv_codec.owns_i2c_bus = false;
    }
    app_audio_adv_codec_unlock();
}

esp_err_t app_audio_adv_codec_enable_adc(bool enabled)
{
    static const uint8_t adc_enable_bulk_data[] = {
        2, 0x14, APP_AUDIO_I2S_ADV_PGA_REG,
        2, 0x17, APP_AUDIO_I2S_ADV_ADC_VOLUME_REG,
        2, 0x1C, 0x6A,
        0
    };
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_ERROR(app_audio_adv_codec_lock(), TAG, "Failed to lock ADV codec");
    ESP_GOTO_ON_FALSE(s_adv_codec.ref_count > 0, ESP_ERR_INVALID_STATE, unlock, TAG, "Codec not acquired");

    s_adv_codec.adc_enabled = enabled;
    if (enabled) {
        ret = app_audio_adv_codec_write_bulk(adc_enable_bulk_data);
        if (ret != ESP_OK) {
            goto unlock;
        }
    }
    ret = app_audio_adv_codec_apply_power_state_locked();

unlock:
    app_audio_adv_codec_unlock();
    return ret;
}

esp_err_t app_audio_adv_codec_enable_dac(bool enabled, uint8_t volume_reg)
{
    uint8_t dac_enable_bulk_data[] = {
        2, 0x01, 0xBF,
        2, 0x02, 0x10,
        2, 0x03, 0x10,
        2, 0x04, 0x20,
        2, 0x05, 0x00,
        2, 0x06, 0x03,
        2, 0x07, 0x00,
        2, 0x08, 0xFF,
        2, 0x0B, 0x00,
        2, 0x0C, 0x00,
        2, 0x10, 0x1F,
        2, 0x11, 0x7F,
        2, 0x12, 0x00,
        2, 0x13, 0x10,
        2, 0x14, 0x1A,
        2, 0x15, 0x40,
        2, 0x0D, 0x01,
        2, 0x31, 0x00,
        2, 0x32, volume_reg,
        2, 0x33, volume_reg,
        2, 0x37, 0x08,
        2, 0x44, 0x58,
        2, 0x45, 0x00,
        0
    };
    static const uint8_t adc_clock_restore_bulk_data[] = {
        2, 0x01, 0xBA,
        2, 0x02, 0x18,
        2, 0x0D, 0x01,
        2, 0x0E, 0x02,
        2, 0x14, APP_AUDIO_I2S_ADV_PGA_REG,
        2, 0x15, 0x00,
        2, 0x17, APP_AUDIO_I2S_ADV_ADC_VOLUME_REG,
        2, 0x1C, 0x6A,
        2, 0x44, 0x08,
        0
    };
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_ERROR(app_audio_adv_codec_lock(), TAG, "Failed to lock ADV codec");
    ESP_GOTO_ON_FALSE(s_adv_codec.ref_count > 0, ESP_ERR_INVALID_STATE, unlock, TAG, "Codec not acquired");

    s_adv_codec.dac_enabled = enabled;
    ret = app_audio_adv_codec_apply_power_state_locked();
    if (ret != ESP_OK) {
        goto unlock;
    }
    if (enabled) {
        ret = app_audio_adv_codec_write_bulk(dac_enable_bulk_data);
        if (ret != ESP_OK) {
            goto unlock;
        }
    } else if (s_adv_codec.adc_enabled) {
        ret = app_audio_adv_codec_write_bulk(adc_clock_restore_bulk_data);
        if (ret != ESP_OK) {
            goto unlock;
        }
    }

unlock:
    app_audio_adv_codec_unlock();
    return ret;
}

esp_err_t app_audio_adv_codec_reset_for_adc(void)
{
    static const uint8_t reset_bulk_data[] = {
        2, 0x00, 0x80,
        2, 0x01, 0xBA,
        2, 0x02, 0x18,
        0
    };
    esp_err_t ret;

    ESP_RETURN_ON_ERROR(app_audio_adv_codec_lock(), TAG, "Failed to lock ADV codec");
    ESP_GOTO_ON_FALSE(s_adv_codec.ref_count > 0, ESP_ERR_INVALID_STATE, unlock, TAG, "Codec not acquired");

    s_adv_codec.adc_enabled = false;
    s_adv_codec.dac_enabled = false;
    s_adv_codec.external_dac_active = false;
    ret = app_audio_adv_codec_write_bulk(reset_bulk_data);
    if (ret != ESP_OK) {
        goto unlock;
    }

unlock:
    app_audio_adv_codec_unlock();
    return ret;
}

esp_err_t app_audio_adv_codec_reset_for_adc_recovery(void)
{
    static const uint8_t common_init_bulk_data[] = {
        2, 0x01, 0xBA,
        2, 0x02, 0x18,
        0
    };
    static const uint8_t adc_enable_bulk_data[] = {
        2, 0x14, APP_AUDIO_I2S_ADV_PGA_REG,
        2, 0x17, APP_AUDIO_I2S_ADV_ADC_VOLUME_REG,
        2, 0x1C, 0x6A,
        0
    };
    static const uint8_t readback_regs[] = { 0x00, 0x01, 0x02, 0x0D, 0x0E, 0x14, 0x17, 0x1C };
    uint8_t readback[sizeof(readback_regs)] = { 0 };
    esp_err_t ret;

    ESP_RETURN_ON_ERROR(app_audio_adv_codec_lock(), TAG, "Failed to lock ADV codec");
    ESP_GOTO_ON_FALSE(s_adv_codec.ref_count > 0, ESP_ERR_INVALID_STATE, unlock, TAG, "Codec not acquired");

    ESP_LOGI(TAG, "Recovering Cardputer ADV ES8311 ADC after DAC playback");
    s_adv_codec.adc_enabled = false;
    s_adv_codec.dac_enabled = false;
    s_adv_codec.external_dac_active = false;
    ret = app_audio_adv_codec_apply_power_state_locked();
    if (ret != ESP_OK) {
        goto unlock;
    }

    ret = app_audio_adv_codec_write_reg_locked(0x00, 0x80);
    if (ret != ESP_OK) {
        goto unlock;
    }
    vTaskDelay(pdMS_TO_TICKS(APP_AUDIO_I2S_ADV_CODEC_RESET_DELAY_MS));

    ret = app_audio_adv_codec_write_bulk(common_init_bulk_data);
    if (ret != ESP_OK) {
        goto unlock;
    }
    ret = app_audio_adv_codec_write_bulk(adc_enable_bulk_data);
    if (ret != ESP_OK) {
        goto unlock;
    }
    s_adv_codec.adc_enabled = true;
    ret = app_audio_adv_codec_apply_power_state_locked();
    if (ret != ESP_OK) {
        goto unlock;
    }

    for (size_t i = 0; i < sizeof(readback_regs); i++) {
        ret = app_audio_adv_codec_read_reg_locked(readback_regs[i], &readback[i]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ES8311 ADC recovery readback failed at reg 0x%02X: %s",
                     (unsigned)readback_regs[i], esp_err_to_name(ret));
            goto unlock;
        }
    }
    ESP_LOGI(TAG,
             "ES8311 ADC recovery readback: 00=0x%02X 01=0x%02X 02=0x%02X 0D=0x%02X 0E=0x%02X 14=0x%02X 17=0x%02X 1C=0x%02X",
             readback[0], readback[1], readback[2], readback[3],
             readback[4], readback[5], readback[6], readback[7]);

unlock:
    app_audio_adv_codec_unlock();
    return ret;
}

i2c_master_bus_handle_t app_audio_adv_codec_get_i2c_bus(void)
{
    return s_adv_codec.i2c_bus;
}

void app_audio_adv_codec_set_external_dac_active(bool active)
{
    if (app_audio_adv_codec_lock() != ESP_OK) {
        return;
    }
    s_adv_codec.external_dac_active = active;
    if (s_adv_codec.ref_count > 0 && s_adv_codec.i2c_dev != NULL) {
        (void)app_audio_adv_codec_apply_power_state_locked();
    }
    app_audio_adv_codec_unlock();
}

void app_audio_adv_codec_log_key_regs(const char *label)
{
    static const uint8_t regs[] = {
        0x00, 0x01, 0x02, 0x09, 0x0A, 0x0D, 0x0E, 0x12,
        0x14, 0x15, 0x17, 0x1C, 0x31, 0x32, 0x33, 0x37, 0x44, 0x45
    };
    uint8_t values[sizeof(regs)] = { 0 };

    if (app_audio_adv_codec_lock() != ESP_OK) {
        return;
    }
    if (s_adv_codec.ref_count == 0 || s_adv_codec.i2c_dev == NULL) {
        app_audio_adv_codec_unlock();
        return;
    }
    for (size_t i = 0; i < sizeof(regs); i++) {
        if (app_audio_adv_codec_read_reg_locked(regs[i], &values[i]) != ESP_OK) {
            ESP_LOGW(TAG, "ES8311 %s readback failed at reg 0x%02X",
                     label != NULL ? label : "key",
                     (unsigned)regs[i]);
            app_audio_adv_codec_unlock();
            return;
        }
    }
    ESP_LOGI(TAG,
             "ES8311 %s regs: 00=%02X 01=%02X 02=%02X 09=%02X 0A=%02X 0D=%02X 0E=%02X 12=%02X 14=%02X 15=%02X 17=%02X 1C=%02X 31=%02X 32=%02X 33=%02X 37=%02X 44=%02X 45=%02X",
             label != NULL ? label : "key",
             values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7],
             values[8], values[9], values[10], values[11], values[12], values[13], values[14],
             values[15], values[16], values[17]);
    app_audio_adv_codec_unlock();
}
