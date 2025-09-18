#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t netmgr_init(void);
esp_err_t netmgr_start(void);

#ifdef __cplusplus
}
#endif
