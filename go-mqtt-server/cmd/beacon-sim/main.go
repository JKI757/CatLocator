package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"math/rand"
	"os"
	"os/signal"
	"syscall"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

type readingPayload struct {
	BeaconID       string             `json:"beacon_id"`
	TagID          string             `json:"tag_id"`
	RSSI           int                `json:"rssi"`
	Timestamp      string             `json:"timestamp"`
	BeaconLocation map[string]float64 `json:"beacon_location"`
	Metadata       map[string]string  `json:"metadata,omitempty"`
}

func main() {
	brokerAddr := flag.String("broker", "tcp://localhost:1883", "MQTT broker address, e.g. tcp://localhost:1883")
	beaconID := flag.String("beacon-id", "sim-beacon-1", "Beacon identifier")
	beaconX := flag.Float64("beacon-x", 0, "Beacon X coordinate in meters")
	beaconY := flag.Float64("beacon-y", 0, "Beacon Y coordinate in meters")
	beaconZ := flag.Float64("beacon-z", 2, "Beacon Z coordinate in meters")
	tagID := flag.String("tag-id", "cat-collar-1", "Tracked tag identifier")
	interval := flag.Duration("interval", 2*time.Second, "Interval between published readings")
	baseRSSI := flag.Int("base-rssi", -60, "Baseline RSSI value to simulate")
	rssiJitter := flag.Int("rssi-jitter", 6, "Maximum random jitter applied to RSSI readings")

	flag.Parse()

	rand.Seed(time.Now().UnixNano())

	clientID := fmt.Sprintf("%s-simulator-%d", *beaconID, time.Now().UnixNano())
	opts := mqtt.NewClientOptions().AddBroker(*brokerAddr).SetClientID(clientID)
	opts = opts.SetOrderMatters(false)

	client := mqtt.NewClient(opts)
	if token := client.Connect(); token.Wait() && token.Error() != nil {
		log.Fatalf("failed to connect to broker: %v", token.Error())
	}
	log.Printf("connected to MQTT broker %s as %s", *brokerAddr, clientID)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	ticker := time.NewTicker(*interval)
	defer ticker.Stop()

	publish := func() {
		payload := readingPayload{
			BeaconID:  *beaconID,
			TagID:     *tagID,
			RSSI:      randomRSSI(*baseRSSI, *rssiJitter),
			Timestamp: time.Now().UTC().Format(time.RFC3339Nano),
			BeaconLocation: map[string]float64{
				"x": *beaconX,
				"y": *beaconY,
				"z": *beaconZ,
			},
			Metadata: map[string]string{
				"source": "simulator",
			},
		}

		data, err := json.Marshal(payload)
		if err != nil {
			log.Printf("failed to encode payload: %v", err)
			return
		}

		topic := fmt.Sprintf("beacons/%s/readings", *beaconID)
		token := client.Publish(topic, 0, false, data)
		token.Wait()
		if err := token.Error(); err != nil {
			log.Printf("publish error: %v", err)
			return
		}
		log.Printf("published %s rssi=%d", topic, payload.RSSI)
	}

	publish()

	for {
		select {
		case <-ctx.Done():
			log.Print("received shutdown signal, disconnecting")
			client.Disconnect(250)
			return
		case <-ticker.C:
			publish()
		}
	}
}

func randomRSSI(base, jitter int) int {
	if jitter <= 0 {
		return base
	}
	delta := rand.Intn(jitter*2+1) - jitter
	return base + delta
}
