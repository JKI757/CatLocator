#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t mqtt_service_init(void);
esp_err_t mqtt_service_start(void);
esp_err_t mqtt_service_publish(const char *topic, const char *payload);

#ifdef __cplusplus
}
#endif
