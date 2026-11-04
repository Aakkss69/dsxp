import Foundation
import ServiceManagement

/// Launch at Login through SMAppService. Only offered for a copy installed in
/// /Applications, so a build in DerivedData never registers itself.
@MainActor
enum LoginItem {
    enum State { case off, on, needsApproval }

    static var canToggle: Bool { Bundle.main.bundlePath.hasPrefix("/Applications/") }

    /// SMAppService has four states; "requires approval" means registered but
    /// waiting for the user in System Settings › General › Login Items.
    static var state: State {
        switch SMAppService.mainApp.status {
        case .enabled: return .on
        case .requiresApproval: return .needsApproval
        default: return .off
        }
    }

    static func toggle() {
        switch state {
        case .needsApproval:
            // registered already; only the user can approve it
            SMAppService.openSystemSettingsLoginItems()
            return
        case .on:
            do { try SMAppService.mainApp.unregister() }
            catch { NSLog("DSXP: login item unregister failed: \(error.localizedDescription)") }
        case .off:
            do { try SMAppService.mainApp.register() }
            catch { NSLog("DSXP: login item register failed: \(error.localizedDescription)") }
            if SMAppService.mainApp.status == .requiresApproval {
                SMAppService.openSystemSettingsLoginItems()
            }
        }
    }
}
