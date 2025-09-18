#include "config_portal.h"

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CONFIG_PORTAL_NAMESPACE    "catcfg"
#define CONFIG_PORTAL_KEY          "config"
#define CONFIG_LISTENER_MAX        8

static const char *TAG = "config_portal";

static config_portal_config_t s_config;
static bool s_config_loaded;
static httpd_handle_t s_http_handle;
static nvs_handle_t s_nvs_handle;
static SemaphoreHandle_t s_config_mutex;

typedef struct {
    config_portal_listener_t cb;
    void *ctx;
} listener_entry_t;

static listener_entry_t s_listeners[CONFIG_LISTENER_MAX];

static esp_err_t load_config_from_nvs(void);
static esp_err_t save_config_to_nvs(void);
static void notify_listeners(void);
static esp_err_t handle_get_config(httpd_req_t *req);
static esp_err_t handle_post_config(httpd_req_t *req);
static void sanitize_config(config_portal_config_t *cfg);

esp_err_t config_portal_init(void)
{
    memset(&s_config, 0, sizeof(s_config));
    s_config.reporting_interval_ms = 5000;

    if (!s_config_mutex) {
        s_config_mutex = xSemaphoreCreateRecursiveMutex();
        ESP_RETURN_ON_FALSE(s_config_mutex != NULL, ESP_ERR_NO_MEM, TAG, "failed to create mutex");
    }

    ESP_RETURN_ON_ERROR(nvs_open(CONFIG_PORTAL_NAMESPACE, NVS_READWRITE, &s_nvs_handle), TAG, "nvs_open failed");

    esp_err_t err = load_config_from_nvs();
    if (err == ESP_OK) {
        s_config_loaded = true;
        sanitize_config(&s_config);
        notify_listeners();
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No persisted configuration yet");
    } else {
        ESP_LOGW(TAG, "Failed to load config: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

esp_err_t config_portal_start_async(void)
{
    if (s_http_handle) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.max_uri_handlers = 8;

    ESP_RETURN_ON_ERROR(httpd_start(&s_http_handle, &cfg), TAG, "httpd_start failed");

    const httpd_uri_t get_cfg = {
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = handle_get_config,
        .user_ctx = NULL,
    };

    const httpd_uri_t post_cfg = {
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = handle_post_config,
        .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_http_handle, &get_cfg);
    httpd_register_uri_handler(s_http_handle, &post_cfg);

    ESP_LOGI(TAG, "Configuration portal HTTP server started on port %d", cfg.server_port);
    return ESP_OK;
}

esp_err_t config_portal_register_listener(config_portal_listener_t cb, void *ctx)
{
    ESP_RETURN_ON_FALSE(cb != NULL, ESP_ERR_INVALID_ARG, TAG, "callback required");

    for (size_t i = 0; i < CONFIG_LISTENER_MAX; ++i) {
        if (s_listeners[i].cb == NULL) {
            s_listeners[i].cb = cb;
            s_listeners[i].ctx = ctx;
            cb(&s_config, ctx);
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "Listener capacity reached");
    return ESP_ERR_NO_MEM;
}

esp_err_t config_portal_get_config(config_portal_config_t *out)
{
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out required");

    if (s_config_mutex) {
        xSemaphoreTakeRecursive(s_config_mutex, portMAX_DELAY);
    }
    memcpy(out, &s_config, sizeof(s_config));
    if (s_config_mutex) {
        xSemaphoreGiveRecursive(s_config_mutex);
    }

    return ESP_OK;
}

bool config_portal_has_credentials(void)
{
    if (s_config_mutex) {
        xSemaphoreTakeRecursive(s_config_mutex, portMAX_DELAY);
    }
    bool ready = s_config.wifi_ssid[0] != '\0' && s_config.mqtt_uri[0] != '\0';
    if (s_config_mutex) {
        xSemaphoreGiveRecursive(s_config_mutex);
    }
    return ready;
}

static void sanitize_config(config_portal_config_t *cfg)
{
    cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';
    cfg->wifi_password[sizeof(cfg->wifi_password) - 1] = '\0';
    cfg->mqtt_uri[sizeof(cfg->mqtt_uri) - 1] = '\0';
    cfg->mqtt_username[sizeof(cfg->mqtt_username) - 1] = '\0';
    cfg->mqtt_password[sizeof(cfg->mqtt_password) - 1] = '\0';
    cfg->beacon_id[sizeof(cfg->beacon_id) - 1] = '\0';
}

static esp_err_t load_config_from_nvs(void)
{
    size_t required = sizeof(s_config);
    esp_err_t err = nvs_get_blob(s_nvs_handle, CONFIG_PORTAL_KEY, &s_config, &required);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded configuration (%u bytes)", (unsigned)required);
    }
    return err;
}

static esp_err_t save_config_to_nvs(void)
{
    ESP_RETURN_ON_ERROR(nvs_set_blob(s_nvs_handle, CONFIG_PORTAL_KEY, &s_config, sizeof(s_config)), TAG, "set blob failed");
    ESP_RETURN_ON_ERROR(nvs_commit(s_nvs_handle), TAG, "commit failed");
    ESP_LOGI(TAG, "Configuration saved");
    return ESP_OK;
}

static void notify_listeners(void)
{
    for (size_t i = 0; i < CONFIG_LISTENER_MAX; ++i) {
        if (s_listeners[i].cb) {
            s_listeners[i].cb(&s_config, s_listeners[i].ctx);
        }
    }
}

static cJSON *config_to_json(const config_portal_config_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddBoolToObject(root, "wifi_configured", cfg->wifi_ssid[0] != '\0');
    cJSON_AddStringToObject(root, "mqtt_uri", cfg->mqtt_uri);
    cJSON_AddStringToObject(root, "mqtt_username", cfg->mqtt_username);
    cJSON_AddStringToObject(root, "beacon_id", cfg->beacon_id);
    cJSON_AddNumberToObject(root, "location_x", cfg->location_x);
    cJSON_AddNumberToObject(root, "location_y", cfg->location_y);
    cJSON_AddNumberToObject(root, "location_z", cfg->location_z);
    cJSON_AddNumberToObject(root, "reporting_interval_ms", cfg->reporting_interval_ms);
    cJSON_AddBoolToObject(root, "mqtt_configured", cfg->mqtt_uri[0] != '\0');
    return root;
}

static esp_err_t handle_get_config(httpd_req_t *req)
{
    if (s_config_mutex) {
        xSemaphoreTakeRecursive(s_config_mutex, portMAX_DELAY);
    }
    cJSON *root = config_to_json(&s_config);
    if (s_config_mutex) {
        xSemaphoreGiveRecursive(s_config_mutex);
    }

    if (!root) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to allocate JSON");
        return ESP_FAIL;
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to encode JSON");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    cJSON_free(json);
    return ESP_OK;
}

static bool update_string(cJSON *obj, const char *key, char *dest, size_t dest_size)
{
    cJSON *node = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(node) && node->valuestring) {
        strlcpy(dest, node->valuestring, dest_size);
        return true;
    }
    return false;
}

static esp_err_t handle_post_config(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid length");
        return ESP_FAIL;
    }

    char *buf = malloc(total + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alloc failed");
        return ESP_ERR_NO_MEM;
    }

    int received = httpd_req_recv(req, buf, total);
    if (received <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid json");
        return ESP_FAIL;
    }

    if (s_config_mutex) {
        xSemaphoreTakeRecursive(s_config_mutex, portMAX_DELAY);
    }

    update_string(root, "wifi_ssid", s_config.wifi_ssid, sizeof(s_config.wifi_ssid));
    update_string(root, "wifi_password", s_config.wifi_password, sizeof(s_config.wifi_password));
    update_string(root, "mqtt_uri", s_config.mqtt_uri, sizeof(s_config.mqtt_uri));
    update_string(root, "mqtt_username", s_config.mqtt_username, sizeof(s_config.mqtt_username));
    update_string(root, "mqtt_password", s_config.mqtt_password, sizeof(s_config.mqtt_password));
    update_string(root, "beacon_id", s_config.beacon_id, sizeof(s_config.beacon_id));

    cJSON *node = NULL;
    node = cJSON_GetObjectItemCaseSensitive(root, "location_x");
    if (cJSON_IsNumber(node)) {
        s_config.location_x = (float)node->valuedouble;
    }
    node = cJSON_GetObjectItemCaseSensitive(root, "location_y");
    if (cJSON_IsNumber(node)) {
        s_config.location_y = (float)node->valuedouble;
    }
    node = cJSON_GetObjectItemCaseSensitive(root, "location_z");
    if (cJSON_IsNumber(node)) {
        s_config.location_z = (float)node->valuedouble;
    }
    node = cJSON_GetObjectItemCaseSensitive(root, "reporting_interval_ms");
    if (cJSON_IsNumber(node) && node->valueint > 0) {
        s_config.reporting_interval_ms = (uint32_t)node->valueint;
    }

    sanitize_config(&s_config);
    esp_err_t save_err = save_config_to_nvs();

    if (s_config_mutex) {
        xSemaphoreGiveRecursive(s_config_mutex);
    }

    cJSON_Delete(root);

    if (save_err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "persist failed");
        return save_err;
    }

    s_config_loaded = true;
    notify_listeners();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    return ESP_OK;
}
