package mqttbroker

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

// PublishMessage represents a QoS 0 publish received from a client.
type PublishMessage struct {
	ClientID string
	Topic    string
	Payload  []byte
}

// Handler is invoked for each received publish message.
type Handler func(context.Context, PublishMessage)

type clientSession struct {
	conn          net.Conn
	reader        *bufio.Reader
	writeMu       sync.Mutex
	subscriptions map[string]struct{}
	clientID      string
	closed        atomic.Bool
}

func newSession(conn net.Conn) *clientSession {
	return &clientSession{
		conn:          conn,
		reader:        bufio.NewReader(conn),
		subscriptions: make(map[string]struct{}),
	}
}

func (c *clientSession) subscribed(topic string) bool {
	_, ok := c.subscriptions[topic]
	return ok
}

func (c *clientSession) addSubscription(topic string) {
	c.subscriptions[topic] = struct{}{}
}

func (c *clientSession) removeAllSubscriptions() {
	for k := range c.subscriptions {
		delete(c.subscriptions, k)
	}
}

func (c *clientSession) writePacket(packet []byte) error {
	if c.closed.Load() {
		return net.ErrClosed
	}
	c.writeMu.Lock()
	defer c.writeMu.Unlock()
	_, err := c.conn.Write(packet)
	return err
}

// Broker is a minimal MQTT v3.1.1 broker that supports QoS 0 publish and subscribe semantics.
type Broker struct {
	logger       *slog.Logger
	listener     net.Listener
	handler      atomic.Value // stores Handler
	mu           sync.Mutex
	wg           sync.WaitGroup
	shuttingDown atomic.Bool

	clientsMu sync.RWMutex
	clients   map[*clientSession]struct{}
}

// New constructs a broker with the supplied logger.
func New(logger *slog.Logger) *Broker {
	b := &Broker{logger: logger, clients: make(map[*clientSession]struct{})}
	b.handler.Store(Handler(func(context.Context, PublishMessage) {}))
	return b
}

// Start begins listening for MQTT clients on the provided bind address.
// The returned channel is closed once the accept loop terminates; fatal errors are sent on it.
func (b *Broker) Start(bind string) (<-chan error, error) {
	ln, err := net.Listen("tcp", bind)
	if err != nil {
		return nil, fmt.Errorf("mqtt listen: %w", err)
	}

	b.mu.Lock()
	b.listener = ln
	b.mu.Unlock()

	errCh := make(chan error, 1)

	b.logger.Info("mqtt broker listening", "addr", bind)

	b.wg.Add(1)
	go func() {
		defer b.wg.Done()
		for {
			conn, err := ln.Accept()
			if err != nil {
				if b.shuttingDown.Load() {
					close(errCh)
					return
				}
				if ne, ok := err.(net.Error); ok && ne.Temporary() {
					b.logger.Warn("temporary accept error", "error", err)
					time.Sleep(50 * time.Millisecond)
					continue
				}
				errCh <- fmt.Errorf("mqtt accept: %w", err)
				close(errCh)
				return
			}

			session := newSession(conn)
			b.addClient(session)

			b.wg.Add(1)
			go func() {
				defer b.wg.Done()
				b.handleConn(session)
			}()
		}
	}()

	return errCh, nil
}

// Stop shuts down the broker and releases resources.
func (b *Broker) Stop() error {
	if !b.shuttingDown.CompareAndSwap(false, true) {
		return nil
	}

	b.mu.Lock()
	ln := b.listener
	b.listener = nil
	b.mu.Unlock()

	if ln != nil {
		_ = ln.Close()
	}

	b.clientsMu.Lock()
	for session := range b.clients {
		session.closed.Store(true)
		_ = session.conn.Close()
	}
	b.clients = make(map[*clientSession]struct{})
	b.clientsMu.Unlock()

	b.wg.Wait()
	return nil
}

// SetPublishHandler installs the function invoked for each received publish.
func (b *Broker) SetPublishHandler(h Handler) {
	if h == nil {
		h = func(context.Context, PublishMessage) {}
	}
	b.handler.Store(h)
}

// Publish sends a QoS 0 message to all clients subscribed to the topic.
func (b *Broker) Publish(topic string, payload []byte) error {
	packet, err := buildPublishPacket(topic, payload)
	if err != nil {
		return err
	}

	b.clientsMu.RLock()
	defer b.clientsMu.RUnlock()

	for session := range b.clients {
		if session.subscribed(topic) {
			if err := session.writePacket(packet); err != nil {
				b.logger.Warn("publish to subscriber failed", "client", session.clientID, "error", err)
			}
		}
	}
	return nil
}

func (b *Broker) addClient(session *clientSession) {
	b.clientsMu.Lock()
	b.clients[session] = struct{}{}
	b.clientsMu.Unlock()
}

func (b *Broker) removeClient(session *clientSession) {
	b.clientsMu.Lock()
	delete(b.clients, session)
	b.clientsMu.Unlock()
}

func (b *Broker) handleConn(session *clientSession) {
	defer func() {
		session.closed.Store(true)
		b.removeClient(session)
		_ = session.conn.Close()
	}()

	ctx := context.Background()

	for {
		header, err := session.reader.ReadByte()
		if err != nil {
			if !errors.Is(err, io.EOF) {
				b.logger.Debug("read header error", "error", err)
			}
			return
		}

		remaining, err := readVarInt(session.reader)
		if err != nil {
			b.logger.Debug("read remaining length error", "error", err)
			return
		}

		payload := make([]byte, remaining)
		if _, err := io.ReadFull(session.reader, payload); err != nil {
			b.logger.Debug("read packet payload error", "error", err)
			return
		}

		packetType := header >> 4

		switch packetType {
		case 1: // CONNECT
			if err := b.handleConnect(session, payload); err != nil {
				b.logger.Debug("handle connect error", "error", err)
				return
			}
		case 3: // PUBLISH
			msg, err := parsePublish(header, payload)
			if err != nil {
				b.logger.Debug("parse publish error", "error", err)
				return
			}
			msg.ClientID = session.clientID
			if h, ok := b.handler.Load().(Handler); ok {
				safeInvoke(h, ctx, msg, b.logger)
			}
			b.forwardToSubscribers(msg.Topic, msg.Payload, session)
		case 8: // SUBSCRIBE
			if err := b.handleSubscribe(session, payload); err != nil {
				b.logger.Debug("handle subscribe error", "error", err)
				return
			}
		case 10: // UNSUBSCRIBE
			if err := b.writeUnsubAck(session, payload); err != nil {
				b.logger.Debug("write unsuback error", "error", err)
				return
			}
		case 12: // PINGREQ
			if err := session.writePacket([]byte{0xD0, 0x00}); err != nil {
				b.logger.Debug("write pingresp error", "error", err)
				return
			}
		case 14: // DISCONNECT
			return
		default:
			b.logger.Debug("unsupported packet", "type", packetType)
			return
		}
	}
}

func (b *Broker) handleConnect(session *clientSession, payload []byte) error {
	rd := bytesReader(payload)

	protoName, err := rd.readString()
	if err != nil {
		return fmt.Errorf("read protocol name: %w", err)
	}
	if protoName != "MQTT" {
		return fmt.Errorf("unsupported protocol %q", protoName)
	}

	level, err := rd.readByte()
	if err != nil {
		return fmt.Errorf("read protocol level: %w", err)
	}
	if level != 4 { // MQTT 3.1.1
		return fmt.Errorf("unsupported protocol level %d", level)
	}

	flags, err := rd.readByte()
	if err != nil {
		return fmt.Errorf("read connect flags: %w", err)
	}
	if flags&(1<<2) != 0 || flags&(1<<5) != 0 || flags&(1<<6) != 0 || flags&(1<<7) != 0 || flags&(1<<3) != 0 || flags&(1<<4) != 0 {
		return fmt.Errorf("unsupported connect flags %08b", flags)
	}

	if _, err := rd.readUint16(); err != nil { // keep alive
		return fmt.Errorf("read keepalive: %w", err)
	}

	clientID, err := rd.readString()
	if err != nil {
		return fmt.Errorf("read client id: %w", err)
	}
	if clientID == "" {
		clientID = fmt.Sprintf("anon-%d", time.Now().UnixNano())
	}
	session.clientID = clientID

	if err := session.writePacket([]byte{0x20, 0x02, 0x00, 0x00}); err != nil {
		return fmt.Errorf("write connack: %w", err)
	}

	return nil
}

func (b *Broker) handleSubscribe(session *clientSession, payload []byte) error {
	rd := bytesReader(payload)

	packetID, err := rd.readUint16()
	if err != nil {
		return fmt.Errorf("read packet id: %w", err)
	}

	topics := make([]string, 0, 1)
	for rd.remaining() > 0 {
		topic, err := rd.readString()
		if err != nil {
			return fmt.Errorf("read topic: %w", err)
		}
		if rd.remaining() == 0 {
			return fmt.Errorf("missing qos byte")
		}
		qos, err := rd.readByte()
		if err != nil {
			return fmt.Errorf("read qos: %w", err)
		}
		if qos != 0 {
			return fmt.Errorf("unsupported qos %d", qos)
		}
		session.addSubscription(topic)
		topics = append(topics, topic)
	}

	packet, err := buildSubAck(packetID, len(topics))
	if err != nil {
		return err
	}
	return session.writePacket(packet)
}

func (b *Broker) writeUnsubAck(session *clientSession, payload []byte) error {
	rd := bytesReader(payload)
	packetID, err := rd.readUint16()
	if err != nil {
		return fmt.Errorf("read packet id: %w", err)
	}
	session.removeAllSubscriptions()

	packet := []byte{0xB0, 0x02, byte(packetID >> 8), byte(packetID & 0xFF)}
	return session.writePacket(packet)
}

func (b *Broker) forwardToSubscribers(topic string, payload []byte, exclude *clientSession) {
	packet, err := buildPublishPacket(topic, payload)
	if err != nil {
		return
	}

	b.clientsMu.RLock()
	defer b.clientsMu.RUnlock()

	for session := range b.clients {
		if session == exclude {
			continue
		}
		if session.subscribed(topic) {
			if err := session.writePacket(packet); err != nil {
				b.logger.Debug("forward publish failed", "client", session.clientID, "error", err)
			}
		}
	}
}

func safeInvoke(h Handler, ctx context.Context, msg PublishMessage, logger *slog.Logger) {
	defer func() {
		if r := recover(); r != nil {
			logger.Error("publish handler panic", "panic", r)
		}
	}()
	h(ctx, msg)
}

func parsePublish(header byte, payload []byte) (PublishMessage, error) {
	qos := (header >> 1) & 0x03
	if qos != 0 {
		return PublishMessage{}, fmt.Errorf("unsupported qos %d", qos)
	}

	rd := bytesReader(payload)
	topic, err := rd.readString()
	if err != nil {
		return PublishMessage{}, fmt.Errorf("read topic: %w", err)
	}

	if rd.remaining() == 0 {
		return PublishMessage{Topic: topic, Payload: nil}, nil
	}

	data := rd.readBytes(rd.remaining())
	return PublishMessage{Topic: topic, Payload: data}, nil
}

func buildPublishPacket(topic string, payload []byte) ([]byte, error) {
	topicLen := len(topic)
	if topicLen > 65535 {
		return nil, fmt.Errorf("topic too long")
	}

	remaining := 2 + topicLen + len(payload)
	remainingBytes := encodeRemainingLength(remaining)

	packet := make([]byte, 0, 1+len(remainingBytes)+remaining)
	packet = append(packet, 0x30)
	packet = append(packet, remainingBytes...)
	packet = append(packet, byte(topicLen>>8), byte(topicLen&0xFF))
	packet = append(packet, topic...)
	packet = append(packet, payload...)
	return packet, nil
}

func buildSubAck(packetID uint16, topics int) ([]byte, error) {
	if topics <= 0 {
		return nil, fmt.Errorf("no topics to ack")
	}
	remaining := 2 + topics
	remainingBytes := encodeRemainingLength(remaining)
	packet := make([]byte, 0, 1+len(remainingBytes)+remaining)
	packet = append(packet, 0x90)
	packet = append(packet, remainingBytes...)
	packet = append(packet, byte(packetID>>8), byte(packetID&0xFF))
	for i := 0; i < topics; i++ {
		packet = append(packet, 0x00)
	}
	return packet, nil
}

type bytesReader []byte

func (b *bytesReader) readByte() (byte, error) {
	if len(*b) == 0 {
		return 0, io.EOF
	}
	v := (*b)[0]
	*b = (*b)[1:]
	return v, nil
}

func (b *bytesReader) readUint16() (uint16, error) {
	if len(*b) < 2 {
		return 0, io.EOF
	}
	v := uint16((*b)[0])<<8 | uint16((*b)[1])
	*b = (*b)[2:]
	return v, nil
}

func (b *bytesReader) readString() (string, error) {
	l, err := b.readUint16()
	if err != nil {
		return "", err
	}
	if len(*b) < int(l) {
		return "", io.ErrUnexpectedEOF
	}
	s := string((*b)[:l])
	*b = (*b)[l:]
	return s, nil
}

func (b *bytesReader) readBytes(n int) []byte {
	if len(*b) < n {
		n = len(*b)
	}
	out := make([]byte, n)
	copy(out, (*b)[:n])
	*b = (*b)[n:]
	return out
}

func (b *bytesReader) remaining() int {
	return len(*b)
}

func readVarInt(r *bufio.Reader) (int, error) {
	multiplier := 1
	value := 0
	for i := 0; i < 4; i++ {
		digit, err := r.ReadByte()
		if err != nil {
			return 0, err
		}
		value += int(digit&127) * multiplier
		if digit&128 == 0 {
			return value, nil
		}
		multiplier *= 128
	}
	return 0, fmt.Errorf("malformed remaining length")
}

func encodeRemainingLength(length int) []byte {
	if length < 0 {
		length = 0
	}

	var encoded []byte
	for {
		digit := byte(length % 128)
		length /= 128
		if length > 0 {
			digit |= 0x80
		}
		encoded = append(encoded, digit)
		if length == 0 {
			break
		}
	}
	return encoded
}
