import AppKit
import DSXPCore

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var statusMenu: StatusMenuController?
    private let mapper = MapperWindowController()
    private var guardFD: Int32 = -1
    private var signalSources: [DispatchSourceSignal] = []
    private var observers: [NSObjectProtocol] = []
    private var refreshTimer: Timer?
    private var activity: NSObjectProtocol?
    private var activityEngaged = false
    private var bluetoothSilentSince: Date?
    private var triedExecutableFolder = ""
    private var pendingPick: (@MainActor (Bool) -> Void)?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        buildMainMenu()
        ProcessInfo.processInfo.disableAutomaticTermination("DSXP bridges a controller to X-Plane")
        ProcessInfo.processInfo.disableSuddenTermination()
        signal(SIGPIPE, SIG_IGN)
        installSignalHandlers()

        // the crash guard: cleans up after us if we ever die without doing so
        guardFD = dsxp_guard_spawn(Bundle.main.executablePath ?? CommandLine.arguments[0])

        let bridge = CoreBridge.shared
        bridge.statusChanged = { [weak self] in self?.statusChanged() }
        bridge.commandsLoaded = { [weak self] in self?.commandsLoaded() }

        let defaults = UserDefaults.standard
        let importLegacy = !defaults.bool(forKey: "ImportedLegacyBindings")
        let folder = XPlaneLocator.locate()
        let rc = bridge.start(commandsPath: folder.map { XPlaneLocator.commandsFile(in: $0).path },
                              importLegacy: importLegacy)
        if rc == 0 && importLegacy { defaults.set(true, forKey: "ImportedLegacyBindings") }

        let menu = StatusMenuController()
        menu.openMapper = { [weak self] in self?.mapper.show() }
        menu.fixAccess = { [weak self] in self?.openInputAccess() }
        statusMenu = menu

        mapper.scheme.onQuit = { NSApp.terminate(nil) }
        mapper.scheme.onPickXPlane = { [weak self] done in self?.pickXPlane(done) }
        mapper.scheme.onOpenAccess = { [weak self] in self?.openInputAccess() }

        observeSystem()
        refreshTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { _ in
            MainActor.assumeIsolated { CoreBridge.shared.refresh() }
        }
        statusChanged()

        if !defaults.bool(forKey: "HasLaunchedBefore") {
            defaults.set(true, forKey: "HasLaunchedBefore")
            mapper.show()
        }
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        mapper.show()
        return true
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { false }

    func applicationWillTerminate(_ notification: Notification) {
        refreshTimer?.invalidate()
        CoreBridge.shared.stop()            // releases X-Plane, quiets the controller, saves
        dsxp_guard_release(guardFD)         // clean exit: the guard leaves quietly
        guardFD = -1
    }

    // MARK: status

    private func statusChanged() {
        let s = CoreBridge.shared.status
        statusMenu?.update(s, accessProblem: accessProblem(s))
        updateActivity(s)

        // no command list yet, but X-Plane is running: learn its folder from the process
        if s.ncmds == 0, s.xp_running {
            let exe = s.xpExecutablePath
            if exe != triedExecutableFolder, let folder = XPlaneLocator.folder(fromExecutable: exe) {
                triedExecutableFolder = exe
                XPlaneLocator.save(folder)
                CoreBridge.shared.setCommandsPath(XPlaneLocator.commandsFile(in: folder).path)
            }
        }
    }

    private func commandsLoaded() {
        pendingPick?(CoreBridge.shared.status.ncmds > 0)
        pendingPick = nil
    }

    /// Input Monitoring trouble: the HID manager would not open, or a Bluetooth
    /// controller is connected but no reports arrive for 3 s while access is not
    /// granted. Access is never requested up front.
    private func accessProblem(_ s: dsxp_status_t) -> Bool {
        if s.hid_open_result != 0 { return true }
        guard s.controllerLink.hasBluetooth, !s.input_live else {
            bluetoothSilentSince = nil
            return false
        }
        let since = bluetoothSilentSince ?? Date()
        bluetoothSilentSince = since
        return Date().timeIntervalSince(since) > 3 &&
            CoreBridge.shared.inputAccess(request: false) != DSXP_ACCESS_GRANTED
    }

    /// Keep the 60 Hz loop from being throttled by App Nap while it matters.
    private func updateActivity(_ s: dsxp_status_t) {
        let want = s.controllerLink.hasBluetooth && s.input_live
        if want {
            if activity == nil || activityEngaged != s.engaged {
                if let a = activity { ProcessInfo.processInfo.endActivity(a) }
                var options: ProcessInfo.ActivityOptions = [.userInitiatedAllowingIdleSystemSleep]
                if s.engaged { options.insert(.latencyCritical) }
                activity = ProcessInfo.processInfo.beginActivity(
                    options: options, reason: "Bridging the DualSense controller to X-Plane")
                activityEngaged = s.engaged
            }
        } else if let a = activity {
            ProcessInfo.processInfo.endActivity(a)
            activity = nil
        }
    }

    // MARK: actions from the mapper

    private func pickXPlane(_ done: @escaping @MainActor (Bool) -> Void) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.prompt = "Choose"
        panel.message = "Choose your X-Plane 12 folder (the one that contains Resources)."
        Task { @MainActor in
            let response: NSApplication.ModalResponse
            if let win = mapper.window { response = await panel.beginSheetModal(for: win) }
            else { response = panel.runModal() }
            guard response == .OK, let url = panel.url else { return done(false) }
            guard XPlaneLocator.isValid(url) else {
                let alert = NSAlert()
                alert.messageText = "That folder has no Resources/plugins/Commands.txt."
                alert.informativeText = "Pick the X-Plane 12 folder itself, for example ~/X-Plane/X-Plane 12."
                if let win = mapper.window { _ = await alert.beginSheetModal(for: win) }
                return done(false)
            }
            XPlaneLocator.save(url)
            pendingPick = done
            CoreBridge.shared.setCommandsPath(XPlaneLocator.commandsFile(in: url).path)
        }
    }

    private func openInputAccess() {
        _ = CoreBridge.shared.inputAccess(request: true)
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent") {
            NSWorkspace.shared.open(url)
        }
    }

    // MARK: system

    private func installSignalHandlers() {
        for sig in [SIGTERM, SIGINT, SIGHUP] {
            signal(sig, SIG_IGN)
            let source = DispatchSource.makeSignalSource(signal: sig, queue: .main)
            source.setEventHandler {
                MainActor.assumeIsolated { NSApp.terminate(nil) }
            }
            source.resume()
            signalSources.append(source)
        }
    }

    private func observeSystem() {
        let ws = NSWorkspace.shared.notificationCenter
        observers.append(ws.addObserver(forName: NSWorkspace.willSleepNotification, object: nil,
                                        queue: .main) { _ in dsxp_set_suspended(true) })
        observers.append(ws.addObserver(forName: NSWorkspace.didWakeNotification, object: nil,
                                        queue: .main) { _ in dsxp_set_suspended(false) })
        observers.append(DistributedNotificationCenter.default().addObserver(
            forName: SingleInstance.openMapperNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.mapper.show() }
        })
    }

    private func buildMainMenu() {
        let main = NSMenu()

        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About DSXP",
                        action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide DSXP", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Quit DSXP", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        main.addItem(submenu: appMenu, title: "DSXP")

        let edit = NSMenu(title: "Edit")
        edit.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        let redo = edit.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "z")
        redo.keyEquivalentModifierMask = [.command, .shift]
        edit.addItem(.separator())
        edit.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        edit.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        edit.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        edit.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        main.addItem(submenu: edit, title: "Edit")

        let window = NSMenu(title: "Window")
        window.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        window.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        main.addItem(submenu: window, title: "Window")

        NSApp.mainMenu = main
        NSApp.windowsMenu = window
    }
}

private extension NSMenu {
    func addItem(submenu: NSMenu, title: String) {
        let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        item.submenu = submenu
        addItem(item)
    }
}
