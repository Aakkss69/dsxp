import Foundation

/// Where DSXP keeps its files. Nothing is ever written inside the app bundle.
enum Paths {
    /// ~/Library/Application Support/DSXP (bridge.conf, controls.conf, feedback.conf, instance.lock)
    static var dataDir: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("DSXP", isDirectory: true)
    }

    /// ~/Library/Logs/DSXP (events-*.log, reports-*.log)
    static var logDir: URL {
        FileManager.default.urls(for: .libraryDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Logs/DSXP", isDirectory: true)
    }

    /// Settings of the original prototype, imported once if present.
    static var legacyBindings: URL {
        FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("dualsense-xplane/bindings.conf")
    }
}
