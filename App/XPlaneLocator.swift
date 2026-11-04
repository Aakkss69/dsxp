import Foundation

/// Finds the X-Plane 12 folder, whose Resources/plugins/Commands.txt gives the
/// mapper its searchable command list. That file belongs to X-Plane and is read
/// in place; it is never copied into DSXP.
enum XPlaneLocator {
    static let defaultsKey = "XPlaneFolder"

    static func commandsFile(in folder: URL) -> URL {
        folder.appendingPathComponent("Resources/plugins/Commands.txt")
    }

    static func isValid(_ folder: URL) -> Bool {
        FileManager.default.isReadableFile(atPath: commandsFile(in: folder).path)
    }

    static var saved: URL? {
        UserDefaults.standard.string(forKey: defaultsKey).map { URL(fileURLWithPath: $0, isDirectory: true) }
    }

    static func save(_ folder: URL) {
        UserDefaults.standard.set(folder.path, forKey: defaultsKey)
    }

    /// Saved choice first, then the usual install locations.
    static func locate() -> URL? {
        var candidates: [URL] = []
        if let s = saved { candidates.append(s) }
        candidates.append(FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("X-Plane/X-Plane 12", isDirectory: true))
        candidates.append(URL(fileURLWithPath: "/Applications/X-Plane 12", isDirectory: true))
        return candidates.first(where: isValid)
    }

    /// The folder of a running X-Plane, from ".../X-Plane 12/X-Plane.app/Contents/MacOS/X-Plane".
    static func folder(fromExecutable exe: String) -> URL? {
        let suffix = "/X-Plane.app/Contents/MacOS/X-Plane"
        guard exe.hasSuffix(suffix) else { return nil }
        let folder = URL(fileURLWithPath: String(exe.dropLast(suffix.count)), isDirectory: true)
        return isValid(folder) ? folder : nil
    }
}
