#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef struct {
    char *sdp_answer;
    char *location;
    char *etag;
    int status_code;
} app_whip_response_t;

esp_err_t app_whip_build_url(char **out_url);
esp_err_t app_whip_generate_token(char **out_token);
esp_err_t app_whip_normalize_offer_sdp(const char *input_sdp, char **out_sdp);
esp_err_t app_whip_normalize_answer_sdp(const char *input_sdp, char **out_sdp);
esp_err_t app_whip_post_offer(const char *whip_url,
                              const char *bearer_token,
                              const char *sdp_offer,
                              app_whip_response_t *out);
esp_err_t app_whip_delete_session(const char *location, const char *bearer_token);
void app_whip_response_free(app_whip_response_t *resp);
