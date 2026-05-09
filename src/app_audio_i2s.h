#pragma once

#include "esp_err.h"

esp_err_t app_audio_i2s_start(void);
esp_err_t app_audio_i2s_stop(void);
esp_err_t app_audio_i2s_pause_for_playback(void);
void app_audio_i2s_mark_post_playback_recovery(void);
