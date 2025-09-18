# CatLocator Project Requirements Document

## Overview
CatLocator is a two-part system designed to locate tagged devices within an indoor environment. It consists of:
- A Go-based MQTT aggregation and analytics server.
- ESP32-based Bluetooth Low Energy (BLE) beacons that scan for nearby tags and report readings to the server.

The system uses Received Signal Strength Indicator (RSSI) measurements from multiple fixed-location beacons to estimate the positions of BLE tags (e.g., cat collars). Beacons upload RSSI readings with timestamps and their own coordinates. The server performs data ingestion, device tracking, derives room-level presence for tagged pets, and exposes integrations for downstream applications (dashboards, alerts). Operators can label historical data, export it for offline training, and deploy the resulting lightweight machine-learning model back to the server for real-time room classification.

## Goals & Non-goals
- Provide an extensible server that can collect, persist, and analyze RSSI readings from many beacons.
- Deliver firmware for ESP32 beacons that can be deployed with minimal local configuration.
- Support both plugged-in and battery-powered beacon deployments through configurable reporting periods.
- Produce accurate location estimates for tags seen by multiple beacons.
- Classify which room a tagged pet currently occupies based on recent beacon reports.
- Enable operators to manage beacons (ID, location, reporting cadence) via a simple local web UI.
- Provide intuitive tooling to label historical readings with room occupancy and improve automated classification.

Non-goals for the initial version:
- 3D building maps or sophisticated user-facing visualization tools.
- Large-scale predictive analytics or cloud-based machine learning pipelines beyond the embedded room classifier.
- Mobile applications or push notification systems.
- Over-the-air firmware updates.

## Success Metrics
- Location estimation accuracy within 2 meters for tags seen by ≥3 beacons in a 50x50 meter indoor space.
- ≥90% accuracy for room-level classification on validation data collected in the deployment environment.
- End-to-end latency (from beacon capture to server availability) under 2 seconds on LAN.
- Beacons run ≥72 hours on typical battery pack with default reporting period.
- Server uptime ≥99% over a 30-day period under nominal load.

## Personas
- **System Operator**: Installs beacons, configures IDs/locations, monitors system health.
- **Pet Owner**: Consumes location data via downstream tools; expects near real-time accuracy.
- **Firmware Developer**: Iterates on beacon behavior; requires clear APIs and diagnostics.
- **Backend Developer**: Extends server analytics or integrates external services.

## Functional Requirements

### Shared Concepts
- Locations use a consistent coordinate system (cartesian X/Y/Z meters from an origin point aligned with the deployment site).
- Each beacon advertises a unique identifier, assigned via the beacon's configuration UI.
- BLE tags to track must broadcast an identifiable address or payload (MAC, UUID).

### Go MQTT Server (`go-mqtt-server`)
1. **MQTT Broker**
   - Provide an MQTT broker endpoint accessible on LAN via configurable port.
   - Authenticate beacons using pre-shared credentials (initially username/password).
   - Enforce TLS optional in v1; document requirements for secure deployment.
2. **Data Ingestion**
   - Accept JSON payloads from beacons containing: beacon_id, timestamp (UTC ISO8601), rssi, tag_id, beacon_location {x,y,z}, optional metadata (battery_level, firmware_version).
   - Store raw readings in SQLite with schemas optimized for high-write workloads and downstream analytics.
   - Validate payload schema and reject malformed data with error logging.
3. **Location Estimation**
   - Maintain recent RSSI readings per tag across beacons in rolling time window (e.g., 30 seconds).
   - Implement baseline trilateration / weighted centroid algorithm using beacon locations and RSSI to estimate tag position.
   - Expose calculated positions via REST API for query by tag_id (current position, confidence score, timestamp).
4. **Room Classification & Training**
   - Provide a web UI where operators can label time-bounded windows of beacon reports with the room occupied by a tagged pet.
   - Persist labeled sessions in SQLite and version datasets for reproducible training.
   - Support exporting labeled datasets for offline training pipelines (e.g., notebook workflows on a development workstation).
   - Accept uploaded model artifacts and metadata, validate compatibility, and activate the newest approved model for inference.
   - Run the trained model on streaming data to classify current room per tag, exposing predictions with confidence scores.
5. **Management API / Dashboard Hooks**
   - Provide REST endpoints for beacon registration, status retrieval, and configuration introspection.
   - Maintain operational metrics (per-beacon message rate, last-heard timestamp, system load) accessible via Prometheus-compatible endpoint.
   - Surface room-classification outcomes and labeling queue status through the UI/API.
6. **Administration & Logging**
   - Support structured logging with log levels and correlation IDs per request.
   - Include configuration via environment variables / config file (port, database DSN, auth secrets, algorithm parameters).
   - Provide health check endpoints for readiness/liveness.

### ESP32 Beacon Firmware (`esp32-beacon`)
1. **Wi-Fi Connectivity**
   - Startup captive portal or serial configurator for provisioning SSID/password and server settings.
   - Persist credentials and reconnect automatically; exponential backoff on failure.
2. **Time Synchronization**
   - Obtain UTC time via NTP on boot and periodically refresh (default every 12 hours); maintain RTC fallback.
3. **BLE Scanning & Data Collection**
   - Continuously scan for BLE advertisements within configurable duty cycle.
   - Filter target tags by whitelist or pattern (configurable).
   - For each detected tag, capture RSSI, tag identifier, and scan timestamp (synced to NTP time).
4. **Data Publishing**
   - Package readings into JSON payload matching server schema and publish to MQTT topic `beacons/{beacon_id}/readings`.
   - Include beacon metadata: beacon_id, location (x,y,z), firmware_version, optional battery_level.
   - Buffer messages locally and retransmit when MQTT connection restored.
5. **Power Management**
   - Configurable reporting interval (e.g., 5s, 30s, 60s) and scan duration to balance accuracy vs. battery life.
   - Deep sleep option when no external power detected, with periodic wake for scans.
6. **Local Configuration Server**
   - Expose HTTP server on local network with:
     - Status page showing connectivity, last publish, firmware version.
     - Forms or REST endpoints to set beacon_id, location coordinates, reporting interval, MQTT credentials/server.
     - Simple authentication (default credentials, require change on first use).
   - Provide API or UI action to trigger firmware diagnostics (e.g., immediate scan, log dump).
7. **Diagnostics & OTA Hooks**
   - Serial debug logging with toggled verbosity.
   - Allow manual firmware update via file upload in UI (OTA in future version).

## Non-functional Requirements
- **Scalability**: Support at least 100 concurrent beacons and 500 tracked tags with horizontal scaling plan (e.g., MQTT clustering, database sharding).
- **Security**: Protect MQTT and HTTP interfaces with authentication and encryption options; store credentials securely.
- **Reliability**: Ensure beacon firmware resilient to Wi-Fi drops; server must handle out-of-order or delayed messages gracefully.
- **Maintainability**: Provide clear module boundaries, code documentation, and CI workflows for linting/testing.
- **Observability**: Implement structured logs, metrics, and tracing hooks where feasible.
- **Model Governance**: Track training data provenance, model versions, and evaluation metrics to support reproducibility.
- **Deployment Profile**: Optimize binaries and dependencies for an NVIDIA Jetson-class Linux host (ARM64), with optional GPU acceleration left for future iterations.

## Milestones
1. **MVP Data Loop**
   - Minimal firmware publishes readings from a bench setup to the server; server stores data and exposes basic REST queries.
2. **Location Estimation Prototype**
   - Implement trilateration algorithm with validation datasets and accuracy metrics.
3. **Room Labeling & ML Training**
   - Build labeling workflows, export datasets for offline training, produce an initial model, and integrate real-time inference path.
4. **Beacon Configuration UX**
   - Deliver HTTP UI and captive portal for provisioning, including persistence and validation.
5. **Production Hardening**
   - Add security hardening, metrics, alerting, and load tests; polish deployment docs.

## Open Questions
- Will tags broadcast standard BLE advertisements or custom payloads? Define parsing logic accordingly.
- How will operators visualize location data? Need downstream integration requirements.
- Do we need indoor map calibration (RSSI fingerprinting) to improve accuracy beyond trilateration?
- What feature set best drives room classification accuracy (e.g., window size, RSSI normalization)?
- How frequently should the room-classification model be retrained in production environments?
