#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ble_scan_init(void);
esp_err_t ble_scan_start(void);
void ble_scan_set_debug(bool enable);
bool ble_scan_debug_enabled(void);

#ifdef __cplusplus
}
#endif
