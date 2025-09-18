#include "time_sync.h"

#include "esp_log.h"
#include "esp_sntp.h"

static const char *TAG = "time_sync";

esp_err_t time_sync_init(void)
{
    ESP_LOGI(TAG, "Initializing SNTP client");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    return ESP_OK;
}

esp_err_t time_sync_start(void)
{
    ESP_LOGI(TAG, "Starting SNTP sync");
    if (!esp_sntp_enabled()) {
        esp_sntp_init();
    } else {
        esp_sntp_restart();
    }
    return ESP_OK;
}
