#include "ble_scan.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "config_portal.h"
#include "device_info.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "mqtt_service.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "nimble/hci_common.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "ble_scan";

static TaskHandle_t s_host_task;
static bool s_scan_started;
static config_portal_config_t s_latest_cfg;
static uint32_t s_reporting_interval_ms = 5000;
static bool s_debug_logging;
static int64_t s_last_missing_beacon_log_us;

typedef struct {
    uint8_t addr[6];
    int64_t last_publish_us;
    bool in_use;
} tag_cache_entry_t;

#define TAG_CACHE_MAX 32
static tag_cache_entry_t s_tag_cache[TAG_CACHE_MAX];

typedef struct {
    char topic[160];
    char payload[512];
} ble_publish_msg_t;

static QueueHandle_t s_publish_queue;
static TaskHandle_t s_publish_task;

static void ble_host_task(void *param);
static void start_scan(void);
static int gap_event_handler(struct ble_gap_event *event, void *arg);
static void format_address(const uint8_t *addr, char *out, size_t len);
static void publish_reading(const struct ble_gap_disc_desc *desc);
static void publish_discovery(const struct ble_gap_disc_desc *desc,
                              const char *addr,
                              const char *tag_name,
                              int rssi,
                              uint16_t manufacturer_id,
                              const char *manufacturer_data,
                              bool tx_power_present,
                              int tx_power_dbm,
                              const char *event_type,
                              int64_t timestamp_us);
static void publish_task(void *param);
static void enqueue_publish(const char *topic, const char *payload);
static void config_listener(const config_portal_config_t *cfg, void *ctx);
static tag_cache_entry_t *find_cache_entry(const uint8_t *addr);
static tag_cache_entry_t *allocate_cache_entry(const uint8_t *addr);
static bool should_publish(tag_cache_entry_t *entry, int64_t now_us);
static void debug_log_advert(const struct ble_gap_disc_desc *desc);
static void schedule_debug_log(const struct ble_gap_disc_desc *desc);
static void debug_log_task(void *param);
static const char *event_type_str(uint8_t event_type);

typedef struct {
    struct ble_gap_disc_desc desc;
    uint8_t data[BLE_HS_ADV_MAX_SZ];
} debug_adv_t;

static QueueHandle_t s_debug_queue;
static TaskHandle_t s_debug_task;

esp_err_t ble_scan_init(void)
{
    ESP_LOGI(TAG, "Initializing NimBLE stack");

    esp_err_t err = config_portal_register_listener(config_listener, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config listener registration failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble init failed");

    s_debug_logging = false;

    if (!s_debug_queue) {
        s_debug_queue = xQueueCreate(16, sizeof(debug_adv_t));
        if (!s_debug_queue) {
            ESP_LOGE(TAG, "Failed to create debug queue");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_debug_task) {
        BaseType_t created = xTaskCreate(debug_log_task, "ble_debug", 4096, NULL, tskIDLE_PRIORITY + 2, &s_debug_task);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create debug log task");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_publish_queue) {
        s_publish_queue = xQueueCreate(16, sizeof(ble_publish_msg_t));
        if (!s_publish_queue) {
            ESP_LOGE(TAG, "Failed to create publish queue");
            return ESP_ERR_NO_MEM;
        }
    }

    if (!s_publish_task) {
        BaseType_t created = xTaskCreate(publish_task, "ble_publish", 4096, NULL, tskIDLE_PRIORITY + 2, &s_publish_task);
        if (created != pdPASS) {
            ESP_LOGE(TAG, "Failed to create publish task");
            return ESP_ERR_NO_MEM;
        }
    }

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
        .itvl = 0x0080,
        .window = 0x0080,
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 0,
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
            if (s_debug_logging) {
                schedule_debug_log(&event->disc);
            }
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

void ble_scan_set_debug(bool enable)
{
    s_debug_logging = enable;
    if (enable && s_debug_queue) {
        xQueueReset(s_debug_queue);
    }
    ESP_LOGI(TAG, "BLE debug logging %s", enable ? "enabled" : "disabled");
}

bool ble_scan_debug_enabled(void)
{
    return s_debug_logging;
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
    bool fields_valid = (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) == 0);

    char tag_name[64] = "";
    if (fields_valid && fields.name != NULL && fields.name_len > 0) {
        size_t len = fields.name_len < sizeof(tag_name) - 1 ? fields.name_len : sizeof(tag_name) - 1;
        memcpy(tag_name, fields.name, len);
        tag_name[len] = '\0';
    }

    uint16_t manufacturer_id = 0xFFFF;
    char manufacturer_data[64] = "";
    if (fields_valid && fields.mfg_data != NULL && fields.mfg_data_len >= 2) {
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

    if (s_latest_cfg.beacon_id[0] == '\0') {
        if (now_us - s_last_missing_beacon_log_us > 5 * 1000 * 1000) {
            ESP_LOGW(TAG, "Beacon ID not configured; sending discovery inventory only");
            s_last_missing_beacon_log_us = now_us;
        }

        publish_discovery(desc,
                          addr,
                          tag_name,
                          desc->rssi,
                          manufacturer_id,
                          manufacturer_data,
                          fields_valid && fields.tx_pwr_lvl_is_present,
                          fields.tx_pwr_lvl,
                          event_type_str(desc->event_type),
                          now_us);
        return;
    }

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

    if (fields_valid && fields.tx_pwr_lvl_is_present) {
        written += snprintf(payload + written, sizeof(payload) - written,
                             ",\"tx_power\":%d",
                             fields.tx_pwr_lvl);
    }

    written += snprintf(payload + written, sizeof(payload) - written, "}");

    if (written < 0 || written >= (int)sizeof(payload)) {
        ESP_LOGW(TAG, "Payload truncated for tag %s", addr);
    }

    enqueue_publish(topic, payload);
}

static void publish_discovery(const struct ble_gap_disc_desc *desc,
                              const char *addr,
                              const char *tag_name,
                              int rssi,
                              uint16_t manufacturer_id,
                              const char *manufacturer_data,
                              bool tx_power_present,
                              int tx_power_dbm,
                              const char *event_type,
                              int64_t timestamp_us)
{
    (void)desc;
    const char *scanner_id = device_info_scanner_id();

    char topic[160];
    snprintf(topic, sizeof(topic), "scanners/%s/inventory", scanner_id);

    time_t seconds = timestamp_us / 1000000;
    struct tm tm_info = {0};
    gmtime_r(&seconds, &tm_info);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &tm_info);

    const char *name_for_payload = (tag_name && tag_name[0]) ? tag_name : addr;

    char payload[512];
    int written = snprintf(payload, sizeof(payload),
                           "{\"scanner_id\":\"%s\",\"tag_address\":\"%s\",\"tag_name\":\"%s\",\"rssi\":%d,\"timestamp\":\"%s\"",
                           scanner_id,
                           addr,
                           name_for_payload,
                           rssi,
                           timestamp);

    if (manufacturer_id != 0xFFFF) {
        written += snprintf(payload + written, sizeof(payload) - written,
                             ",\"manufacturer_id\":%u",
                             manufacturer_id);
    }
    if (manufacturer_data && manufacturer_data[0] != '\0') {
        written += snprintf(payload + written, sizeof(payload) - written,
                             ",\"manufacturer_data\":\"%s\"",
                             manufacturer_data);
    }
    if (tx_power_present) {
        written += snprintf(payload + written, sizeof(payload) - written,
                             ",\"tx_power\":%d",
                             tx_power_dbm);
    }
    if (event_type && event_type[0] != '\0') {
        written += snprintf(payload + written, sizeof(payload) - written,
                             ",\"event_type\":\"%s\"",
                             event_type);
    }

    written += snprintf(payload + written, sizeof(payload) - written, "}");

    if (written < 0 || written >= (int)sizeof(payload)) {
        ESP_LOGW(TAG, "Discovery payload truncated for %s", addr);
    }

    enqueue_publish(topic, payload);
}

static void format_address(const uint8_t *addr, char *out, size_t len)
{
    snprintf(out, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
}

static void debug_log_advert(const struct ble_gap_disc_desc *desc)
{
    char addr[18];
    format_address(desc->addr.val, addr, sizeof(addr));

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    if (ble_hs_adv_parse_fields(&fields, desc->data, desc->length_data) != 0) {
        ESP_LOGD(TAG, "Debug: failed to parse advertisement fields for %s (corrupted data)", addr);
        return;
    }

    char name[64] = "";
    if (fields.name && fields.name_len > 0) {
        size_t len = fields.name_len < sizeof(name) - 1 ? fields.name_len : sizeof(name) - 1;
        memcpy(name, fields.name, len);
        name[len] = '\0';
    }

    uint16_t manufacturer_id = 0xFFFF;
    char manufacturer_data[64] = "";
    if (fields.mfg_data && fields.mfg_data_len >= 2) {
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

    const char *event_type = event_type_str(desc->event_type);

    char raw_data[2 * 64 + 4];
    size_t raw_len = desc->length_data;
    size_t max_bytes = sizeof(raw_data) / 2;
    if (raw_len > max_bytes) {
        raw_len = max_bytes;
    }
    char *raw_ptr = raw_data;
    for (size_t i = 0; i < raw_len; ++i) {
        sprintf(raw_ptr, "%02X", desc->data[i]);
        raw_ptr += 2;
    }
    if (raw_len < desc->length_data && (size_t)(raw_ptr - raw_data) <= sizeof(raw_data) - 4) {
        strcpy(raw_ptr, "...");
    } else {
        *raw_ptr = '\0';
    }

    ESP_LOGI(TAG,
             "Debug ADV addr=%s type=%s rssi=%d name=%s tx_power=%s raw=%s",
             addr,
             event_type,
             desc->rssi,
             name[0] ? name : "<unknown>",
             fields.tx_pwr_lvl_is_present ? "present" : "n/a",
             raw_data);

    if (manufacturer_id != 0xFFFF) {
        ESP_LOGI(TAG,
                 "  manufacturer=0x%04X data=%s",
                 manufacturer_id,
                 manufacturer_data[0] ? manufacturer_data : "<none>");

        if (manufacturer_id == 0x004C && fields.mfg_data_len >= 4) {
            const uint8_t *mfg = fields.mfg_data;
            uint8_t type = mfg[2];
            uint8_t subtype = mfg[3];
            if (type == 0x02 && subtype == 0x15 && fields.mfg_data_len >= 4 + 16 + 2 + 2 + 1) {
                const uint8_t *uuid = &mfg[4];
                uint16_t major = ((uint16_t)mfg[20] << 8) | mfg[21];
                uint16_t minor = ((uint16_t)mfg[22] << 8) | mfg[23];
                int8_t tx = (int8_t)mfg[24];
                ESP_LOGI(TAG,
                         "    iBeacon UUID=%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X major=%u minor=%u tx=%d",
                         uuid[0], uuid[1], uuid[2], uuid[3],
                         uuid[4], uuid[5], uuid[6], uuid[7],
                         uuid[8], uuid[9], uuid[10], uuid[11],
                         uuid[12], uuid[13], uuid[14], uuid[15],
                         major,
                         minor,
                         tx);
            } else {
                ESP_LOGI(TAG, "    Apple AD type=0x%02X subtype=0x%02X", type, subtype);
            }
        }
    }

    if (fields.uuids16 && fields.num_uuids16 > 0) {
        for (uint8_t i = 0; i < fields.num_uuids16; ++i) {
            ESP_LOGI(TAG, "  uuid16=0x%04X", fields.uuids16[i].value);
        }
    }

    if (fields.uuids128 && fields.num_uuids128 > 0) {
        for (uint8_t i = 0; i < fields.num_uuids128; ++i) {
            const ble_uuid128_t *uuid = &fields.uuids128[i];
            ESP_LOGI(TAG,
                     "  uuid128=%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                     uuid->value[15], uuid->value[14], uuid->value[13], uuid->value[12],
                     uuid->value[11], uuid->value[10], uuid->value[9], uuid->value[8],
                     uuid->value[7], uuid->value[6], uuid->value[5], uuid->value[4],
                     uuid->value[3], uuid->value[2], uuid->value[1], uuid->value[0]);
        }
    }
}

static void publish_task(void *param)
{
    ble_publish_msg_t msg;
    while (true) {
        if (xQueueReceive(s_publish_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        esp_err_t err = mqtt_service_publish(msg.topic, msg.payload);
        if (err == ESP_OK) {
            if (s_debug_logging) {
                ESP_LOGD(TAG, "Published MQTT message topic=%s", msg.topic);
            }
        } else if (err == ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "MQTT not ready; retrying topic=%s", msg.topic);
            vTaskDelay(pdMS_TO_TICKS(500));
            enqueue_publish(msg.topic, msg.payload);
        } else {
            ESP_LOGW(TAG, "Failed to publish MQTT message (topic=%s err=%s)", msg.topic, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(250));
        }
    }
}

static void enqueue_publish(const char *topic, const char *payload)
{
    if (!topic || !payload || !s_publish_queue) {
        return;
    }

    ble_publish_msg_t msg = {0};
    strlcpy(msg.topic, topic, sizeof(msg.topic));
    strlcpy(msg.payload, payload, sizeof(msg.payload));

    if (xQueueSend(s_publish_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Publish queue full; dropping message for %s", msg.topic);
    }
}

static const char *event_type_str(uint8_t event_type)
{
    switch (event_type) {
        case BLE_HCI_ADV_RPT_EVTYPE_ADV_IND:
            return "ADV_IND";
        case BLE_HCI_ADV_RPT_EVTYPE_DIR_IND:
            return "ADV_DIRECT_IND";
        case BLE_HCI_ADV_RPT_EVTYPE_SCAN_IND:
            return "ADV_SCAN_IND";
        case BLE_HCI_ADV_RPT_EVTYPE_NONCONN_IND:
            return "ADV_NONCONN_IND";
        case BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP:
            return "SCAN_RSP";
        default:
            break;
    }
    return "UNKNOWN";
}

static void schedule_debug_log(const struct ble_gap_disc_desc *desc)
{
    if (!s_debug_queue) {
        return;
    }

    debug_adv_t entry = {0};
    entry.desc = *desc;
    size_t copy_len = desc->length_data;
    if (copy_len > sizeof(entry.data)) {
        copy_len = sizeof(entry.data);
    }
    memcpy(entry.data, desc->data, copy_len);
    entry.desc.data = entry.data;
    entry.desc.length_data = copy_len;

    xQueueSend(s_debug_queue, &entry, 0);
}

static void debug_log_task(void *param)
{
    (void)param;

    debug_adv_t adv;
    while (true) {
        if (xQueueReceive(s_debug_queue, &adv, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        debug_log_advert(&adv.desc);
    }
}
