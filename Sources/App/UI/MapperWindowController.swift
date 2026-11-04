import AppKit
import WebKit

/// The mapper window: the web UI in a WKWebView, served by SchemeHandler.
/// While it is open DSXP behaves like a normal app (Dock icon, Cmd-Tab); when it
/// closes the web view is released, which stops polling, and DSXP goes back to
/// living in the menu bar.
@MainActor
final class MapperWindowController: NSObject, NSWindowDelegate, WKUIDelegate, WKNavigationDelegate {
    let scheme = SchemeHandler()
    private(set) var window: NSWindow?
    private var webView: WKWebView?

    func show() {
        if window == nil { build() }
        NSApp.setActivationPolicy(.regular)
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate()
    }

    private func build() {
        let config = WKWebViewConfiguration()
        config.setURLSchemeHandler(scheme, forURLScheme: "dsxp")
        config.websiteDataStore = .nonPersistent()

        let web = WKWebView(frame: .zero, configuration: config)
        #if DEBUG
        web.isInspectable = true
        #endif
        web.uiDelegate = self
        web.navigationDelegate = self
        web.underPageBackgroundColor = .windowBackgroundColor   // public API; no private KVC

        let win = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1280, height: 900),
                           styleMask: [.titled, .closable, .miniaturizable, .resizable],
                           backing: .buffered, defer: false)
        win.title = "DSXP"
        win.isReleasedWhenClosed = false
        win.minSize = NSSize(width: 760, height: 560)
        win.contentView = web
        win.delegate = self
        if !win.setFrameUsingName("MapperWindow") { win.center() }
        win.setFrameAutosaveName("MapperWindow")

        window = win
        webView = web
        web.load(URLRequest(url: URL(string: "dsxp://app/")!))
    }

    func windowWillClose(_ notification: Notification) {
        // the motor test buttons keep running until Stop; closing the window stops them
        _ = CoreBridge.shared.api(method: "POST", path: "/api/rumble", query: "left=0&right=0")
        webView?.stopLoading()
        window?.contentView = nil
        webView = nil
        window = nil
        NSApp.setActivationPolicy(.accessory)
    }

    // MARK: WKUIDelegate - the page uses confirm() for Quit

    func webView(_ webView: WKWebView, runJavaScriptConfirmPanelWithMessage message: String,
                 initiatedByFrame frame: WKFrameInfo) async -> Bool {
        guard let win = window else { return false }
        let alert = NSAlert()
        alert.messageText = message
        alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Cancel")
        return await alert.beginSheetModal(for: win) == .alertFirstButtonReturn
    }

    func webView(_ webView: WKWebView, runJavaScriptAlertPanelWithMessage message: String,
                 initiatedByFrame frame: WKFrameInfo) async {
        guard let win = window else { return }
        let alert = NSAlert()
        alert.messageText = message
        _ = await alert.beginSheetModal(for: win)
    }

    // MARK: WKNavigationDelegate - the window only ever shows DSXP's own page

    /// The page's WebContent process can be killed by the system (memory pressure)
    /// or crash; without this the window would stay blank.
    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        webView.load(URLRequest(url: URL(string: "dsxp://app/")!))
    }

    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction) async
        -> WKNavigationActionPolicy {
        navigationAction.request.url?.scheme == "dsxp" ? .allow : .cancel
    }
}
