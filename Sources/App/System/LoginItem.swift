import Foundation
import ServiceManagement

/// Launch at Login through SMAppService. Only offered for a copy installed in
/// /Applications, so a build in DerivedData never registers itself.
@MainActor
enum LoginItem {
    static var canToggle: Bool { Bundle.main.bundlePath.hasPrefix("/Applications/") }

    static var isEnabled: Bool { SMAppService.mainApp.status == .enabled }

    static func toggle() {
        do {
            if isEnabled { try SMAppService.mainApp.unregister() }
            else { try SMAppService.mainApp.register() }
        } catch {
            NSLog("DSXP: login item change failed: \(error.localizedDescription)")
        }
        if SMAppService.mainApp.status == .requiresApproval {
            SMAppService.openSystemSettingsLoginItems()
        }
    }
}
