import AppKit

/// The three config files in ~/Library/Application Support/DSXP. The core writes
/// them on first run (Sources/Core/runtime/config_store.c); this only opens them.
enum ConfigFiles {
    static let names = ["bridge.conf", "controls.conf", "feedback.conf"]

    /// Only the three known names resolve: the mapper page passes the name in a URL.
    static func url(_ name: String) -> URL? {
        names.contains(name) ? Paths.dataDir.appendingPathComponent(name) : nil
    }

    /// Opens one file in the app chosen for .conf files, or TextEdit when there is
    /// none. An empty name opens the folder in Finder.
    @MainActor @discardableResult
    static func open(_ name: String) -> Bool {
        let ws = NSWorkspace.shared
        if name.isEmpty {
            try? FileManager.default.createDirectory(at: Paths.dataDir, withIntermediateDirectories: true)
            return ws.open(Paths.dataDir)
        }
        guard let file = url(name), FileManager.default.fileExists(atPath: file.path) else { return false }
        if ws.urlForApplication(toOpen: file) != nil { return ws.open(file) }
        guard let textEdit = ws.urlForApplication(withBundleIdentifier: "com.apple.TextEdit") else { return false }
        ws.open([file], withApplicationAt: textEdit, configuration: NSWorkspace.OpenConfiguration())
        return true
    }
}
