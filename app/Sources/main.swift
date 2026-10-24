// main.swift: app entry, menus, URL/file opening, command-line options for acceptance runs.
import AppKit

setvbuf(stdout, nil, _IONBF, 0)

// mpv draws through Vulkan (MoltenVK). The Vulkan loader finds the driver through an ICD file: use the one bundled in Resources, so the app does not depend on Homebrew's directories.
// Builds without the bundled file (development builds) keep the loader's default search.
if let icd = Bundle.main.url(forResource: "MoltenVK_icd", withExtension: "json", subdirectory: "vulkan/icd.d") {
    setenv("VK_DRIVER_FILES", icd.path, 1); setenv("VK_ICD_FILENAMES", icd.path, 1)
}

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

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuItemValidation {
    let options = parseOptions()
    var controller: AppController!
    var openQueue = OpenQueue()                      // open requests that arrive before the controller exists (Finder "Open With" on a cold launch)

    func applicationDidFinishLaunching(_ n: Notification) {
        buildMenu()
        controller = AppController(options: options)
        controller.window.makeKeyAndOrderFront(nil)
#if TP_TESTING
        HomeTestDriver.startIfRequested(controller)
#endif
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

    // Playback menu: every toolbar action is also reachable from the menu bar (keyboard and VoiceOver), without focusing the non-activating controls panel. No key equivalents: the
    // player's own keys (Space, arrows, M, F, E) stay as they are.
    @objc func playbackPlayPause(_ s: Any?) { controller.session?.togglePause() }
    @objc func playbackBack10(_ s: Any?) { controller.session?.seekRelative(-10) }
    @objc func playbackForward10(_ s: Any?) { controller.session?.seekRelative(10) }
    @objc func playbackMute(_ s: Any?) {
#if TP_TESTING
        LatencyProbe.trigger = "menu"
#endif
        controller.session?.toggleMute()
    }
    @objc func playbackControlsSize(_ s: Any?) { controller.session?.toggleControlsSize() }
    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        switch item.action {
        case #selector(playbackPlayPause(_:)), #selector(playbackBack10(_:)), #selector(playbackForward10(_:)), #selector(playbackMute(_:)):
            guard let s = controller.session else { return false }
            return !s.isEnding && s.playbackStarted
        case #selector(playbackControlsSize(_:)):
            guard let s = controller.session else { return false }
            return !s.isEnding && s.canToggleControlsSize
        case #selector(setAppearance(_:)):
            item.state = (item.representedObject as? String) == controller.home.appearancePreference.rawValue ? .on : .off
            return true
        default: return true
        }
    }

    @objc func openDocument(_ s: Any?) { controller.chooseFile() }
    @objc func openTorrentDocument(_ s: Any?) { controller.chooseTorrent() }
    @objc func setAppearance(_ s: NSMenuItem) { if let a = HomeAppearance(rawValue: s.representedObject as? String ?? "") { controller.home.setAppearance(a) } }
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
        file.addItem(withTitle: "Open Torrent…", action: #selector(openTorrentDocument(_:)), keyEquivalent: "o").keyEquivalentModifierMask = [.command, .shift]
        let m = file.addItem(withTitle: "Open Magnet Link from Clipboard", action: #selector(openMagnet(_:)), keyEquivalent: "l")
        m.keyEquivalentModifierMask = [.command]
        file.addItem(.separator())
        file.addItem(withTitle: "Reveal Telemetry Log in Finder", action: #selector(openRevealLogs(_:)), keyEquivalent: "")
        file.addItem(withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        let viewItem = NSMenuItem(); main.addItem(viewItem)
        let view = NSMenu(title: "View"); viewItem.submenu = view
        view.addItem(withTitle: "Enter Full Screen", action: #selector(NSWindow.toggleFullScreen(_:)), keyEquivalent: "f").keyEquivalentModifierMask = [.command, .control]
        view.addItem(.separator())
        let appearanceItem = view.addItem(withTitle: "Home Appearance", action: nil, keyEquivalent: ""); let appearanceMenu = NSMenu(title: "Home Appearance"); appearanceItem.submenu = appearanceMenu
        for a in HomeAppearance.allCases { let i = appearanceMenu.addItem(withTitle: a.title, action: #selector(setAppearance(_:)), keyEquivalent: ""); i.representedObject = a.rawValue; i.target = self }
        let playbackItem = NSMenuItem(); main.addItem(playbackItem)
        let playback = NSMenu(title: "Playback"); playbackItem.submenu = playback
        for (title, sel) in [("Play / Pause", #selector(playbackPlayPause(_:))), ("Back 10 Seconds", #selector(playbackBack10(_:))), ("Forward 10 Seconds", #selector(playbackForward10(_:))),
                             ("Mute / Unmute", #selector(playbackMute(_:))), ("Show More / Fewer Controls", #selector(playbackControlsSize(_:)))] {
            playback.addItem(withTitle: title, action: sel, keyEquivalent: "").target = self
        }
        NSApp.mainMenu = main
    }
}

let delegate = AppDelegate()
NSApplication.shared.delegate = delegate
NSApplication.shared.setActivationPolicy(.regular)
NSApplication.shared.run()
