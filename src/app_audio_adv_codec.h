#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

esp_err_t app_audio_adv_codec_acquire(void);
void app_audio_adv_codec_release(void);
esp_err_t app_audio_adv_codec_enable_adc(bool enabled);
esp_err_t app_audio_adv_codec_enable_dac(bool enabled, uint8_t volume_reg);
esp_err_t app_audio_adv_codec_reset_for_adc(void);
esp_err_t app_audio_adv_codec_reset_for_adc_recovery(void);
esp_err_t app_audio_adv_codec_write_bulk(const uint8_t *bulk_data);
i2c_master_bus_handle_t app_audio_adv_codec_get_i2c_bus(void);
void app_audio_adv_codec_set_external_dac_active(bool active);
void app_audio_adv_codec_log_key_regs(const char *label);
