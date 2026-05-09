#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    APP_SESSION_AUDIO_CODEC_PCM16 = 0,
    APP_SESSION_AUDIO_CODEC_G711A,
    APP_SESSION_AUDIO_CODEC_G722,
    APP_SESSION_AUDIO_CODEC_OPUS,
} app_session_audio_codec_t;

typedef void (*app_session_audio_info_cb_t)(app_session_audio_codec_t codec,
                                            uint32_t sample_rate,
                                            uint8_t channels,
                                            void *ctx);

typedef void (*app_session_audio_frame_cb_t)(app_session_audio_codec_t codec,
                                             const void *data,
                                             size_t size,
                                             uint32_t pts,
                                             void *ctx);

esp_err_t app_session_start(void);
esp_err_t app_session_send_audio(const void *data, size_t size, uint32_t pts);
bool app_session_is_audio_send_ready(void);
esp_err_t app_session_set_audio_receive_callbacks(app_session_audio_info_cb_t info_cb,
                                                  app_session_audio_frame_cb_t frame_cb,
                                                  void *ctx);
