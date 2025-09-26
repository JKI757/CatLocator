# CatLocator iOS Labeling App

SwiftUI app used to mark room occupancy during training runs.

## Features
- Configurable MQTT host/credentials and room list
- Start/Stop commands publish to `catlocator/training/commands` with timestamps
- Manual connect/disconnect controls and connection status indicator
- Persists settings in `UserDefaults`

## Build
Open `CatLocator_iOS.xcodeproj` in Xcode 16+ and run on an iPhone/iPad (or Simulator).

## Usage
1. Configure MQTT host/port/credentials in Settings.
2. Manage rooms and choose the active room.
3. Use Start/Stop during experiments; server logs commands and beacon RSSIs for training datasets.
4. Telemetry will be exported from the web server for the periods for which a label has been selected.  i.e put the cat in a room, select that room, press start, wait a while and hopefully let the cat explore the room a bit, then press stop.  Do this for all rooms, then push the export button on the server and it will download a csv file with RSSIs, beacon IDs, and what room location the target beacon was in at the time.  Ideally can be used to train a small ML model, or for triangulation.

## Telemetry
Commands are published as JSON to `catlocator/training/commands`:
```json
{
  "room": "Living Room",
  "command": "start",
  "timestamp": "2024-05-01T17:20:00Z",
  "source": "catlocator-ios"
}
```
