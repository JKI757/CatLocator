#include "serial_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config_portal.h"
#include "ble_scan.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_dev.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "esp_vfs_usb_serial_jtag.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#endif

static const char *TAG = "serial_cli";

#if CONFIG_ESP_CONSOLE_UART
#ifndef CLI_UART_NUM
#  ifdef CONFIG_ESP_CONSOLE_UART_NUM
#    define CLI_UART_NUM CONFIG_ESP_CONSOLE_UART_NUM
#  else
#    define CLI_UART_NUM UART_NUM_0
#  endif
#endif

#ifndef CONFIG_ESP_CONSOLE_UART_BAUDRATE
#define CONFIG_ESP_CONSOLE_UART_BAUDRATE 115200
#endif
#endif

static bool s_cli_started;

static void configure_console(void)
{
    /* Disable buffering so input/output appears immediately */
    setvbuf(stdin, NULL, _IONBF, 0);
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    esp_vfs_dev_usb_serial_jtag_use_driver();
    esp_vfs_dev_usb_serial_jtag_set_rx_line_endings(ESP_LINE_ENDINGS_CRLF);
    esp_vfs_dev_usb_serial_jtag_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
#elif CONFIG_ESP_CONSOLE_UART
    const uart_config_t uart_config = {
        .baud_rate = CONFIG_ESP_CONSOLE_UART_BAUDRATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    (void)uart_driver_install(CLI_UART_NUM, 256, 0, 0, NULL, 0);
    (void)uart_param_config(CLI_UART_NUM, &uart_config);

    esp_vfs_dev_uart_use_driver(CLI_UART_NUM);
    esp_vfs_dev_uart_port_set_rx_line_endings(CLI_UART_NUM, ESP_LINE_ENDINGS_CRLF);
    esp_vfs_dev_uart_port_set_tx_line_endings(CLI_UART_NUM, ESP_LINE_ENDINGS_CRLF);
#else
#warning "Serial CLI: no console transport configured"
#endif
}

static void print_config(const config_portal_config_t *cfg)
{
    printf("\nCurrent configuration:\n");
    printf("  Wi-Fi SSID : %s\n", cfg->wifi_ssid[0] ? cfg->wifi_ssid : "<unset>");
    printf("  MQTT URI   : %s\n", cfg->mqtt_uri[0] ? cfg->mqtt_uri : "<unset>");
    printf("  MQTT User  : %s\n", cfg->mqtt_username[0] ? cfg->mqtt_username : "<unset>");
    printf("  Beacon ID  : %s\n", cfg->beacon_id[0] ? cfg->beacon_id : "<unset>");
    printf("  Location   : (%.2f, %.2f, %.2f)\n", cfg->location_x, cfg->location_y, cfg->location_z);
    printf("  Interval   : %u ms\n\n", (unsigned)cfg->reporting_interval_ms);
}

static void trim_newline(char *str)
{
    if (!str) {
        return;
    }

    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == '\n' || str[len - 1] == '\r')) {
        str[--len] = '\0';
    }
}

static bool read_line(const char *prompt, char *out, size_t out_len)
{
    if (prompt) {
        printf("%s", prompt);
        fflush(stdout);
    }

    if (!fgets(out, out_len, stdin)) {
        return false;
    }

    trim_newline(out);
    return true;
}

static void show_config(void)
{
    config_portal_config_t cfg;
    if (config_portal_get_config(&cfg) != ESP_OK) {
        printf("Failed to read configuration\n");
        return;
    }

    print_config(&cfg);
}

static void set_wifi_credentials(void)
{
    char ssid[sizeof(((config_portal_config_t *)0)->wifi_ssid)];
    char password[sizeof(((config_portal_config_t *)0)->wifi_password)];

    if (!read_line("Enter Wi-Fi SSID: ", ssid, sizeof(ssid))) {
        printf("Input error\n");
        return;
    }

    if (!read_line("Enter Wi-Fi password (leave empty for open): ", password, sizeof(password))) {
        printf("Input error\n");
        return;
    }

    config_portal_config_t cfg;
    if (config_portal_get_config(&cfg) != ESP_OK) {
        printf("Failed to read configuration\n");
        return;
    }

    strlcpy(cfg.wifi_ssid, ssid, sizeof(cfg.wifi_ssid));
    strlcpy(cfg.wifi_password, password, sizeof(cfg.wifi_password));

    if (config_portal_set_config(&cfg) == ESP_OK) {
        printf("Wi-Fi credentials updated\n");
    } else {
        printf("Failed to persist Wi-Fi credentials\n");
    }
}

static void set_mqtt_settings(void)
{
    char uri[sizeof(((config_portal_config_t *)0)->mqtt_uri)];
    char username[sizeof(((config_portal_config_t *)0)->mqtt_username)];
    char password[sizeof(((config_portal_config_t *)0)->mqtt_password)];

    if (!read_line("Enter MQTT broker URI: ", uri, sizeof(uri))) {
        printf("Input error\n");
        return;
    }

    if (!read_line("MQTT username (optional): ", username, sizeof(username))) {
        printf("Input error\n");
        return;
    }

    if (!read_line("MQTT password (optional): ", password, sizeof(password))) {
        printf("Input error\n");
        return;
    }

    config_portal_config_t cfg;
    if (config_portal_get_config(&cfg) != ESP_OK) {
        printf("Failed to read configuration\n");
        return;
    }

    strlcpy(cfg.mqtt_uri, uri, sizeof(cfg.mqtt_uri));
    strlcpy(cfg.mqtt_username, username, sizeof(cfg.mqtt_username));
    strlcpy(cfg.mqtt_password, password, sizeof(cfg.mqtt_password));

    if (config_portal_set_config(&cfg) == ESP_OK) {
        printf("MQTT settings updated\n");
    } else {
        printf("Failed to persist MQTT settings\n");
    }
}

static void set_beacon_info(void)
{
    char beacon_id[sizeof(((config_portal_config_t *)0)->beacon_id)];
    char location_x[16];
    char location_y[16];
    char location_z[16];

    if (!read_line("Enter beacon ID: ", beacon_id, sizeof(beacon_id))) {
        printf("Input error\n");
        return;
    }

    if (!read_line("Location X (meters): ", location_x, sizeof(location_x))) {
        printf("Input error\n");
        return;
    }

    if (!read_line("Location Y (meters): ", location_y, sizeof(location_y))) {
        printf("Input error\n");
        return;
    }

    if (!read_line("Location Z (meters): ", location_z, sizeof(location_z))) {
        printf("Input error\n");
        return;
    }

    config_portal_config_t cfg;
    if (config_portal_get_config(&cfg) != ESP_OK) {
        printf("Failed to read configuration\n");
        return;
    }

    strlcpy(cfg.beacon_id, beacon_id, sizeof(cfg.beacon_id));
    cfg.location_x = strtof(location_x, NULL);
    cfg.location_y = strtof(location_y, NULL);
    cfg.location_z = strtof(location_z, NULL);

    if (config_portal_set_config(&cfg) == ESP_OK) {
        printf("Beacon metadata updated\n");
    } else {
        printf("Failed to persist beacon metadata\n");
    }
}

static void clear_config(void)
{
    config_portal_config_t cfg = {0};
    cfg.reporting_interval_ms = 5000;

    if (config_portal_set_config(&cfg) == ESP_OK) {
        printf("Configuration cleared\n");
    } else {
        printf("Failed to clear configuration\n");
    }
}

static void print_menu(void)
{
    printf("\nCatLocator Provisioning Menu\n");
    printf("--------------------------------\n");
    printf("1) Show current configuration\n");
    printf("2) Set Wi-Fi credentials\n");
    printf("3) Set MQTT settings\n");
    printf("4) Set beacon ID and location\n");
    printf("5) Clear configuration\n");
    printf("6) Toggle BLE debug logging (currently %s)\n", ble_scan_debug_enabled() ? "ON" : "OFF");
    printf("h) Show this menu\n");
    printf("q) Quit menu (CLI remains active)\n\n");
}

static void cli_task(void *param)
{
    (void)param;

    print_menu();

    char input[128];

    while (true) {
        if (!read_line("Select option: ", input, sizeof(input))) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (strlen(input) == 0) {
            continue;
        }

        switch (input[0]) {
            case '1':
                show_config();
                break;
            case '2':
                set_wifi_credentials();
                break;
            case '3':
                set_mqtt_settings();
                break;
            case '4':
                set_beacon_info();
                break;
            case '5':
                clear_config();
                break;
            case '6': {
                bool new_state = !ble_scan_debug_enabled();
                ble_scan_set_debug(new_state);
                printf("BLE debug logging %s\n", new_state ? "enabled" : "disabled");
                break;
            }
            case 'h':
            case 'H':
                print_menu();
                break;
            case 'q':
            case 'Q':
                printf("Exiting menu. Type 'h' to show options again.\n");
                break;
            default:
                printf("Unknown option '%s'. Type 'h' for help.\n", input);
                break;
        }
    }
}

esp_err_t serial_cli_init(void)
{
    if (s_cli_started) {
        return ESP_OK;
    }

    configure_console();

    BaseType_t task = xTaskCreate(cli_task, "cli", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    if (task != pdPASS) {
        ESP_LOGE(TAG, "Failed to start CLI task");
        return ESP_ERR_NO_MEM;
    }

    s_cli_started = true;
    ESP_LOGI(TAG, "Serial CLI started");
    return ESP_OK;
}
