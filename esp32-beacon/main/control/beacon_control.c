#include "beacon_control.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "config_portal.h"
#include "device_info.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_service.h"

static const char *TAG = "beacon_control";

static char s_control_topic[160];
static char s_state_topic[160];

static void publish_state(const config_portal_config_t *cfg, const char *status, const char *error_msg);
static void handle_message(const char *topic, const char *payload, size_t len, void *ctx);
static void handle_assign(const cJSON *root);
static void handle_clear(void);
static void handle_reset(void);

esp_err_t beacon_control_init(void)
{
    const char *scanner_id = device_info_scanner_id();
    if (!scanner_id || scanner_id[0] == '\0') {
        ESP_LOGE(TAG, "Scanner ID unavailable");
        return ESP_FAIL;
    }

    int written = snprintf(s_control_topic, sizeof(s_control_topic), "scanners/%s/control", scanner_id);
    if (written <= 0 || written >= (int)sizeof(s_control_topic)) {
        ESP_LOGE(TAG, "Control topic truncated");
        return ESP_ERR_INVALID_SIZE;
    }

    written = snprintf(s_state_topic, sizeof(s_state_topic), "scanners/%s/state", scanner_id);
    if (written <= 0 || written >= (int)sizeof(s_state_topic)) {
        ESP_LOGE(TAG, "State topic truncated");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = mqtt_service_register_handler(handle_message, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT handler: %s", esp_err_to_name(err));
        return err;
    }

    err = mqtt_service_subscribe(s_control_topic, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to subscribe to %s: %s", s_control_topic, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Beacon control listening on %s", s_control_topic);
    return ESP_OK;
}

static void handle_assign(const cJSON *root)
{
    const cJSON *beacon_id_item = cJSON_GetObjectItemCaseSensitive(root, "beacon_id");
    if (!cJSON_IsString(beacon_id_item) || beacon_id_item->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "assign command missing beacon_id");
        return;
    }

    config_portal_config_t cfg = {0};
    if (config_portal_get_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to fetch config for assign command");
        return;
    }

    strlcpy(cfg.beacon_id, beacon_id_item->valuestring, sizeof(cfg.beacon_id));

    const cJSON *location = cJSON_GetObjectItemCaseSensitive(root, "location");
    if (cJSON_IsObject(location)) {
        const cJSON *x = cJSON_GetObjectItemCaseSensitive(location, "x");
        const cJSON *y = cJSON_GetObjectItemCaseSensitive(location, "y");
        const cJSON *z = cJSON_GetObjectItemCaseSensitive(location, "z");
        if (cJSON_IsNumber(x)) {
            cfg.location_x = x->valuedouble;
        }
        if (cJSON_IsNumber(y)) {
            cfg.location_y = y->valuedouble;
        }
        if (cJSON_IsNumber(z)) {
            cfg.location_z = z->valuedouble;
        }
    }

    esp_err_t err = config_portal_set_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist config: %s", esp_err_to_name(err));
        publish_state(&cfg, "error", "persist_failed");
        return;
    }

    ESP_LOGI(TAG, "Assigned beacon_id=%s", cfg.beacon_id);
    publish_state(&cfg, "assigned", NULL);
}

static void handle_clear(void)
{
    config_portal_config_t cfg = {0};
    if (config_portal_get_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to fetch config for clear command");
        return;
    }

    if (cfg.beacon_id[0] == '\0') {
        ESP_LOGI(TAG, "Beacon ID already cleared");
        publish_state(&cfg, "cleared", NULL);
        return;
    }

    cfg.beacon_id[0] = '\0';

    esp_err_t err = config_portal_set_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist config: %s", esp_err_to_name(err));
        publish_state(&cfg, "error", "clear_failed");
        return;
    }

    ESP_LOGI(TAG, "Cleared beacon ID; returning to discovery mode");
    publish_state(&cfg, "cleared", NULL);
}

static void handle_reset(void)
{
    config_portal_config_t cfg = {0};
    if (config_portal_get_config(&cfg) == ESP_OK) {
        publish_state(&cfg, "rebooting", NULL);
    }

    ESP_LOGW(TAG, "Reset command received; rebooting");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
}

static void handle_message(const char *topic, const char *payload, size_t len, void *ctx)
{
    (void)ctx;
    if (!topic || strcmp(topic, s_control_topic) != 0 || !payload) {
        return;
    }

    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse control payload");
        return;
    }

    const cJSON *command = cJSON_GetObjectItemCaseSensitive(root, "command");
    if (!cJSON_IsString(command) || command->valuestring[0] == '\0') {
        ESP_LOGW(TAG, "Control payload missing command");
        cJSON_Delete(root);
        return;
    }

    if (strcmp(command->valuestring, "assign") == 0) {
        handle_assign(root);
    } else if (strcmp(command->valuestring, "clear") == 0) {
        handle_clear();
    } else if (strcmp(command->valuestring, "reset") == 0) {
        handle_reset();
    } else if (strcmp(command->valuestring, "state") == 0) {
        config_portal_config_t cfg = {0};
        if (config_portal_get_config(&cfg) == ESP_OK) {
            publish_state(&cfg, "state", NULL);
        }
    } else {
        ESP_LOGW(TAG, "Unknown control command: %s", command->valuestring);
    }

    cJSON_Delete(root);
}

static void publish_state(const config_portal_config_t *cfg, const char *status, const char *error_msg)
{
    if (!cfg) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_info = {0};
    gmtime_r(&now, &tm_info);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &tm_info);

    char payload[256];
    int written = snprintf(payload, sizeof(payload),
                           "{\"status\":\"%s\",\"timestamp\":\"%s\",\"beacon_id\":\"%s\"",
                           status ? status : "state",
                           timestamp,
                           cfg->beacon_id);
    if (written < 0 || written >= (int)sizeof(payload)) {
        ESP_LOGW(TAG, "State payload truncated");
        return;
    }

    if (error_msg && error_msg[0] != '\0') {
        written += snprintf(payload + written, sizeof(payload) - written,
                            ",\"error\":\"%s\"", error_msg);
    }

    written += snprintf(payload + written, sizeof(payload) - written,
                        ",\"location\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}",
                        cfg->location_x,
                        cfg->location_y,
                        cfg->location_z);
    if (written < 0 || written >= (int)sizeof(payload)) {
        ESP_LOGW(TAG, "State payload truncated (final)");
        return;
    }

    esp_err_t err = mqtt_service_publish(s_state_topic, payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish state: %s", esp_err_to_name(err));
    }
}
