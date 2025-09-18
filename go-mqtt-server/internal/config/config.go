package config

import (
	"fmt"
	"os"
	"strconv"
)

// Config lists the tunable parameters for the CatLocator server.
type Config struct {
	HTTPPort        int
	MQTTBindAddress string
	MetricsPort     int
	DatabasePath    string
	LogLevel        string
}

const (
	defaultHTTPPort        = 8080
	defaultMQTTBindAddress = ":1883"
	defaultMetricsPort     = 9090
	defaultDatabasePath    = "data/catlocator.db"
	defaultLogLevel        = "info"
)

// Load derives configuration values from environment variables, falling back to defaults.
func Load() (Config, error) {
	cfg := Config{
		HTTPPort:        defaultHTTPPort,
		MQTTBindAddress: defaultMQTTBindAddress,
		MetricsPort:     defaultMetricsPort,
		DatabasePath:    defaultDatabasePath,
		LogLevel:        defaultLogLevel,
	}

	if v := os.Getenv("CATLOCATOR_HTTP_PORT"); v != "" {
		port, err := strconv.Atoi(v)
		if err != nil {
			return Config{}, fmt.Errorf("invalid CATLOCATOR_HTTP_PORT: %w", err)
		}
		cfg.HTTPPort = port
	}

	if v := os.Getenv("CATLOCATOR_MQTT_BIND"); v != "" {
		cfg.MQTTBindAddress = v
	}

	if v := os.Getenv("CATLOCATOR_METRICS_PORT"); v != "" {
		port, err := strconv.Atoi(v)
		if err != nil {
			return Config{}, fmt.Errorf("invalid CATLOCATOR_METRICS_PORT: %w", err)
		}
		cfg.MetricsPort = port
	}

	if v := os.Getenv("CATLOCATOR_DATABASE_PATH"); v != "" {
		cfg.DatabasePath = v
	}

	if v := os.Getenv("CATLOCATOR_LOG_LEVEL"); v != "" {
		cfg.LogLevel = v
	}

	return cfg, nil
}
