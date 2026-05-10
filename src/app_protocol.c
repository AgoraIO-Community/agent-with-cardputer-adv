#include "app_protocol.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"

#define APP_PROTOCOL_TIMEOUT_MS 15000
#define APP_PROTOCOL_RX_BUFFER_SIZE 2048

typedef enum {
    APP_PROTOCOL_HEADER_PROFILE_FULL = 0,
    APP_PROTOCOL_HEADER_PROFILE_NO_ENCODING,
    APP_PROTOCOL_HEADER_PROFILE_NO_NGROK_HEADER,
    APP_PROTOCOL_HEADER_PROFILE_MINIMAL,
} app_protocol_header_profile_t;

typedef struct {
    esp_tls_proto_ver_t tls_version;
    app_protocol_header_profile_t header_profile;
    const char *label;
} app_protocol_attempt_t;

typedef struct {
    char *body;
    size_t body_len;
} app_protocol_http_ctx_t;

static const char *TAG = "app_protocol";

static const app_protocol_attempt_t APP_PROTOCOL_GET_ATTEMPTS[] = {
    { ESP_TLS_VER_TLS_1_3, APP_PROTOCOL_HEADER_PROFILE_FULL, "tls1.3/full" },
    { ESP_TLS_VER_TLS_1_2, APP_PROTOCOL_HEADER_PROFILE_FULL, "tls1.2/full" },
    { ESP_TLS_VER_TLS_1_2, APP_PROTOCOL_HEADER_PROFILE_NO_ENCODING, "tls1.2/no-encoding" },
    { ESP_TLS_VER_TLS_1_2, APP_PROTOCOL_HEADER_PROFILE_NO_NGROK_HEADER, "tls1.2/no-ngrok-header" },
    { ESP_TLS_VER_TLS_1_2, APP_PROTOCOL_HEADER_PROFILE_MINIMAL, "tls1.2/minimal" },
};

static bool app_protocol_has_placeholder(const char *value)
{
    return value == NULL || value[0] == '\0' || strcmp(value, "CHANGE_ME") == 0 || strstr(value, "CHANGE_ME") != NULL;
}

static esp_err_t app_protocol_dup_format(char **out, const char *fmt, ...)
{
    va_list args;
    va_list copy;
    int len;
    char *buf;

    if (out == NULL || fmt == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    va_start(args, fmt);
    va_copy(copy, args);
    len = vsnprintf(NULL, 0, fmt, args);
    va_end(args);
    if (len < 0) {
        va_end(copy);
        return ESP_FAIL;
    }

    buf = calloc(1, (size_t)len + 1U);
    if (buf == NULL) {
        va_end(copy);
        return ESP_ERR_NO_MEM;
    }
    vsnprintf(buf, (size_t)len + 1U, fmt, copy);
    va_end(copy);
    *out = buf;
    return ESP_OK;
}

static esp_err_t app_protocol_append_body(app_protocol_http_ctx_t *ctx, const char *data, size_t len)
{
    char *next;

    if (ctx == NULL || data == NULL || len == 0) {
        return ESP_OK;
    }

    next = realloc(ctx->body, ctx->body_len + len + 1U);
    if (next == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ctx->body = next;
    memcpy(ctx->body + ctx->body_len, data, len);
    ctx->body_len += len;
    ctx->body[ctx->body_len] = '\0';
    return ESP_OK;
}

static esp_err_t app_protocol_parse_base_url(char *scheme, size_t scheme_size,
                                             char *host, size_t host_size,
                                             int *port, char *base_path, size_t base_path_size)
{
    const char *url = APP_PROTOCOL_BASE_URL;
    const char *scheme_end;
    const char *host_start;
    const char *host_end;
    const char *path_start;
    size_t scheme_len;
    size_t host_len;
    size_t path_len;
    const char *port_sep;

    if (scheme == NULL || host == NULL || port == NULL || base_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    scheme_end = strstr(url, "://");
    if (scheme_end == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    scheme_len = (size_t)(scheme_end - url);
    if (scheme_len + 1U > scheme_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(scheme, url, scheme_len);
    scheme[scheme_len] = '\0';

    host_start = scheme_end + 3;
    path_start = strchr(host_start, '/');
    host_end = path_start ? path_start : (host_start + strlen(host_start));
    port_sep = memchr(host_start, ':', (size_t)(host_end - host_start));
    if (port_sep != NULL) {
        host_len = (size_t)(port_sep - host_start);
        *port = atoi(port_sep + 1);
    } else {
        host_len = (size_t)(host_end - host_start);
        *port = strcmp(scheme, "https") == 0 ? 443 : 80;
    }
    if (host_len == 0 || host_len + 1U > host_size) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    if (path_start != NULL) {
        path_len = strlen(path_start);
        if (path_len + 1U > base_path_size) {
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(base_path, path_start, path_len);
        base_path[path_len] = '\0';
    } else {
        if (base_path_size < 2U) {
            return ESP_ERR_INVALID_SIZE;
        }
        strcpy(base_path, "/");
    }
    return ESP_OK;
}

static esp_err_t app_protocol_build_request_path(char **out_path, const char *path, const char *query)
{
    if (path == NULL || out_path == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (query != NULL && query[0] != '\0') {
        return app_protocol_dup_format(out_path, "%s?%s", path, query);
    }
    return app_protocol_dup_format(out_path, "%s", path);
}

static esp_err_t app_protocol_parse_status_code(const char *header_block, int *out_status_code)
{
    const char *line_end;
    const char *space;
    char code_buf[4] = { 0 };

    if (header_block == NULL || out_status_code == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    line_end = strstr(header_block, "\r\n");
    if (line_end == NULL) {
        return ESP_FAIL;
    }
    if (strncmp(header_block, "HTTP/1.", 7) != 0) {
        ESP_LOGE(TAG, "Unexpected HTTP status line prefix");
        return ESP_FAIL;
    }
    space = strchr(header_block, ' ');
    if (space == NULL || (line_end - space) < 4) {
        return ESP_FAIL;
    }
    memcpy(code_buf, space + 1, 3);
    *out_status_code = atoi(code_buf);
    return ESP_OK;
}

static const char *app_protocol_tls_version_label(esp_tls_proto_ver_t tls_version)
{
    switch (tls_version) {
    case ESP_TLS_VER_TLS_1_2:
        return "TLS1.2";
    case ESP_TLS_VER_TLS_1_3:
        return "TLS1.3";
    default:
        return "TLSany";
    }
}

static const char *app_protocol_header_profile_label(app_protocol_header_profile_t profile)
{
    switch (profile) {
    case APP_PROTOCOL_HEADER_PROFILE_FULL:
        return "full";
    case APP_PROTOCOL_HEADER_PROFILE_NO_ENCODING:
        return "no-encoding";
    case APP_PROTOCOL_HEADER_PROFILE_NO_NGROK_HEADER:
        return "no-ngrok-header";
    case APP_PROTOCOL_HEADER_PROFILE_MINIMAL:
        return "minimal";
    default:
        return "unknown";
    }
}

static esp_err_t app_protocol_build_request(char **out_request,
                                            const char *method,
                                            const char *path,
                                            const char *host,
                                            const char *json_body,
                                            app_protocol_header_profile_t profile)
{
    if (out_request == NULL || method == NULL || path == NULL || host == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (profile) {
    case APP_PROTOCOL_HEADER_PROFILE_FULL:
        if (json_body != NULL) {
            return app_protocol_dup_format(
                out_request,
                "%s %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: esp32-m5stack\r\n"
                "Accept: application/json\r\n"
                "Accept-Encoding: identity\r\n"
                "ngrok-skip-browser-warning: true\r\n"
                "Connection: close\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %u\r\n"
                "\r\n"
                "%s",
                method,
                path,
                host,
                (unsigned)strlen(json_body),
                json_body);
        }
        return app_protocol_dup_format(
            out_request,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: esp32-m5stack\r\n"
            "Accept: application/json\r\n"
            "Accept-Encoding: identity\r\n"
            "ngrok-skip-browser-warning: true\r\n"
            "Connection: close\r\n"
            "\r\n",
            method,
            path,
            host);
    case APP_PROTOCOL_HEADER_PROFILE_NO_ENCODING:
        if (json_body != NULL) {
            return app_protocol_dup_format(
                out_request,
                "%s %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: esp32-m5stack\r\n"
                "Accept: application/json\r\n"
                "ngrok-skip-browser-warning: true\r\n"
                "Connection: close\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %u\r\n"
                "\r\n"
                "%s",
                method,
                path,
                host,
                (unsigned)strlen(json_body),
                json_body);
        }
        return app_protocol_dup_format(
            out_request,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: esp32-m5stack\r\n"
            "Accept: application/json\r\n"
            "ngrok-skip-browser-warning: true\r\n"
            "Connection: close\r\n"
            "\r\n",
            method,
            path,
            host);
    case APP_PROTOCOL_HEADER_PROFILE_NO_NGROK_HEADER:
        if (json_body != NULL) {
            return app_protocol_dup_format(
                out_request,
                "%s %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: esp32-m5stack\r\n"
                "Accept: application/json\r\n"
                "Connection: close\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %u\r\n"
                "\r\n"
                "%s",
                method,
                path,
                host,
                (unsigned)strlen(json_body),
                json_body);
        }
        return app_protocol_dup_format(
            out_request,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: esp32-m5stack\r\n"
            "Accept: application/json\r\n"
            "Connection: close\r\n"
            "\r\n",
            method,
            path,
            host);
    case APP_PROTOCOL_HEADER_PROFILE_MINIMAL:
        if (json_body != NULL) {
            return app_protocol_dup_format(
                out_request,
                "%s %s HTTP/1.1\r\n"
                "Host: %s\r\n"
                "User-Agent: esp32-m5stack\r\n"
                "Accept: application/json\r\n"
                "Connection: close\r\n"
                "Content-Type: application/json\r\n"
                "Content-Length: %u\r\n"
                "\r\n"
                "%s",
                method,
                path,
                host,
                (unsigned)strlen(json_body),
                json_body);
        }
        return app_protocol_dup_format(
            out_request,
            "%s %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "User-Agent: esp32-m5stack\r\n"
            "Accept: application/json\r\n"
            "Connection: close\r\n"
            "\r\n",
            method,
            path,
            host);
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

static esp_err_t app_protocol_try_tls_request(const app_protocol_attempt_t *attempt,
                                              const char *method,
                                              const char *path,
                                              const char *host,
                                              int port,
                                              bool is_plain_tcp,
                                              const char *json_body,
                                              char **out_body)
{
    char *request = NULL;
    char *header_end = NULL;
    app_protocol_http_ctx_t ctx = { 0 };
    char read_buf[APP_PROTOCOL_RX_BUFFER_SIZE];
    esp_tls_t *tls = NULL;
    esp_tls_cfg_t cfg = { 0 };
    esp_err_t err = ESP_FAIL;
    int status_code = 0;
    size_t total_read = 0;
    size_t written_bytes = 0;

    if (attempt == NULL || method == NULL || path == NULL || host == NULL || out_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(app_protocol_build_request(&request, method, path, host, json_body, attempt->header_profile),
                        TAG, "Failed to build HTTP request");
    tls = esp_tls_init();
    if (tls == NULL) {
        free(request);
        return ESP_ERR_NO_MEM;
    }
    cfg.timeout_ms = APP_PROTOCOL_TIMEOUT_MS;
    cfg.is_plain_tcp = is_plain_tcp;
    if (!is_plain_tcp) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.tls_version = attempt->tls_version;
    }

    ESP_LOGI(TAG, "Protocol attempt=%s transport=%s tls=%s headers=%s",
             attempt->label,
             is_plain_tcp ? "tcp" : "tls",
             app_protocol_tls_version_label(attempt->tls_version),
             app_protocol_header_profile_label(attempt->header_profile));
    ESP_LOGI(TAG, "HTTP %s %s", method, path);
    ESP_LOGI(TAG, "Connecting %s host=%s port=%d", is_plain_tcp ? "TCP" : "TLS", host, port);
    if (esp_tls_conn_new_sync(host, (int)strlen(host), port, &cfg, tls) != 1) {
        int tls_code = 0;
        int tls_flags = 0;
        esp_tls_error_handle_t tls_error = NULL;
        if (!is_plain_tcp) {
            esp_tls_get_error_handle(tls, &tls_error);
            if (tls_error != NULL &&
                esp_tls_get_and_clear_last_error(tls_error, &tls_code, &tls_flags) == ESP_OK &&
                (tls_code != 0 || tls_flags != 0)) {
                ESP_LOGE(TAG, "Protocol TLS error: esp_tls_code=0x%x tls_flags=0x%x", tls_code, tls_flags);
            }
        }
        esp_tls_conn_destroy(tls);
        free(request);
        return ESP_ERR_HTTP_CONNECT;
    }
    ESP_LOGI(TAG, "%s connected", is_plain_tcp ? "TCP" : "TLS");
    ESP_LOGI(TAG, "HTTP request len=%u", (unsigned)strlen(request));

    while (written_bytes < strlen(request)) {
        int ret = esp_tls_conn_write(tls, request + written_bytes, strlen(request) - written_bytes);
        if (ret > 0) {
            written_bytes += (size_t)ret;
            continue;
        }
        if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        ESP_LOGE(TAG, "TLS write failed: %d", ret);
        esp_tls_conn_destroy(tls);
        free(request);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "TLS wrote %u bytes", (unsigned)written_bytes);
    free(request);
    request = NULL;

    while (true) {
        int ret = esp_tls_conn_read(tls, read_buf, sizeof(read_buf));
        if (ret > 0) {
            total_read += (size_t)ret;
            if (total_read == (size_t)ret) {
                size_t preview_len = (size_t)ret > 256U ? 256U : (size_t)ret;
                ESP_LOGI(TAG, "Raw response first chunk (%u bytes total so far):\n%.*s",
                         (unsigned)total_read,
                         (int)preview_len,
                         read_buf);
            } else {
                ESP_LOGI(TAG, "TLS read chunk=%d total=%u", ret, (unsigned)total_read);
            }
            err = app_protocol_append_body(&ctx, read_buf, (size_t)ret);
            if (err != ESP_OK) {
                esp_tls_conn_destroy(tls);
                free(ctx.body);
                return err;
            }
            continue;
        }
        if (ret == 0) {
            ESP_LOGI(TAG, "TLS peer closed connection after %u bytes", (unsigned)total_read);
            break;
        }
        if (ret == ESP_TLS_ERR_SSL_WANT_READ || ret == ESP_TLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        ESP_LOGE(TAG, "TLS read failed: %d after %u bytes", ret, (unsigned)total_read);
        esp_tls_conn_destroy(tls);
        free(ctx.body);
        return ESP_FAIL;
    }
    esp_tls_conn_destroy(tls);

    if (ctx.body == NULL || ctx.body_len == 0) {
        ESP_LOGE(TAG, "Protocol response body is empty");
        free(ctx.body);
        return ESP_FAIL;
    }

    header_end = strstr(ctx.body, "\r\n\r\n");
    if (header_end == NULL) {
        ESP_LOGE(TAG, "Failed to find HTTP header terminator");
        ESP_LOG_BUFFER_HEXDUMP(TAG, ctx.body, ctx.body_len > 256U ? 256U : ctx.body_len, ESP_LOG_INFO);
        free(ctx.body);
        return ESP_ERR_HTTP_FETCH_HEADER;
    }

    {
        size_t header_preview_len = (size_t)(header_end - ctx.body);
        if (header_preview_len > 256U) {
            header_preview_len = 256U;
        }
        ESP_LOGI(TAG, "HTTP response headers:\n%.*s", (int)header_preview_len, ctx.body);
    }

    err = app_protocol_parse_status_code(ctx.body, &status_code);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse HTTP status line");
        free(ctx.body);
        return err;
    }

    {
        size_t header_len = (size_t)(header_end - ctx.body) + 4U;
        size_t body_len = ctx.body_len - header_len;
        memmove(ctx.body, ctx.body + header_len, body_len);
        ctx.body[body_len] = '\0';
        ctx.body_len = body_len;
    }

    ESP_LOGI(TAG, "HTTP status=%d body_len=%u", status_code, (unsigned)ctx.body_len);

    if (status_code < 200 || status_code >= 300) {
        ESP_LOGE(TAG, "Protocol request returned status %d body=%s", status_code, ctx.body);
        free(ctx.body);
        return ESP_FAIL;
    }
    if (ctx.body == NULL || ctx.body_len == 0) {
        ESP_LOGE(TAG, "Protocol response body is empty");
        free(ctx.body);
        return ESP_FAIL;
    }

    *out_body = ctx.body;
    return ESP_OK;
}

static esp_err_t app_protocol_perform_json_request(const char *method,
                                                   const char *path,
                                                  const char *query,
                                                  const char *json_body,
                                                  char **out_body)
{
    char scheme[8];
    char host[128];
    char base_path[128];
    char *request_path = NULL;
    char *full_path = NULL;
    esp_err_t err = ESP_FAIL;
    int port = 0;
    size_t attempt_index;
    bool is_plain_tcp = false;

    if (method == NULL || path == NULL || out_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (app_protocol_has_placeholder(APP_PROTOCOL_BASE_URL)) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(app_protocol_parse_base_url(scheme, sizeof(scheme), host, sizeof(host), &port,
                                                    base_path, sizeof(base_path)),
                        TAG, "Failed to parse protocol base URL");
    if (strcmp(scheme, "https") == 0) {
        is_plain_tcp = false;
    } else if (strcmp(scheme, "http") == 0) {
        is_plain_tcp = true;
    } else {
        ESP_LOGE(TAG, "Only http and https base URLs are supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_RETURN_ON_ERROR(app_protocol_build_request_path(&request_path, path, query), TAG, "Failed to build request path");
    if (strcmp(base_path, "/") != 0) {
        ESP_RETURN_ON_ERROR(app_protocol_dup_format(&full_path, "%s%s", base_path, request_path), TAG,
                            "Failed to build request URL path");
        free(request_path);
        request_path = NULL;
    } else {
        full_path = request_path;
        request_path = NULL;
    }

    for (attempt_index = 0; attempt_index < (sizeof(APP_PROTOCOL_GET_ATTEMPTS) / sizeof(APP_PROTOCOL_GET_ATTEMPTS[0])); ++attempt_index) {
        if (is_plain_tcp && attempt_index > 0) {
            break;
        }
        err = app_protocol_try_tls_request(&APP_PROTOCOL_GET_ATTEMPTS[attempt_index],
                                           method,
                                           full_path,
                                           host,
                                           port,
                                           is_plain_tcp,
                                           json_body,
                                           out_body);
        if (err == ESP_OK) {
            free(full_path);
            return ESP_OK;
        }
        if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NO_MEM) {
            free(full_path);
            return err;
        }
        ESP_LOGW(TAG, "Protocol attempt %s failed: %s",
                 APP_PROTOCOL_GET_ATTEMPTS[attempt_index].label,
                 esp_err_to_name(err));
    }

    free(full_path);
    return err;
}

static esp_err_t app_protocol_copy_json_string(char *dst, size_t dst_size, const char *json, const char *name)
{
    char pattern[64];
    const char *key;
    const char *value_start;
    const char *value_end;
    size_t len;

    if (dst == NULL || dst_size == 0 || json == NULL || name == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", name);
    key = strstr(json, pattern);
    if (key == NULL) {
        ESP_LOGE(TAG, "Missing string field: %s", name);
        return ESP_FAIL;
    }
    value_start = strchr(key + strlen(pattern), ':');
    if (value_start == NULL) {
        return ESP_FAIL;
    }
    value_start = strchr(value_start, '"');
    if (value_start == NULL) {
        return ESP_FAIL;
    }
    value_start++;
    value_end = strchr(value_start, '"');
    if (value_end == NULL) {
        return ESP_FAIL;
    }
    len = (size_t)(value_end - value_start);
    if (len + 1U > dst_size) {
        ESP_LOGE(TAG, "Field too long: %s", name);
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(dst, value_start, len);
    dst[len] = '\0';
    return ESP_OK;
}

esp_err_t app_protocol_get_config(app_protocol_config_t *out_config)
{
    char *resp_body = NULL;
    esp_err_t err;
    const char *data_json;

    if (out_config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_config, 0, sizeof(*out_config));

    err = app_protocol_perform_json_request("GET", APP_PROTOCOL_GET_CONFIG_PATH, NULL, NULL, &resp_body);
    ESP_RETURN_ON_ERROR(err, TAG, "Get Config failed");

    data_json = strstr(resp_body, "\"data\"");
    if (data_json == NULL) {
        free(resp_body);
        return ESP_FAIL;
    }

    err = app_protocol_copy_json_string(out_config->app_id, sizeof(out_config->app_id), data_json, "app_id");
    if (err == ESP_OK) {
        err = app_protocol_copy_json_string(out_config->token, sizeof(out_config->token), data_json, "token");
    }
    if (err == ESP_OK) {
        err = app_protocol_copy_json_string(out_config->uid, sizeof(out_config->uid), data_json, "uid");
    }
    if (err == ESP_OK) {
        err = app_protocol_copy_json_string(out_config->channel_name, sizeof(out_config->channel_name), data_json, "channel_name");
    }
    if (err == ESP_OK) {
        err = app_protocol_copy_json_string(out_config->agent_uid, sizeof(out_config->agent_uid), data_json, "agent_uid");
    }
    free(resp_body);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Protocol config acquired: channel=%s uid=%s agent_uid=%s",
                 out_config->channel_name, out_config->uid, out_config->agent_uid);
    }
    return err;
}

esp_err_t app_protocol_start_agent(const app_protocol_config_t *config)
{
    char *body = NULL;
    char *resp_body = NULL;
    esp_err_t err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(app_protocol_dup_format(&body,
                                                "{\"channelName\":\"%s\",\"rtcUid\":\"%s\",\"userUid\":\"%s\",\"parameters\":{\"output_audio_codec\":\"%s\"}}",
                                                config->channel_name,
                                                config->agent_uid,
                                                config->uid,
                                                APP_AGORA_PROTOCOL_OUTPUT_AUDIO_CODEC),
                        TAG, "Failed to build Start Agent request");

    err = app_protocol_perform_json_request("POST", APP_PROTOCOL_START_AGENT_PATH, NULL, body, &resp_body);
    free(body);
    if (err == ESP_OK) {
        const char *data_json = strstr(resp_body, "\"data\"");
        if (data_json != NULL) {
            (void)app_protocol_copy_json_string(((app_protocol_config_t *)config)->agent_id,
                                                sizeof(((app_protocol_config_t *)config)->agent_id),
                                                data_json, "agent_id");
        }
        free(resp_body);
        ESP_LOGI(TAG, "Agent start request accepted");
    }
    return err;
}

esp_err_t app_protocol_stop_agent(const app_protocol_config_t *config)
{
    char *body = NULL;
    char *resp_body = NULL;
    esp_err_t err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(app_protocol_dup_format(&body,
                                                "{\"agentId\":\"%s\"}",
                                                config->agent_id),
                        TAG, "Failed to build Stop Agent request");

    err = app_protocol_perform_json_request("POST", APP_PROTOCOL_STOP_AGENT_PATH, NULL, body, &resp_body);
    free(body);
    free(resp_body);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Agent stop request accepted");
    }
    return err;
}
