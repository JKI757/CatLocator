#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char uri[128];
    char hostname[64];
    uint16_t port;
    bool tls;
} mdns_discovery_info_t;

typedef void (*mdns_discovery_listener_t)(const mdns_discovery_info_t *info, void *ctx);

esp_err_t mdns_discovery_init(void);
esp_err_t mdns_discovery_start(void);
esp_err_t mdns_discovery_register_listener(mdns_discovery_listener_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

