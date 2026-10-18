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

    func applicationDidFinishLaunching(_ n: Notification) {
        buildMenu()
        controller = AppController(options: options)
        controller.window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        NSAppleEventManager.shared().setEventHandler(self, andSelector: #selector(handleURLEvent(_:reply:)), forEventClass: AEEventClass(kInternetEventClass), andEventID: AEEventID(kAEGetURL))
        if let t = options.openTarget { DispatchQueue.main.async { self.controller.open(source: t) } }
    }
    @objc func handleURLEvent(_ e: NSAppleEventDescriptor, reply: NSAppleEventDescriptor) {
        if let s = e.paramDescriptor(forKeyword: keyDirectObject)?.stringValue { controller.open(source: s) }
    }
    func application(_ app: NSApplication, open urls: [URL]) {
        for u in urls { if u.isFileURL { controller.open(source: u.path) } else if u.scheme == "magnet" { controller.open(source: u.absoluteString) } }
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { false }
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if let s = controller?.session, !s.isEnding { s.end(reason: "app quit"); DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { NSApp.reply(toApplicationShouldTerminate: true) }; return .terminateLater }
        return .terminateNow
    }
    func applicationShouldHandleReopen(_ app: NSApplication, hasVisibleWindows flag: Bool) -> Bool { if !flag { controller.window.makeKeyAndOrderFront(nil) }; return true }

    @objc func openTorrent(_ s: Any?) { controller.chooseTorrent() }
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
        file.addItem(withTitle: "Open Torrent…", action: #selector(openTorrent(_:)), keyEquivalent: "o")
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
