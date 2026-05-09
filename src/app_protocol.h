#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    char app_id[80];
    char token[768];
    char uid[32];
    char channel_name[80];
    char agent_uid[32];
    char agent_id[80];
} app_protocol_config_t;

esp_err_t app_protocol_get_config(app_protocol_config_t *out_config);
esp_err_t app_protocol_start_agent(const app_protocol_config_t *config);
esp_err_t app_protocol_stop_agent(const app_protocol_config_t *config);
