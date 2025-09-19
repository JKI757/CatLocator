import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var settingsStore: AppSettingsStore
    @Environment(\.dismiss) private var dismiss

    @State private var hostText: String = ""
    @State private var portText: String = ""
    @State private var username: String = ""
    @State private var password: String = ""
    @State private var newRoom: String = ""

    var body: some View {
        NavigationStack {
            Form {
                Section("MQTT Server") {
                    TextField("Host", text: $hostText)
                        .textContentType(.URL)
                        .keyboardType(.URL)
                    TextField("Port", text: $portText)
                        .keyboardType(.numberPad)
                    TextField("Username", text: $username)
                        .textContentType(.username)
                    SecureField("Password", text: $password)
                        .textContentType(.password)
                }

                Section("Rooms") {
                    if settingsStore.rooms.isEmpty {
                        Text("No rooms configured")
                            .foregroundColor(.secondary)
                    } else {
                        ForEach(settingsStore.rooms, id: \.self) { room in
                            HStack {
                                Text(room)
                                Spacer()
                                if settingsStore.selectedRoom == room {
                                    Image(systemName: "checkmark")
                                        .foregroundColor(.accentColor)
                                }
                            }
                            .contentShape(Rectangle())
                            .onTapGesture {
                                settingsStore.selectRoom(room)
                            }
                        }
                        .onDelete(perform: deleteRooms)
                    }

                    HStack {
                        TextField("Add room", text: $newRoom)
                        Button("Add") {
                            settingsStore.addRoom(named: newRoom)
                            newRoom = ""
                        }
                        .disabled(newRoom.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                    }
                }
            }
            .navigationTitle("Settings")
            .toolbar {
                ToolbarItem(placement: .navigationBarTrailing) {
                    EditButton()
                }
                ToolbarItem(placement: .cancellationAction) {
                    Button("Done") {
                        applyChanges()
                        dismiss()
                    }
                }
            }
            .onAppear(perform: loadFields)
        }
    }

    private func loadFields() {
        hostText = settingsStore.settings.serverHost
        portText = String(settingsStore.settings.serverPort)
        username = settingsStore.settings.username
        password = settingsStore.settings.password
    }

    private func applyChanges() {
        settingsStore.updateServer(host: hostText)
        if let portValue = Int(portText) {
            settingsStore.updatePort(portValue)
        }
        settingsStore.updateUsername(username)
        settingsStore.updatePassword(password)
    }

    private func deleteRooms(at offsets: IndexSet) {
        settingsStore.removeRooms(at: offsets)
    }
}
