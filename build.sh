#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
BIN_ROOT="$ROOT_DIR/bin"
SERVER_BIN_DIR="$BIN_ROOT/server"
FIRMWARE_BIN_DIR="$BIN_ROOT/firmware"

mkdir -p "$SERVER_BIN_DIR" "$FIRMWARE_BIN_DIR"
rm -f "$SERVER_BIN_DIR"/* "$FIRMWARE_BIN_DIR"/* 2>/dev/null || true

echo "[1/2] Building Go server binaries"
pushd "$ROOT_DIR/go-mqtt-server" >/dev/null
./build.sh
cp -a bin/. "$SERVER_BIN_DIR"/
popd >/dev/null

echo "[2/2] Building ESP32 firmware"
IDF_CMD=${IDF_CMD:-idf.py}

# Ensure ESP-IDF environment is available
if ! command -v "$IDF_CMD" >/dev/null 2>&1; then
  if [ -n "${IDF_EXPORT_SH:-}" ] && [ -f "$IDF_EXPORT_SH" ]; then
    echo "Sourcing ESP-IDF env from $IDF_EXPORT_SH"
    . "$IDF_EXPORT_SH" >/dev/null 2>&1
  elif [ -f "$ROOT_DIR/esp32-beacon/esp-idf/export.sh" ]; then
    echo "Sourcing ESP-IDF env from esp32-beacon/esp-idf/export.sh"
    . "$ROOT_DIR/esp32-beacon/esp-idf/export.sh" >/dev/null 2>&1
  elif [ -n "${IDF_PATH:-}" ] && [ -f "$IDF_PATH/export.sh" ]; then
    echo "Sourcing ESP-IDF env from $IDF_PATH/export.sh"
    . "$IDF_PATH/export.sh" >/dev/null 2>&1
  fi
fi

if ! command -v "$IDF_CMD" >/dev/null 2>&1; then
  echo "idf.py not found. Set IDF_PATH or IDF_EXPORT_SH, or define IDF_CMD." >&2
  exit 1
fi
pushd "$ROOT_DIR/esp32-beacon" >/dev/null
$IDF_CMD build
ARTIFACTS=(
  build/catlocator_beacon.bin
  build/catlocator_beacon.elf
  build/partition_table/partition-table.bin
  build/bootloader/bootloader.bin
)
for artifact in "${ARTIFACTS[@]}"; do
  if [ -f "$artifact" ]; then
    cp "$artifact" "$FIRMWARE_BIN_DIR"/
  fi
done
popd >/dev/null

echo "Build complete. Server binaries in $SERVER_BIN_DIR, firmware artifacts in $FIRMWARE_BIN_DIR"
