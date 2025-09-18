#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "ble_scan.h"
#include "config_portal.h"
#include "lora_bridge.h"
#include "mqtt_service.h"
#include "netmgr.h"
#include "time_sync.h"

static const char *TAG = "app_main";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "CatLocator beacon firmware starting up");

    init_nvs();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(config_portal_init());
    ESP_ERROR_CHECK(netmgr_init());
    ESP_ERROR_CHECK(time_sync_init());
    ESP_ERROR_CHECK(mqtt_service_init());
    ESP_ERROR_CHECK(ble_scan_init());
    ESP_ERROR_CHECK(lora_bridge_init());

    ESP_ERROR_CHECK(config_portal_start_async());
    ESP_ERROR_CHECK(netmgr_start());
    ESP_ERROR_CHECK(time_sync_start());
    ESP_ERROR_CHECK(mqtt_service_start());
    ESP_ERROR_CHECK(ble_scan_start());

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
