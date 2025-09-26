#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char mqtt_uri[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char beacon_id[32];
    float location_x;
    float location_y;
    float location_z;
    uint32_t reporting_interval_ms;
} config_portal_config_t;

typedef void (*config_portal_listener_t)(const config_portal_config_t *config, void *ctx);

esp_err_t config_portal_init(void);
esp_err_t config_portal_start_async(void);
esp_err_t config_portal_register_listener(config_portal_listener_t cb, void *ctx);
esp_err_t config_portal_get_config(config_portal_config_t *out);
bool config_portal_has_credentials(void);
esp_err_t config_portal_set_config(const config_portal_config_t *cfg);

#ifdef __cplusplus
}
#endif
