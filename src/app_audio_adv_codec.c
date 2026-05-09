#include "app_audio_adv_codec.h"

#include <string.h>

#include "app_config.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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
        2, 0x32, volume_reg,
        2, 0x33, volume_reg,
        2, 0x37, 0x48,
        2, 0x44, 0x08,
        2, 0x45, 0x08,
        0
    };
    esp_err_t ret = ESP_OK;

    ESP_RETURN_ON_ERROR(app_audio_adv_codec_lock(), TAG, "Failed to lock ADV codec");
    ESP_GOTO_ON_FALSE(s_adv_codec.ref_count > 0, ESP_ERR_INVALID_STATE, unlock, TAG, "Codec not acquired");

    s_adv_codec.dac_enabled = enabled;
    if (enabled) {
        ret = app_audio_adv_codec_write_bulk(dac_enable_bulk_data);
        if (ret != ESP_OK) {
            goto unlock;
        }
    }
    ret = app_audio_adv_codec_apply_power_state_locked();

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
