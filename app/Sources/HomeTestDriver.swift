// HomeTestDriver: TEST-ONLY (compiled only into testing builds), inert unless TP_HOME_SCRIPT is set. Drives the home screen without a person: synthetic launcher states, real open
// requests that fail offline, drops through a private pasteboard, appearance switching, captures of the home window and a timeline of the fade.
// Script: "seconds:command=arg;..." (seconds after launch). Log: TP_HOME_LOG (JSON lines). Captures: TP_HOME_DIR.
// Commands: show=idle|fetching|nopeers|choosing|choosingnovideo|errorfile|errortorrent|errormagnet | open=<path> | magnet=<text> | appearance=system|light|dark | size=WxH
//           seedrecent=<path> | recents | openrecent=<i> | removerecent=<i> | clearall (presses Clear All: shows the confirmation) | alert | alertclick=<0 Clear|1 Cancel> | drop=<path>|dropmagnet=<link>|dropjunk | dragon|dragoff | action=<cancel|play|tryagain|editlink|openfile|startover> | sample=<ms> | end (ends the playback session) | closered | cmdw | performclose | winfo | capture=<name> | state | quit
import AppKit

#if TP_TESTING

final class HomeTestDriver {
    private let c: AppController
    private let logHandle: FileHandle?
    private let dir: String
    private let t0 = Date()
    private static var keep: HomeTestDriver?

    static func startIfRequested(_ c: AppController) {
        guard let script = ProcessInfo.processInfo.environment["TP_HOME_SCRIPT"] else { return }
        let d = HomeTestDriver(c); keep = d
        for step in script.split(separator: ";") {
            let parts = step.split(separator: ":", maxSplits: 1).map(String.init)
            guard parts.count == 2, let t = Double(parts[0]) else { continue }
            let kv = parts[1].split(separator: "=", maxSplits: 1).map(String.init)
            DispatchQueue.main.asyncAfter(deadline: .now() + t) { d.run(kv[0], kv.count > 1 ? kv[1] : "") }
        }
    }
    private init(_ c: AppController) {
        self.c = c
        dir = ProcessInfo.processInfo.environment["TP_HOME_DIR"] ?? NSTemporaryDirectory()
        if let p = ProcessInfo.processInfo.environment["TP_HOME_LOG"] { if !FileManager.default.fileExists(atPath: p) { FileManager.default.createFile(atPath: p, contents: nil) }; logHandle = FileHandle(forWritingAtPath: p); _ = try? logHandle?.seekToEnd() } else { logHandle = nil }
    }
    private func log(_ kind: String, _ f: [String: Any]) {
        var m = f; m["k"] = kind; m["t"] = (Date().timeIntervalSince(t0) * 1000).rounded() / 1000
        func safe(_ v: Any) -> Any { switch v { case let d as Double: return d.isFinite ? d : "nonfinite"; case let a as [Any]: return a.map(safe); case let x as [String: Any]: return x.mapValues(safe); case is Int, is Bool, is String: return v; default: return "\(v)" } }
        if let d = try? JSONSerialization.data(withJSONObject: safe(m), options: [.sortedKeys]) { logHandle?.write(d); logHandle?.write("\n".data(using: .utf8)!) }
    }

    private func fakeFiles() -> [TPMeta.FileEntry] {
        [.init(index: 0, path: "Sintel/Sintel.2010.1080p.mkv", size: 1_240_000_000, isVideo: true), .init(index: 1, path: "Sintel/Sintel.2010.720p.mkv", size: 620_000_000, isVideo: true),
         .init(index: 2, path: "Sintel/subs/english.srt", size: 120_000, isVideo: false), .init(index: 3, path: "Sintel/poster.jpg", size: 480_000, isVideo: false)]
    }
    private func showSynthetic(_ name: String) {
        let s: HomeState
        switch name {
        case "idle": s = .idle
        case "fetching": s = .fetching(fromMagnet: true, peers: 3, elapsed: 7)
        case "nopeers": s = .fetching(fromMagnet: true, peers: 0, elapsed: 21)
        case "choosing", "choosingnovideo":
            c.files = name == "choosing" ? fakeFiles() : Array(fakeFiles().suffix(2))
            s = .choosing(name: "Sintel (2010)", hasVideo: name == "choosing")
        case "errorfile": s = .failed(kind: .file, title: "Could not open this file", message: "The file does not exist.", retryable: false)
        case "errortorrent": s = .failed(kind: .torrent, title: "Could not open this torrent", message: "Not a valid torrent file (bencode parse error).", retryable: true)
        case "errormagnet": s = .failed(kind: .magnetSyntax, title: HomePresentation.magnetSyntaxTitle, message: HomePresentation.magnetSyntaxMessage, retryable: false)
        default: return
        }
        c.home.apply(HomePresentation.make(s))
        if name == "choosing" { c.home.table.reloadData(); c.home.table.selectRowIndexes(IndexSet(integer: 0), byExtendingSelection: false); c.home.setPlayEnabled(true) }
    }

    private func state() -> [String: Any] {
        let h = c.home, w = c.window
        var buttons: [String] = []
        func walk(_ v: NSView) { if let b = v as? NSButton, !b.isHidden, b.window != nil { buttons.append(b.title.isEmpty ? (b.accessibilityLabel() ?? "?") : b.title) }; v.subviews.forEach(walk) }
        walk(h)
        return ["mode": "\(h.presentation.mode)", "title": h.presentation.title, "detail": h.presentation.detail, "actions": h.presentation.actions.map { $0.title }, "buttons": buttons,
                "appearance": h.appearancePreference.rawValue, "effective": h.effectiveAppearance.name.rawValue, "reduceTransparency": h.environment.reduceTransparency, "reduceMotion": h.environment.reduceMotion,
                "increaseContrast": h.environment.increaseContrast, "frame": NSStringFromRect(w.frame), "alpha": w.alphaValue, "level": w.level.rawValue, "visible": w.isVisible, "key": w.isKeyWindow,
                "phase": "\(c.phase)", "dragHighlight": h.isDragHighlighted, "closeButtonVisible": !(w.standardWindowButton(.closeButton)?.isHidden ?? true), "firstResponder": "\(String(describing: type(of: w.firstResponder as Any)))", "hasGlassCard": !h.card.subviews.isEmpty, "layout": h.layoutFrames, "magnetField": h.magnetField.stringValue, "rows": c.files.count, "selectedRow": h.table.selectedRow,
                "bg": NSStringFromRect(NSRect(x: 0, y: 0, width: 0, height: 0)), "windowBackground": "\(w.backgroundColor?.usingColorSpace(.sRGB) ?? .clear)"]
    }

    func run(_ cmd: String, _ arg: String) {
        switch cmd {
        case "show": showSynthetic(arg); log("show", ["name": arg])
        case "open": c.open(requests: [arg]); log("open", ["arg": arg])
        case "magnet": c.openMagnet(arg); log("magnet", ["arg": arg])
        case "appearance": if let a = HomeAppearance(rawValue: arg) { c.home.setAppearance(a) }; log("appearance", ["set": arg])
        case "size": let p = arg.split(separator: "x").compactMap { Double($0) }; if p.count == 2 { c.window.setContentSize(NSSize(width: p[0], height: p[1])) }; log("size", ["arg": arg])
        case "drop", "dropmagnet", "dropjunk":
            let pb = NSPasteboard(name: NSPasteboard.Name("io.github.gdfaler.TorrentPlayer.test.\(UUID().uuidString)")); pb.clearContents()
            if cmd == "drop" { pb.writeObjects([URL(fileURLWithPath: arg) as NSURL]) } else if cmd == "dropmagnet" { pb.setString(arg, forType: .string) } else { pb.setString("hello", forType: .string) }
            let accepted = c.home.accepts(pb), handled = c.home.handleDrop(pb)
            log(cmd, ["accepts": accepted, "handled": handled])
        case "dragon": c.home.card.setDragging(true); log(cmd, ["highlight": c.home.isDragHighlighted])
        case "dragoff": c.home.card.setDragging(false); log(cmd, ["highlight": c.home.isDragHighlighted])
        case "action": if let a = [HomeAction.cancel, .play, .tryAgain, .editLink, .openFile, .startOver].first(where: { "\($0)".lowercased() == arg }) { c.perform(a) }; log("action", ["arg": arg])
        case "sample":                                                  // window alpha / level / visibility every 25 ms for <ms>
            let n = (Int(arg) ?? 800) / 25; var rows: [[String: Any]] = []
            for i in 0...n { DispatchQueue.main.asyncAfter(deadline: .now() + Double(i) * 0.025) { [self] in
                rows.append(["ms": i * 25, "alpha": (c.window.alphaValue * 100).rounded() / 100, "visible": c.window.isVisible, "level": c.window.level.rawValue])
                if i == n { log("sample", ["rows": rows]) } } }
        case "end": c.session?.end(reason: "test"); log("end", ["hadSession": c.session != nil])
        case "clickbutton":                                             // performClick on a home button (file | torrent | paste | link | an action name)
            if let b = c.home.testButton(arg) { b.performClick(nil) }; log("clickbutton", ["arg": arg, "found": c.home.testButton(arg) != nil])
        case "menuopen": if arg == "torrent" { c.chooseTorrent() } else { c.chooseFile() }; log("menuopen", ["arg": arg])
        case "sheet":                                                   // the open panel sheet, if any: its message and allowed types
            if let p = c.window.attachedSheet as? NSOpenPanel { log("sheet", ["message": p.message ?? "", "types": p.allowedContentTypes.map { $0.identifier }.sorted(), "multiple": p.allowsMultipleSelection, "dirs": p.canChooseDirectories]) } else { log("sheet", ["none": true]) }
        case "closesheet": if let s = c.window.attachedSheet { c.window.endSheet(s, returnCode: .cancel); s.orderOut(nil) }; log("closesheet", [:])
        case "recents":                                                 // the Recent section as the user sees it
            log("recents", ["visible": c.home.recentsVisible, "count": c.recents.entries.count, "rows": c.home.recentRows.map { ["title": $0.title, "subtitle": $0.subtitle, "available": $0.available] }])
        case "seedrecent": log("seedrecent", ["recorded": c.recents.record(path: arg)]); c.refreshRecents()          // test seed (a real run records only after playback started)
        case "openrecent": if let i = Int(arg) { c.home.testClickRecent(i) }; log("openrecent", ["index": arg])
        case "removerecent": if let i = Int(arg) { c.home.testRemoveRecent(i) }; log("removerecent", ["index": arg, "count": c.recents.entries.count])
        case "clearall": c.home.testPressClearAll(); log("clearall", ["count": c.recents.entries.count])
        case "alert":
            if let a = c.window.attachedSheet, let t = a.value(forKey: "title") as? String { _ = t }
            let buttons = c.window.attachedSheet?.contentView.map { Self.buttons(in: $0).map { $0.title } } ?? []
            log("alert", ["sheet": c.window.attachedSheet != nil, "buttons": buttons, "count": c.recents.entries.count])
        case "alertclick":                                              // 0 = default (Clear All), 1 = Cancel
            if let sheet = c.window.attachedSheet, let cv = sheet.contentView { let bs = Self.buttons(in: cv).sorted { $0.frame.minX > $1.frame.minX }; if let i = Int(arg), i < bs.count { bs[i].performClick(nil) } }
            log("alertclick", ["arg": arg])
        case "closered", "cmdw", "performclose":                        // close the PLAYBACK window the way a user does: red button click, Cmd-W through the main menu, or performClose
            guard let w = NSApp.windows.first(where: { $0.title == "TorrentPlayer" && $0.isVisible && $0.contentView != nil }) else { log(cmd, ["error": "no playback window"]); return }
            log(cmd, ["fullscreen": w.styleMask.contains(.fullScreen), "windows": NSApp.windows.filter { $0.isVisible }.count])
            if cmd == "closered" { w.standardWindowButton(.closeButton)?.performClick(nil) }
            else if cmd == "performclose" { w.performClose(nil) }
            else if let e = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: .command, timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: w.windowNumber, context: nil, characters: "w", charactersIgnoringModifiers: "w", isARepeat: false, keyCode: 13) { NSApp.sendEvent(e) }
        case "winfo":                                                   // visible windows and whether a session exists (duplicates / stale windows)
            log("winfo", ["visible": NSApp.windows.filter { $0.isVisible }.map { "\($0.title)|\(type(of: $0))" }.sorted(), "session": c.session != nil, "phase": "\(c.phase)", "homeVisible": c.window.isVisible, "homeAlpha": c.window.alphaValue, "key": NSApp.keyWindow?.title ?? "-", "recents": c.recents.entries.count])
        case "capture": capture(arg)
        case "state": log("state", state())
        case "quit": c.telemetry.close(); NSApp.terminate(nil)
        default: log("unknown", ["cmd": cmd])
        }
    }

    private static func buttons(in v: NSView) -> [NSButton] { v.subviews.flatMap { ($0 as? NSButton).map { [$0] } ?? [] + buttons(in: $0) } }
    private typealias CapFn = @convention(c) (CGRect, UInt32, UInt32, UInt32) -> Unmanaged<CGImage>?
    private func capture(_ name: String) {                                  // window-server composite of OUR window (CGWindowListCreateImage by window id); evidence, not approval
        guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "CGWindowListCreateImage") else { return }
        let fn = unsafeBitCast(sym, to: CapFn.self)
        let primaryH = NSScreen.screens.first?.frame.height ?? 0, f = c.window.frame
        guard let raw = fn(CGRect(x: f.minX, y: primaryH - f.maxY, width: f.width, height: f.height), 8 /* kCGWindowListOptionIncludingWindow */, UInt32(c.window.windowNumber), 1 /* bounds ignore framing */)?.takeRetainedValue(),
              let ctx = CGContext(data: nil, width: raw.width, height: raw.height, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { log("capture", ["name": name, "error": "no image"]); return }
        ctx.draw(raw, in: CGRect(x: 0, y: 0, width: raw.width, height: raw.height))                // lazily backed: copy at once
        if let img = ctx.makeImage(), let d = NSBitmapImageRep(cgImage: img).representation(using: .png, properties: [:]) { try? d.write(to: URL(fileURLWithPath: dir + "/" + name + ".png")) }
        log("capture", ["name": name, "w": raw.width, "h": raw.height])
    }
}

#endif
