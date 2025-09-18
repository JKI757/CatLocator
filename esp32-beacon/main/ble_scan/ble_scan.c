#include "ble_scan.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config_portal.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mqtt_service.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ble_scan";

static TaskHandle_t s_host_task;
static bool s_scan_started;
static config_portal_config_t s_latest_cfg;
static uint32_t s_reporting_interval_ms = 5000;

typedef struct {
    uint8_t addr[6];
    int64_t last_publish_us;
    bool in_use;
} tag_cache_entry_t;

#define TAG_CACHE_MAX 32
static tag_cache_entry_t s_tag_cache[TAG_CACHE_MAX];

static void ble_host_task(void *param);
static void start_scan(void);
static int gap_event_handler(struct ble_gap_event *event, void *arg);
static void format_address(const uint8_t *addr, char *out, size_t len);
static void publish_reading(const struct ble_gap_disc_desc *desc);
static void config_listener(const config_portal_config_t *cfg, void *ctx);
static tag_cache_entry_t *find_cache_entry(const uint8_t *addr);
static tag_cache_entry_t *allocate_cache_entry(const uint8_t *addr);
static bool should_publish(tag_cache_entry_t *entry, int64_t now_us);

esp_err_t ble_scan_init(void)
{
    ESP_LOGI(TAG, "Initializing NimBLE stack");

    ESP_ERROR_CHECK(config_portal_register_listener(config_listener, NULL));

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init failed");

    ble_hs_cfg.sync_cb = start_scan;
    ble_hs_cfg.reset_cb = NULL;

    if (!s_host_task) {
        nimble_port_freertos_init(ble_host_task);
    }

    memset(s_tag_cache, 0, sizeof(s_tag_cache));

    return ESP_OK;
}

esp_err_t ble_scan_start(void)
{
    if (s_scan_started) {
        return ESP_OK;
    }

    if (ble_gap_disc_active()) {
        return ESP_OK;
    }

    start_scan();
    return ESP_OK;
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void start_scan(void)
{
    struct ble_gap_disc_params params = {
        .itvl = 0x00A0,
        .window = 0x0050,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 1,
    };

    int rc = ble_gap_disc(0, BLE_HS_FOREVER, &params, gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to start scanning: %d", rc);
    } else {
        s_scan_started = true;
        ESP_LOGI(TAG, "BLE scanning started");
    }
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            publish_reading(&event->disc);
            break;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            s_scan_started = false;
            start_scan();
            break;
        default:
            break;
    }
    return 0;
}

static void config_listener(const config_portal_config_t *cfg, void *ctx)
{
    if (!cfg) {
        return;
    }

    s_latest_cfg = *cfg;
    if (cfg->reporting_interval_ms > 0) {
        s_reporting_interval_ms = cfg->reporting_interval_ms;
    }
}

static tag_cache_entry_t *find_cache_entry(const uint8_t *addr)
{
    for (size_t i = 0; i < TAG_CACHE_MAX; ++i) {
        if (s_tag_cache[i].in_use && memcmp(s_tag_cache[i].addr, addr, 6) == 0) {
            return &s_tag_cache[i];
        }
    }
    return NULL;
}

static tag_cache_entry_t *allocate_cache_entry(const uint8_t *addr)
{
    for (size_t i = 0; i < TAG_CACHE_MAX; ++i) {
        if (!s_tag_cache[i].in_use) {
            s_tag_cache[i].in_use = true;
            memcpy(s_tag_cache[i].addr, addr, 6);
            s_tag_cache[i].last_publish_us = 0;
            return &s_tag_cache[i];
        }
    }

    size_t oldest_index = 0;
    int64_t oldest_time = s_tag_cache[0].last_publish_us;
    for (size_t i = 1; i < TAG_CACHE_MAX; ++i) {
        if (!s_tag_cache[i].in_use || s_tag_cache[i].last_publish_us < oldest_time) {
            oldest_index = i;
            oldest_time = s_tag_cache[i].last_publish_us;
        }
    }

    s_tag_cache[oldest_index].in_use = true;
    memcpy(s_tag_cache[oldest_index].addr, addr, 6);
    s_tag_cache[oldest_index].last_publish_us = 0;
    return &s_tag_cache[oldest_index];
}

static bool should_publish(tag_cache_entry_t *entry, int64_t now_us)
{
    if (!entry) {
        return true;
    }
    if (s_reporting_interval_ms == 0) {
        return true;
    }
    int64_t interval_us = (int64_t)s_reporting_interval_ms * 1000;
    return (now_us - entry->last_publish_us) >= interval_us;
}

static void publish_reading(const struct ble_gap_disc_desc *desc)
{
    if (s_latest_cfg.beacon_id[0] == '\0') {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    tag_cache_entry_t *entry = find_cache_entry(desc->addr.val);
    if (!entry) {
        entry = allocate_cache_entry(desc->addr.val);
    }
    if (!entry || !should_publish(entry, now_us)) {
        return;
    }
    entry->last_publish_us = now_us;

    char addr[18];
    format_address(desc->addr.val, addr, sizeof(addr));

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) != 0) {
        ESP_LOGW(TAG, "Failed to parse advertisement fields");
    }

    char tag_name[64] = "";
    if (fields.name != NULL && fields.name_len > 0) {
        size_t len = fields.name_len < sizeof(tag_name) - 1 ? fields.name_len : sizeof(tag_name) - 1;
        memcpy(tag_name, fields.name, len);
        tag_name[len] = '\0';
    }

    uint16_t manufacturer_id = 0xFFFF;
    char manufacturer_data[64] = "";
    if (fields.mfg_data != NULL && fields.mfg_data_len >= 2) {
        manufacturer_id = ((uint16_t)fields.mfg_data[1] << 8) | fields.mfg_data[0];
        size_t copy_len = fields.mfg_data_len - 2;
        if (copy_len > sizeof(manufacturer_data) / 2) {
            copy_len = sizeof(manufacturer_data) / 2;
        }
        char *ptr = manufacturer_data;
        for (size_t i = 0; i < copy_len; ++i) {
            sprintf(ptr, "%02X", fields.mfg_data[i + 2]);
            ptr += 2;
        }
        *ptr = '\0';
    }

    time_t now = now_us / 1000000;
    struct tm tm_info = {0};
    gmtime_r(&now, &tm_info);

    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &tm_info);

    char topic[128];
    snprintf(topic, sizeof(topic), "beacons/%s/readings", s_latest_cfg.beacon_id);

    char payload[512];
    int written = snprintf(payload, sizeof(payload),
                           "{\"beacon_id\":\"%s\",\"tag_id\":\"%s\",\"rssi\":%d,\"timestamp\":\"%s\",\"beacon_location\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}",
                           s_latest_cfg.beacon_id,
                           tag_name[0] ? tag_name : addr,
                           desc->rssi,
                           timestamp,
                           s_latest_cfg.location_x,
                           s_latest_cfg.location_y,
                           s_latest_cfg.location_z);

    if (manufacturer_id != 0xFFFF) {
        written += snprintf(payload + written, sizeof(payload) - written,
                             ",\"manufacturer_id\":%u",
                             manufacturer_id);
    }

    if (manufacturer_data[0] != '\0') {
        written += snprintf(payload + written, sizeof(payload) - written,
                             ",\"manufacturer_data\":\"%s\"",
                             manufacturer_data);
    }

    if (fields.tx_pwr_lvl_is_present) {
        written += snprintf(payload + written, sizeof(payload) - written,
                             ",\"tx_power\":%d",
                             fields.tx_pwr_lvl);
    }

    written += snprintf(payload + written, sizeof(payload) - written, "}");

    if (written < 0 || written >= (int)sizeof(payload)) {
        ESP_LOGW(TAG, "Payload truncated for tag %s", addr);
    }

    esp_err_t err = mqtt_service_publish(topic, payload);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to publish reading: %s", esp_err_to_name(err));
    }
}

static void format_address(const uint8_t *addr, char *out, size_t len)
{
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}
