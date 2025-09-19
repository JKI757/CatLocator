package app

import (
	"context"
	"encoding/csv"
	"encoding/json"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"time"

	"catlocator/go-mqtt-server/internal/config"
	"catlocator/go-mqtt-server/internal/model"
	"catlocator/go-mqtt-server/internal/mqttbroker"
	"catlocator/go-mqtt-server/internal/store"
)

// App wires together the CatLocator services and manages their lifecycle.
type App struct {
	cfg    config.Config
	logger *slog.Logger
	store  *store.Store
	broker *mqttbroker.Broker
}

// New constructs a new application instance.
func New(cfg config.Config, logger *slog.Logger) *App {
	return &App{cfg: cfg, logger: logger}
}

// Run starts all configured services and blocks until the context is cancelled or an error occurs.
func (a *App) Run(ctx context.Context) error {
	db, err := store.Open(a.cfg.DatabasePath)
	if err != nil {
		return err
	}
	a.store = db

	if err := a.store.InitSchema(ctx); err != nil {
		return err
	}

	defer func() {
		if cerr := a.store.Close(); cerr != nil {
			a.logger.Error("close store", "error", cerr)
		}
	}()

	broker := mqttbroker.New(a.logger)
	broker.SetPublishHandler(a.handleMQTTPublish)
	brokerErrCh, err := broker.Start(a.cfg.MQTTBindAddress)
	if err != nil {
		return err
	}
	a.broker = broker

	httpErrCh := make(chan error, 1)

	httpServer := &http.Server{
		Addr:    fmt.Sprintf(":%d", a.cfg.HTTPPort),
		Handler: a.routes(),
	}

	go func() {
		a.logger.Info("http server started", "addr", httpServer.Addr)
		if err := httpServer.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
			httpErrCh <- fmt.Errorf("http server: %w", err)
		}
	}()

	for {
		select {
		case <-ctx.Done():
			shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()

			if err := httpServer.Shutdown(shutdownCtx); err != nil {
				return fmt.Errorf("http server shutdown: %w", err)
			}
			a.logger.Info("http server stopped")

			if err := a.broker.Stop(); err != nil {
				return err
			}
			a.logger.Info("mqtt broker stopped")
			return nil
		case err := <-httpErrCh:
			if err != nil {
				_ = a.broker.Stop()
				return err
			}
		case err, ok := <-brokerErrCh:
			if !ok {
				brokerErrCh = nil
				continue
			}
			if err != nil {
				_ = httpServer.Shutdown(context.Background())
				_ = a.broker.Stop()
				return err
			}
		}
	}
}

func (a *App) handleMQTTPublish(ctx context.Context, msg mqttbroker.PublishMessage) {
	switch {
	case strings.HasPrefix(msg.Topic, "beacons/"):
		a.handleBeaconReading(ctx, msg)
	case strings.HasPrefix(msg.Topic, "catlocator/training/commands"):
		a.handleTrainingCommand(ctx, msg)
	default:
		// ignore for now
	}
}

func (a *App) handleBeaconReading(ctx context.Context, msg mqttbroker.PublishMessage) {
	var reading model.BeaconReading
	if err := json.Unmarshal(msg.Payload, &reading); err != nil {
		a.logger.Warn("mqtt payload decode failed", "topic", msg.Topic, "error", err)
		a.recordIngestionError(ctx, "", msg.Payload, fmt.Errorf("decode payload: %w", err))
		return
	}

	if reading.BeaconID == "" {
		parts := strings.Split(msg.Topic, "/")
		if len(parts) >= 2 {
			reading.BeaconID = parts[1]
		}
	}

	if reading.Timestamp.IsZero() {
		reading.Timestamp = time.Now().UTC()
	}

	if reading.BeaconID == "" || reading.TagID == "" {
		err := fmt.Errorf("missing required identifiers (beacon_id=%q tag_id=%q)", reading.BeaconID, reading.TagID)
		a.logger.Warn("mqtt payload validation failed", "topic", msg.Topic, "error", err)
		a.recordIngestionError(ctx, reading.BeaconID, msg.Payload, err)
		return
	}

	storeCtx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()

	if err := a.store.InsertBeaconReading(storeCtx, reading); err != nil {
		a.logger.Error("failed to persist beacon reading", "beacon", reading.BeaconID, "tag", reading.TagID, "error", err)
		a.recordIngestionError(ctx, reading.BeaconID, msg.Payload, err)
		return
	}

	a.logger.Info("ingested beacon reading", "beacon", reading.BeaconID, "tag", reading.TagID, "rssi", reading.RSSI)
}

func (a *App) handleTrainingCommand(ctx context.Context, msg mqttbroker.PublishMessage) {
	type payloadSchema struct {
		Room      string `json:"room"`
		Command   string `json:"command"`
		Timestamp string `json:"timestamp"`
		Source    string `json:"source"`
	}

	var payload payloadSchema
	if err := json.Unmarshal(msg.Payload, &payload); err != nil {
		a.logger.Warn("training command decode failed", "error", err)
		a.recordIngestionError(ctx, "training", msg.Payload, fmt.Errorf("decode payload: %w", err))
		return
	}

	room := strings.TrimSpace(payload.Room)
	command := strings.TrimSpace(strings.ToLower(payload.Command))
	if room == "" || (command != "start" && command != "stop") {
		err := fmt.Errorf("invalid training command (room=%q command=%q)", room, command)
		a.logger.Warn("training command validation failed", "error", err)
		a.recordIngestionError(ctx, "training", msg.Payload, err)
		return
	}

	parsedTime, err := time.Parse(time.RFC3339Nano, payload.Timestamp)
	if err != nil {
		parsedTime, err = time.Parse(time.RFC3339, payload.Timestamp)
	}
	if err != nil {
		parsedTime = time.Now().UTC()
	}

	commandModel := model.TrainingCommand{
		Room:       room,
		Command:    command,
		Timestamp:  parsedTime,
		Source:     payload.Source,
		ReceivedAt: time.Now().UTC(),
	}

	storeCtx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()

	if err := a.store.InsertTrainingCommand(storeCtx, commandModel); err != nil {
		a.logger.Error("failed to persist training command", "room", room, "command", command, "error", err)
		a.recordIngestionError(ctx, "training", msg.Payload, err)
		return
	}

	a.logger.Info("ingested training command", "room", room, "command", command, "source", payload.Source)
}

func (a *App) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", a.handleHealthz)
	mux.HandleFunc("/readyz", a.handleReadyz)
	mux.HandleFunc("/api/readings", a.handleRecentReadings)
	mux.HandleFunc("/api/training/commands", a.handleRecentCommands)
	mux.HandleFunc("/api/config", a.handleConfig)
	mux.HandleFunc("/api/beacon-control/publish", a.handleBeaconPublish)
	mux.HandleFunc("/api/export/training", a.handleExportTraining)
	mux.HandleFunc("/api/admin/wipe", a.handleWipeDatabase)
	mux.HandleFunc("/static/", func(w http.ResponseWriter, r *http.Request) {
		http.StripPrefix("/static/", http.FileServer(http.Dir("web"))).ServeHTTP(w, r)
	})
	mux.HandleFunc("/", a.handleIndex)

	// TODO: expose configuration, labeling, and API routes.

	return mux
}

func (a *App) handleHealthz(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	_, _ = w.Write([]byte(`{"status":"ok"}`))
}

func (a *App) handleReadyz(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	// Readiness logic will evolve as dependencies (database, MQTT broker) come online.
	if a.store == nil || a.broker == nil {
		w.WriteHeader(http.StatusServiceUnavailable)
		_, _ = w.Write([]byte(`{"status":"starting"}`))
		return
	}
	_, _ = w.Write([]byte(`{"status":"ready"}`))
}

func (a *App) handleConfig(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		a.serveConfig(w, r)
	case http.MethodPost:
		a.updateConfig(w, r)
	default:
		w.Header().Set("Allow", "GET, POST")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (a *App) handleBeaconPublish(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	if a.broker == nil {
		http.Error(w, "broker unavailable", http.StatusServiceUnavailable)
		return
	}

	var req struct {
		BeaconID string          `json:"beacon_id"`
		Topic    string          `json:"topic"`
		Payload  json.RawMessage `json:"payload"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "invalid payload", http.StatusBadRequest)
		return
	}

	topic := req.Topic
	if topic == "" {
		topic = fmt.Sprintf("beacons/%s/commands", strings.TrimSpace(req.BeaconID))
	}
	if topic == "" {
		http.Error(w, "topic or beacon_id required", http.StatusBadRequest)
		return
	}

	payload := req.Payload
	if len(payload) == 0 {
		payload = []byte("{}")
	}

	if err := a.broker.Publish(topic, payload); err != nil {
		a.logger.Error("failed to publish control message", "topic", topic, "error", err)
		http.Error(w, "failed to publish", http.StatusInternalServerError)
		return
	}

	w.WriteHeader(http.StatusAccepted)
	_, _ = w.Write([]byte(`{"status":"queued"}`))
}

func (a *App) serveConfig(w http.ResponseWriter, r *http.Request) {
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
	defer cancel()

	persisted, err := a.store.AppConfig(ctx)
	if err != nil {
		a.logger.Error("failed to load app config", "error", err)
		http.Error(w, "failed to load config", http.StatusInternalServerError)
		return
	}

	active := map[string]any{
		"http_port":     a.cfg.HTTPPort,
		"mqtt_bind":     a.cfg.MQTTBindAddress,
		"metrics_port":  a.cfg.MetricsPort,
		"database_path": a.cfg.DatabasePath,
		"log_level":     a.cfg.LogLevel,
	}

	response := struct {
		Active    map[string]any    `json:"active"`
		Persisted map[string]string `json:"persisted"`
	}{
		Active:    active,
		Persisted: persisted,
	}

	w.Header().Set("Content-Type", "application/json")
	if err := json.NewEncoder(w).Encode(response); err != nil {
		a.logger.Error("failed to encode config response", "error", err)
	}
}

func (a *App) updateConfig(w http.ResponseWriter, r *http.Request) {
	var req struct {
		HTTPPort *int `json:"http_port"`
	}

	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		http.Error(w, "invalid payload", http.StatusBadRequest)
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
	defer cancel()

	type updateResult struct {
		Key   string `json:"key"`
		Value string `json:"value"`
	}

	updates := []updateResult{}

	if req.HTTPPort != nil {
		port := *req.HTTPPort
		if port < 1 || port > 65535 {
			http.Error(w, "http_port must be between 1 and 65535", http.StatusBadRequest)
			return
		}
		if err := a.store.UpsertAppConfig(ctx, "http_port", strconv.Itoa(port)); err != nil {
			a.logger.Error("failed to update http_port", "error", err)
			http.Error(w, "failed to persist config", http.StatusInternalServerError)
			return
		}
		updates = append(updates, updateResult{Key: "http_port", Value: strconv.Itoa(port)})
	}

	if len(updates) == 0 {
		w.WriteHeader(http.StatusBadRequest)
		_, _ = w.Write([]byte(`{"error":"no supported fields provided"}`))
		return
	}

	resp := struct {
		Updates         []updateResult `json:"updates"`
		RequiresRestart bool           `json:"requires_restart"`
	}{
		Updates:         updates,
		RequiresRestart: true,
	}

	w.Header().Set("Content-Type", "application/json")
	if err := json.NewEncoder(w).Encode(resp); err != nil {
		a.logger.Error("failed to encode update response", "error", err)
	}
}

func (a *App) handleRecentReadings(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	if a.store == nil {
		http.Error(w, "store not initialized", http.StatusServiceUnavailable)
		return
	}

	var sinceOpt *time.Time
	if since := r.URL.Query().Get("since"); since != "" {
		if ts, err := time.Parse(time.RFC3339Nano, since); err == nil {
			sinceOpt = &ts
		} else if ts, err := time.Parse(time.RFC3339, since); err == nil {
			sinceOpt = &ts
		}
	}

	limit := 25
	if v := r.URL.Query().Get("limit"); v != "" {
		if parsed, err := strconv.Atoi(v); err == nil {
			if parsed > 0 && parsed <= 250 {
				limit = parsed
			}
		}
	}

	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
	defer cancel()

	readings, err := a.store.RecentBeaconReadings(ctx, limit, sinceOpt)
	if err != nil {
		a.logger.Error("failed to load recent readings", "error", err)
		http.Error(w, "failed to load readings", http.StatusInternalServerError)
		return
	}

	response := struct {
		Readings []model.StoredBeaconReading `json:"readings"`
	}{Readings: readings}

	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	if err := json.NewEncoder(w).Encode(response); err != nil {
		a.logger.Error("failed to encode readings response", "error", err)
	}
}

func (a *App) handleRecentCommands(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	if a.store == nil {
		http.Error(w, "store not initialized", http.StatusServiceUnavailable)
		return
	}

	limit := 50
	if v := r.URL.Query().Get("limit"); v != "" {
		if parsed, err := strconv.Atoi(v); err == nil {
			if parsed > 0 && parsed <= 500 {
				limit = parsed
			}
		}
	}

	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
	defer cancel()

	commands, err := a.store.RecentTrainingCommands(ctx, limit)
	if err != nil {
		a.logger.Error("failed to load training commands", "error", err)
		http.Error(w, "failed to load commands", http.StatusInternalServerError)
		return
	}

	response := struct {
		Commands []model.TrainingCommand `json:"commands"`
	}{Commands: commands}

	w.Header().Set("Content-Type", "application/json")
	if err := json.NewEncoder(w).Encode(response); err != nil {
		a.logger.Error("failed to encode commands response", "error", err)
	}
}

func (a *App) handleExportTraining(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	if a.store == nil {
		http.Error(w, "store not initialized", http.StatusServiceUnavailable)
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), 10*time.Second)
	defer cancel()

	readings, err := a.store.AllBeaconReadings(ctx)
	if err != nil {
		a.logger.Error("export: failed to load readings", "error", err)
		http.Error(w, "failed to load readings", http.StatusInternalServerError)
		return
	}

	commands, err := a.store.AllTrainingCommands(ctx)
	if err != nil {
		a.logger.Error("export: failed to load commands", "error", err)
		http.Error(w, "failed to load commands", http.StatusInternalServerError)
		return
	}

	sort.Slice(readings, func(i, j int) bool {
		return readings[i].RecordedAt.Before(readings[j].RecordedAt)
	})

	sort.Slice(commands, func(i, j int) bool {
		return commands[i].Timestamp.Before(commands[j].Timestamp)
	})

	activeRoom := ""
	activeSource := ""
	cmdIdx := 0

	w.Header().Set("Content-Type", "text/csv")
	w.Header().Set("Content-Disposition", "attachment; filename=catlocator_training.csv")

	csvWriter := csv.NewWriter(w)
	defer csvWriter.Flush()

	if err := csvWriter.Write([]string{
		"recorded_at",
		"received_at",
		"beacon_id",
		"tag_id",
		"rssi",
		"room",
		"beacon_x",
		"beacon_y",
		"beacon_z",
		"session_source",
	}); err != nil {
		a.logger.Error("export: failed to write header", "error", err)
		return
	}

	for _, reading := range readings {
		ts := reading.RecordedAt
		// apply command updates up to reading timestamp
		for cmdIdx < len(commands) && !commands[cmdIdx].Timestamp.After(ts) {
			cmd := commands[cmdIdx]
			cmdIdx++
			commandValue := strings.ToLower(strings.TrimSpace(cmd.Command))
			switch commandValue {
			case "start":
				activeRoom = cmd.Room
				activeSource = cmd.Source
			case "stop":
				if activeRoom == "" || strings.EqualFold(activeRoom, cmd.Room) {
					activeRoom = ""
					activeSource = ""
				}
			}
		}

		if activeRoom == "" {
			continue
		}

		row := []string{
			reading.RecordedAt.UTC().Format(time.RFC3339Nano),
			reading.ReceivedAt.UTC().Format(time.RFC3339Nano),
			reading.BeaconID,
			reading.TagID,
			strconv.Itoa(reading.RSSI),
			activeRoom,
			fmt.Sprintf("%.4f", reading.BeaconLocation.X),
			fmt.Sprintf("%.4f", reading.BeaconLocation.Y),
			fmt.Sprintf("%.4f", reading.BeaconLocation.Z),
			activeSource,
		}

		if err := csvWriter.Write(row); err != nil {
			a.logger.Error("export: failed to write row", "error", err)
			return
		}
	}

	if err := csvWriter.Error(); err != nil {
		a.logger.Error("export: writer error", "error", err)
	}
}

func (a *App) handleWipeDatabase(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		w.Header().Set("Allow", http.MethodPost)
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	if a.store == nil {
		http.Error(w, "store not initialized", http.StatusServiceUnavailable)
		return
	}

	var body struct {
		Confirm string `json:"confirm"`
	}
	if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
		http.Error(w, "invalid payload", http.StatusBadRequest)
		return
	}

	if strings.ToLower(strings.TrimSpace(body.Confirm)) != "wipe" {
		http.Error(w, "confirmation required", http.StatusBadRequest)
		return
	}

	ctx, cancel := context.WithTimeout(r.Context(), 5*time.Second)
	defer cancel()

	if err := a.store.WipeData(ctx); err != nil {
		a.logger.Error("wipe: failed", "error", err)
		http.Error(w, "failed to wipe data", http.StatusInternalServerError)
		return
	}

	a.logger.Warn("wipe: all telemetry cleared")
	w.WriteHeader(http.StatusNoContent)
}

func (a *App) recordIngestionError(ctx context.Context, beaconID string, payload []byte, cause error) {
	if a.store == nil {
		return
	}

	recCtx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()

	entry := model.IngestionError{
		BeaconID: beaconID,
		Payload:  truncateString(string(payload), 4096),
		Error:    cause.Error(),
	}

	if err := a.store.InsertIngestionError(recCtx, entry); err != nil {
		a.logger.Error("failed to persist ingestion error", "error", err)
	}
}

func truncateString(s string, max int) string {
	if max <= 0 || len(s) <= max {
		return s
	}
	runes := []rune(s)
	if len(runes) <= max {
		return s
	}
	return string(runes[:max])
}

func (a *App) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}

	fileServer := http.FileServer(http.Dir("web"))
	fileServer.ServeHTTP(w, r)
}
