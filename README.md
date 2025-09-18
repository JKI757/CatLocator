# CatLocator

Two coordinated projects deliver indoor pet localization:

1. **`go-mqtt-server/`** – Go backend with embedded MQTT broker, REST APIs, SQLite storage, and room-classification tooling
2. **`esp32-beacon/`** – ESP-IDF firmware for ESP32-S3 beacons handling provisioning, BLE scanning, MQTT publishing, and optional LoRa

Consult each subdirectory for detailed build and usage instructions. The overall requirements and roadmap live in `project-prd.md`.

## Build All
Run `./build.sh` to build the Go server (native + linux/arm64) and the ESP32 firmware. Artifacts land under `bin/server/` and `bin/firmware/`.

> Note: Ensure `esp32-beacon/esp-idf` is a symlink or checkout of ESP-IDF containing `export.sh`, or set `IDF_EXPORT_SH`/`IDF_PATH` before running the build script.
