// GlassTestDriver: TEST-ONLY, inert unless TP_GLASS_SCRIPT is set. Drives the production glass controls with synthetic mouse events and logs what happened, so the hit-testing,
// compact / expanded / transition states, resize, fullscreen, appearance and accessibility behaviour can be checked without a person (same idea as --open-script and TP_SNAPSHOT_DIR).
// Script: "seconds:command=arg;seconds:command;..." (seconds after the controls attach). Log: TP_GLASS_LOG (JSON lines). Captures: TP_GLASS_DIR.
// Commands: click=<control> | clickhidden=<control> (click where that control sits even if collapsed) | clickat=<control>,<fraction> (seek bar press/release at a fraction)
//           expand | collapse | toggle | size=WxH | fs | appearance=dark|light|system | hide | show | probe=<name> | capture=<name>
import AppKit

#if TP_TESTING          // compiled only into testing builds (TP_BUILD_TESTING=1 tools/scripts/build_release.sh <name>); release builds contain no test hooks of this driver

final class GlassTestDriver {
    private let controls: GlassControls
    private weak var window: NSWindow?
    private let logHandle: FileHandle?
    private let dir: String
    private let t0 = Date()
    private var clickDepth = 0

    static func startIfRequested(controls: GlassControls, window: NSWindow, state: @escaping () -> [String: Any]) {
        guard let script = ProcessInfo.processInfo.environment["TP_GLASS_SCRIPT"] else { return }
        let d = GlassTestDriver(controls: controls, window: window, state: state)
        d.keepAlive = d
        for step in script.split(separator: ";") {
            let parts = step.split(separator: ":", maxSplits: 1).map(String.init)
            guard parts.count == 2, let t = Double(parts[0]) else { continue }
            let kv = parts[1].split(separator: "=", maxSplits: 1).map(String.init)
            DispatchQueue.main.asyncAfter(deadline: .now() + t) { d.run(kv[0], kv.count > 1 ? kv[1] : "") }
        }
    }
    private var keepAlive: GlassTestDriver?
    private let state: () -> [String: Any]

    private init(controls: GlassControls, window: NSWindow, state: @escaping () -> [String: Any]) {
        self.controls = controls; self.window = window; self.state = state
        dir = ProcessInfo.processInfo.environment["TP_GLASS_DIR"] ?? NSTemporaryDirectory()
        if let p = ProcessInfo.processInfo.environment["TP_GLASS_LOG"] { if !FileManager.default.fileExists(atPath: p) { FileManager.default.createFile(atPath: p, contents: nil) }; logHandle = FileHandle(forWritingAtPath: p); _ = try? logHandle?.seekToEnd() } else { logHandle = nil }      // appends: one log across session switches
    }

    private func log(_ kind: String, _ f: [String: Any]) {
        var m = f; m["k"] = kind; m["t"] = (Date().timeIntervalSince(t0) * 1000).rounded() / 1000
        func safe(_ v: Any) -> Any { switch v { case let d as Double: return d.isFinite ? d : "nonfinite"; case let a as [Any]: return a.map(safe); case let x as [String: Any]: return x.mapValues(safe); case is Int, is Bool, is String: return v; default: return "\(v)" } }
        if let d = try? JSONSerialization.data(withJSONObject: safe(m), options: [.sortedKeys]) { logHandle?.write(d); logHandle?.write("\n".data(using: .utf8)!) }
    }

    private func control(_ s: String) -> GlassControl? { GlassControl(rawValue: s) }
    private func centre(of c: GlassControl) -> NSPoint? {
        guard let item = controls.view.items[c], let sv = item.superview else { return nil }
        return sv.convert(NSPoint(x: item.frame.midX, y: item.frame.midY), to: nil)
    }
    private func event(_ type: NSEvent.EventType, _ p: NSPoint) -> NSEvent {
        NSEvent.mouseEvent(with: type, location: p, modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: controls.panel.windowNumber, context: nil, eventNumber: 0, clickCount: 1, pressure: 1)!
    }
    private func hitName(at p: NSPoint) -> String {
        guard let cv = controls.panel.contentView, let v = cv.hitTest(cv.convert(p, from: nil)) else { return "nil" }
        var x: NSView? = v
        while let y = x { if let i = y.identifier?.rawValue, !i.isEmpty { return i }; x = y.superview }
        return String(describing: type(of: v))
    }
    private func counts() -> [String: Int] { Dictionary(uniqueKeysWithValues: controls.view.actionCounts.map { ($0.key.rawValue, $0.value) }) }

    private var clickBusy = false                                         // clicks are serialised: a stalled main thread must not make two clicks overlap in one measurement
    private func click(_ target: String, hidden: Bool) {
        if clickBusy { DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [self] in click(target, hidden: hidden) }; return }
        guard let c = control(target), let p = centre(of: c) else { log("click", ["target": target, "error": "no such control"]); return }
        clickBusy = true
        let before = counts(), s0 = state(), fs0 = window?.styleMask.contains(.fullScreen) ?? false
        let hit = hitName(at: p)
        let hitClass = controls.panel.contentView.flatMap { cv in cv.hitTest(cv.convert(p, from: nil)).map { String(describing: type(of: $0)) } } ?? "nil"
        // both events are POSTED (not sent): an opaque NSButton tracks the mouse in a nested loop that waits for the mouse-up from the event queue
        NSApp.postEvent(event(.leftMouseDown, p), atStart: false); NSApp.postEvent(event(.leftMouseUp, p), atStart: false)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.12) { [self] in
            clickBusy = false
            let after = counts()
            var fired: [String] = []; for (k, v) in after where v != before[k, default: 0] { fired.append(k) }
            log("click", ["target": target, "atHiddenPosition": hidden, "hit": hit, "hitClass": hitClass, "winKey": NSApp.keyWindow?.title ?? "-", "appActive": NSApp.isActive, "fired": fired.sorted(), "expanded": controls.layout?.expanded ?? false, "state_before": s0, "state_after": state(), "fullscreen_before": fs0, "frame": "\(controls.view.items[c]!.frame)"])
        }
    }

    func run(_ cmd: String, _ arg: String) {
        switch cmd {
        case "click": click(arg, hidden: false)
        case "clickhidden": click(arg, hidden: true)
        case "clickat":
            let a = arg.split(separator: ",").map(String.init)
            guard a.count == 2, let f = Double(a[1]) else { return }
            let bar = controls.view.seekBar
            let p = bar.convert(NSPoint(x: 6 + CGFloat(f) * (bar.bounds.width - 12), y: bar.bounds.midY), to: nil)
            controls.panel.sendEvent(event(.leftMouseDown, p)); controls.panel.sendEvent(event(.leftMouseUp, p))
            log("seek_click", ["fraction": f, "bar_width": Double(bar.bounds.width)])
        case "expand", "collapse", "toggle":
            let t = CFAbsoluteTimeGetCurrent()
            if cmd == "toggle" || (cmd == "expand") == (controls.layout?.expanded == false) { controls.toggleExpanded() }
            log(cmd, ["expanded": controls.expandedWanted, "main_thread_ms": ((CFAbsoluteTimeGetCurrent() - t) * 1000).rounded()])
        case "size":
            let a = arg.split(separator: "x").compactMap { Double($0) }
            if a.count == 2 { window?.setContentSize(NSSize(width: a[0], height: a[1])) }
            log("size", ["asked": arg, "content": window?.contentView.map { "\($0.bounds.size)" } ?? "-", "window": window.map { "\($0.frame)" } ?? "-"])
        case "fs": window?.toggleFullScreen(nil); log("fs", ["requested": true])
        case "appearance": NSApp.appearance = arg == "dark" ? NSAppearance(named: .darkAqua) : arg == "light" ? NSAppearance(named: .aqua) : nil; log("appearance", ["set": arg])
        case "hide": controls.setVisible(false); log("hide", [:])
        case "show": controls.setVisible(true); log("show", [:])
        case "key":                                                     // a real key event through NSApp (reaches the player's key monitor)
            let keys: [String: (UInt16, String)] = ["e": (14, "e"), "m": (46, "m"), "f": (3, "f"), "space": (49, " "), "left": (123, String(UnicodeScalar(NSLeftArrowFunctionKey)!)), "right": (124, String(UnicodeScalar(NSRightArrowFunctionKey)!))]
            if let k = keys[arg], let e = NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: [], timestamp: ProcessInfo.processInfo.systemUptime, windowNumber: window?.windowNumber ?? 0, context: nil, characters: k.1, charactersIgnoringModifiers: k.1, isARepeat: false, keyCode: k.0) { NSApp.sendEvent(e) }
            log("key", ["key": arg, "state": state(), "expanded": controls.expandedWanted])
        case "drag":                                                    // press on the seek bar and keep the button down (no release): the controls must not hide
            let a = arg.split(separator: ",").compactMap { Double($0) }
            guard a.count == 2 else { return }
            let bar = controls.view.seekBar
            func at(_ f: Double) -> NSPoint { bar.convert(NSPoint(x: 6 + CGFloat(f) * (bar.bounds.width - 12), y: bar.bounds.midY), to: nil) }
            controls.panel.sendEvent(event(.leftMouseDown, at(a[0]))); controls.panel.sendEvent(event(.leftMouseDragged, at(a[1])))
            log("drag", ["from": a[0], "to": a[1], "dragging": bar.dragging])
        case "release":
            let bar = controls.view.seekBar
            controls.panel.sendEvent(event(.leftMouseUp, bar.convert(NSPoint(x: 6 + CGFloat(Double(arg) ?? 0.5) * (bar.bounds.width - 12), y: bar.bounds.midY), to: nil)))
            log("release", ["at": arg, "dragging": bar.dragging])
        case "seq":                                                     // seq=name,frames,interval_ms: composites of the toolbar panel over time (stacked into one sheet)
            let a = arg.split(separator: ",").map(String.init)
            guard a.count == 3, let n = Int(a[1]), let ms = Double(a[2]) else { return }
            sequence(a[0], frames: n, interval: ms / 1000)
        case "contrast": contrast(arg)
        case "dump": dump(arg)
        case "probe": probe(arg)
        case "capture": capture(arg)
        default: log("unknown", ["cmd": cmd])
        }
    }

    private func probe(_ name: String) {
        var items: [String: Any] = [:]
        for (c, i) in controls.view.items {
            let p = centre(of: c)
            items[c.rawValue] = ["hidden": i.isHidden, "contentAlpha": Double(i.content.alphaValue), "frame": "\(i.frame)", "showsGlass": i.isShowingGlass, "debug": i.debugInfo, "hitAtCentre": p.map { hitName(at: $0) } ?? "-"]
        }
        let l = controls.layout
        log("probe", ["name": name, "items": items, "expanded": l?.expanded ?? false, "canExpand": l?.canExpand ?? false, "hiddenByWidth": (l?.hidden ?? []).map { $0.rawValue }, "toolbar": l.map { "\($0.toolbar)" } ?? "-",
                      "panel": "\(controls.panel.frame)", "panelAlpha": Double(controls.panel.alphaValue), "panelIgnoresMouse": controls.panel.ignoresMouseEvents, "panelVisible": controls.panel.isVisible, "shownToUser": controls.isShownToUser,
                      "isChild": window?.childWindows?.contains(controls.panel) ?? false, "children": window?.childWindows?.count ?? -1, "pointerInWindow": window.map { $0.frame.contains(NSEvent.mouseLocation) } ?? false, "visibleWindows": NSApp.windows.filter { $0.isVisible }.map { "\(type(of: $0)):\($0.title)" }, "allWindows": NSApp.windows.map { "\(type(of: $0)) '\($0.title)' vis=\($0.isVisible) \($0.frame) delegate=\(String(describing: $0.delegate.map { type(of: $0) })) attached=\($0 === window)" }, "dragging": controls.view.seekBar.dragging, "window": window.map { "\($0.frame)" } ?? "-", "content": window?.contentView.map { "\($0.bounds.size)" } ?? "-",
                      "contentMin": window.map { "\($0.contentMinSize)" } ?? "-", "fullscreen": window?.styleMask.contains(.fullScreen) ?? false, "shown": controls.shown,
                      "panelAppearance": controls.panel.effectiveAppearance.name.rawValue, "windowAppearance": window?.effectiveAppearance.name.rawValue ?? "-",
                      "surface": controls.policy.treatment.surface == .glass ? "glass" : "opaque", "animates": controls.policy.motion.animates, "gradientOpacity": controls.policy.treatment.gradientOpacity,
                      "hairline": controls.policy.treatment.hairlineOpacity, "state": state(), "counts": counts(),
                      "chevron": ["target": Double(controls.view.chevron.targetAngle), "angle": Double(controls.view.chevron.currentAngle), "turning": controls.view.chevron.isTurning, "title": controls.view.chevron.title, "hasImage": controls.view.chevron.image != nil]])
    }

    /// Measured readability: white icon luma against the glass behind it (a patch just inside the left edge of each visible control, clear of the icon), from the window-server composite.
    private func contrast(_ name: String) {
        guard let img = grab(controls.panel.frame) else { return }
        let pf = controls.panel.frame, sx = Double(img.width) / Double(pf.width), sy = Double(img.height) / Double(pf.height)
        guard let ctx = CGContext(data: nil, width: img.width, height: img.height, bitsPerComponent: 8, bytesPerRow: img.width * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue), let px = ctx.data?.assumingMemoryBound(to: UInt8.self) else { return }
        ctx.draw(img, in: CGRect(x: 0, y: 0, width: img.width, height: img.height))
        func lin(_ c: Double) -> Double { c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4) }
        func luma(_ x0: Int, _ y0: Int, _ w: Int, _ h: Int) -> Double {            // y0 from the TOP of the image
            var sum = 0.0, n = 0.0
            for y in y0..<(y0 + h) { for x in x0..<(x0 + w) { let o = (y * img.width + x) * 4; sum += 0.2126 * lin(Double(px[o]) / 255) + 0.7152 * lin(Double(px[o + 1]) / 255) + 0.0722 * lin(Double(px[o + 2]) / 255); n += 1 } }
            return sum / max(n, 1)
        }
        let iconL = 0.2126 * lin(0.96) + 0.7152 * lin(0.96) + 0.0722 * lin(0.96)      // white icon at alpha 0.96 is modelled as 0.96 grey
        var per: [String: Double] = [:], minC = 99.0
        for (c, i) in controls.view.items where !i.isHidden && i.content.alphaValue > 0.5 {
            let f = i.convert(i.bounds, to: controls.panel.contentView)
            let x = Int((f.minX + 5) * sx), y = Int((Double(pf.height) - f.midY - 4) * sy), w = Int(6 * sx), h = Int(8 * sy)
            guard x >= 0, y >= 0, x + w < img.width, y + h < img.height else { continue }
            let bg = luma(x, y, w, h), r = (max(iconL, bg) + 0.05) / (min(iconL, bg) + 0.05)
            per[c.rawValue] = (r * 100).rounded() / 100; minC = min(minC, r)
        }
        log("contrast", ["name": name, "min": (minC * 100).rounded() / 100, "per_control": per])
    }

    private func dump(_ name: String) {
        func layerInfo(_ l: CALayer) -> [String: Any] {
            var d: [String: Any] = ["cls": "\(type(of: l))", "opacity": Double(l.opacity), "hidden": l.isHidden, "hasBackground": l.backgroundColor != nil, "cornerRadius": Double(l.cornerRadius)]
            if let bg = l.backgroundColor { d["backgroundAlpha"] = Double(bg.alpha) }
            if let f = l.filters, !f.isEmpty { d["filters"] = f.map { "\(type(of: $0))" } }
            if let f = l.backgroundFilters, !f.isEmpty { d["backgroundFilters"] = f.map { "\(type(of: $0))" } }
            if l.compositingFilter != nil { d["compositingFilter"] = "yes" }
            return d
        }
        func view(_ v: NSView, _ depth: Int) -> [String: Any] {
            var d: [String: Any] = ["cls": "\(type(of: v))", "frame": "\(v.frame)", "alpha": Double(v.alphaValue), "hidden": v.isHidden, "isOpaque": v.isOpaque]
            if let l = v.layer { d["layer"] = layerInfo(l) }
            if let g = v as? NSGlassEffectView { d["glass"] = ["style": g.style == .clear ? "clear" : "regular", "tint": g.tintColor.map { "\($0)" } ?? "nil", "interactive": g.effectIsInteractive, "cornerRadius": Double(g.cornerRadius)] }
            if depth > 0 { d["subviews"] = v.subviews.map { view($0, depth - 1) } }
            return d
        }
        func panel(_ p: NSPanel) -> [String: Any] { ["isOpaque": p.isOpaque, "backgroundAlpha": Double(p.backgroundColor.alphaComponent), "hasShadow": p.hasShadow, "alpha": Double(p.alphaValue), "appearance": p.effectiveAppearance.name.rawValue, "frame": "\(p.frame)", "level": p.level.rawValue] }
        let out: [String: Any] = ["name": name, "toolbarPanel": panel(controls.panel), "toolbar": controls.panel.contentView.map { view($0, 6) } ?? [:], "scrimPanel": controls.scrimPanelInfo]
        if let d = try? JSONSerialization.data(withJSONObject: out, options: [.prettyPrinted, .sortedKeys]) { try? d.write(to: URL(fileURLWithPath: dir + "/" + name + "_hierarchy.json")) }
        log("dump", ["name": name])
    }

    private func sequence(_ name: String, frames n: Int, interval: Double) {
        var imgs: [CGImage] = []
        for i in 0..<n {
            DispatchQueue.main.asyncAfter(deadline: .now() + interval * Double(i)) { [self] in
                if let im = grab(controls.panel.frame) { imgs.append(im) }
                if imgs.count == n, let first = imgs.first, let ctx = CGContext(data: nil, width: first.width, height: first.height * n, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) {
                    for (j, im) in imgs.enumerated() { ctx.draw(im, in: CGRect(x: 0, y: first.height * (n - 1 - j), width: first.width, height: first.height)) }
                    if let out = ctx.makeImage(), let d = NSBitmapImageRep(cgImage: out).representation(using: .png, properties: [:]) { try? d.write(to: URL(fileURLWithPath: dir + "/" + name + "_sheet.png")) }
                    log("sequence", ["name": name, "frames": n])
                }
            }
        }
    }
    private func grab(_ r: NSRect) -> CGImage? {                          // window-server composite of OUR windows, copied at once (the image is lazily backed)
        guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "CGWindowListCreateImage") else { return nil }
        let fn = unsafeBitCast(sym, to: CapFn.self)
        let primaryH = NSScreen.screens.first?.frame.height ?? 0
        guard let raw = fn(CGRect(x: r.minX, y: primaryH - r.maxY, width: r.width, height: r.height), 1, 0, 8)?.takeRetainedValue(),
              let ctx = CGContext(data: nil, width: raw.width, height: raw.height, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
        ctx.draw(raw, in: CGRect(x: 0, y: 0, width: raw.width, height: raw.height))
        return ctx.makeImage()
    }
    private typealias CapFn = @convention(c) (CGRect, UInt32, UInt32, UInt32) -> Unmanaged<CGImage>?
    private func capture(_ name: String) {                                  // window-server composite of OUR windows only (no permission prompt); evidence, not approval
        guard let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "CGWindowListCreateImage"), let w = window, let cv = w.contentView else { return }
        let fn = unsafeBitCast(sym, to: CapFn.self)
        let primaryH = NSScreen.screens.first?.frame.height ?? 0
        let r = w.convertToScreen(cv.convert(cv.bounds, to: nil))
        guard let raw = fn(CGRect(x: r.minX, y: primaryH - r.maxY, width: r.width, height: r.height), 1, 0, 8)?.takeRetainedValue(),
              let ctx = CGContext(data: nil, width: raw.width, height: raw.height, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return }
        ctx.draw(raw, in: CGRect(x: 0, y: 0, width: raw.width, height: raw.height))                // the window-server image is lazily backed: copy it at once
        if let img = ctx.makeImage(), let d = NSBitmapImageRep(cgImage: img).representation(using: .png, properties: [:]) { try? d.write(to: URL(fileURLWithPath: dir + "/" + name + ".png")) }
        log("capture", ["name": name])
    }
}
#endif
