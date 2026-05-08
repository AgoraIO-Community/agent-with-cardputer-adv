#include "app_time_sync.h"

#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define APP_TIME_SYNC_SERVER "pool.ntp.org"
#define APP_TIME_SYNC_MIN_UNIX 1700000000LL
#define APP_TIME_SYNC_TIMEOUT_MS 15000

static const char *TAG = "app_time_sync";
static bool s_sntp_initialized;

static bool app_time_sync_is_valid(void)
{
    time_t now = 0;
    time(&now);
    return ((long long)now) >= APP_TIME_SYNC_MIN_UNIX;
}

esp_err_t app_time_sync_wait(void)
{
    esp_err_t err;
    time_t now = 0;

    if (app_time_sync_is_valid()) {
        time(&now);
        ESP_LOGI(TAG, "System time already valid: %lld", (long long)now);
        return ESP_OK;
    }

    if (!s_sntp_initialized) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(APP_TIME_SYNC_SERVER);

        err = esp_netif_sntp_init(&config);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
            return err;
        }
        s_sntp_initialized = true;
    }

    ESP_LOGI(TAG, "Waiting for SNTP time sync");
    err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(APP_TIME_SYNC_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP sync timed out: %s", esp_err_to_name(err));
        return err;
    }

    time(&now);
    ESP_LOGI(TAG, "SNTP time synced: %lld", (long long)now);
    return ESP_OK;
}
