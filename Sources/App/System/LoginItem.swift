import AppKit
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
            catch { report("turn off", error) }
        case .off:
            do { try SMAppService.mainApp.register() }
            catch { report("turn on", error) }
            if SMAppService.mainApp.status == .requiresApproval {
                SMAppService.openSystemSettingsLoginItems()
            }
        }
    }

    /// A failed change is shown, not only logged: the menu item simply staying as it was
    /// would leave the user guessing.
    private static func report(_ what: String, _ error: Error) {
        NSLog("DSXP: login item \(what) failed: \(error.localizedDescription)")
        let alert = NSAlert()
        alert.messageText = "Could not \(what) Launch at Login"
        alert.informativeText = error.localizedDescription
        alert.addButton(withTitle: "OK")
        alert.runModal()
    }
}
