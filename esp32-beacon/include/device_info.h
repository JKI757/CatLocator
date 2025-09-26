#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t device_info_init(void);
const char *device_info_scanner_id(void);
esp_err_t device_info_get_scanner_id(char *out, size_t len);

#ifdef __cplusplus
}
#endif

