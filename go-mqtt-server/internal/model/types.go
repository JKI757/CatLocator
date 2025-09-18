package model

import "time"

// Location describes the fixed coordinates of a beacon in meters.
type Location struct {
	X float64 `json:"x"`
	Y float64 `json:"y"`
	Z float64 `json:"z"`
}

// BeaconReading captures a single RSSI observation published by a beacon.
type BeaconReading struct {
	BeaconID       string            `json:"beacon_id"`
	TagID          string            `json:"tag_id"`
	RSSI           int               `json:"rssi"`
	Timestamp      time.Time         `json:"timestamp"`
	BeaconLocation Location          `json:"beacon_location"`
	Metadata       map[string]string `json:"metadata,omitempty"`
}

// StoredBeaconReading extends BeaconReading with database metadata.
type StoredBeaconReading struct {
	BeaconReading
	RecordedAt time.Time `json:"recorded_at"`
	ReceivedAt time.Time `json:"received_at"`
}

// IngestionError captures a payload that failed validation.
type IngestionError struct {
	BeaconID string `json:"beacon_id"`
	Payload  string `json:"payload"`
	Error    string `json:"error"`
}

// AppConfigEntry represents a persisted configuration key/value pair.
type AppConfigEntry struct {
	Key   string `json:"key"`
	Value string `json:"value"`
}
