#include "netmgr.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"

#include "config_portal.h"

static const char *TAG = "netmgr";

#define NETMGR_CONNECTED_BIT BIT0

typedef enum {
    NETMGR_CMD_DISCONNECT = 0,
    NETMGR_CMD_CONNECT,
} netmgr_cmd_t;

static esp_netif_t *s_sta_netif;
static EventGroupHandle_t s_wifi_events;
static wifi_config_t s_wifi_cfg;
static bool s_wifi_cfg_valid;
static bool s_wifi_started;
static QueueHandle_t s_cmd_queue;
static TaskHandle_t s_cmd_task;

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static void apply_config(const config_portal_config_t *config, void *ctx);
static const char *disconnect_reason_str(uint8_t reason);
static void try_connect(void);
static void cmd_task(void *arg);

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

    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_wifi_events != NULL, ESP_ERR_NO_MEM, TAG, "event group alloc failed");
    }

    if (!s_cmd_queue) {
        s_cmd_queue = xQueueCreate(4, sizeof(netmgr_cmd_t));
        ESP_RETURN_ON_FALSE(s_cmd_queue != NULL, ESP_ERR_NO_MEM, TAG, "cmd queue alloc failed");
    }

    if (!s_cmd_task) {
        BaseType_t created = xTaskCreate(cmd_task, "netmgr_cmd", 4096, NULL, tskIDLE_PRIORITY + 2, &s_cmd_task);
        ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "cmd task create failed");
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG, "wifi handler register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL), TAG, "ip handler register failed");

    esp_err_t err = config_portal_register_listener(apply_config, NULL);
    ESP_RETURN_ON_ERROR(err, TAG, "failed to register config listener");

    ESP_LOGI(TAG, "Wi-Fi manager initialized");
    return ESP_OK;
}

esp_err_t netmgr_start(void)
{
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start failed");
    s_wifi_started = true;

    if (s_wifi_cfg_valid) {
        try_connect();
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
        if (s_wifi_cfg_valid) {
            ESP_LOGW(TAG, "Wi-Fi credentials cleared or not set");
        }
        s_wifi_cfg_valid = false;
        if (s_wifi_started && s_cmd_queue) {
            netmgr_cmd_t cmd = NETMGR_CMD_DISCONNECT;
            if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
                netmgr_cmd_t dropped;
                if (xQueueReceive(s_cmd_queue, &dropped, 0) == pdTRUE) {
                    xQueueSend(s_cmd_queue, &cmd, 0);
                }
            }
        }
        return;
    }

    wifi_config_t new_cfg = {0};
    strlcpy((char *)new_cfg.sta.ssid, config->wifi_ssid, sizeof(new_cfg.sta.ssid));
    strlcpy((char *)new_cfg.sta.password, config->wifi_password, sizeof(new_cfg.sta.password));
    new_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    new_cfg.sta.pmf_cfg.capable = true;
    new_cfg.sta.pmf_cfg.required = false;
#if defined(CONFIG_ESP_WIFI_ENABLE_WPA3_SAE) || defined(CONFIG_ESP_WIFI_ENABLE_SAE_PK)
    new_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
#endif

    bool changed = !s_wifi_cfg_valid || memcmp(&s_wifi_cfg, &new_cfg, sizeof(new_cfg)) != 0;
    s_wifi_cfg = new_cfg;
    s_wifi_cfg_valid = true;

    if (!changed) {
        return;
    }

    ESP_LOGI(TAG, "Applying new Wi-Fi credentials for SSID '%s'", s_wifi_cfg.sta.ssid);

    if (s_wifi_started) {
        try_connect();
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_wifi_cfg_valid) {
            try_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Disconnected from AP (reason %d: %s)",
                 disc->reason,
                 disconnect_reason_str(disc->reason));
        xEventGroupClearBits(s_wifi_events, NETMGR_CONNECTED_BIT);
        if (s_wifi_cfg_valid) {
            try_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_events, NETMGR_CONNECTED_BIT);
    }
}

static void try_connect(void)
{
    if (!s_wifi_started || !s_cmd_queue) {
        return;
    }

    netmgr_cmd_t cmd = NETMGR_CMD_CONNECT;
    if (xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        netmgr_cmd_t dropped;
        if (xQueueReceive(s_cmd_queue, &dropped, 0) == pdTRUE) {
            xQueueSend(s_cmd_queue, &cmd, 0);
        }
    }
}

static const char *disconnect_reason_str(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_AUTH_EXPIRE:
            return "auth expired";
        case WIFI_REASON_AUTH_LEAVE:
            return "auth leave";
        case WIFI_REASON_ASSOC_EXPIRE:
            return "assoc expired";
        case WIFI_REASON_ASSOC_TOOMANY:
            return "too many sta";
        case WIFI_REASON_NOT_AUTHED:
            return "not authed";
        case WIFI_REASON_NOT_ASSOCED:
            return "not assoc";
        case WIFI_REASON_ASSOC_LEAVE:
            return "assoc leave";
        case WIFI_REASON_ASSOC_NOT_AUTHED:
            return "assoc not authed";
        case WIFI_REASON_DISASSOC_PWRCAP_BAD:
            return "power cap bad";
        case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
            return "supchan bad";
        case WIFI_REASON_IE_INVALID:
            return "ie invalid";
        case WIFI_REASON_MIC_FAILURE:
            return "mic failure";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
            return "4-way timeout";
        case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
            return "group key timeout";
        case WIFI_REASON_IE_IN_4WAY_DIFFERS:
            return "ie mismatch";
        case WIFI_REASON_INVALID_PMKID:
            return "invalid pmkid";
        case WIFI_REASON_AUTH_FAIL:
            return "auth fail";
        case WIFI_REASON_ASSOC_FAIL:
            return "assoc fail";
        case WIFI_REASON_CONNECTION_FAIL:
            return "connection fail";
        case WIFI_REASON_NO_AP_FOUND:
            return "no ap found";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "handshake timeout";
        default:
            break;
    }
    return "unknown";
}

static void cmd_task(void *arg)
{
    (void)arg;

    while (true) {
        netmgr_cmd_t cmd;
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (cmd == NETMGR_CMD_DISCONNECT) {
            esp_wifi_disconnect();
            continue;
        }

        if (cmd != NETMGR_CMD_CONNECT) {
            continue;
        }

        if (!s_wifi_cfg_valid) {
            ESP_LOGW(TAG, "Connect command received without valid credentials");
            continue;
        }

        ESP_LOGI(TAG, "Connecting to SSID '%s'", s_wifi_cfg.sta.ssid);

        esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &s_wifi_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to apply STA config: %s", esp_err_to_name(err));
            continue;
        }

        err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        }

        /* simple backoff to avoid hammering the event loop */
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
