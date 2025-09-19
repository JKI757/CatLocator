import Foundation
import Combine
import Network

enum MQTTCommand: String {
    case start
    case stop
}

struct CommandMessage: Codable {
    let room: String
    let command: String
    let timestamp: String
    let source: String
}

final class MQTTManager: ObservableObject {
    enum ConnectionState: Equatable {
        case disconnected
        case connecting
        case connected
        case error(String)
    }

    @Published private(set) var state: ConnectionState = .disconnected

    private let settingsStore: AppSettingsStore
    private let queue = DispatchQueue(label: "catlocator.mqtt")
    private var cancellables = Set<AnyCancellable>()
    private var connection: NWConnection?
    private var pendingPublishes: [Data] = []
    private var currentConfig: ConnectionConfig?
    private let clientID = "catlocator-ios-" + UUID().uuidString.prefix(8)

    init(settingsStore: AppSettingsStore) {
        self.settingsStore = settingsStore

        settingsStore.connectionConfigPublisher
            .receive(on: DispatchQueue.main)
            .sink { [weak self] config in
                self?.reconfigure(with: config)
            }
            .store(in: &cancellables)
    }

    func connect() {
        queue.async { [weak self] in
            self?.connectIfNeeded()
        }
    }

    deinit {
        closeConnection()
    }

    func publishCommand(room: String, command: MQTTCommand) {
        let isoFormatter = ISO8601DateFormatter()
        isoFormatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        let payloadStruct = CommandMessage(room: room,
                                           command: command.rawValue,
                                           timestamp: isoFormatter.string(from: Date()),
                                           source: "catlocator-ios")
        guard let payloadData = try? JSONEncoder().encode(payloadStruct) else {
            return
        }
        let topic = "catlocator/training/commands"
        let packet = makePublishPacket(topic: topic, payload: payloadData)

        queue.async { [weak self] in
            guard let self = self else { return }
            if self.state != .connected {
                self.pendingPublishes.append(packet)
                self.connectIfNeeded()
            } else {
                self.send(packet)
            }
        }
    }

    private func reconfigure(with config: ConnectionConfig) {
        queue.async {
            self.currentConfig = config
            self.closeConnection()
            self.connectIfNeeded()
        }
    }

    private func connectIfNeeded() {
        guard let config = currentConfig, !config.host.isEmpty else {
            DispatchQueue.main.async { self.state = .disconnected }
            return
        }
        if case .connecting = state { return }
        if case .connected = state { return }

        DispatchQueue.main.async { self.state = .connecting }

        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        let connection = NWConnection(host: NWEndpoint.Host(config.host),
                                      port: NWEndpoint.Port(integerLiteral: UInt16(config.port)),
                                      using: params)
        self.connection = connection

        connection.stateUpdateHandler = { [weak self] newState in
            guard let self = self else { return }
            switch newState {
            case .ready:
                self.handleConnectionReady()
            case .failed(let error):
                self.handleConnectionError(error.localizedDescription)
            case .waiting(let error):
                self.handleConnectionError(error.localizedDescription)
            case .cancelled:
                DispatchQueue.main.async { self.state = .disconnected }
            default:
                break
            }
        }

        connection.start(queue: queue)
    }

    private func handleConnectionReady() {
        guard let config = currentConfig else { return }
        let packet = makeConnectPacket(config: config)
        send(packet)
        receiveConnAck()
    }

    private func receiveConnAck() {
        connection?.receive(minimumIncompleteLength: 4, maximumLength: 4) { [weak self] data, _, _, error in
            guard let self = self else { return }
            if let data, data.count >= 4, data[0] == 0x20, data[1] == 0x02, data[3] == 0x00 {
                DispatchQueue.main.async { self.state = .connected }
                self.flushPending()
            } else {
                let message = error?.localizedDescription ?? "MQTT CONNACK failed"
                self.handleConnectionError(message)
            }
        }
    }

    private func flushPending() {
        pendingPublishes.forEach { send($0) }
        pendingPublishes.removeAll()
    }

    private func send(_ packet: Data) {
        connection?.send(content: packet, completion: .contentProcessed({ [weak self] error in
            if let error = error {
                self?.handleConnectionError(error.localizedDescription)
            }
        }))
    }

    private func handleConnectionError(_ message: String) {
        DispatchQueue.main.async { self.state = .error(message) }
        closeConnection()
    }

    func disconnect() {
        queue.async { [weak self] in
            self?.closeConnection()
        }
    }

    private func closeConnection() {
        connection?.cancel()
        connection = nil
        DispatchQueue.main.async { self.state = .disconnected }
    }

    private func makeConnectPacket(config: ConnectionConfig) -> Data {
        var payload = Data()
        payload.append(0x00)
        payload.append(0x04)
        payload.append(contentsOf: "MQTT".utf8)
        payload.append(0x04) // protocol level 4

        var connectFlags: UInt8 = 0x02 // clean session
        if !config.username.isEmpty { connectFlags |= 0x80 }
        if !config.password.isEmpty { connectFlags |= 0x40 }
        payload.append(connectFlags)

        let keepAlive: UInt16 = 60
        payload.append(UInt8(keepAlive >> 8))
        payload.append(UInt8(keepAlive & 0xFF))

        appendString(clientID, to: &payload)
        if !config.username.isEmpty {
            appendString(config.username, to: &payload)
        }
        if !config.password.isEmpty {
            appendString(config.password, to: &payload)
        }

        var packet = Data([0x10])
        packet.append(encodeRemainingLength(payload.count))
        packet.append(payload)
        return packet
    }

    private func makePublishPacket(topic: String, payload: Data) -> Data {
        var body = Data()
        appendString(topic, to: &body)
        body.append(payload)

        var packet = Data([0x30])
        packet.append(encodeRemainingLength(body.count))
        packet.append(body)
        return packet
    }

    private func appendString(_ string: String, to data: inout Data) {
        let utf8 = Array(string.utf8)
        let length = UInt16(utf8.count)
        data.append(UInt8(length >> 8))
        data.append(UInt8(length & 0xFF))
        data.append(contentsOf: utf8)
    }

    private func encodeRemainingLength(_ length: Int) -> Data {
        var value = length
        var encoded = Data()
        repeat {
            var digit = UInt8(value % 128)
            value /= 128
            if value > 0 {
                digit |= 0x80
            }
            encoded.append(digit)
        } while value > 0
        return encoded
    }
}
