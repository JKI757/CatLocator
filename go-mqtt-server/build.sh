#!/usr/bin/env bash
set -euo pipefail

BIN_DIR="$(dirname "$0")/bin"
mkdir -p "$BIN_DIR"

GO=${GO:-/opt/homebrew/bin/go}

# Determine executable suffix (e.g., .exe on Windows)
GOEXE="$($GO env GOEXE)"

# Local build
$GO build -o "$BIN_DIR/catlocator-server${GOEXE}" ./cmd/server

# ARM64 cross build for Raspberry Pi/Jetson (linux/arm64)
GOOS=linux GOARCH=arm64 $GO build -o "$BIN_DIR/catlocator-server-linux-arm64" ./cmd/server

echo "Binaries available in $BIN_DIR"
