#include "mqtt_service.h"

#include <string.h>

#include "config_portal.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "mqtt_client.h"

static const char *TAG = "mqtt_service";

static esp_mqtt_client_handle_t s_client;
static bool s_should_start;
static config_portal_config_t s_current_cfg;

static void apply_config(const config_portal_config_t *config, void *ctx);
static esp_err_t start_client_locked(void);
static bool mqtt_uri_valid(const char *uri);

esp_err_t mqtt_service_init(void)
{
    esp_err_t err = config_portal_register_listener(apply_config, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config listener registration failed: %s", esp_err_to_name(err));
        return err;
    }
    s_should_start = false;
    return ESP_OK;
}

esp_err_t mqtt_service_start(void)
{
    s_should_start = true;
    return start_client_locked();
}

esp_err_t mqtt_service_publish(const char *topic, const char *payload)
{
    ESP_RETURN_ON_FALSE(s_client != NULL, ESP_ERR_INVALID_STATE, TAG, "client not initialized");
    ESP_RETURN_ON_FALSE(topic != NULL, ESP_ERR_INVALID_ARG, TAG, "topic required");

    int msg_id = esp_mqtt_client_publish(s_client, topic, payload, 0, 1, 0);
    ESP_RETURN_ON_FALSE(msg_id >= 0, ESP_FAIL, TAG, "publish failed");
    return ESP_OK;
}

static void apply_config(const config_portal_config_t *config, void *ctx)
{
    if (!config) {
        return;
    }

    bool changed = memcmp(&s_current_cfg, config, sizeof(s_current_cfg)) != 0;
    s_current_cfg = *config;

    if (!changed) {
        return;
    }

    if (s_current_cfg.mqtt_uri[0] == '\0') {
        if (s_client) {
            esp_mqtt_client_stop(s_client);
            esp_mqtt_client_destroy(s_client);
            s_client = NULL;
            ESP_LOGW(TAG, "MQTT credentials removed; client stopped");
        }
        return;
    }

    ESP_LOGI(TAG, "MQTT configuration updated (broker=%s)", s_current_cfg.mqtt_uri);

    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }

    if (s_should_start) {
        start_client_locked();
    }
}

static esp_err_t start_client_locked(void)
{
    if (s_current_cfg.mqtt_uri[0] == '\0') {
        ESP_LOGW(TAG, "MQTT URI not set; MQTT client idle");
        return ESP_ERR_INVALID_STATE;
    }

    if (!mqtt_uri_valid(s_current_cfg.mqtt_uri)) {
        ESP_LOGW(TAG, "MQTT URI '%s' is invalid", s_current_cfg.mqtt_uri);
        return ESP_ERR_INVALID_ARG;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = s_current_cfg.mqtt_uri,
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

    ESP_LOGI(TAG, "MQTT client started");
    return ESP_OK;
}

static bool mqtt_uri_valid(const char *uri)
{
    if (!uri || uri[0] == '\0') {
        return false;
    }

    return strncmp(uri, "mqtt://", 7) == 0 || strncmp(uri, "mqtts://", 8) == 0;
}
