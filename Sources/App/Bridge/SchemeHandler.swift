import Foundation
import WebKit

/// Serves the mapper UI on dsxp://app/ - the bundled web files plus the JSON API.
/// Nothing listens on a network port, so no other program or web page can
/// reach the API.
@MainActor
final class SchemeHandler: NSObject, WKURLSchemeHandler {
    /// Asks the user, then answers true if DSXP is quitting.
    var onQuit: ((@escaping @MainActor (Bool) -> Void) -> Void)?
    var onPickXPlane: ((@escaping @MainActor (Bool) -> Void) -> Void)?
    var onOpenAccess: (() -> Void)?

    private var live = Set<ObjectIdentifier>()

    private static let files: [String: (name: String, type: String)] = [
        "/": ("index.html", "text/html; charset=utf-8"),
        "/index.html": ("index.html", "text/html; charset=utf-8"),
        "/app.css": ("app.css", "text/css; charset=utf-8"),
        "/app.js": ("app.js", "text/javascript; charset=utf-8"),
    ]

    private static let csp = "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; "
        + "connect-src 'self'; img-src 'self' data:; object-src 'none'; base-uri 'none'"

    func webView(_ webView: WKWebView, start urlSchemeTask: any WKURLSchemeTask) {
        let id = ObjectIdentifier(urlSchemeTask)
        live.insert(id)
        guard let url = urlSchemeTask.request.url else {
            respond(urlSchemeTask, 400, "text/plain; charset=utf-8", Data("bad request".utf8))
            return
        }
        let method = urlSchemeTask.request.httpMethod ?? "GET"
        let path = url.path.isEmpty ? "/" : url.path
        let query = URLComponents(url: url, resolvingAgainstBaseURL: false)?.percentEncodedQuery ?? ""

        if let file = Self.files[path] {
            guard method == "GET" else { return respond(urlSchemeTask, 405, "text/plain", Data()) }
            serve(file: file.name, type: file.type, task: urlSchemeTask)
            return
        }
        switch path {
        case "/api/quit":
            guard method == "POST" else { return respond(urlSchemeTask, 405, "text/plain", Data()) }
            guard let quit = onQuit else { return respondOK(urlSchemeTask) }
            quit { [weak self] ok in
                self?.respond(urlSchemeTask, 200, "application/json", Data("{\"ok\":\(ok)}".utf8))
            }
        case "/api/pickxp":
            guard method == "POST" else { return respond(urlSchemeTask, 405, "text/plain", Data()) }
            guard let pick = onPickXPlane else { return respondOK(urlSchemeTask) }
            pick { [weak self] ok in
                self?.respond(urlSchemeTask, 200, "application/json", Data("{\"ok\":\(ok)}".utf8))
            }
        case "/api/openaccess":
            guard method == "POST" else { return respond(urlSchemeTask, 405, "text/plain", Data()) }
            onOpenAccess?()
            respondOK(urlSchemeTask)
        case "/api/openconfig":
            // ?file=controls.conf opens that file, no file opens the folder
            guard method == "POST" else { return respond(urlSchemeTask, 405, "text/plain", Data()) }
            let file = URLComponents(url: url, resolvingAgainstBaseURL: false)?
                .queryItems?.first { $0.name == "file" }?.value ?? ""
            guard file.isEmpty || ConfigFiles.url(file) != nil else {
                return respond(urlSchemeTask, 400, "text/plain; charset=utf-8", Data("unknown config file".utf8))
            }
            let opened = ConfigFiles.open(file)
            respond(urlSchemeTask, 200, "application/json", Data("{\"ok\":\(opened)}".utf8))
        default:
            if path.hasPrefix("/api/") {
                let r = CoreBridge.shared.api(method: method, path: path, query: query)
                respond(urlSchemeTask, r.status, r.contentType, r.body)
            } else {
                respond(urlSchemeTask, 404, "text/plain; charset=utf-8", Data("no route".utf8))
            }
        }
    }

    func webView(_ webView: WKWebView, stop urlSchemeTask: any WKURLSchemeTask) {
        live.remove(ObjectIdentifier(urlSchemeTask))
    }

    private func serve(file: String, type: String, task: any WKURLSchemeTask) {
        guard let url = Bundle.main.resourceURL?.appendingPathComponent("web/\(file)"),
              let data = try? Data(contentsOf: url) else {
            respond(task, 404, "text/plain; charset=utf-8", Data("no route".utf8))
            return
        }
        respond(task, 200, type, data, extra: type.hasPrefix("text/html") ? ["Content-Security-Policy": Self.csp] : [:])
    }

    private func respondOK(_ task: any WKURLSchemeTask) {
        respond(task, 200, "application/json", Data("{\"ok\":true}".utf8))
    }

    private func respond(_ task: any WKURLSchemeTask, _ status: Int, _ type: String, _ body: Data,
                         extra: [String: String] = [:]) {
        let id = ObjectIdentifier(task)
        guard live.contains(id), let url = task.request.url else { return }   // stopped tasks must not be touched
        live.remove(id)
        var headers = ["Content-Type": type, "Content-Length": "\(body.count)", "Cache-Control": "no-store"]
        headers.merge(extra) { _, new in new }
        guard let response = HTTPURLResponse(url: url, statusCode: status, httpVersion: "HTTP/1.1",
                                             headerFields: headers) else { return }
        task.didReceive(response)
        task.didReceive(body)
        task.didFinish()
    }
}
