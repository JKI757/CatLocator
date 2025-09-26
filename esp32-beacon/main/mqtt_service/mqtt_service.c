#include "mqtt_service.h"

#include <string.h>

#include "config_portal.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns_discovery.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt_service";

static esp_mqtt_client_handle_t s_client;
static bool s_should_start;
static config_portal_config_t s_current_cfg;
static SemaphoreHandle_t s_lock;
static mdns_discovery_info_t s_discovered_info;

static void apply_config(const config_portal_config_t *config, void *ctx);
static esp_err_t start_client_locked(void);
static bool mqtt_uri_valid(const char *uri);
static void stop_client_locked(void);
static const char *active_uri_locked(void);
static void mdns_listener(const mdns_discovery_info_t *info, void *ctx);

esp_err_t mqtt_service_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");
    }

    memset(&s_current_cfg, 0, sizeof(s_current_cfg));
    memset(&s_discovered_info, 0, sizeof(s_discovered_info));

    esp_err_t err = config_portal_register_listener(apply_config, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config listener registration failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_discovery_register_listener(mdns_listener, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mdns listener registration failed: %s", esp_err_to_name(err));
    }

    s_should_start = false;
    return ESP_OK;
}

esp_err_t mqtt_service_start(void)
{
    ESP_RETURN_ON_FALSE(s_lock != NULL, ESP_ERR_INVALID_STATE, TAG, "service not initialized");

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_should_start = true;
    esp_err_t err = start_client_locked();
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t mqtt_service_publish(const char *topic, const char *payload)
{
    ESP_RETURN_ON_FALSE(topic != NULL, ESP_ERR_INVALID_ARG, TAG, "topic required");

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(500)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_mqtt_client_handle_t client = s_client;
    if (!client) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    int msg_id = esp_mqtt_client_publish(client, topic, payload, 0, 1, 0);
    xSemaphoreGive(s_lock);
    ESP_RETURN_ON_FALSE(msg_id >= 0, ESP_FAIL, TAG, "publish failed");
    return ESP_OK;
}

static void apply_config(const config_portal_config_t *config, void *ctx)
{
    if (!config || s_lock == NULL) {
        return;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "config apply timeout waiting for lock");
        return;
    }

    bool changed = memcmp(&s_current_cfg, config, sizeof(s_current_cfg)) != 0;
    s_current_cfg = *config;

    if (!changed) {
        xSemaphoreGive(s_lock);
        return;
    }

    if (s_current_cfg.mqtt_uri[0]) {
        ESP_LOGI(TAG, "MQTT configuration updated (broker=%s)", s_current_cfg.mqtt_uri);
    } else {
        ESP_LOGI(TAG, "MQTT broker URI cleared; relying on discovery");
    }

    stop_client_locked();

    if (s_should_start) {
        esp_err_t err = start_client_locked();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "MQTT restart after config change failed: %s", esp_err_to_name(err));
        }
    }

    xSemaphoreGive(s_lock);
}

static esp_err_t start_client_locked(void)
{
    const char *uri = active_uri_locked();
    if (!uri || uri[0] == '\0') {
        ESP_LOGW(TAG, "MQTT URI not available; waiting for configuration or discovery");
        return ESP_ERR_INVALID_STATE;
    }

    if (!mqtt_uri_valid(uri)) {
        ESP_LOGW(TAG, "MQTT URI '%s' is invalid", uri);
        return ESP_ERR_INVALID_ARG;
    }

    stop_client_locked();

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = uri,
        .credentials.username = s_current_cfg.mqtt_username[0] ? s_current_cfg.mqtt_username : NULL,
        .credentials.authentication.password = s_current_cfg.mqtt_password[0] ? s_current_cfg.mqtt_password : NULL,
    };

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_ERR_NO_MEM, TAG, "failed to init client");

    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        return err;
    }

    const char *source = s_current_cfg.mqtt_uri[0] ? "configured" : "mdns";
    ESP_LOGI(TAG, "MQTT client started (source=%s, broker=%s)", source, uri);
    return ESP_OK;
}

static void stop_client_locked(void)
{
    if (!s_client) {
        return;
    }

    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
}

static const char *active_uri_locked(void)
{
    if (s_current_cfg.mqtt_uri[0]) {
        return s_current_cfg.mqtt_uri;
    }
    if (s_discovered_info.uri[0]) {
        return s_discovered_info.uri;
    }
    return NULL;
}

static void mdns_listener(const mdns_discovery_info_t *info, void *ctx)
{
    (void)ctx;
    if (!info || s_lock == NULL) {
        return;
    }

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }

    bool changed = strncmp(s_discovered_info.uri, info->uri, sizeof(s_discovered_info.uri)) != 0 ||
                   s_discovered_info.port != info->port || s_discovered_info.tls != info->tls;

    s_discovered_info = *info;

    esp_err_t err = ESP_OK;
    bool should_restart = s_should_start && s_current_cfg.mqtt_uri[0] == '\0' && changed;

    if (changed) {
        ESP_LOGI(TAG, "Discovered MQTT broker via mDNS: %s", info->uri);
    }

    if (should_restart) {
        stop_client_locked();
        err = start_client_locked();
    }

    xSemaphoreGive(s_lock);

    if (should_restart && err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Failed to start MQTT client with discovered broker: %s", esp_err_to_name(err));
    }
}

static bool mqtt_uri_valid(const char *uri)
{
    if (!uri || uri[0] == '\0') {
        return false;
    }

    return strncmp(uri, "mqtt://", 7) == 0 || strncmp(uri, "mqtts://", 8) == 0;
}
