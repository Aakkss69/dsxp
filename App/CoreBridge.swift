import Foundation
import DSXPCore

/// Controller link as reported by the core.
enum ControllerLink {
    case none, bluetooth, usb, both

    init(_ raw: Int32) {
        switch UInt32(bitPattern: raw) {
        case DSXP_LINK_BT.rawValue: self = .bluetooth
        case DSXP_LINK_USB.rawValue: self = .usb
        case DSXP_LINK_BT_AND_USB.rawValue: self = .both
        default: self = .none
        }
    }

    var hasBluetooth: Bool { self == .bluetooth || self == .both }
}

extension dsxp_status_t {
    var controllerLink: ControllerLink { ControllerLink(link) }
    var xpExecutablePath: String { Self.string(xp_exe_path) }
    var commandsFilePath: String { Self.string(commands_path) }

    private static func string<T>(_ tuple: T) -> String {
        withUnsafeBytes(of: tuple) { raw in
            String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
        }
    }
}

/// The app's single handle on the C core (Core/include/dsxp_core.h).
@MainActor
final class CoreBridge {
    static let shared = CoreBridge()

    private(set) var status = dsxp_status_t()
    var statusChanged: (() -> Void)?
    var commandsLoaded: (() -> Void)?

    @discardableResult
    func start(commandsPath: String?, importLegacy: Bool) -> Int32 {
        let data = Paths.dataDir.path
        let logs = Paths.logDir.path
        let commands = commandsPath ?? ""
        let legacy = importLegacy ? Paths.legacyBindings.path : nil
        var verbose = false
        #if DEBUG
        verbose = true
        #endif
        let rc = data.withCString { d in
            logs.withCString { l in
                commands.withCString { c in
                    withOptionalCString(legacy) { lg -> Int32 in
                        var cfg = dsxp_config_t()
                        cfg.data_dir = d
                        cfg.log_dir = l
                        cfg.commands_path = c
                        cfg.import_bindings = lg
                        cfg.log_retention_days = 14
                        cfg.verbose = verbose
                        cfg.on_event = coreEvent
                        return dsxp_start(&cfg)
                    }
                }
            }
        }
        refresh()
        return rc
    }

    func stop() { dsxp_stop(1.0) }

    func refresh() {
        var s = dsxp_status_t()
        dsxp_get_status(&s)
        status = s
        statusChanged?()
    }

    func setCommandsPath(_ path: String) { dsxp_set_commands_path(path) }

    func inputAccess(request: Bool) -> dsxp_access_t { dsxp_input_access(request) }

    /// One call of the mapper's JSON API.
    func api(method: String, path: String, query: String) -> (status: Int, contentType: String, body: Data) {
        var r = dsxp_response_t()
        method.withCString { m in
            path.withCString { p in
                query.withCString { q in dsxp_api(m, p, q, &r) }
            }
        }
        defer { dsxp_response_free(&r) }
        let type = r.content_type.map { String(cString: $0) } ?? "text/plain; charset=utf-8"
        let body = r.body.map { Data(bytes: $0, count: r.len) } ?? Data()
        return (Int(r.status), type, body)
    }

    fileprivate func handle(event: Int) {
        switch event {
        case DSXP_EV_STATUS: refresh()
        case DSXP_EV_COMMANDS_LOADED: refresh(); commandsLoaded?()
        default: break
        }
    }
}

/// Called by the core on its own threads; hop to the main thread.
private func coreEvent(_ ctx: UnsafeMutableRawPointer?, _ event: Int32) {
    let ev = Int(event)
    DispatchQueue.main.async {
        MainActor.assumeIsolated { CoreBridge.shared.handle(event: ev) }
    }
}

private func withOptionalCString<R>(_ s: String?, _ body: (UnsafePointer<CChar>?) -> R) -> R {
    if let s { return s.withCString { body($0) } }
    return body(nil)
}
