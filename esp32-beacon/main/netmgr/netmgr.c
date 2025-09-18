#include "netmgr.h"

#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "config_portal.h"

static const char *TAG = "netmgr";

#define NETMGR_CONNECTED_BIT BIT0

static esp_netif_t *s_sta_netif;
static EventGroupHandle_t s_wifi_events;
static wifi_config_t s_wifi_cfg;
static bool s_wifi_cfg_valid;
static bool s_wifi_started;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void apply_config(const config_portal_config_t *config, void *ctx);

esp_err_t netmgr_init(void)
{
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_FAIL, TAG, "failed to create default wifi sta");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "set storage failed");

    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_wifi_events != NULL, ESP_ERR_NO_MEM, TAG, "event group alloc failed");
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG, "wifi handler register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL), TAG, "ip handler register failed");

    ESP_ERROR_CHECK(config_portal_register_listener(apply_config, NULL));

    ESP_LOGI(TAG, "Wi-Fi manager initialized");
    return ESP_OK;
}

esp_err_t netmgr_start(void)
{
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start failed");
    s_wifi_started = true;

    if (s_wifi_cfg_valid) {
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &s_wifi_cfg), TAG, "set config failed");
        ESP_RETURN_ON_ERROR(esp_wifi_connect(), TAG, "wifi connect failed");
        ESP_LOGI(TAG, "Connecting to SSID '%s'", s_wifi_cfg.sta.ssid);
    } else {
        ESP_LOGW(TAG, "Wi-Fi credentials not provisioned yet");
    }

    return ESP_OK;
}

static void apply_config(const config_portal_config_t *config, void *ctx)
{
    if (!config) {
        return;
    }

    if (config->wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi credentials cleared or not set");
        s_wifi_cfg_valid = false;
        return;
    }

    wifi_config_t new_cfg = {0};
    strlcpy((char *)new_cfg.sta.ssid, config->wifi_ssid, sizeof(new_cfg.sta.ssid));
    strlcpy((char *)new_cfg.sta.password, config->wifi_password, sizeof(new_cfg.sta.password));
    new_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    bool changed = !s_wifi_cfg_valid || memcmp(&s_wifi_cfg, &new_cfg, sizeof(new_cfg)) != 0;
    s_wifi_cfg = new_cfg;
    s_wifi_cfg_valid = true;

    if (!changed) {
        return;
    }

    ESP_LOGI(TAG, "Applying new Wi-Fi credentials for SSID '%s'", s_wifi_cfg.sta.ssid);

    if (s_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_set_config(WIFI_IF_STA, &s_wifi_cfg);
        esp_wifi_connect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_wifi_cfg_valid) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected from AP (reason %d)", ((wifi_event_sta_disconnected_t *)event_data)->reason);
        xEventGroupClearBits(s_wifi_events, NETMGR_CONNECTED_BIT);
        if (s_wifi_cfg_valid) {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, NETMGR_CONNECTED_BIT);
    }
}
