#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_scan_init(void);
esp_err_t ble_scan_start(void);

#ifdef __cplusplus
}
#endif
