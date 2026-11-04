import Foundation

/// An flock on Application Support/DSXP/instance.lock. The lock dies with the
/// process, so a crash never leaves it stuck. dsxp-cli takes the same lock.
enum SingleInstance {
    static let openMapperNotification = Notification.Name("io.github.aakkss69.DSXP.openMapper")

    nonisolated(unsafe) private static var lockFD: Int32 = -1

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
