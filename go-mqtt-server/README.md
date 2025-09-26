# CatLocator Go Server

This service ingests telemetry from CatLocator beacons, estimates locations, and exposes REST/MQTT interfaces for operators and downstream tools.

## Features
- Embedded MQTT broker for beacon connections
- REST APIs for room classification, beacon management, and configuration
- SQLite persistence of raw readings, labels, and model metadata
- Web dashboard for live monitoring, configuration edits, and command publishing

## Prerequisites
- Go 1.22+

## Getting Started
```bash
/opt/homebrew/bin/go mod tidy
/opt/homebrew/bin/go build ./...
/opt/homebrew/bin/go run ./cmd/server
```

Run the simulator to produce sample data:
```bash
/opt/homebrew/bin/go run ./cmd/beacon-sim --broker tcp://localhost:1883
```

## Frontend
The dashboard (served at `/`) polls `/api/readings`, offers configuration and beacon control forms, lets you define room coordinates (used by triangulation), and provides export/wipe tools.

## Cross-Build
Run `./build.sh` to produce native and Linux/ARM64 binaries in `bin/`.

## Training Data Export
- `/api/training/commands` – recent start/stop events from labeling clients
- `/api/export/training` – CSV of beacon readings labeled with active room, suitable for ML training
- `/api/admin/wipe` – clears telemetry/commands (configuration retained)

The dashboard shows both beacon readings and training commands, and includes Export/Wipe controls.

## Location Stub
The dashboard `Cat Location` tab calls `/api/location/cat` (currently stubbed). Replace `stubbedInference()` in `internal/app/app.go` with real model inference once available.

## Cat Location
- `/api/location/cat` returns both the stubbed ML estimate and a RSSI-based triangulation using beacon coordinates.
- Dashboard tab “Cat Location” refreshes this endpoint for quick visualization.
