#include "app_wifi.h"

#include <string.h>

#include "app_config.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static const char *TAG = "app_wifi";

static EventGroupHandle_t s_wifi_event_group;
static esp_netif_t *s_wifi_netif;

enum {
    APP_WIFI_CONNECTED_BIT = BIT0,
};

static void app_wifi_log_ip(void)
{
    esp_netif_ip_info_t ip_info = {0};

    if (s_wifi_netif == NULL) {
        ESP_LOGW(TAG, "Wi-Fi netif is not ready");
        return;
    }

    if (esp_netif_get_ip_info(s_wifi_netif, &ip_info) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read IP information");
        return;
    }

    ESP_LOGI(TAG, "IP address: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&ip_info.gw));
    ESP_LOGI(TAG, "Netmask: " IPSTR, IP2STR(&ip_info.netmask));
}

static void app_wifi_event_handler(void *arg,
                                   esp_event_base_t event_base,
                                   int32_t event_id,
                                   void *event_data)
{
    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "Wi-Fi station started, connecting");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "Wi-Fi disconnected, retrying");
            xEventGroupClearBits(s_wifi_event_group, APP_WIFI_CONNECTED_BIT);
            esp_wifi_connect();
            break;
        default:
            break;
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "Wi-Fi connected");
        ESP_LOGI(TAG, "IP address: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, APP_WIFI_CONNECTED_BIT);
    }
}

static esp_err_t app_wifi_init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    return err;
}

esp_err_t app_wifi_start(void)
{
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };
    EventBits_t bits;

    if (strcmp(APP_WIFI_SSID, "CHANGE_ME") == 0 ||
        strcmp(APP_WIFI_PASSWORD, "CHANGE_ME") == 0) {
        ESP_LOGE(TAG, "Set APP_WIFI_SSID and APP_WIFI_PASSWORD before running on hardware");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_RETURN_ON_ERROR(app_wifi_init_nvs(), TAG, "Failed to initialize NVS");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Failed to initialize esp_netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Failed to create default event loop");

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create Wi-Fi event group");
        return ESP_ERR_NO_MEM;
    }

    s_wifi_netif = esp_netif_create_default_wifi_sta();
    if (s_wifi_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi STA netif");
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &app_wifi_event_handler, NULL, NULL),
        TAG,
        "Failed to register Wi-Fi event handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &app_wifi_event_handler, NULL, NULL),
        TAG,
        "Failed to register IP event handler");

    memcpy(wifi_cfg.sta.ssid, APP_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    memcpy(wifi_cfg.sta.password, APP_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));

    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Failed to set Wi-Fi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "Failed to set Wi-Fi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Failed to start Wi-Fi");

    ESP_LOGI(TAG, "Waiting for Wi-Fi connection");
    bits = xEventGroupWaitBits(
        s_wifi_event_group,
        APP_WIFI_CONNECTED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY);

    if ((bits & APP_WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "Timed out waiting for Wi-Fi connection");
        return ESP_ERR_TIMEOUT;
    }

    app_wifi_log_ip();
    return ESP_OK;
}
