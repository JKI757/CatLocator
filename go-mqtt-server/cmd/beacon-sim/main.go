package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"log"
	"math"
	"math/rand"
	"os"
	"os/signal"
	"syscall"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

type beacon struct {
	ID string  `json:"id"`
	X  float64 `json:"x"`
	Y  float64 `json:"y"`
	Z  float64 `json:"z"`
}

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
	beaconConfig := flag.String("beacons", "", "JSON file containing beacon definitions {id,x,y,z}[]")
	tagID := flag.String("tag-id", "cat-collar-1", "Tracked tag identifier")
	initialTagX := flag.Float64("tag-x", 0, "Initial tag X coordinate (m); random if zero")
	initialTagY := flag.Float64("tag-y", 0, "Initial tag Y coordinate (m); random if zero")
	initialTagZ := flag.Float64("tag-z", 0, "Initial tag Z coordinate (m); random if zero")
	houseWidth := flag.Float64("house-width", feetToMeters(40), "House width (m)")
	houseDepth := flag.Float64("house-depth", feetToMeters(25), "House depth (m)")
	houseHeight := flag.Float64("house-height", feetToMeters(30), "House height (m); default ~3 stories")
	tagStep := flag.Float64("tag-step", 0.6, "Approximate movement per interval (m)")
	stationary := flag.Bool("tag-stationary", false, "Keep the tag fixed at the initial coordinates")
	interval := flag.Duration("interval", 2*time.Second, "Interval between published readings")
	txPower := flag.Float64("tx-power", -59.0, "Expected RSSI at 1m")
	pathLoss := flag.Float64("path-loss", 2.0, "Path loss exponent")
	noiseStd := flag.Float64("noise-std", 2.0, "Gaussian noise applied to RSSI")

	flag.Parse()

	beacons := loadBeacons(*beaconConfig, *houseWidth, *houseDepth, *houseHeight)
	if len(beacons) == 0 {
		log.Fatal("no beacons configured")
	}

	rand.Seed(time.Now().UnixNano())
	tagPos := randomPoint(*houseWidth, *houseDepth, *houseHeight)
	if *initialTagX != 0 || *initialTagY != 0 || *initialTagZ != 0 {
		tagPos[0], tagPos[1], tagPos[2] = *initialTagX, *initialTagY, *initialTagZ
		clamp(tagPos[:], *houseWidth, *houseDepth, *houseHeight)
	}

	clientID := fmt.Sprintf("catlocator-sim-%d", time.Now().UnixNano())
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

	updateTag := func() {
		if *stationary {
			return
		}
		for i := 0; i < 3; i++ {
			delta := rand.NormFloat64() * *tagStep
			tagPos[i] += delta
		}
		clamp(tagPos[:], *houseWidth, *houseDepth, *houseHeight)
	}

	publish := func() {
		updateTag()
		for _, b := range beacons {
			dist := math.Sqrt((b.X-tagPos[0])*(b.X-tagPos[0]) + (b.Y-tagPos[1])*(b.Y-tagPos[1]) + (b.Z-tagPos[2])*(b.Z-tagPos[2]))
			rssi := distanceToRSSI(dist, *txPower, *pathLoss)
			rssi += rand.NormFloat64() * *noiseStd

			payload := readingPayload{
				BeaconID:  b.ID,
				TagID:     *tagID,
				RSSI:      int(math.Round(rssi)),
				Timestamp: time.Now().UTC().Format(time.RFC3339Nano),
				BeaconLocation: map[string]float64{
					"x": b.X,
					"y": b.Y,
					"z": b.Z,
				},
				Metadata: map[string]string{
					"source":         "simulator",
					"tag_x":          fmt.Sprintf("%.2f", tagPos[0]),
					"tag_y":          fmt.Sprintf("%.2f", tagPos[1]),
					"tag_z":          fmt.Sprintf("%.2f", tagPos[2]),
					"tag_stationary": fmt.Sprintf("%t", *stationary),
				},
			}

			data, err := json.Marshal(payload)
			if err != nil {
				log.Printf("failed to encode payload: %v", err)
				continue
			}

			topic := fmt.Sprintf("beacons/%s/readings", b.ID)
			token := client.Publish(topic, 0, false, data)
			token.Wait()
			if err := token.Error(); err != nil {
				log.Printf("publish error: %v", err)
				continue
			}
			log.Printf("published %s rssi=%.1f", topic, rssi)
		}
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

func loadBeacons(path string, width, depth, height float64) []beacon {
	if path == "" {
		hw := width
		hd := depth
		floorHeight := height / 3
		levels := []float64{floorHeight / 2, (3 * floorHeight) / 2, (5 * floorHeight) / 2}
		corners := [][2]float64{{0, 0}, {hw, 0}, {0, hd}, {hw, hd}}
		var beacons []beacon
		id := 1
		for _, z := range levels {
			for _, c := range corners {
				beacons = append(beacons, beacon{
					ID: fmt.Sprintf("sim-beacon-%d", id),
					X:  c[0],
					Y:  c[1],
					Z:  z,
				})
				id++
			}
		}
		return beacons
	}

	data, err := os.ReadFile(path)
	if err != nil {
		log.Fatalf("failed to read beacon config: %v", err)
	}
	var beacons []beacon
	if err := json.Unmarshal(data, &beacons); err != nil {
		log.Fatalf("failed to parse beacon config: %v", err)
	}
	return beacons
}

func distanceToRSSI(distance, txPower, pathLoss float64) float64 {
	if distance <= 0 {
		distance = 0.01
	}
	return txPower - 10*pathLoss*math.Log10(distance)
}

func randomPoint(width, depth, height float64) [3]float64 {
	return [3]float64{
		rand.Float64() * width,
		rand.Float64() * depth,
		rand.Float64() * height,
	}
}

func clamp(pos []float64, width, depth, height float64) {
	if pos[0] < 0 {
		pos[0] = 0
	} else if pos[0] > width {
		pos[0] = width
	}
	if pos[1] < 0 {
		pos[1] = 0
	} else if pos[1] > depth {
		pos[1] = depth
	}
	if pos[2] < 0 {
		pos[2] = 0
	} else if pos[2] > height {
		pos[2] = height
	}
}

func feetToMeters(ft float64) float64 {
	return ft * 0.3048
}
