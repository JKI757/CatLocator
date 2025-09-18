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
The dashboard (served at `/`) polls `/api/readings`, offers configuration and beacon control forms, and lets you clear or filter recent telemetry.

## Cross-Build
Run `./build.sh` to produce native and Linux/ARM64 binaries in `bin/`.
