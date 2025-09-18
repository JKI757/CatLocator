package store

import (
	"context"
	"database/sql"
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
		`CREATE TABLE IF NOT EXISTS app_config (
			key TEXT PRIMARY KEY,
			value TEXT NOT NULL,
			updated_at TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
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
