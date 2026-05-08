#include "app_webrtc_probe.h"

#include "esp_log.h"
#include "esp_peer_default.h"

static const char *TAG = "app_webrtc_probe";

esp_err_t app_webrtc_probe(void)
{
    const esp_peer_ops_t *peer_impl = esp_peer_get_default_impl();

    ESP_LOGI(TAG, "esp_peer default impl: %p", (void *)peer_impl);

    if (peer_impl == NULL) {
        ESP_LOGE(TAG, "Failed to resolve esp_peer default implementation");
        return ESP_FAIL;
    }
    return ESP_OK;
}
