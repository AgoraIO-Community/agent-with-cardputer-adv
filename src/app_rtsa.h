#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "app_protocol.h"
#include "app_session.h"

esp_err_t app_rtsa_start(const app_protocol_config_t *config);
esp_err_t app_rtsa_send_audio(const void *data, size_t size, uint32_t pts);
bool app_rtsa_is_audio_send_ready(void);
esp_err_t app_rtsa_set_audio_receive_callbacks(app_session_audio_info_cb_t info_cb,
                                               app_session_audio_frame_cb_t frame_cb,
                                               void *ctx);
