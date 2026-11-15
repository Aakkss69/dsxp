import AppKit

/// An flock on Application Support/DSXP/instance.lock. The lock dies with the
/// process, so a crash never leaves it stuck. dsxp-cli takes the same lock.
enum SingleInstance {
    static let openMapperNotification = Notification.Name("io.github.aakkss69.DSXP.openMapper")

    nonisolated(unsafe) private static var lockFD: Int32 = -1

    /// Try the lock for up to `seconds`: an instance that is still quitting holds it for
    /// about a second (150 ms, then up to 1 s in dsxp_stop).
    static func acquire(waitingUpTo seconds: Double) -> Bool {
        let until = Date().addingTimeInterval(seconds)
        while Date() < until {
            if acquire() { return true }
            usleep(100_000)
        }
        return acquire()
    }

    /// Another copy of the app itself is running (not dsxp-cli, which takes the same lock).
    static func otherAppIsRunning() -> Bool {
        guard let id = Bundle.main.bundleIdentifier else { return false }
        let me = ProcessInfo.processInfo.processIdentifier
        return NSRunningApplication.runningApplications(withBundleIdentifier: id)
            .contains { $0.processIdentifier != me && !$0.isTerminated }
    }

    /// The lock is held, but not by the app: say so instead of quitting without a word.
    @MainActor static func reportBusy() {
        let alert = NSAlert()
        alert.messageText = "DSXP is already running"
        alert.informativeText = "Another DSXP bridge holds the controller, most likely dsxp-cli "
            + "in a Terminal window. Quit it, then open DSXP again."
        alert.addButton(withTitle: "OK")
        NSApp.activate(ignoringOtherApps: true)
        alert.runModal()
    }

    static func acquire() -> Bool {
        let dir = Paths.dataDir
        try? FileManager.default.createDirectory(
            at: dir, withIntermediateDirectories: true, attributes: [.posixPermissions: 0o700])
        let fd = open(dir.appendingPathComponent("instance.lock").path, O_RDWR | O_CREAT | O_CLOEXEC, 0o600)
        guard fd >= 0 else { return true }          // cannot lock: do not block startup
        if flock(fd, LOCK_EX | LOCK_NB) != 0 {
            close(fd)
            return false
        }
        lockFD = fd                                 // held for the life of the process
        return true
    }

    static func signalRunningInstance() {
        DistributedNotificationCenter.default().postNotificationName(
            openMapperNotification, object: nil, userInfo: nil, deliverImmediately: true)
    }
}
