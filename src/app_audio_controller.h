#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "app_session.h"

typedef enum {
    APP_AUDIO_MODE_IDLE = 0,
    APP_AUDIO_MODE_RECORDING,
    APP_AUDIO_MODE_PLAYBACK,
} app_audio_mode_t;

esp_err_t app_audio_controller_init(void);
esp_err_t app_audio_controller_start(void);
esp_err_t app_audio_controller_stop(void);
void app_audio_controller_handle_remote_stream_info(app_session_audio_codec_t codec,
                                                    uint32_t sample_rate,
                                                    uint8_t channels);
void app_audio_controller_handle_remote_audio_frame(app_session_audio_codec_t codec,
                                                    const void *data,
                                                    size_t size,
                                                    uint32_t pts);
bool app_audio_controller_is_in_playback(void);
app_audio_mode_t app_audio_controller_get_mode(void);
bool app_audio_controller_has_received_remote_audio(void);
