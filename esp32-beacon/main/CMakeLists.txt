set(srcs
    "app_main.c"
    "config_portal/config_portal.c"
    "netmgr/netmgr.c"
    "time_sync/time_sync.c"
    "mqtt_service/mqtt_service.c"
    "mdns_discovery/mdns_discovery.c"
    "ble_scan/ble_scan.c"
    "lora_bridge/lora_bridge.c"
    "serial_cli/serial_cli.c"
)

set(reqs esp_http_server esp_wifi esp_netif esp_event nvs_flash json mqtt bt esp_timer lwip driver vfs)

idf_build_get_property(__mdns_dir COMPONENT_DIR mdns)
if(__mdns_dir)
    list(APPEND reqs mdns)
endif()

idf_component_register(SRCS ${srcs}
                      INCLUDE_DIRS "." "../include"
                      REQUIRES ${reqs})
