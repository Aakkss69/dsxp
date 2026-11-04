import AppKit
import DSXPCore

// Guard mode: the app re-executes itself with --dsxp-guard to clean up if it
// ever dies without doing so (see Sources/Core/platform/guard.c). Nothing else runs.
let arguments = CommandLine.arguments
if let i = arguments.firstIndex(of: "--dsxp-guard"), i + 1 < arguments.count,
   let fd = Int32(arguments[i + 1]) {
    exit(dsxp_guard_main(fd))
}

// One bridge at a time. A second launch asks the running one to show its mapper.
guard SingleInstance.acquire() else {
    SingleInstance.signalRunningInstance()
    exit(0)
}

let delegate = AppDelegate()
let app = NSApplication.shared
app.delegate = delegate
app.run()
