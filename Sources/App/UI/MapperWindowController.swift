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
    private var colorObservers: [NSObjectProtocol] = []
    private var appearanceObservation: NSKeyValueObservation?

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
        win.title = "DSXP Mapper"
        win.subtitle = "DualSense → X-Plane 12"
        // an empty toolbar gives the standard unified title bar that shows the subtitle
        win.toolbar = NSToolbar(identifier: "MapperToolbar")
        win.toolbarStyle = .unified
        win.isReleasedWhenClosed = false
        win.minSize = NSSize(width: 760, height: 560)
        win.contentView = web
        win.delegate = self
        if !win.setFrameUsingName("MapperWindow") { win.center() }
        win.setFrameAutosaveName("MapperWindow")

        window = win
        webView = web
        observeSystemColors(win)
        web.load(URLRequest(url: URL(string: "dsxp://app/")!))
    }

    // MARK: system colors

    /// The page draws with the system's semantic colors, but WebKit has no CSS
    /// keyword for the window background or the user's accent color, so they are
    /// passed in as --win and --accent. Re-sent when the appearance (Light/Dark)
    /// or the accent color changes.
    private func pushSystemColors() {
        guard let web = webView, let win = window else { return }
        var accent = "", background = ""
        win.effectiveAppearance.performAsCurrentDrawingAppearance {
            accent = Self.css(NSColor.controlAccentColor)
            background = Self.css(NSColor.windowBackgroundColor)
        }
        guard !accent.isEmpty, !background.isEmpty else { return }
        web.evaluateJavaScript("""
            document.documentElement.style.setProperty('--accent','\(accent)');
            document.documentElement.style.setProperty('--win','\(background)');
            """)
    }

    private static func css(_ color: NSColor) -> String {
        guard let c = color.usingColorSpace(.sRGB) else { return "" }
        func byte(_ v: CGFloat) -> Int { Int((min(max(v, 0), 1) * 255).rounded()) }
        return String(format: "rgba(%d,%d,%d,%.3f)", byte(c.redComponent), byte(c.greenComponent),
                      byte(c.blueComponent), Double(c.alphaComponent))
    }

    private func observeSystemColors(_ win: NSWindow) {
        colorObservers.append(NotificationCenter.default.addObserver(
            forName: NSColor.systemColorsDidChangeNotification, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.pushSystemColors() }
            })
        appearanceObservation = win.observe(\.effectiveAppearance) { [weak self] _, _ in
            Task { @MainActor in self?.pushSystemColors() }
        }
    }

    // MARK: quit

    /// The page's Quit DSXP… button: a native confirmation sheet. `done` answers the
    /// page (true = quitting), then the app terminates.
    func confirmQuit(_ done: @escaping @MainActor (Bool) -> Void) {
        guard let win = window else { return done(false) }
        let alert = NSAlert()
        alert.messageText = "Quit DSXP?"
        alert.informativeText = "X-Plane gets its controls back and the controller goes quiet."
        alert.addButton(withTitle: "Quit")
        alert.addButton(withTitle: "Cancel")
        Task { @MainActor in
            let quit = await alert.beginSheetModal(for: win) == .alertFirstButtonReturn
            done(quit)
            if quit {
                // let the page show its "DSXP has quit" note before the window goes
                try? await Task.sleep(nanoseconds: 150_000_000)
                NSApp.terminate(nil)
            }
        }
    }

    func windowWillClose(_ notification: Notification) {
        // the motor test buttons keep running until Stop; closing the window stops them
        _ = CoreBridge.shared.api(method: "POST", path: "/api/rumble", query: "left=0&right=0")
        // so does a motion calibration: with the window gone nobody can see it, and left
        // running it keeps motion's axes on the sticks, mutes DSXP's rumble and could save
        // a calibration taken from ordinary flying (the core restores the previous one)
        _ = CoreBridge.shared.api(method: "POST", path: "/api/calibrate", query: "action=abort")
        colorObservers.forEach(NotificationCenter.default.removeObserver)
        colorObservers.removeAll()
        appearanceObservation = nil
        webView?.stopLoading()
        window?.contentView = nil
        webView = nil
        window = nil
        NSApp.setActivationPolicy(.accessory)
    }

    // MARK: WKUIDelegate - JavaScript dialogs become native sheets

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

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        pushSystemColors()
    }

    func webView(_ webView: WKWebView, decidePolicyFor navigationAction: WKNavigationAction) async
        -> WKNavigationActionPolicy {
        navigationAction.request.url?.scheme == "dsxp" ? .allow : .cancel
    }
}
