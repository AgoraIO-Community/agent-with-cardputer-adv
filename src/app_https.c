#include "app_https.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "esp_heap_caps.h"

static const char *TAG = "app_https";

static esp_err_t app_https_event_handler(esp_http_client_event_t *event)
{
    switch (event->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGI(TAG, "HTTPS client connected");
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTPS transaction finished");
        break;
    case HTTP_EVENT_ERROR:
        ESP_LOGW(TAG, "HTTPS client event error");
        break;
    default:
        break;
    }

    return ESP_OK;
}

esp_err_t app_https_get_test(const char *url)
{
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = app_https_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client;
    esp_err_t err;
    int status_code;
    int64_t content_length;
    size_t free_heap_before;
    size_t free_heap_after;

    if (url == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    free_heap_before = esp_get_free_heap_size();
    ESP_LOGI(TAG, "HTTPS GET %s", url);
    ESP_LOGI(TAG, "Free heap before HTTPS: %u", (unsigned)free_heap_before);

    client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_ERR_NO_MEM;
    }

    err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        int tls_code = 0;
        int tls_flags = 0;
        if (esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags) == ESP_OK &&
            (tls_code != 0 || tls_flags != 0)) {
            ESP_LOGE(TAG, "HTTPS TLS error: esp_tls_code=0x%x tls_flags=0x%x", tls_code, tls_flags);
        }
        ESP_LOGE(TAG, "HTTPS GET failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }

    status_code = esp_http_client_get_status_code(client);
    content_length = esp_http_client_get_content_length(client);
    free_heap_after = esp_get_free_heap_size();

    ESP_LOGI(TAG, "HTTPS GET completed with status %d", status_code);
    ESP_LOGI(TAG, "HTTPS response length: %lld", content_length);
    ESP_LOGI(TAG, "Free heap after HTTPS: %u", (unsigned)free_heap_after);

    esp_http_client_cleanup(client);

    if (status_code < 200 || status_code >= 400) {
        ESP_LOGE(TAG, "Unexpected HTTPS status code: %d", status_code);
        return ESP_FAIL;
    }

    return ESP_OK;
}
