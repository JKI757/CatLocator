import SwiftUI

@main
struct CatLocator_iOSApp: App {
    @StateObject private var settingsStore: AppSettingsStore
    @StateObject private var mqttManager: MQTTManager

    init() {
        let store = AppSettingsStore()
        _settingsStore = StateObject(wrappedValue: store)
        _mqttManager = StateObject(wrappedValue: MQTTManager(settingsStore: store))
    }

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(settingsStore)
                .environmentObject(mqttManager)
        }
    }
}
