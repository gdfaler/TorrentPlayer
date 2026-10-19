// main.swift: app entry, menus, URL/file opening, command-line options for acceptance runs.
import AppKit

setvbuf(stdout, nil, _IONBF, 0)

func parseOptions() -> AppOptions {
    var o = AppOptions()
    var a = Array(CommandLine.arguments.dropFirst())
    func next() -> String? { a.isEmpty ? nil : a.removeFirst() }
    while let x = next() {
        switch x {
        case "--open": o.openTarget = next()
        case "--autoplay": o.autoplay = true
        case "--file-index": o.fileIndex = Int(next() ?? "")
        case "--peer": if let p = next() { o.extraPeers.append(p) }
        case "--cache-dir": o.cacheRoot = next()
        case "--limit-mib": o.limitMiB = Int64(next() ?? "")
        case "--exit-after": o.exitAfter = Double(next() ?? "")
        case "--open-script": if let v = next(), let c = v.firstIndex(of: ":"), let t = Double(v[v.startIndex..<c]) { o.openScript.append((t, String(v[v.index(after: c)...]))) }
        case "--seek-script": for part in (next() ?? "").split(separator: ",") { let kv = part.split(separator: ":"); if kv.count == 2, let t = Double(kv[0]), let p = Double(kv[1]) { o.seekScript.append((t, p)) } }
        case "--loopback": o.loopback = true
        case "--listen": o.listen = next()
        case "--web-seeds": o.webSeedMode = ["off": 0, "assist": 1, "always": 2][next() ?? "assist"] ?? 1
        case "--mute": o.mute = true
        case "--ao": o.audioOutput = next()
        case "--behind-seconds": o.behindSeconds = Double(next() ?? "")
        case "--ahead-seconds": o.aheadSeconds = Double(next() ?? "")
        case "--start-timeout": o.startTimeout = Double(next() ?? "") ?? 300; o.startTimeoutGiven = true
        default: break                                  // ignore macOS-injected flags like -NSDocumentRevisionsDebugMode
        }
    }
    return o
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    let options = parseOptions()
    var controller: AppController!
    var openQueue = OpenQueue()                      // open requests that arrive before the controller exists (Finder "Open With" on a cold launch)

    func applicationDidFinishLaunching(_ n: Notification) {
        buildMenu()
        controller = AppController(options: options)
        controller.window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        NSAppleEventManager.shared().setEventHandler(self, andSelector: #selector(handleURLEvent(_:reply:)), forEventClass: AEEventClass(kInternetEventClass), andEventID: AEEventID(kAEGetURL))
        let early = openQueue.markReady()
        if !early.isEmpty { DispatchQueue.main.async { self.controller.open(requests: early) } }
        else if let t = options.openTarget { DispatchQueue.main.async { self.controller.open(source: t) } }
    }
    @objc func handleURLEvent(_ e: NSAppleEventDescriptor, reply: NSAppleEventDescriptor) {
        if let s = e.paramDescriptor(forKeyword: keyDirectObject)?.stringValue { controller.open(source: s) }
    }
    func application(_ app: NSApplication, open urls: [URL]) {
        // in the order macOS provides; only the first openable item is used (no playlist)
        let requests = urls.compactMap { u -> String? in u.isFileURL ? u.path : (u.scheme == "magnet" ? u.absoluteString : nil) }
        guard !requests.isEmpty, let now = openQueue.submit(requests) else { return }                // held until the app is ready
        controller.open(requests: now)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { false }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if let s = controller?.session, !s.isEnding { s.end(reason: "app quit"); DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { NSApp.reply(toApplicationShouldTerminate: true) }; return .terminateLater }
        return .terminateNow
    }
    func applicationShouldHandleReopen(_ app: NSApplication, hasVisibleWindows flag: Bool) -> Bool { if !flag { controller.window.makeKeyAndOrderFront(nil) }; return true }

    @objc func openDocument(_ s: Any?) { controller.chooseFile() }
    @objc func openMagnet(_ s: Any?) { controller.window.makeKeyAndOrderFront(nil); controller.pasteMagnet() }
    @objc func openRevealLogs(_ s: Any?) { NSWorkspace.shared.activateFileViewerSelecting([controller.telemetry.url]) }

    func buildMenu() {
        let main = NSMenu()
        let appItem = NSMenuItem(); main.addItem(appItem)
        let app = NSMenu(); appItem.submenu = app
        app.addItem(withTitle: "About TorrentPlayer", action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)), keyEquivalent: "")
        app.addItem(.separator())
        app.addItem(withTitle: "Quit TorrentPlayer", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        let fileItem = NSMenuItem(); main.addItem(fileItem)
        let file = NSMenu(title: "File"); fileItem.submenu = file
        file.addItem(withTitle: "Open…", action: #selector(openDocument(_:)), keyEquivalent: "o")
        let m = file.addItem(withTitle: "Open Magnet Link from Clipboard", action: #selector(openMagnet(_:)), keyEquivalent: "l")
        m.keyEquivalentModifierMask = [.command]
        file.addItem(.separator())
        file.addItem(withTitle: "Reveal Telemetry Log in Finder", action: #selector(openRevealLogs(_:)), keyEquivalent: "")
        file.addItem(withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        let viewItem = NSMenuItem(); main.addItem(viewItem)
        let view = NSMenu(title: "View"); viewItem.submenu = view
        view.addItem(withTitle: "Enter Full Screen", action: #selector(NSWindow.toggleFullScreen(_:)), keyEquivalent: "f").keyEquivalentModifierMask = [.command, .control]
        NSApp.mainMenu = main
    }
}

let delegate = AppDelegate()
NSApplication.shared.delegate = delegate
NSApplication.shared.setActivationPolicy(.regular)
NSApplication.shared.run()
