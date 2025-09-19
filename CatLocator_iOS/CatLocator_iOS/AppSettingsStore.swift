import Foundation
import Combine

struct AppSettings: Codable {
    var serverHost: String = ""
    var serverPort: Int = 1883
    var username: String = ""
    var password: String = ""
    var rooms: [String] = ["Living Room", "Kitchen", "Bedroom"]
    var selectedRoom: String? = "Living Room"
}

struct ConnectionConfig: Equatable {
    let host: String
    let port: Int
    let username: String
    let password: String
}

final class AppSettingsStore: ObservableObject {
    @Published private(set) var settings: AppSettings {
        didSet { persist() }
    }

    private let defaultsKey = "CatLocatorAppSettings"
    private let encoder = JSONEncoder()
    private let decoder = JSONDecoder()

    init() {
        if let data = UserDefaults.standard.data(forKey: defaultsKey),
           let decoded = try? decoder.decode(AppSettings.self, from: data) {
            settings = decoded
        } else {
            settings = AppSettings()
            persist()
        }
        normalizeSelection()
    }

    var rooms: [String] {
        settings.rooms
    }

    var selectedRoom: String? {
        settings.selectedRoom ?? settings.rooms.first
    }

    func selectRoom(_ room: String) {
        guard !room.isEmpty else { return }
        if !settings.rooms.contains(room) {
            settings.rooms.append(room)
        }
        settings.selectedRoom = room
        persist()
    }

    func addRoom(named name: String) {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        if !settings.rooms.contains(trimmed) {
            settings.rooms.append(trimmed)
        }
        selectRoom(trimmed)
    }

    func removeRooms(at offsets: IndexSet) {
        var updated = settings.rooms
        offsets.forEach { index in
            if index < updated.count {
                let removed = updated.remove(at: index)
                if settings.selectedRoom == removed {
                    settings.selectedRoom = nil
                }
            }
        }
        settings.rooms = updated
        normalizeSelection()
        persist()
    }

    func updateServer(host: String) {
        settings.serverHost = host.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    func updatePort(_ port: Int) {
        let clamped = min(65535, max(1, port))
        settings.serverPort = clamped
    }

    func updateUsername(_ username: String) {
        settings.username = username
    }

    func updatePassword(_ password: String) {
        settings.password = password
    }

    var connectionConfigPublisher: AnyPublisher<ConnectionConfig, Never> {
        $settings
            .map { settings in
                ConnectionConfig(host: settings.serverHost,
                                 port: settings.serverPort,
                                 username: settings.username,
                                 password: settings.password)
            }
            .removeDuplicates()
            .eraseToAnyPublisher()
    }

    private func normalizeSelection() {
        if settings.rooms.isEmpty {
            settings.rooms = ["Living Room"]
        }
        if let selected = settings.selectedRoom, settings.rooms.contains(selected) {
            // ok
        } else {
            settings.selectedRoom = settings.rooms.first
        }
    }

    private func persist() {
        if let data = try? encoder.encode(settings) {
            UserDefaults.standard.set(data, forKey: defaultsKey)
        }
    }
}
