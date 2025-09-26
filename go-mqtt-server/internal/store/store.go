package store

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"catlocator/go-mqtt-server/internal/model"

	_ "modernc.org/sqlite"
)

// Store wraps the SQLite database connection and schema lifecycle.
type Store struct {
	db *sql.DB
}

// Open initializes the database connection, creating directories as needed.
func Open(path string) (*Store, error) {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return nil, fmt.Errorf("create db directory: %w", err)
	}

	dsn := fmt.Sprintf("file:%s?_pragma=foreign_keys(ON)", path)

	db, err := sql.Open("sqlite", dsn)
	if err != nil {
		return nil, fmt.Errorf("open sqlite: %w", err)
	}

	db.SetMaxOpenConns(1)
	db.SetConnMaxLifetime(0)
	db.SetConnMaxIdleTime(5 * time.Minute)

	return &Store{db: db}, nil
}

// Close releases the underlying database handle.
func (s *Store) Close() error {
	if s.db == nil {
		return nil
	}
	return s.db.Close()
}

// InitSchema ensures baseline tables exist.
func (s *Store) InitSchema(ctx context.Context) error {
	stmts := []string{
		`CREATE TABLE IF NOT EXISTS beacon_readings (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			beacon_id TEXT NOT NULL,
			tag_id TEXT NOT NULL,
			rssi INTEGER NOT NULL,
			x REAL NOT NULL,
			y REAL NOT NULL,
			z REAL NOT NULL,
			recorded_at TEXT NOT NULL,
			received_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
		);`,
		`CREATE INDEX IF NOT EXISTS idx_beacon_readings_tag_time ON beacon_readings(tag_id, recorded_at);`,
		`CREATE TABLE IF NOT EXISTS room_labels (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			tag_id TEXT NOT NULL,
			room_name TEXT NOT NULL,
			window_start TEXT NOT NULL,
			window_end TEXT NOT NULL,
			notes TEXT,
			created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
		);`,
		`CREATE TABLE IF NOT EXISTS room_models (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			version TEXT NOT NULL,
			artifact_path TEXT NOT NULL,
			trained_on TEXT NOT NULL,
			accuracy REAL,
			notes TEXT,
			created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
		);`,
		`CREATE TABLE IF NOT EXISTS ingestion_errors (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			beacon_id TEXT,
			payload TEXT,
			error TEXT NOT NULL,
			created_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
		);`,
		`CREATE TABLE IF NOT EXISTS training_commands (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			room TEXT NOT NULL,
			command TEXT NOT NULL,
			command_timestamp TEXT NOT NULL,
			source TEXT,
			received_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
		);`,
		`CREATE TABLE IF NOT EXISTS app_config (
			key TEXT PRIMARY KEY,
			value TEXT NOT NULL,
			updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
		);`,
		`CREATE TABLE IF NOT EXISTS discovered_beacons (
			scanner_id TEXT NOT NULL,
			tag_address TEXT NOT NULL,
			tag_name TEXT,
			rssi INTEGER,
			manufacturer_id INTEGER,
			manufacturer_data TEXT,
			tx_power INTEGER,
			event_type TEXT,
			last_seen TEXT NOT NULL,
			PRIMARY KEY (scanner_id, tag_address)
		);`,
	}

	for _, stmt := range stmts {
		if _, err := s.db.ExecContext(ctx, stmt); err != nil {
			return fmt.Errorf("init schema: %w", err)
		}
	}

	return nil
}

// DB exposes the underlying sql.DB for callers that need raw access.
func (s *Store) DB() *sql.DB {
	return s.db
}

// InsertBeaconReading persists a validated beacon reading.
func (s *Store) InsertBeaconReading(ctx context.Context, r model.BeaconReading) error {
	if s.db == nil {
		return fmt.Errorf("store not initialized")
	}

	recordedAt := r.Timestamp
	if recordedAt.IsZero() {
		recordedAt = time.Now().UTC()
	}

	_, err := s.db.ExecContext(
		ctx,
		`INSERT INTO beacon_readings (beacon_id, tag_id, rssi, x, y, z, recorded_at) VALUES (?, ?, ?, ?, ?, ?, ?);`,
		r.BeaconID,
		r.TagID,
		r.RSSI,
		r.BeaconLocation.X,
		r.BeaconLocation.Y,
		r.BeaconLocation.Z,
		recordedAt.UTC().Format(time.RFC3339Nano),
	)
	if err != nil {
		return fmt.Errorf("insert beacon reading: %w", err)
	}

	return nil
}

// InsertIngestionError records a payload that failed validation.
func (s *Store) InsertIngestionError(ctx context.Context, e model.IngestionError) error {
	if s.db == nil {
		return fmt.Errorf("store not initialized")
	}

	_, err := s.db.ExecContext(
		ctx,
		`INSERT INTO ingestion_errors (beacon_id, payload, error) VALUES (?, ?, ?);`,
		e.BeaconID,
		e.Payload,
		e.Error,
	)
	if err != nil {
		return fmt.Errorf("insert ingestion error: %w", err)
	}
	return nil
}

// RecentBeaconReadings returns the most recent readings ordered by received time descending.
func (s *Store) RecentBeaconReadings(ctx context.Context, limit int, since *time.Time) ([]model.StoredBeaconReading, error) {
	if s.db == nil {
		return nil, fmt.Errorf("store not initialized")
	}

	if limit <= 0 {
		limit = 25
	}

	query := `SELECT beacon_id, tag_id, rssi, x, y, z, recorded_at, received_at FROM beacon_readings`
	var args []interface{}
	if since != nil {
		query += ` WHERE received_at > ?`
		args = append(args, since.UTC().Format(time.RFC3339Nano))
	}
	query += ` ORDER BY received_at DESC LIMIT ?`
	args = append(args, limit)

	rows, err := s.db.QueryContext(ctx, query+";", args...)
	if err != nil {
		return nil, fmt.Errorf("query recent beacon readings: %w", err)
	}
	defer rows.Close()

	readings := make([]model.StoredBeaconReading, 0, limit)

	for rows.Next() {
		var (
			beaconID      string
			tagID         string
			rssi          int
			x, y, z       float64
			recordedAtStr string
			receivedAtStr string
		)

		if err := rows.Scan(&beaconID, &tagID, &rssi, &x, &y, &z, &recordedAtStr, &receivedAtStr); err != nil {
			return nil, fmt.Errorf("scan beacon reading: %w", err)
		}

		recordedAt, err := time.Parse(time.RFC3339Nano, recordedAtStr)
		if err != nil {
			recordedAt, _ = time.Parse("2006-01-02T15:04:05Z07:00", recordedAtStr)
		}

		receivedAt, err := time.Parse(time.RFC3339Nano, receivedAtStr)
		if err != nil {
			receivedAt, _ = time.Parse("2006-01-02T15:04:05Z07:00", receivedAtStr)
		}

		readings = append(readings, model.StoredBeaconReading{
			BeaconReading: model.BeaconReading{
				BeaconID:  beaconID,
				TagID:     tagID,
				RSSI:      rssi,
				Timestamp: recordedAt,
				BeaconLocation: model.Location{
					X: x,
					Y: y,
					Z: z,
				},
			},
			RecordedAt: recordedAt,
			ReceivedAt: receivedAt,
		})
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate beacon readings: %w", err)
	}

	return readings, nil
}

// UpsertDiscoveredBeacon records or updates metadata for a beacon observed during discovery mode.
func (s *Store) UpsertDiscoveredBeacon(ctx context.Context, beacon model.DiscoveredBeacon) error {
	if s.db == nil {
		return fmt.Errorf("store not initialized")
	}

	if beacon.LastSeen.IsZero() {
		beacon.LastSeen = time.Now().UTC()
	}

	var manufacturerID sql.NullInt64
	if beacon.ManufacturerID != nil {
		manufacturerID = sql.NullInt64{Int64: int64(*beacon.ManufacturerID), Valid: true}
	}

	var txPower sql.NullInt64
	if beacon.TxPower != nil {
		txPower = sql.NullInt64{Int64: int64(*beacon.TxPower), Valid: true}
	}

	_, err := s.db.ExecContext(
		ctx,
		`INSERT INTO discovered_beacons (scanner_id, tag_address, tag_name, rssi, manufacturer_id, manufacturer_data, tx_power, event_type, last_seen)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
		 ON CONFLICT(scanner_id, tag_address)
		 DO UPDATE SET tag_name = excluded.tag_name,
				 rssi = excluded.rssi,
				 manufacturer_id = excluded.manufacturer_id,
				 manufacturer_data = excluded.manufacturer_data,
				 tx_power = excluded.tx_power,
				 event_type = excluded.event_type,
				 last_seen = excluded.last_seen;`,
		beacon.ScannerID,
		beacon.TagAddress,
		beacon.TagName,
		beacon.RSSI,
		manufacturerID,
		beacon.ManufacturerData,
		txPower,
		beacon.EventType,
		beacon.LastSeen.UTC().Format(time.RFC3339Nano),
	)
	if err != nil {
		return fmt.Errorf("upsert discovered beacon: %w", err)
	}
	return nil
}

// ListDiscoveredBeacons returns the set of recently seen beacons published during discovery mode.
func (s *Store) ListDiscoveredBeacons(ctx context.Context) ([]model.DiscoveredBeacon, error) {
	if s.db == nil {
		return nil, fmt.Errorf("store not initialized")
	}

	rows, err := s.db.QueryContext(ctx, `SELECT scanner_id, tag_address, tag_name, rssi, manufacturer_id, manufacturer_data, tx_power, event_type, last_seen FROM discovered_beacons ORDER BY last_seen DESC;`)
	if err != nil {
		return nil, fmt.Errorf("query discovered beacons: %w", err)
	}
	defer rows.Close()

	var beacons []model.DiscoveredBeacon

	for rows.Next() {
		var (
			scannerID       string
			tagAddress      string
			tagName         sql.NullString
			rssi            sql.NullInt64
			manufacturerID  sql.NullInt64
			manufacturerRaw sql.NullString
			txPower         sql.NullInt64
			eventType       sql.NullString
			lastSeenStr     string
		)

		if err := rows.Scan(&scannerID, &tagAddress, &tagName, &rssi, &manufacturerID, &manufacturerRaw, &txPower, &eventType, &lastSeenStr); err != nil {
			return nil, fmt.Errorf("scan discovered beacon: %w", err)
		}

		lastSeen, err := time.Parse(time.RFC3339Nano, lastSeenStr)
		if err != nil {
			lastSeen, _ = time.Parse("2006-01-02T15:04:05Z07:00", lastSeenStr)
		}

		beacon := model.DiscoveredBeacon{
			ScannerID:        scannerID,
			TagAddress:       tagAddress,
			RSSI:             int(rssi.Int64),
			ManufacturerData: manufacturerRaw.String,
			EventType:        eventType.String,
			LastSeen:         lastSeen,
		}
		if tagName.Valid {
			beacon.TagName = tagName.String
		}
		if manufacturerID.Valid {
			id := int(manufacturerID.Int64)
			beacon.ManufacturerID = &id
		}
		if txPower.Valid {
			power := int(txPower.Int64)
			beacon.TxPower = &power
		}

		beacons = append(beacons, beacon)
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate discovered beacons: %w", err)
	}

	return beacons, nil
}

// UpsertAppConfig stores or updates a configuration key/value pair.
func (s *Store) UpsertAppConfig(ctx context.Context, key, value string) error {
	if s.db == nil {
		return fmt.Errorf("store not initialized")
	}

	_, err := s.db.ExecContext(
		ctx,
		`INSERT INTO app_config (key, value, updated_at) VALUES (?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
		 ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at;`,
		key,
		value,
	)
	if err != nil {
		return fmt.Errorf("upsert app config: %w", err)
	}
	return nil
}

// AppConfig returns all configuration entries as a map.
func (s *Store) AppConfig(ctx context.Context) (map[string]string, error) {
	if s.db == nil {
		return nil, fmt.Errorf("store not initialized")
	}

	rows, err := s.db.QueryContext(ctx, `SELECT key, value FROM app_config;`)
	if err != nil {
		return nil, fmt.Errorf("query app config: %w", err)
	}
	defer rows.Close()

	config := make(map[string]string)
	for rows.Next() {
		var key, value string
		if err := rows.Scan(&key, &value); err != nil {
			return nil, fmt.Errorf("scan app config: %w", err)
		}
		config[key] = value
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate app config: %w", err)
	}

	return config, nil
}

const roomDefinitionsKey = "room_definitions"

// GetRoomDefinitions loads stored room definitions (may be empty).
func (s *Store) GetRoomDefinitions(ctx context.Context) ([]model.RoomDefinition, error) {
	if s.db == nil {
		return nil, fmt.Errorf("store not initialized")
	}

	var raw string
	err := s.db.QueryRowContext(ctx, `SELECT value FROM app_config WHERE key = ?;`, roomDefinitionsKey).Scan(&raw)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, fmt.Errorf("get room definitions: %w", err)
	}

	var rooms []model.RoomDefinition
	if err := json.Unmarshal([]byte(raw), &rooms); err != nil {
		return nil, fmt.Errorf("decode room definitions: %w", err)
	}
	return rooms, nil
}

// SaveRoomDefinitions persists room definitions.
func (s *Store) SaveRoomDefinitions(ctx context.Context, rooms []model.RoomDefinition) error {
	if s.db == nil {
		return fmt.Errorf("store not initialized")
	}

	bytes, err := json.Marshal(rooms)
	if err != nil {
		return fmt.Errorf("encode room definitions: %w", err)
	}

	if _, err := s.db.ExecContext(ctx,
		`INSERT INTO app_config (key, value, updated_at) VALUES (?, ?, strftime('%Y-%m-%dT%H:%M:%fZ','now'))
		 ON CONFLICT(key) DO UPDATE SET value = excluded.value, updated_at = excluded.updated_at;`,
		roomDefinitionsKey, string(bytes)); err != nil {
		return fmt.Errorf("save room definitions: %w", err)
	}
	return nil
}

// WipeData removes all telemetry, labeling, and command data while preserving configuration.
func (s *Store) WipeData(ctx context.Context) error {
	if s.db == nil {
		return fmt.Errorf("store not initialized")
	}

	stmts := []string{
		`DELETE FROM beacon_readings;`,
		`DELETE FROM ingestion_errors;`,
		`DELETE FROM training_commands;`,
		`DELETE FROM room_labels;`,
		`DELETE FROM room_models;`,
	}

	for _, stmt := range stmts {
		if _, err := s.db.ExecContext(ctx, stmt); err != nil {
			return fmt.Errorf("wipe data: %w", err)
		}
	}

	return nil
}

// AllBeaconReadings returns every stored beacon reading ordered by recorded time.
func (s *Store) AllBeaconReadings(ctx context.Context) ([]model.StoredBeaconReading, error) {
	if s.db == nil {
		return nil, fmt.Errorf("store not initialized")
	}

	rows, err := s.db.QueryContext(
		ctx,
		`SELECT beacon_id, tag_id, rssi, x, y, z, recorded_at, received_at
		 FROM beacon_readings
		 ORDER BY recorded_at ASC;`)
	if err != nil {
		return nil, fmt.Errorf("query beacon readings: %w", err)
	}
	defer rows.Close()

	var readings []model.StoredBeaconReading
	for rows.Next() {
		var (
			beaconID      string
			tagID         string
			rssi          int
			x, y, z       float64
			recordedAtStr string
			receivedAtStr string
		)
		if err := rows.Scan(&beaconID, &tagID, &rssi, &x, &y, &z, &recordedAtStr, &receivedAtStr); err != nil {
			return nil, fmt.Errorf("scan beacon reading: %w", err)
		}

		recordedAt, err := time.Parse(time.RFC3339Nano, recordedAtStr)
		if err != nil {
			recordedAt, _ = time.Parse("2006-01-02T15:04:05Z07:00", recordedAtStr)
		}

		receivedAt, err := time.Parse(time.RFC3339Nano, receivedAtStr)
		if err != nil {
			receivedAt, _ = time.Parse("2006-01-02T15:04:05Z07:00", receivedAtStr)
		}

		readings = append(readings, model.StoredBeaconReading{
			BeaconReading: model.BeaconReading{
				BeaconID:  beaconID,
				TagID:     tagID,
				RSSI:      rssi,
				Timestamp: recordedAt,
				BeaconLocation: model.Location{
					X: x,
					Y: y,
					Z: z,
				},
			},
			RecordedAt: recordedAt,
			ReceivedAt: receivedAt,
		})
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate beacon readings: %w", err)
	}

	return readings, nil
}

// LatestBeaconReadings returns the most recent reading for each beacon.
func (s *Store) LatestBeaconReadings(ctx context.Context) ([]model.StoredBeaconReading, error) {
	if s.db == nil {
		return nil, fmt.Errorf("store not initialized")
	}

	rows, err := s.db.QueryContext(
		ctx,
		`SELECT br.beacon_id, br.tag_id, br.rssi, br.x, br.y, br.z, br.recorded_at, br.received_at
		 FROM beacon_readings br
		 INNER JOIN (
			SELECT beacon_id, MAX(recorded_at) AS max_ts
			FROM beacon_readings
			GROUP BY beacon_id
		 ) latest ON br.beacon_id = latest.beacon_id AND br.recorded_at = latest.max_ts;`)
	if err != nil {
		return nil, fmt.Errorf("latest readings query: %w", err)
	}
	defer rows.Close()

	var readings []model.StoredBeaconReading
	for rows.Next() {
		var (
			beaconID      string
			tagID         string
			rssi          int
			x, y, z       float64
			recordedAtStr string
			receivedAtStr string
		)
		if err := rows.Scan(&beaconID, &tagID, &rssi, &x, &y, &z, &recordedAtStr, &receivedAtStr); err != nil {
			return nil, fmt.Errorf("scan latest reading: %w", err)
		}

		recordedAt, err := time.Parse(time.RFC3339Nano, recordedAtStr)
		if err != nil {
			recordedAt, _ = time.Parse("2006-01-02T15:04:05Z07:00", recordedAtStr)
		}
		receivedAt, err := time.Parse(time.RFC3339Nano, receivedAtStr)
		if err != nil {
			receivedAt, _ = time.Parse("2006-01-02T15:04:05Z07:00", receivedAtStr)
		}

		readings = append(readings, model.StoredBeaconReading{
			BeaconReading: model.BeaconReading{
				BeaconID:  beaconID,
				TagID:     tagID,
				RSSI:      rssi,
				Timestamp: recordedAt,
				BeaconLocation: model.Location{
					X: x,
					Y: y,
					Z: z,
				},
			},
			RecordedAt: recordedAt,
			ReceivedAt: receivedAt,
		})
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate latest readings: %w", err)
	}

	return readings, nil
}

// InsertTrainingCommand stores a command emitted from labeling clients.
func (s *Store) InsertTrainingCommand(ctx context.Context, cmd model.TrainingCommand) error {
	if s.db == nil {
		return fmt.Errorf("store not initialized")
	}

	_, err := s.db.ExecContext(
		ctx,
		`INSERT INTO training_commands (room, command, command_timestamp, source, received_at)
		 VALUES (?, ?, ?, ?, ?);`,
		cmd.Room,
		cmd.Command,
		cmd.Timestamp.UTC().Format(time.RFC3339Nano),
		cmd.Source,
		cmd.ReceivedAt.UTC().Format(time.RFC3339Nano),
	)
	if err != nil {
		return fmt.Errorf("insert training command: %w", err)
	}
	return nil
}

// RecentTrainingCommands returns recent commands ordered newest first.
func (s *Store) RecentTrainingCommands(ctx context.Context, limit int) ([]model.TrainingCommand, error) {
	if s.db == nil {
		return nil, fmt.Errorf("store not initialized")
	}

	if limit <= 0 {
		limit = 50
	}

	rows, err := s.db.QueryContext(
		ctx,
		`SELECT room, command, command_timestamp, source, received_at
		 FROM training_commands
		 ORDER BY received_at DESC
		 LIMIT ?;`,
		limit,
	)
	if err != nil {
		return nil, fmt.Errorf("query training commands: %w", err)
	}
	defer rows.Close()

	var commands []model.TrainingCommand
	for rows.Next() {
		var room, command, commandTS, source sql.NullString
		var received string
		if err := rows.Scan(&room, &command, &commandTS, &source, &received); err != nil {
			return nil, fmt.Errorf("scan training command: %w", err)
		}

		ts, err := time.Parse(time.RFC3339Nano, commandTS.String)
		if err != nil {
			ts, _ = time.Parse("2006-01-02T15:04:05Z07:00", commandTS.String)
		}
		receivedAt, err := time.Parse(time.RFC3339Nano, received)
		if err != nil {
			receivedAt, _ = time.Parse("2006-01-02T15:04:05Z07:00", received)
		}

		commands = append(commands, model.TrainingCommand{
			Room:       room.String,
			Command:    command.String,
			Timestamp:  ts,
			Source:     source.String,
			ReceivedAt: receivedAt,
		})
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate training commands: %w", err)
	}

	return commands, nil
}

// AllTrainingCommands returns all commands ordered by command timestamp ascending.
func (s *Store) AllTrainingCommands(ctx context.Context) ([]model.TrainingCommand, error) {
	if s.db == nil {
		return nil, fmt.Errorf("store not initialized")
	}

	rows, err := s.db.QueryContext(
		ctx,
		`SELECT room, command, command_timestamp, source, received_at
		 FROM training_commands
		 ORDER BY command_timestamp ASC;`)
	if err != nil {
		return nil, fmt.Errorf("query training commands: %w", err)
	}
	defer rows.Close()

	var commands []model.TrainingCommand
	for rows.Next() {
		var room, command, tsStr, source sql.NullString
		var receivedStr string
		if err := rows.Scan(&room, &command, &tsStr, &source, &receivedStr); err != nil {
			return nil, fmt.Errorf("scan training command: %w", err)
		}

		ts, err := time.Parse(time.RFC3339Nano, tsStr.String)
		if err != nil {
			ts, _ = time.Parse("2006-01-02T15:04:05Z07:00", tsStr.String)
		}
		receivedAt, err := time.Parse(time.RFC3339Nano, receivedStr)
		if err != nil {
			receivedAt, _ = time.Parse("2006-01-02T15:04:05Z07:00", receivedStr)
		}

		commands = append(commands, model.TrainingCommand{
			Room:       room.String,
			Command:    command.String,
			Timestamp:  ts,
			Source:     source.String,
			ReceivedAt: receivedAt,
		})
	}

	if err := rows.Err(); err != nil {
		return nil, fmt.Errorf("iterate training commands: %w", err)
	}

	return commands, nil
}
