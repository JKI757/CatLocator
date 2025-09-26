# CatLocator ESP32-S3 Beacon Firmware

ESP-IDF project for the CatLocator beacons. Firmware responsibilities:

- Provision Wi-Fi/MQTT credentials via HTTP portal with NVS persistence
- Manage Wi-Fi STA connection and SNTP time sync
- Scan BLE advertisements (NimBLE), enrich metadata, throttle per-tag publishes
- Publish JSON telemetry to the CatLocator server via MQTT
- (Optional) prep SX1255 LoRa bridge for future backhaul

## Requirements
- ESP-IDF 5.1+
- ESP32-S3 beacon hardware with BLE/Wi-Fi and SX1255

## Build & Flash
Create a local symlink to your ESP-IDF checkout (ignored by git) so the tooling is available from the project root:

```bash
ln -s /path/to/esp-idf esp-idf
```

Then export the environment via the symlink before building or flashing:

```bash
source esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

On the first build, `idf.py` will pull the official `espressif/mdns` managed component (network required).

## Serial CLI Provisioning
If Wi-Fi or MQTT credentials are absent, the firmware stays up and exposes a serial console on the default USB/UART. Connect via `idf.py monitor` (or any serial terminal). The menu lets you:

- Show the current configuration snapshot.
- Set Wi-Fi credentials.
- Set MQTT broker URI/credentials (use `mqtt://` or `mqtts://`).
- Set beacon ID and coordinates.
- Clear the stored configuration.
- Toggle verbose BLE scan debugging to stream advertisements (RSSI, names, UUIDs, manufacturer/iBeacon data) over the console.

Changes are applied immediately and pushed to Wi-Fi, MQTT, and BLE modules.

## Provisioning API
- `GET /api/config` – inspect current settings
- `POST /api/config` – JSON payload with Wi-Fi, MQTT, beacon metadata, and reporting interval

Settings propagate live to networking and MQTT components; BLE reporting interval drives publish throttling.

## Telemetry Format
Published JSON: `beacons/<beacon_id>/readings`
```json
{
  "beacon_id": "kitchen",
  "tag_id": "AA:BB:CC:DD:EE:FF",
  "rssi": -62,
  "timestamp": "2024-05-01T17:20:00Z",
  "beacon_location": {"x": 1.0, "y": 2.0, "z": 0.0},
  "manufacturer_id": 76,
  "manufacturer_data": "0215...",
  "tx_power": -4
}
```

## LoRa Bridge
Configure SPI host/pins in `menuconfig` under **CatLocator LoRa Bridge**. Driver currently initialises bus/reset; extend for SX1255 packet handling as needed.
