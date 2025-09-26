#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "ble_scan.h"
#include "config_portal.h"
#include "lora_bridge.h"
#include "mqtt_service.h"
#include "netmgr.h"
#include "serial_cli.h"
#include "time_sync.h"

static const char *TAG = "app_main";

static void log_error(const char *what, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
    }
}

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

    bool config_ready = true;
    bool netmgr_ready = true;
    bool time_sync_ready = true;
    bool mqtt_ready = true;
    bool ble_ready = true;
    bool lora_ready = true;

    esp_err_t err = config_portal_init();
    if (err != ESP_OK) {
        log_error("config_portal_init", err);
        config_ready = false;
    }

    err = netmgr_init();
    if (err != ESP_OK) {
        log_error("netmgr_init", err);
        netmgr_ready = false;
    }

    err = time_sync_init();
    if (err != ESP_OK) {
        log_error("time_sync_init", err);
        time_sync_ready = false;
    }

    err = mqtt_service_init();
    if (err != ESP_OK) {
        log_error("mqtt_service_init", err);
        mqtt_ready = false;
    }

    err = ble_scan_init();
    if (err != ESP_OK) {
        log_error("ble_scan_init", err);
        ble_ready = false;
    }

    err = lora_bridge_init();
    if (err != ESP_OK) {
        log_error("lora_bridge_init", err);
        lora_ready = false;
    }

    if (config_ready) {
        log_error("config_portal_start_async", config_portal_start_async());
    }

    if (netmgr_ready) {
        log_error("netmgr_start", netmgr_start());
    }

    if (time_sync_ready) {
        log_error("time_sync_start", time_sync_start());
    }

    if (mqtt_ready) {
        log_error("mqtt_service_start", mqtt_service_start());
    }

    if (ble_ready) {
        log_error("ble_scan_start", ble_scan_start());
    }

    log_error("serial_cli_init", serial_cli_init());

    if (!config_ready || !config_portal_has_credentials()) {
        ESP_LOGW(TAG, "Credentials not provisioned. Use the serial CLI or HTTP portal to configure the device.");
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
