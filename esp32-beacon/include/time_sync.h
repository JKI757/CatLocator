#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t time_sync_init(void);
esp_err_t time_sync_start(void);

#ifdef __cplusplus
}
#endif
