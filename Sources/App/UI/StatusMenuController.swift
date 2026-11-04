import AppKit
import DSXPCore

/// The menu-bar item: what the bridge is doing, and the few things you can do.
@MainActor
final class StatusMenuController: NSObject, NSMenuDelegate {
    var openMapper: (() -> Void)?
    var fixAccess: (() -> Void)?

    private let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    private let menu = NSMenu()
    private let controllerLine = NSMenuItem()
    private let xplaneLine = NSMenuItem()
    private let bridgeLine = NSMenuItem()
    private let accessLine = NSMenuItem()
    private let loginItem = NSMenuItem()

    override init() {
        super.init()
        menu.delegate = self
        menu.autoenablesItems = false
        for line in [controllerLine, xplaneLine, bridgeLine] {
            line.isEnabled = false
            menu.addItem(line)
        }
        accessLine.title = "Input access problem… Fix"
        accessLine.target = self
        accessLine.action = #selector(fixAccessAction)
        accessLine.isHidden = true
        menu.addItem(accessLine)
        menu.addItem(.separator())
        add("Open Mapper…", #selector(openMapperAction), "m")
        loginItem.title = "Launch at Login"
        loginItem.target = self
        loginItem.action = #selector(toggleLogin)
        menu.addItem(loginItem)
        add("Open Logs Folder", #selector(openLogs), "")
        menu.addItem(.separator())
        add("Quit DSXP", #selector(quitAction), "q")
        item.menu = menu
    }

    private func add(_ title: String, _ action: Selector, _ key: String) {
        let mi = NSMenuItem(title: title, action: action, keyEquivalent: key)
        mi.target = self
        menu.addItem(mi)
    }

    func update(_ s: dsxp_status_t, accessProblem: Bool) {
        let link = s.controllerLink
        let symbol = s.engaged ? "gamecontroller.fill" : "gamecontroller"
        let image = NSImage(systemSymbolName: symbol, accessibilityDescription: "DSXP")
        image?.isTemplate = true
        item.button?.image = image
        item.button?.appearsDisabled = link == .none
        item.button?.toolTip = "DSXP - " + (s.engaged ? "driving X-Plane" : "idle")

        switch link {
        case .bluetooth:
            controllerLine.title = s.input_live
                ? String(format: "Controller: Bluetooth · %.0f Hz", s.report_hz)
                : "Controller: Bluetooth (no input yet)"
        case .usb: controllerLine.title = "Controller: USB (X-Plane reads it directly)"
        case .both: controllerLine.title = "Controller: USB + Bluetooth (USB wins)"
        case .none: controllerLine.title = "Controller: not connected"
        }
        if !s.xp_running {
            xplaneLine.title = "X-Plane 12: not running"
        } else if s.xp_answering {
            xplaneLine.title = "X-Plane 12: answering"
        } else {
            xplaneLine.title = "X-Plane 12: running, not answering - enable Settings › Network"
        }
        bridgeLine.title = s.suspended ? "Bridge: paused (sleep)" : s.engaged ? "Bridge: engaged" : "Bridge: idle"
        accessLine.isHidden = !accessProblem
    }

    func menuNeedsUpdate(_ menu: NSMenu) {
        CoreBridge.shared.refresh()
        loginItem.state = LoginItem.isEnabled ? .on : .off
        loginItem.isEnabled = LoginItem.canToggle
        loginItem.title = LoginItem.canToggle ? "Launch at Login" : "Launch at Login (install in /Applications)"
    }

    @objc private func openMapperAction() { openMapper?() }
    @objc private func fixAccessAction() { fixAccess?() }
    @objc private func toggleLogin() { LoginItem.toggle() }
    @objc private func quitAction() { NSApp.terminate(nil) }

    @objc private func openLogs() {
        try? FileManager.default.createDirectory(at: Paths.logDir, withIntermediateDirectories: true)
        NSWorkspace.shared.open(Paths.logDir)
    }
}
