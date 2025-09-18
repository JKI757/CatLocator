#include "lora_bridge.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "lora_bridge";

static bool s_initialized;

esp_err_t lora_bridge_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_CATLOCATOR_LORA_MOSI_GPIO,
        .miso_io_num = CONFIG_CATLOCATOR_LORA_MISO_GPIO,
        .sclk_io_num = CONFIG_CATLOCATOR_LORA_SCLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0,
    };

    spi_host_device_t host = CONFIG_CATLOCATOR_LORA_SPI_HOST - 1;
    if (host < SPI1_HOST || host > SPI3_HOST) {
        host = SPI2_HOST;
    }

    esp_err_t err = spi_bus_initialize(host, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Failed to init SPI bus: %s", esp_err_to_name(err));
        return err;
    }

    gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << CONFIG_CATLOCATOR_LORA_RESET_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&rst_cfg), TAG, "reset gpio config failed");
    gpio_set_level(CONFIG_CATLOCATOR_LORA_RESET_GPIO, 1);

    ESP_LOGI(TAG, "LoRa bridge initialized (host=%d)", host);
    s_initialized = true;
    return ESP_OK;
}
