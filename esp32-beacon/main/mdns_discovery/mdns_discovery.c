#include "mdns_discovery.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#if __has_include("mdns.h")
#define MDNS_DISCOVERY_SUPPORTED 1
#include "mdns.h"
#else
#define MDNS_DISCOVERY_SUPPORTED 0
#endif

#if MDNS_DISCOVERY_SUPPORTED

#define MDNS_DISCOVERY_CONNECTED_BIT BIT0

static const char *TAG = "mdns_discovery";

static EventGroupHandle_t s_event_group;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static mdns_discovery_listener_t s_listener;
static void *s_listener_ctx;
static mdns_discovery_info_t s_last_info;
static bool s_initialized;

static void discovery_task(void *arg);
static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);
static bool info_from_result(const mdns_result_t *result, mdns_discovery_info_t *out_info);
static bool info_equal(const mdns_discovery_info_t *a, const mdns_discovery_info_t *b);
static bool string_truthy(const char *value);
static void notify_listener(const mdns_discovery_info_t *info);

esp_err_t mdns_discovery_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");
    }

    if (!s_event_group) {
        s_event_group = xEventGroupCreate();
        ESP_RETURN_ON_FALSE(s_event_group != NULL, ESP_ERR_NO_MEM, TAG, "event group alloc failed");
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL), TAG, "ip handler register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, event_handler, NULL), TAG, "wifi handler register failed");

    esp_err_t err = mdns_init();
    if (err == ESP_ERR_INVALID_STATE) {
        err = ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "mdns_init failed");

    uint8_t mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (mac_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_read_mac failed: %s", esp_err_to_name(mac_err));
    }

    char hostname[48];
    if (mac_err == ESP_OK) {
        snprintf(hostname, sizeof(hostname), "catlocator-beacon-%02x%02x%02x", mac[3], mac[4], mac[5]);
    } else {
        snprintf(hostname, sizeof(hostname), "catlocator-beacon");
    }
    mdns_hostname_set(hostname);
    mdns_instance_name_set("CatLocator Beacon");

    memset(&s_last_info, 0, sizeof(s_last_info));
    s_initialized = true;
    ESP_LOGI(TAG, "mDNS discovery initialized (hostname=%s)", hostname);
    return ESP_OK;
}

esp_err_t mdns_discovery_start(void)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "mdns discovery not initialized");

    if (!s_task) {
        BaseType_t created = xTaskCreatePinnedToCore(discovery_task, "mdns_discovery", 4096, NULL, tskIDLE_PRIORITY + 1, &s_task, tskNO_AFFINITY);
        ESP_RETURN_ON_FALSE(created == pdPASS, ESP_ERR_NO_MEM, TAG, "discovery task create failed");
    }

    return ESP_OK;
}

esp_err_t mdns_discovery_register_listener(mdns_discovery_listener_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "mdns discovery not initialized");

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_listener = cb;
    s_listener_ctx = ctx;
    mdns_discovery_info_t snapshot = s_last_info;
    xSemaphoreGive(s_lock);

    if (cb && snapshot.uri[0] != '\0') {
        cb(&snapshot, ctx);
    }

    return ESP_OK;
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (!s_event_group) {
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_event_group, MDNS_DISCOVERY_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_event_group, MDNS_DISCOVERY_CONNECTED_BIT);
    }
}

static void discovery_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "mDNS discovery task started");

    for (;;) {
        xEventGroupWaitBits(s_event_group, MDNS_DISCOVERY_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_catlocator", "_tcp", 3000, 8, &results);
        if (err != ESP_OK) {
            if (err != ESP_ERR_NOT_FOUND) {
                ESP_LOGW(TAG, "mdns query failed: %s", esp_err_to_name(err));
            }
        }

        if (results) {
            mdns_result_t *item = results;
            mdns_discovery_info_t info;
            bool announced = false;
            while (item && !announced) {
                if (info_from_result(item, &info)) {
                    notify_listener(&info);
                    announced = true;
                }
                item = item->next;
            }
            mdns_query_results_free(results);
        }

        EventBits_t bits = xEventGroupGetBits(s_event_group);
        if ((bits & MDNS_DISCOVERY_CONNECTED_BIT) == 0) {
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

static void notify_listener(const mdns_discovery_info_t *info)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    bool changed = !info_equal(info, &s_last_info);
    if (changed) {
        s_last_info = *info;
    }

    mdns_discovery_listener_t listener = s_listener;
    void *ctx = s_listener_ctx;
    mdns_discovery_info_t snapshot = s_last_info;

    xSemaphoreGive(s_lock);

    if (listener && changed) {
        listener(&snapshot, ctx);
    }
}

static bool info_equal(const mdns_discovery_info_t *a, const mdns_discovery_info_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }

    return (a->port == b->port) && (a->tls == b->tls) && strcmp(a->uri, b->uri) == 0 && strcmp(a->hostname, b->hostname) == 0;
}

static bool info_from_result(const mdns_result_t *result, mdns_discovery_info_t *out_info)
{
    if (!result || !out_info) {
        return false;
    }

    memset(out_info, 0, sizeof(*out_info));

    uint16_t port = result->port;
    bool tls = false;
    char hostbuf[64] = "";
    char addrbuf[64] = "";

    if (result->hostname) {
        strlcpy(hostbuf, result->hostname, sizeof(hostbuf));
    }

    for (const mdns_ip_addr_t *a = result->addr; a != NULL; a = a->next) {
        if (a->addr.type == MDNS_IP_PROTOCOL_V4) {
            ip4addr_ntoa_r((const ip4_addr_t *)&a->addr.u_addr.ip4, addrbuf, sizeof(addrbuf));
            break;
        }
    }

    for (int i = 0; i < result->txt_count; ++i) {
        const mdns_txt_item_t *txt = &result->txt[i];
        if (!txt->key) {
            continue;
        }
        const char *value = txt->value ? txt->value : "";
        size_t key_len = strlen(txt->key);
        if (key_len == 0) {
            continue;
        }

        char key[32];
        if (key_len >= sizeof(key)) {
            key_len = sizeof(key) - 1;
        }
        memcpy(key, txt->key, key_len);
        key[key_len] = '\0';

        if (strcasecmp(key, "mqtt_port") == 0) {
            int v = atoi(value);
            if (v > 0 && v <= UINT16_MAX) {
                port = (uint16_t)v;
            }
        } else if (strcasecmp(key, "tls") == 0 || strcasecmp(key, "secure") == 0) {
            tls = string_truthy(value);
        } else if (strcasecmp(key, "host") == 0) {
            strlcpy(hostbuf, value, sizeof(hostbuf));
        }
    }

    const char *host_for_uri = addrbuf[0] ? addrbuf : hostbuf;
    if (!host_for_uri[0]) {
        ESP_LOGW(TAG, "mdns result missing hostname/address");
        return false;
    }

    const char *scheme = tls ? "mqtts" : "mqtt";

    if (!addrbuf[0] && hostbuf[0] && strchr(host_for_uri, '.') == NULL) {
        snprintf(out_info->uri, sizeof(out_info->uri), "%s://%s.local:%u", scheme, host_for_uri, port);
    } else {
        snprintf(out_info->uri, sizeof(out_info->uri), "%s://%s:%u", scheme, host_for_uri, port);
    }

    strlcpy(out_info->hostname, hostbuf[0] ? hostbuf : host_for_uri, sizeof(out_info->hostname));
    out_info->port = port;
    out_info->tls = tls;

    ESP_LOGI(TAG, "Discovered CatLocator service (%s)", out_info->uri);
    return true;
}

#else  /* MDNS_DISCOVERY_SUPPORTED */

esp_err_t mdns_discovery_init(void)
{
    ESP_LOGW("mdns_discovery", "mDNS support not available in this ESP-IDF; discovery disabled");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mdns_discovery_start(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t mdns_discovery_register_listener(mdns_discovery_listener_t cb, void *ctx)
{
    (void)cb;
    (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif  /* MDNS_DISCOVERY_SUPPORTED */
static bool string_truthy(const char *value)
{
    if (!value) {
        return false;
    }
    if (value[0] == '1' || value[0] == 'Y' || value[0] == 'y' || value[0] == 'T' || value[0] == 't') {
        return true;
    }
    if (strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0) {
        return true;
    }
    return false;
}
