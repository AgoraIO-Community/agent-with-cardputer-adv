#pragma once

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "app_session.h"

esp_err_t app_audio_playback_init(void);
esp_err_t app_audio_playback_stop(void);
esp_err_t app_audio_playback_play_test_tone(uint32_t frequency_hz, uint32_t duration_ms);
esp_err_t app_audio_playback_run_self_test_pattern(void);
void app_audio_playback_handle_stream_info(app_session_audio_codec_t codec, uint32_t sample_rate, uint8_t channels);
void app_audio_playback_handle_audio_frame(app_session_audio_codec_t codec, const void *data, size_t size, uint32_t pts);
bool app_audio_playback_has_received_audio(void);
bool app_audio_playback_is_recently_active(uint32_t window_ms);
i2c_master_bus_handle_t app_audio_playback_get_i2c_bus(void);
