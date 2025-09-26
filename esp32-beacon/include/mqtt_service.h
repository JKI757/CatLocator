#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*mqtt_service_message_cb_t)(const char *topic, const char *payload, size_t len, void *ctx);

esp_err_t mqtt_service_init(void);
esp_err_t mqtt_service_start(void);
esp_err_t mqtt_service_publish(const char *topic, const char *payload);
esp_err_t mqtt_service_register_handler(mqtt_service_message_cb_t cb, void *ctx);
esp_err_t mqtt_service_subscribe(const char *topic, int qos);

#ifdef __cplusplus
}
#endif
