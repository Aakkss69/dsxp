import AppKit
import DSXPCore

// Guard mode: the app re-executes itself with --dsxp-guard to clean up if it
// ever dies without doing so (see Sources/Core/platform/guard.c). Nothing else runs.
let arguments = CommandLine.arguments
// Arguments: --dsxp-guard <pipe fd> <socket fd or -1> <X-Plane port>
if let i = arguments.firstIndex(of: "--dsxp-guard"), i + 3 < arguments.count,
   let pipeFD = Int32(arguments[i + 1]), let sockFD = Int32(arguments[i + 2]),
   let port = Int32(arguments[i + 3]) {
    exit(dsxp_guard_main(pipeFD, sockFD, port))
}

// One bridge at a time. A second launch asks the running app to show its mapper. When the
// lock is held but no other copy of the app runs, it is an instance still quitting (wait a
// moment) or dsxp-cli (say so rather than vanishing).
if !SingleInstance.acquire() {
    if SingleInstance.otherAppIsRunning() {
        SingleInstance.signalRunningInstance()
        exit(0)
    }
    if !SingleInstance.acquire(waitingUpTo: 2.0) {
        _ = NSApplication.shared
        SingleInstance.reportBusy()
        exit(1)
    }
}

let delegate = AppDelegate()
let app = NSApplication.shared
app.delegate = delegate
app.run()
