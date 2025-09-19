import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var settingsStore: AppSettingsStore
    @EnvironmentObject private var mqttManager: MQTTManager

    @State private var showingSettings = false
    @State private var commandStatus: String?
    @State private var commandStatusWorkItem: DispatchWorkItem?

    var body: some View {
        NavigationStack {
            VStack(spacing: 32) {
                roomPicker
                commandButtons
                connectionStatus
                connectionControls
            }
            .padding()
            .navigationTitle("CatLocator Trainer")
            .toolbar {
                ToolbarItem(placement: .primaryAction) {
                    Button(action: { showingSettings = true }) {
                        Image(systemName: "gearshape")
                    }
                    .accessibilityLabel("Settings")
                }
            }
        }
        .sheet(isPresented: $showingSettings) {
            SettingsView()
                .environmentObject(settingsStore)
        }
        .onAppear {
            mqttManager.connect()
        }
    }

    private var roomPicker: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Room")
                .font(.headline)
            Picker("Room", selection: Binding(
                get: { settingsStore.selectedRoom ?? settingsStore.rooms.first ?? "" },
                set: { settingsStore.selectRoom($0) }
            )) {
                ForEach(settingsStore.rooms, id: \.self) { room in
                    Text(room).tag(room)
                }
            }
            .pickerStyle(.menu)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var commandButtons: some View {
        VStack(spacing: 12) {
            HStack(spacing: 24) {
                Button(action: { sendCommand(.start) }) {
                    Label("Start", systemImage: "play.circle")
                        .padding()
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)

                Button(action: { sendCommand(.stop) }) {
                    Label("Stop", systemImage: "stop.circle")
                        .padding()
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
            }

            if let status = commandStatus {
                Text(status)
                    .font(.footnote)
                    .foregroundColor(.secondary)
                    .transition(.opacity)
            }
        }
    }

    private var connectionStatus: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(statusColor)
                .frame(width: 12, height: 12)
            Text(statusText)
                .font(.subheadline)
                .foregroundColor(.secondary)
            Spacer()
        }
    }

    private var connectionControls: some View {
        HStack(spacing: 16) {
            Button("Connect") {
                mqttManager.connect()
            }
            .buttonStyle(.borderedProminent)
            .disabled(mqttManager.state == .connected || mqttManager.state == .connecting)

            Button("Disconnect") {
                mqttManager.disconnect()
            }
            .buttonStyle(.bordered)
            .disabled(mqttManager.state == .disconnected)
        }
    }

    private var statusText: String {
        switch mqttManager.state {
        case .disconnected:
            return "Disconnected"
        case .connecting:
            return "Connecting…"
        case .connected:
            return "Connected"
        case .error(let message):
            return "Error: \(message)"
        }
    }

    private var statusColor: Color {
        switch mqttManager.state {
        case .connected:
            return .green
        case .connecting:
            return .orange
        case .error:
            return .red
        case .disconnected:
            return .gray
        }
    }

    private func sendCommand(_ command: MQTTCommand) {
        guard let room = settingsStore.selectedRoom else {
            showStatus(message: "No room selected")
            return
        }
        mqttManager.publishCommand(room: room, command: command)
        showStatus(message: "Sent \(command.rawValue.capitalized) for \(room)")
    }

    private func showStatus(message: String) {
        withAnimation {
            commandStatus = message
        }

        commandStatusWorkItem?.cancel()
        let workItem = DispatchWorkItem {
            withAnimation {
                commandStatus = nil
            }
        }
        commandStatusWorkItem = workItem
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0, execute: workItem)
    }
}

#Preview {
    let store = AppSettingsStore()
    let mqtt = MQTTManager(settingsStore: store)
    ContentView()
        .environmentObject(store)
        .environmentObject(mqtt)
}
