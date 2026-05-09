#include "app_whip.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#define APP_WHIP_TIMEOUT_MS 15000

typedef struct {
    char *body;
    size_t body_len;
    char *location;
    char *etag;
} app_whip_http_ctx_t;

static const char *TAG = "app_whip";

static const char *app_whip_audio_rtpmap(void)
{
#if APP_AUDIO_CODEC == APP_AUDIO_CODEC_G711A
    return "a=rtpmap:8 PCMA/8000";
#else
    return "a=rtpmap:111 opus/48000/2";
#endif
}

static const char *app_whip_audio_cname(void)
{
#if APP_AUDIO_CODEC == APP_AUDIO_CODEC_G711A
    return "a=ssrc:6 cname:webrtc-pcma";
#else
    return "a=ssrc:6 cname:webrtc-opus";
#endif
}

static const char *app_whip_audio_mline(void)
{
#if APP_AUDIO_CODEC == APP_AUDIO_CODEC_G711A
    return "m=audio 9 UDP/TLS/RTP/SAVPF 8";
#else
    return "m=audio 9 UDP/TLS/RTP/SAVPF 111";
#endif
}

static const char *app_whip_audio_codec_attrs(void)
{
#if APP_AUDIO_CODEC == APP_AUDIO_CODEC_G711A
    return NULL;
#else
    return NULL;
#endif
}

static const char *app_whip_pull_audio_rtpmap(void)
{
    return "a=rtpmap:8 PCMA/8000";
}

static const char *app_whip_pull_audio_mline(void)
{
    return "m=audio 9 UDP/TLS/RTP/SAVPF 8";
}

static esp_err_t app_whip_extract_line_with_prefix(const char *input_sdp, const char *prefix, char **out_line);
static esp_err_t app_whip_dup_string(char **dst, const char *src, size_t len);
static esp_err_t app_whip_format_alloc(char **out, const char *fmt, ...);

static esp_err_t app_whip_build_pull_answer_mline(const char *input_sdp, char **out_line)
{
    char *input_mline = NULL;
    esp_err_t ret;

    if (input_sdp == NULL || out_line == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = app_whip_extract_line_with_prefix(input_sdp, "m=audio ", &input_mline);
    if (ret != ESP_OK) {
        return app_whip_dup_string(out_line, "m=audio 4702 RTP/SAVPF 8", strlen("m=audio 4702 RTP/SAVPF 8"));
    }

    if (strstr(input_mline, "RTP/SAVPF") != NULL && strstr(input_mline, " 8") == NULL) {
        ret = app_whip_format_alloc(out_line, "%s 8", input_mline);
    } else {
        ret = app_whip_dup_string(out_line, input_mline, strlen(input_mline));
    }

    free(input_mline);
    return ret;
}

static esp_err_t app_whip_normalize_offer_sdp_for_direction(const char *input_sdp, const char *direction, char **out_sdp);
static esp_err_t app_whip_normalize_answer_sdp_for_direction(const char *input_sdp, const char *direction, char **out_sdp);
static esp_err_t app_whip_normalize_pull_answer_passthrough(const char *input_sdp, char **out_sdp);

static bool app_whip_has_placeholder(const char *value)
{
    return value == NULL || value[0] == '\0' || strcmp(value, "CHANGE_ME") == 0;
}

static void app_whip_free_string(char **value)
{
    if (value == NULL || *value == NULL) {
        return;
    }
    free(*value);
    *value = NULL;
}

static esp_err_t app_whip_dup_string(char **dst, const char *src, size_t len)
{
    char *copy;

    if (dst == NULL || src == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    copy = calloc(1, len + 1);
    if (copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, src, len);
    app_whip_free_string(dst);
    *dst = copy;
    return ESP_OK;
}

static esp_err_t app_whip_format_alloc(char **out, const char *fmt, ...)
{
    va_list args;
    va_list args_copy;
    int len;
    char *buf;

    if (out == NULL || fmt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    va_start(args, fmt);
    va_copy(args_copy, args);
    len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (len < 0) {
        va_end(args_copy);
        return ESP_FAIL;
    }

    buf = calloc(1, (size_t)len + 1U);
    if (buf == NULL) {
        va_end(args_copy);
        return ESP_ERR_NO_MEM;
    }

    if (vsnprintf(buf, (size_t)len + 1U, fmt, args_copy) != len) {
        free(buf);
        va_end(args_copy);
        return ESP_FAIL;
    }
    va_end(args_copy);

    *out = buf;
    return ESP_OK;
}

static esp_err_t app_whip_base64url_encode(const unsigned char *input, size_t input_len, char **out)
{
    size_t encoded_len = 0;
    unsigned char *encoded;
    size_t i;
    size_t trimmed_len;
    int rc;

    if (input == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (mbedtls_base64_encode(NULL, 0, &encoded_len, input, input_len) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) {
        return ESP_FAIL;
    }

    encoded = calloc(1, encoded_len + 1U);
    if (encoded == NULL) {
        return ESP_ERR_NO_MEM;
    }

    rc = mbedtls_base64_encode(encoded, encoded_len, &encoded_len, input, input_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to base64 encode, rc=%d", rc);
        goto cleanup;
    }

    for (i = 0; i < encoded_len; i++) {
        if (encoded[i] == '+') {
            encoded[i] = '-';
        } else if (encoded[i] == '/') {
            encoded[i] = '_';
        }
    }

    trimmed_len = encoded_len;
    while (trimmed_len > 0 && encoded[trimmed_len - 1] == '=') {
        trimmed_len--;
    }
    encoded[trimmed_len] = '\0';
    *out = (char *)encoded;
    return ESP_OK;

cleanup:
    free(encoded);
    return ESP_FAIL;
}

esp_err_t app_whip_build_url(char **out_url)
{
    if (out_url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (app_whip_has_placeholder(APP_AGORA_WHIP_SERVER) || app_whip_has_placeholder(APP_AGORA_STREAM_ID) ||
        app_whip_has_placeholder(APP_AGORA_UID)) {
        return ESP_ERR_INVALID_STATE;
    }
    return app_whip_format_alloc(out_url, "https://%s/push/%s?Uid=%s", APP_AGORA_WHIP_SERVER, APP_AGORA_STREAM_ID,
                                 APP_AGORA_UID);
}

esp_err_t app_whip_build_pull_url(char **out_url)
{
    if (out_url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (app_whip_has_placeholder(APP_AGORA_WHIP_SERVER) || app_whip_has_placeholder(APP_AGORA_STREAM_ID) ||
        app_whip_has_placeholder(APP_AGORA_REMOTE_PULL_UID)) {
        return ESP_ERR_INVALID_STATE;
    }
    return app_whip_format_alloc(out_url, "https://%s/pull/%s", APP_AGORA_WHIP_SERVER, APP_AGORA_STREAM_ID);
}

esp_err_t app_whip_generate_token(char **out_token)
{
    static const char *header_json = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    char *header_b64 = NULL;
    char *payload_json = NULL;
    char *payload_b64 = NULL;
    char *signing_input = NULL;
    char *signature_b64 = NULL;
    unsigned char signature[32];
    const mbedtls_md_info_t *md_info;
    unsigned char key_block[64];
    unsigned char inner_digest[32];
    unsigned char ipad[64];
    unsigned char opad[64];
    size_t cert_len;
    long long exp_unix;
    esp_err_t ret;

    if (out_token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (app_whip_has_placeholder(APP_AGORA_APP_ID) || app_whip_has_placeholder(APP_AGORA_APP_CERTIFICATE) ||
        app_whip_has_placeholder(APP_AGORA_STREAM_ID)) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = app_whip_base64url_encode((const unsigned char *)header_json, strlen(header_json), &header_b64);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "Failed to encode JWT header");

    exp_unix = (long long)(time(NULL) + APP_AGORA_WHIP_TOKEN_TTL_SEC);
    if (exp_unix < 1700000000LL) {
        ESP_LOGW(TAG, "System time looks unset (exp=%lld). JWT exp may be rejected by server", exp_unix);
    }

    ret = app_whip_format_alloc(&payload_json,
                                "{\"version\":\"1.0\","
                                "\"appId\":\"%s\","
                                "\"appID\":\"%s\","
                                "\"streamId\":\"%s\","
                                "\"streamID\":\"%s\","
                                "\"exp\":%lld,"
                                "\"action\":\"push\","
                                "\"uid\":\"%s\"}",
                                APP_AGORA_APP_ID, APP_AGORA_APP_ID,
                                APP_AGORA_STREAM_ID, APP_AGORA_STREAM_ID,
                                exp_unix, APP_AGORA_UID);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "Failed to build JWT payload");
    ESP_LOGI(TAG, "JWT payload summary: appId=<masked> streamId=%s exp=%lld uid=%s",
             APP_AGORA_STREAM_ID, exp_unix, APP_AGORA_UID);

    ret = app_whip_base64url_encode((const unsigned char *)payload_json, strlen(payload_json), &payload_b64);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "Failed to encode JWT payload");

    ret = app_whip_format_alloc(&signing_input, "%s.%s", header_b64, payload_b64);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "Failed to build JWT signing input");

    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        ret = ESP_FAIL;
        goto cleanup;
    }
    memset(key_block, 0, sizeof(key_block));
    cert_len = strlen(APP_AGORA_APP_CERTIFICATE);
    if (cert_len > sizeof(key_block)) {
        if (mbedtls_md(md_info, (const unsigned char *)APP_AGORA_APP_CERTIFICATE, cert_len, key_block) != 0) {
            ret = ESP_FAIL;
            goto cleanup;
        }
    } else {
        memcpy(key_block, APP_AGORA_APP_CERTIFICATE, cert_len);
    }
    for (size_t i = 0; i < sizeof(key_block); i++) {
        ipad[i] = (unsigned char)(key_block[i] ^ 0x36U);
        opad[i] = (unsigned char)(key_block[i] ^ 0x5cU);
    }

    {
        unsigned char *inner_input = NULL;
        size_t inner_input_len = sizeof(ipad) + strlen(signing_input);

        inner_input = calloc(1, inner_input_len);
        if (inner_input == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        memcpy(inner_input, ipad, sizeof(ipad));
        memcpy(inner_input + sizeof(ipad), signing_input, strlen(signing_input));
        if (mbedtls_md(md_info, inner_input, inner_input_len, inner_digest) != 0) {
            free(inner_input);
            ret = ESP_FAIL;
            goto cleanup;
        }
        free(inner_input);
    }

    {
        unsigned char outer_input[sizeof(opad) + sizeof(inner_digest)];

        memcpy(outer_input, opad, sizeof(opad));
        memcpy(outer_input + sizeof(opad), inner_digest, sizeof(inner_digest));
        if (mbedtls_md(md_info, outer_input, sizeof(outer_input), signature) != 0) {
            ret = ESP_FAIL;
            goto cleanup;
        }
    }

    if (mbedtls_md_get_size(md_info) != sizeof(signature)) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    ret = app_whip_base64url_encode(signature, sizeof(signature), &signature_b64);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "Failed to encode JWT signature");

    ret = app_whip_format_alloc(out_token, "%s.%s", signing_input, signature_b64);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "Failed to build JWT token");

cleanup:
    free(header_b64);
    free(payload_json);
    free(payload_b64);
    free(signing_input);
    free(signature_b64);
    return ret;
}

esp_err_t app_whip_generate_pull_token(char **out_token)
{
    static const char *header_json = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    char *header_b64 = NULL;
    char *payload_json = NULL;
    char *payload_b64 = NULL;
    char *signing_input = NULL;
    char *signature_b64 = NULL;
    unsigned char signature[32];
    const mbedtls_md_info_t *md_info;
    unsigned char key_block[64];
    unsigned char inner_digest[32];
    unsigned char ipad[64];
    unsigned char opad[64];
    size_t cert_len;
    long long exp_unix;
    esp_err_t ret;

    if (out_token == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (app_whip_has_placeholder(APP_AGORA_APP_ID) || app_whip_has_placeholder(APP_AGORA_APP_CERTIFICATE) ||
        app_whip_has_placeholder(APP_AGORA_STREAM_ID)) {
        return ESP_ERR_INVALID_STATE;
    }

    ret = app_whip_base64url_encode((const unsigned char *)header_json, strlen(header_json), &header_b64);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "Failed to encode pull JWT header");

    exp_unix = (long long)(time(NULL) + APP_AGORA_WHIP_TOKEN_TTL_SEC);
    ret = app_whip_format_alloc(&payload_json,
                                "{\"version\":\"1.0\","
                                "\"appId\":\"%s\","
                                "\"appID\":\"%s\","
                                "\"streamId\":\"%s\","
                                "\"streamID\":\"%s\","
                                "\"exp\":%lld,"
                                "\"action\":\"pull\"}",
                                APP_AGORA_APP_ID, APP_AGORA_APP_ID,
                                APP_AGORA_STREAM_ID, APP_AGORA_STREAM_ID,
                                exp_unix);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "Failed to build pull JWT payload");
    ESP_LOGI(TAG, "Pull JWT payload summary: appId=<masked> streamId=%s exp=%lld uid=<omitted>",
             APP_AGORA_STREAM_ID, exp_unix);

    ret = app_whip_base64url_encode((const unsigned char *)payload_json, strlen(payload_json), &payload_b64);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "Failed to encode pull JWT payload");

    ret = app_whip_format_alloc(&signing_input, "%s.%s", header_b64, payload_b64);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "Failed to build pull JWT signing input");

    md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == NULL) {
        ret = ESP_FAIL;
        goto cleanup;
    }
    memset(key_block, 0, sizeof(key_block));
    cert_len = strlen(APP_AGORA_APP_CERTIFICATE);
    if (cert_len > sizeof(key_block)) {
        if (mbedtls_md(md_info, (const unsigned char *)APP_AGORA_APP_CERTIFICATE, cert_len, key_block) != 0) {
            ret = ESP_FAIL;
            goto cleanup;
        }
    } else {
        memcpy(key_block, APP_AGORA_APP_CERTIFICATE, cert_len);
    }
    for (size_t i = 0; i < sizeof(key_block); i++) {
        ipad[i] = (unsigned char)(key_block[i] ^ 0x36U);
        opad[i] = (unsigned char)(key_block[i] ^ 0x5cU);
    }

    {
        unsigned char *inner_input = NULL;
        size_t inner_input_len = sizeof(ipad) + strlen(signing_input);

        inner_input = calloc(1, inner_input_len);
        if (inner_input == NULL) {
            ret = ESP_ERR_NO_MEM;
            goto cleanup;
        }
        memcpy(inner_input, ipad, sizeof(ipad));
        memcpy(inner_input + sizeof(ipad), signing_input, strlen(signing_input));
        if (mbedtls_md(md_info, inner_input, inner_input_len, inner_digest) != 0) {
            free(inner_input);
            ret = ESP_FAIL;
            goto cleanup;
        }
        free(inner_input);
    }

    {
        unsigned char outer_input[sizeof(opad) + sizeof(inner_digest)];

        memcpy(outer_input, opad, sizeof(opad));
        memcpy(outer_input + sizeof(opad), inner_digest, sizeof(inner_digest));
        if (mbedtls_md(md_info, outer_input, sizeof(outer_input), signature) != 0) {
            ret = ESP_FAIL;
            goto cleanup;
        }
    }

    ret = app_whip_base64url_encode(signature, sizeof(signature), &signature_b64);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "Failed to encode pull JWT signature");

    ret = app_whip_format_alloc(out_token, "%s.%s", signing_input, signature_b64);
    ESP_GOTO_ON_ERROR(ret, cleanup, TAG, "Failed to build pull JWT token");

cleanup:
    free(header_b64);
    free(payload_json);
    free(payload_b64);
    free(signing_input);
    free(signature_b64);
    return ret;
}

static esp_err_t app_whip_normalize_offer_sdp_for_direction(const char *input_sdp, const char *direction, char **out_sdp)
{
    const char *cursor;
    char *normalized = NULL;
    size_t capacity = 0;
    size_t length = 0;
    bool sendonly_emitted = false;
    bool audio_rtpmap_emitted = false;
    bool audio_codec_attrs_emitted = false;
    bool pull_offer = strcmp(direction, "a=recvonly") == 0;

    if (input_sdp == NULL || out_sdp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cursor = input_sdp;
    while (*cursor != '\0') {
        const char *line_end = strstr(cursor, "\n");
        size_t line_len = line_end ? (size_t)(line_end - cursor) : strlen(cursor);
        const char *line = cursor;

        if (line_len > 0 && line[line_len - 1] == '\r') {
            line_len--;
        }

        if (line_len >= strlen("m=audio 9 UDP/TLS/RTP/SAVP") &&
            strncmp(line, "m=audio 9 UDP/TLS/RTP/SAVP", strlen("m=audio 9 UDP/TLS/RTP/SAVP")) == 0) {
            line = pull_offer ? app_whip_pull_audio_mline() : app_whip_audio_mline();
            line_len = strlen(line);
        } else if (line_len >= strlen("a=rtpmap:111 ") &&
                   strncmp(line, "a=rtpmap:111 ", strlen("a=rtpmap:111 ")) == 0) {
            if (audio_rtpmap_emitted) {
                cursor = line_end ? line_end + 1 : cursor + line_len;
                continue;
            }
            line = pull_offer ? app_whip_pull_audio_rtpmap() : app_whip_audio_rtpmap();
            line_len = strlen(line);
            audio_rtpmap_emitted = true;
        } else if (pull_offer &&
                   ((line_len >= strlen("a=ssrc:") && strncmp(line, "a=ssrc:", strlen("a=ssrc:")) == 0) ||
                    (line_len >= strlen("a=msid:") && strncmp(line, "a=msid:", strlen("a=msid:")) == 0))) {
            cursor = line_end ? line_end + 1 : cursor + line_len;
            continue;
        } else if (line_len >= strlen("a=fmtp:111 ") &&
                   strncmp(line, "a=fmtp:111 ", strlen("a=fmtp:111 ")) == 0) {
            cursor = line_end ? line_end + 1 : cursor + line_len;
            continue;
        } else if (line_len >= strlen("a=rtcp-fb:111 ") &&
                   strncmp(line, "a=rtcp-fb:111 ", strlen("a=rtcp-fb:111 ")) == 0) {
            cursor = line_end ? line_end + 1 : cursor + line_len;
            continue;
        } else if (line_len >= strlen("a=maxptime:") &&
                   strncmp(line, "a=maxptime:", strlen("a=maxptime:")) == 0) {
            cursor = line_end ? line_end + 1 : cursor + line_len;
            continue;
        } else if ((line_len == strlen("a=sendrecv") && strncmp(line, "a=sendrecv", line_len) == 0) ||
                   (line_len == strlen("a=recvonly") && strncmp(line, "a=recvonly", line_len) == 0) ||
                   (line_len == strlen("a=inactive") && strncmp(line, "a=inactive", line_len) == 0)) {
            if (sendonly_emitted) {
                cursor = line_end ? line_end + 1 : cursor + line_len;
                continue;
            }
            line = direction;
            line_len = strlen(line);
            sendonly_emitted = true;
        } else if (line_len == strlen("a=sendonly") && strncmp(line, "a=sendonly", line_len) == 0) {
            if (sendonly_emitted) {
                cursor = line_end ? line_end + 1 : cursor + line_len;
                continue;
            }
            line = direction;
            line_len = strlen(line);
            sendonly_emitted = true;
        }

        if (length + line_len + 2 >= capacity) {
            size_t next_capacity = capacity == 0 ? 1024 : capacity * 2;
            char *next = realloc(normalized, next_capacity);
            if (next == NULL) {
                free(normalized);
                return ESP_ERR_NO_MEM;
            }
            normalized = next;
            capacity = next_capacity;
        }

        memcpy(normalized + length, line, line_len);
        length += line_len;
        normalized[length++] = '\n';

        if (!pull_offer &&
            audio_rtpmap_emitted && !audio_codec_attrs_emitted && app_whip_audio_codec_attrs() != NULL &&
            strcmp(line, app_whip_audio_rtpmap()) == 0) {
            const char *attrs = app_whip_audio_codec_attrs();
            size_t attrs_len = strlen(attrs);

            if (length + attrs_len + 2 >= capacity) {
                size_t next_capacity = capacity == 0 ? 1024 : capacity * 2;
                while (length + attrs_len + 2 >= next_capacity) {
                    next_capacity *= 2;
                }
                char *next = realloc(normalized, next_capacity);
                if (next == NULL) {
                    free(normalized);
                    return ESP_ERR_NO_MEM;
                }
                normalized = next;
                capacity = next_capacity;
            }

            memcpy(normalized + length, attrs, attrs_len);
            length += attrs_len;
            normalized[length++] = '\n';
            audio_codec_attrs_emitted = true;
        }

        cursor = line_end ? line_end + 1 : cursor + line_len;
    }

    if (normalized == NULL) {
        return ESP_FAIL;
    }
    normalized[length] = '\0';
    *out_sdp = normalized;
    return ESP_OK;
}

esp_err_t app_whip_normalize_push_offer_sdp(const char *input_sdp, char **out_sdp)
{
    return app_whip_normalize_offer_sdp_for_direction(input_sdp, "a=sendonly", out_sdp);
}

esp_err_t app_whip_normalize_pull_offer_sdp(const char *input_sdp, char **out_sdp)
{
    return app_whip_normalize_offer_sdp_for_direction(input_sdp, "a=recvonly", out_sdp);
}

static esp_err_t app_whip_sdp_append(char **buffer, size_t *capacity, size_t *length,
                                     const char *line, size_t line_len)
{
    if (buffer == NULL || capacity == NULL || length == NULL || line == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (*length + line_len + 3 >= *capacity) {
        size_t next_capacity = *capacity == 0 ? 2048 : *capacity;
        while (*length + line_len + 3 >= next_capacity) {
            next_capacity *= 2;
        }
        char *next = realloc(*buffer, next_capacity);
        if (next == NULL) {
            return ESP_ERR_NO_MEM;
        }
        *buffer = next;
        *capacity = next_capacity;
    }

    memcpy(*buffer + *length, line, line_len);
    *length += line_len;
    (*buffer)[(*length)++] = '\r';
    (*buffer)[(*length)++] = '\n';
    return ESP_OK;
}

static esp_err_t app_whip_extract_line_value(const char *input_sdp, const char *prefix, char **out_value)
{
    const char *start;
    const char *end;
    size_t len;

    if (input_sdp == NULL || prefix == NULL || out_value == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    start = strstr(input_sdp, prefix);
    if (start == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    start += strlen(prefix);
    end = strpbrk(start, "\r\n");
    len = end ? (size_t)(end - start) : strlen(start);
    return app_whip_dup_string(out_value, start, len);
}

static esp_err_t app_whip_extract_line_with_prefix(const char *input_sdp, const char *prefix, char **out_line)
{
    const char *start;
    const char *end;
    size_t len;

    if (input_sdp == NULL || prefix == NULL || out_line == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    start = strstr(input_sdp, prefix);
    if (start == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    end = strpbrk(start, "\r\n");
    len = end ? (size_t)(end - start) : strlen(start);
    return app_whip_dup_string(out_line, start, len);
}

static esp_err_t app_whip_append_lines_with_prefix(const char *input_sdp,
                                                   const char *prefix,
                                                   char **buffer,
                                                   size_t *capacity,
                                                   size_t *length)
{
    const char *cursor;
    size_t prefix_len;
    esp_err_t ret = ESP_OK;

    if (input_sdp == NULL || prefix == NULL || buffer == NULL || capacity == NULL || length == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cursor = input_sdp;
    prefix_len = strlen(prefix);
    while (*cursor != '\0') {
        const char *line_end = strstr(cursor, "\n");
        size_t line_len = line_end ? (size_t)(line_end - cursor) : strlen(cursor);

        if (line_len > 0 && cursor[line_len - 1] == '\r') {
            line_len--;
        }
        if (line_len >= prefix_len && strncmp(cursor, prefix, prefix_len) == 0) {
            ret = app_whip_sdp_append(buffer, capacity, length, cursor, line_len);
            if (ret != ESP_OK) {
                return ret;
            }
        }
        cursor = line_end ? line_end + 1 : cursor + line_len;
    }
    return ESP_OK;
}

static esp_err_t app_whip_normalize_candidate_value(const char *candidate_value, char **out_candidate)
{
    char foundation[64];
    char transport[16];
    char ip[64];
    char cand_type[32];
    unsigned long priority = 0;
    int component = 0;
    int port = 0;
    int matched;

    if (candidate_value == NULL || out_candidate == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    matched = sscanf(candidate_value, "%63s %d %15s %lu %63s %d typ %31s",
                     foundation, &component, transport, &priority, ip, &port, cand_type);
    if (matched < 7) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return app_whip_format_alloc(out_candidate, "0 %d UDP %lu %s %d typ %s",
                                 component, priority, ip, port, cand_type);
}

static esp_err_t app_whip_normalize_answer_sdp_for_direction(const char *input_sdp, const char *direction, char **out_sdp)
{
    char *normalized = NULL;
    size_t capacity = 0;
    size_t length = 0;
    char *group_bundle = NULL;
    char *msid_semantic = NULL;
    char *remote_media_msid = NULL;
    char *ice_ufrag = NULL;
    char *ice_pwd = NULL;
    char *fingerprint = NULL;
    char *candidate1 = NULL;
    char *candidate2 = NULL;
    char *normalized_candidate1 = NULL;
    char *pull_mline = NULL;
    char *rtcp_line = NULL;
    char *extmap_line = NULL;
    char *maxptime_line = NULL;
    esp_err_t ret;
    bool pull_answer = strcmp(direction, "a=sendonly") == 0;

    if (input_sdp == NULL || out_sdp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = app_whip_extract_line_value(input_sdp, "a=group:BUNDLE ", &group_bundle);
    if (ret == ESP_ERR_NOT_FOUND) {
        ret = app_whip_dup_string(&group_bundle, "0", 1);
    }
    if (ret != ESP_OK) {
        goto cleanup;
    }
    (void)app_whip_extract_line_value(input_sdp, "a=msid-semantic: ", &msid_semantic);
    (void)app_whip_extract_line_value(input_sdp, "a=msid:", &remote_media_msid);
    ESP_GOTO_ON_ERROR(app_whip_extract_line_value(input_sdp, "a=ice-ufrag:", &ice_ufrag), cleanup, TAG,
                      "Missing WHIP answer ice-ufrag");
    ESP_GOTO_ON_ERROR(app_whip_extract_line_value(input_sdp, "a=ice-pwd:", &ice_pwd), cleanup, TAG,
                      "Missing WHIP answer ice-pwd");
    ESP_GOTO_ON_ERROR(app_whip_extract_line_value(input_sdp, "a=fingerprint:sha-256 ", &fingerprint), cleanup, TAG,
                      "Missing WHIP answer fingerprint");
    (void)app_whip_extract_line_with_prefix(input_sdp, "a=rtcp:", &rtcp_line);
    (void)app_whip_extract_line_with_prefix(input_sdp, "a=extmap:", &extmap_line);
    (void)app_whip_extract_line_with_prefix(input_sdp, "a=maxptime:", &maxptime_line);

    (void)app_whip_extract_line_value(input_sdp, "a=candidate:", &candidate1);
    if (candidate1 != NULL) {
        const char *next = strstr(input_sdp, candidate1);
        if (next != NULL) {
            (void)app_whip_extract_line_value(next + strlen(candidate1), "a=candidate:", &candidate2);
        }
    }
    if (candidate1 != NULL) {
        ESP_GOTO_ON_ERROR(app_whip_normalize_candidate_value(candidate1, &normalized_candidate1), cleanup, TAG,
                          "Failed to normalize WHIP candidate");
    }
    ret = app_whip_sdp_append(&normalized, &capacity, &length, "v=0", strlen("v=0"));
    if (ret != ESP_OK) {
        goto cleanup;
    }
    ret = app_whip_sdp_append(&normalized, &capacity, &length, "o=- 0 0 IN IP4 127.0.0.1",
                              strlen("o=- 0 0 IN IP4 127.0.0.1"));
    if (ret != ESP_OK) {
        goto cleanup;
    }
    ret = app_whip_sdp_append(&normalized, &capacity, &length, "s=AgoraGateway", strlen("s=AgoraGateway"));
    if (ret != ESP_OK) {
        goto cleanup;
    }
    ret = app_whip_sdp_append(&normalized, &capacity, &length, "t=0 0", strlen("t=0 0"));
    if (ret != ESP_OK) {
        goto cleanup;
    }
    {
        char *line = NULL;
        ret = app_whip_format_alloc(&line, "a=group:BUNDLE %s", group_bundle);
        if (ret != ESP_OK) {
            goto cleanup;
        }
        ret = app_whip_sdp_append(&normalized, &capacity, &length, line, strlen(line));
        free(line);
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }
    if (pull_answer) {
        if (msid_semantic != NULL) {
            char *line = NULL;
            ret = app_whip_format_alloc(&line, "a=msid-semantic: %s", msid_semantic);
            if (ret != ESP_OK) {
                goto cleanup;
            }
            ret = app_whip_sdp_append(&normalized, &capacity, &length, line, strlen(line));
            free(line);
            if (ret != ESP_OK) {
                goto cleanup;
            }
        }
    } else {
        ret = app_whip_sdp_append(&normalized, &capacity, &length, "a=msid-semantic: esp-webrtc",
                                  strlen("a=msid-semantic: esp-webrtc"));
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }
    if (pull_answer) {
        ESP_GOTO_ON_ERROR(app_whip_build_pull_answer_mline(input_sdp, &pull_mline), cleanup, TAG,
                          "Failed to build pull-answer m-line");
        ret = app_whip_sdp_append(&normalized, &capacity, &length, pull_mline, strlen(pull_mline));
        if (ret != ESP_OK) {
            goto cleanup;
        }
    } else {
        ret = app_whip_sdp_append(&normalized, &capacity, &length,
                                  app_whip_audio_mline(), strlen(app_whip_audio_mline()));
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }
    if (pull_answer && rtcp_line != NULL) {
        ret = app_whip_sdp_append(&normalized, &capacity, &length, rtcp_line, strlen(rtcp_line));
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }
    ret = app_whip_sdp_append(&normalized, &capacity, &length,
                              app_whip_audio_rtpmap(), strlen(app_whip_audio_rtpmap()));
    if (ret != ESP_OK) {
        goto cleanup;
    }
    if (app_whip_audio_codec_attrs() != NULL) {
        ret = app_whip_sdp_append(&normalized, &capacity, &length,
                                  app_whip_audio_codec_attrs(), strlen(app_whip_audio_codec_attrs()));
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }
    if (pull_answer) {
        ret = app_whip_append_lines_with_prefix(input_sdp, "a=ssrc:", &normalized, &capacity, &length);
        if (ret != ESP_OK) {
            goto cleanup;
        }
    } else {
        ret = app_whip_sdp_append(&normalized, &capacity, &length,
                                  app_whip_audio_cname(), strlen(app_whip_audio_cname()));
        if (ret != ESP_OK) {
            goto cleanup;
        }
        ret = app_whip_sdp_append(&normalized, &capacity, &length, "a=ssrc:6 msid:esp_stream a0",
                                  strlen("a=ssrc:6 msid:esp_stream a0"));
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }
    ret = app_whip_sdp_append(&normalized, &capacity, &length, "a=mid:0", strlen("a=mid:0"));
    if (ret != ESP_OK) {
        goto cleanup;
    }
    if (pull_answer) {
        if (remote_media_msid != NULL) {
            char *line = NULL;
            ret = app_whip_format_alloc(&line, "a=msid:%s", remote_media_msid);
            if (ret != ESP_OK) {
                goto cleanup;
            }
            ret = app_whip_sdp_append(&normalized, &capacity, &length, line, strlen(line));
            free(line);
            if (ret != ESP_OK) {
                goto cleanup;
            }
        }
    } else {
        ret = app_whip_sdp_append(&normalized, &capacity, &length, "a=msid:esp_stream a0",
                                  strlen("a=msid:esp_stream a0"));
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }
    ret = app_whip_sdp_append(&normalized, &capacity, &length, "c=IN IP4 0.0.0.0",
                              strlen("c=IN IP4 0.0.0.0"));
    if (ret != ESP_OK) {
        goto cleanup;
    }
    ret = app_whip_sdp_append(&normalized, &capacity, &length, "a=rtcp-mux", strlen("a=rtcp-mux"));
    if (ret != ESP_OK) {
        goto cleanup;
    }
    if (pull_answer && extmap_line != NULL) {
        ret = app_whip_sdp_append(&normalized, &capacity, &length, extmap_line, strlen(extmap_line));
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }
    if (pull_answer && maxptime_line != NULL) {
        ret = app_whip_sdp_append(&normalized, &capacity, &length, maxptime_line, strlen(maxptime_line));
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }
    ret = app_whip_sdp_append(&normalized, &capacity, &length, direction, strlen(direction));
    if (ret != ESP_OK) {
        goto cleanup;
    }
    {
        char *line = NULL;
        ret = app_whip_format_alloc(&line, "a=fingerprint:sha-256 %s", fingerprint);
        if (ret != ESP_OK) {
            goto cleanup;
        }
        ret = app_whip_sdp_append(&normalized, &capacity, &length, line, strlen(line));
        free(line);
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }
    ret = app_whip_sdp_append(&normalized, &capacity, &length, "a=setup:active", strlen("a=setup:active"));
    if (ret != ESP_OK) {
        goto cleanup;
    }
    {
        char *line = NULL;
        ret = app_whip_format_alloc(&line, "a=ice-ufrag:%s", ice_ufrag);
        if (ret != ESP_OK) {
            goto cleanup;
        }
        ret = app_whip_sdp_append(&normalized, &capacity, &length, line, strlen(line));
        free(line);
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }
    {
        char *line = NULL;
        ret = app_whip_format_alloc(&line, "a=ice-pwd:%s", ice_pwd);
        if (ret != ESP_OK) {
            goto cleanup;
        }
        ret = app_whip_sdp_append(&normalized, &capacity, &length, line, strlen(line));
        free(line);
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }
    if (normalized_candidate1 != NULL) {
        char *line = NULL;
        ret = app_whip_format_alloc(&line, "a=candidate:%s", normalized_candidate1);
        if (ret != ESP_OK) {
            goto cleanup;
        }
        ret = app_whip_sdp_append(&normalized, &capacity, &length, line, strlen(line));
        free(line);
        if (ret != ESP_OK) {
            goto cleanup;
        }
    }

    normalized[length] = '\0';
    *out_sdp = normalized;
    normalized = NULL;
    ret = ESP_OK;

cleanup:
    free(normalized);
    free(group_bundle);
    free(msid_semantic);
    free(remote_media_msid);
    free(ice_ufrag);
    free(ice_pwd);
    free(fingerprint);
    free(candidate1);
    free(candidate2);
    free(normalized_candidate1);
    free(pull_mline);
    free(rtcp_line);
    free(extmap_line);
    free(maxptime_line);
    return ret;
}

static esp_err_t app_whip_normalize_pull_answer_passthrough(const char *input_sdp, char **out_sdp)
{
    const char *cursor;
    char *normalized = NULL;
    size_t capacity = 0;
    size_t length = 0;
    bool has_rtpmap = false;
    bool inserted_rtpmap = false;
    esp_err_t ret;

    if (input_sdp == NULL || out_sdp == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (strstr(input_sdp, "a=rtpmap:8 PCMA/8000") != NULL) {
        has_rtpmap = true;
    }

    cursor = input_sdp;
    while (*cursor != '\0') {
        const char *line_end = strstr(cursor, "\n");
        size_t line_len = line_end ? (size_t)(line_end - cursor) : strlen(cursor);
        const char *line = cursor;

        if (line_len > 0 && line[line_len - 1] == '\r') {
            line_len--;
        }

        if (line_len == 0) {
            cursor = line_end ? line_end + 1 : cursor + line_len;
            continue;
        }

        ret = app_whip_sdp_append(&normalized, &capacity, &length, line, line_len);
        if (ret != ESP_OK) {
            free(normalized);
            return ret;
        }

        if (!has_rtpmap &&
            line_len >= strlen("m=audio ") &&
            strncmp(line, "m=audio ", strlen("m=audio ")) == 0 &&
            strstr(line, " 8") != NULL &&
            !inserted_rtpmap) {
            ret = app_whip_sdp_append(&normalized, &capacity, &length,
                                      "a=rtpmap:8 PCMA/8000", strlen("a=rtpmap:8 PCMA/8000"));
            if (ret != ESP_OK) {
                free(normalized);
                return ret;
            }
            inserted_rtpmap = true;
        }

        cursor = line_end ? line_end + 1 : cursor + line_len;
    }

    if (normalized == NULL) {
        return ESP_FAIL;
    }
    normalized[length] = '\0';
    *out_sdp = normalized;
    return ESP_OK;
}

esp_err_t app_whip_normalize_push_answer_sdp(const char *input_sdp, char **out_sdp)
{
    return app_whip_normalize_answer_sdp_for_direction(input_sdp, "a=recvonly", out_sdp);
}

esp_err_t app_whip_normalize_pull_answer_sdp(const char *input_sdp, char **out_sdp)
{
    return app_whip_normalize_pull_answer_passthrough(input_sdp, out_sdp);
}

static esp_err_t app_whip_append_body(app_whip_http_ctx_t *ctx, const char *data, int len)
{
    char *next;

    if (ctx == NULL || data == NULL || len <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    next = realloc(ctx->body, ctx->body_len + (size_t)len + 1);
    if (next == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->body = next;
    memcpy(ctx->body + ctx->body_len, data, (size_t)len);
    ctx->body_len += (size_t)len;
    ctx->body[ctx->body_len] = '\0';
    return ESP_OK;
}

static esp_err_t app_whip_event_handler(esp_http_client_event_t *event)
{
    app_whip_http_ctx_t *ctx = (app_whip_http_ctx_t *)event->user_data;

    if (ctx == NULL) {
        return ESP_OK;
    }

    switch (event->event_id) {
    case HTTP_EVENT_ON_HEADER:
        if (event->header_key != NULL && event->header_value != NULL) {
            if (strcasecmp(event->header_key, "Location") == 0) {
                ESP_RETURN_ON_ERROR(app_whip_dup_string(&ctx->location, event->header_value,
                                                        strlen(event->header_value)),
                                    TAG, "Failed to store Location header");
            } else if (strcasecmp(event->header_key, "ETag") == 0) {
                ESP_RETURN_ON_ERROR(app_whip_dup_string(&ctx->etag, event->header_value,
                                                        strlen(event->header_value)),
                                    TAG, "Failed to store ETag header");
            }
        }
        break;
    case HTTP_EVENT_ON_DATA:
        if (event->data != NULL && event->data_len > 0) {
            ESP_RETURN_ON_ERROR(app_whip_append_body(ctx, (const char *)event->data, event->data_len),
                                TAG, "Failed to append WHIP response body");
        }
        break;
    default:
        break;
    }

    return ESP_OK;
}

static esp_err_t app_whip_set_auth_header(esp_http_client_handle_t client, const char *bearer_token)
{
    char *header;
    int written;

    if (client == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bearer_token == NULL || bearer_token[0] == '\0' || strcmp(bearer_token, "CHANGE_ME") == 0) {
        return ESP_OK;
    }

    header = calloc(1, strlen("Bearer ") + strlen(bearer_token) + 1);
    if (header == NULL) {
        return ESP_ERR_NO_MEM;
    }
    written = snprintf(header, strlen("Bearer ") + strlen(bearer_token) + 1, "Bearer %s", bearer_token);
    if (written <= 0) {
        free(header);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", header);
    free(header);
    return ESP_OK;
}

static esp_err_t app_whip_perform_request(esp_http_client_method_t method,
                                          const char *url,
                                          const char *bearer_token,
                                          const char *content_type,
                                          const char *body,
                                          app_whip_http_ctx_t *http_ctx,
                                          int *status_code)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = method,
        .event_handler = app_whip_event_handler,
        .user_data = http_ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = APP_WHIP_TIMEOUT_MS,
    };
    esp_http_client_handle_t client;
    esp_err_t ret;

    if (url == NULL || http_ctx == NULL || status_code == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (content_type != NULL) {
        esp_http_client_set_header(client, "Content-Type", content_type);
    }
    ESP_GOTO_ON_ERROR(app_whip_set_auth_header(client, bearer_token), cleanup, TAG, "Failed to set auth header");

    if (body != NULL) {
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    ret = esp_http_client_perform(client);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(ret));
        esp_http_client_cleanup(client);
        return ret;
    }

    *status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return ESP_OK;

cleanup:
    esp_http_client_cleanup(client);
    return ret;
}

esp_err_t app_whip_post_offer(const char *whip_url,
                              const char *bearer_token,
                              const char *sdp_offer,
                              app_whip_response_t *out)
{
    app_whip_http_ctx_t http_ctx = { 0 };
    esp_err_t err;

    if (whip_url == NULL || sdp_offer == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    ESP_LOGI(TAG, "WHIP POST %s", whip_url);

    err = app_whip_perform_request(HTTP_METHOD_POST, whip_url, bearer_token, "application/sdp", sdp_offer,
                                   &http_ctx, &out->status_code);
    if (err != ESP_OK) {
        app_whip_free_string(&http_ctx.body);
        app_whip_free_string(&http_ctx.location);
        app_whip_free_string(&http_ctx.etag);
        return err;
    }

    out->sdp_answer = http_ctx.body;
    out->location = http_ctx.location;
    out->etag = http_ctx.etag;

    ESP_LOGI(TAG, "WHIP POST status: %d", out->status_code);
    ESP_LOGI(TAG, "WHIP Location: %s", out->location ? out->location : "<missing>");
    ESP_LOGI(TAG, "WHIP ETag: %s", out->etag ? out->etag : "<missing>");
    ESP_LOGI(TAG, "WHIP SDP answer bytes: %u", out->sdp_answer ? (unsigned)strlen(out->sdp_answer) : 0U);
    if (out->sdp_answer != NULL && out->sdp_answer[0] != '\0') {
        ESP_LOGI(TAG, "WHIP response body:\n%s", out->sdp_answer);
    }

    if (out->status_code != 201) {
        ESP_LOGE(TAG, "Unexpected WHIP status: %d", out->status_code);
        return ESP_FAIL;
    }
    if (out->sdp_answer == NULL || out->sdp_answer[0] == '\0') {
        ESP_LOGE(TAG, "WHIP response did not contain SDP answer");
        return ESP_FAIL;
    }
    if (out->location == NULL || out->location[0] == '\0') {
        ESP_LOGE(TAG, "WHIP response did not contain Location header");
        return ESP_FAIL;
    }
    return ESP_OK;
}


esp_err_t app_whip_delete_session(const char *location, const char *bearer_token)
{
    app_whip_http_ctx_t http_ctx = { 0 };
    int status_code = 0;
    esp_err_t err;

    if (location == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "WHIP DELETE %s", location);
    err = app_whip_perform_request(HTTP_METHOD_DELETE, location, bearer_token, NULL, NULL, &http_ctx, &status_code);

    app_whip_free_string(&http_ctx.body);
    app_whip_free_string(&http_ctx.location);
    app_whip_free_string(&http_ctx.etag);

    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "WHIP DELETE status: %d", status_code);
    if (status_code < 200 || status_code >= 300) {
        ESP_LOGE(TAG, "Unexpected WHIP DELETE status: %d", status_code);
        return ESP_FAIL;
    }
    return ESP_OK;
}

void app_whip_response_free(app_whip_response_t *resp)
{
    if (resp == NULL) {
        return;
    }
    app_whip_free_string(&resp->sdp_answer);
    app_whip_free_string(&resp->location);
    app_whip_free_string(&resp->etag);
    resp->status_code = 0;
}
