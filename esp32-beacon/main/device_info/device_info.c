#include "device_info.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"

static const char *TAG = "device_info";

static char s_scanner_id[32];
static bool s_initialized;

static esp_err_t ensure_scanner_id(void)
{
    if (s_initialized && s_scanner_id[0] != '\0') {
        return ESP_OK;
    }

    uint8_t mac[6] = {0};
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, mac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_get_mac failed: %s", esp_err_to_name(err));
        return err;
    }

    int written = snprintf(s_scanner_id, sizeof(s_scanner_id), "scanner-%02X%02X%02X%02X%02X%02X",
                           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (written <= 0 || written >= (int)sizeof(s_scanner_id)) {
        s_scanner_id[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Scanner ID set to %s", s_scanner_id);
    return ESP_OK;
}

esp_err_t device_info_init(void)
{
    return ensure_scanner_id();
}

const char *device_info_scanner_id(void)
{
    if (ensure_scanner_id() != ESP_OK) {
        return "scanner-unknown";
    }
    return s_scanner_id;
}

esp_err_t device_info_get_scanner_id(char *out, size_t len)
{
    if (!out || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_scanner_id();
    if (err != ESP_OK) {
        return err;
    }
    strlcpy(out, s_scanner_id, len);
    return ESP_OK;
}
