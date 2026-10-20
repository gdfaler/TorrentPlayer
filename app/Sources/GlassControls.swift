// GlassControls: the playback controls as native AppKit Liquid Glass (NSGlassEffectView) in a non-activating child panel over mpv's own window.
// Layout, metrics and the accessibility / readability policy come from GlassMetrics.swift (pure, unit-tested); this file only turns them into views.
// The controls are thin: every button calls an existing PlayerSession action through a closure (togglePause, seekRelative, toggleMute, toggleFullScreen, seekFraction),
// no playback state lives here. All visual constants are PROVISIONAL (see GlassMetrics.swift).
import AppKit
import QuartzCore

// MARK: environment
enum GlassAccessibility {
    /// The system's accessibility settings. Testing builds (TP_TESTING) can force them with TP_FORCE_REDUCE_MOTION / _REDUCE_TRANSPARENCY / _INCREASE_CONTRAST so the policies can be exercised without touching System Settings.
    static func environment() -> GlassEnvironment {
        let ws = NSWorkspace.shared
        var e = GlassEnvironment(reduceMotion: ws.accessibilityDisplayShouldReduceMotion, reduceTransparency: ws.accessibilityDisplayShouldReduceTransparency, increaseContrast: ws.accessibilityDisplayShouldIncreaseContrast)
#if TP_TESTING
        let env = ProcessInfo.processInfo.environment
        if env["TP_FORCE_REDUCE_MOTION"] != nil { e.reduceMotion = true }
        if env["TP_FORCE_REDUCE_TRANSPARENCY"] != nil { e.reduceTransparency = true }
        if env["TP_FORCE_INCREASE_CONTRAST"] != nil { e.increaseContrast = true }
#endif
        return e
    }
}

extension GlassRGB {
    func nsColor(alpha: CGFloat = 1) -> NSColor { NSColor(srgbRed: CGFloat(r), green: CGFloat(g), blue: CGFloat(b), alpha: alpha) }
}
extension GlassFont {
    var nsFont: NSFont {
        let w: NSFont.Weight = weight == .semibold ? .semibold : weight == .medium ? .medium : .regular
        return monospacedDigits ? .monospacedDigitSystemFont(ofSize: size, weight: w) : .systemFont(ofSize: size, weight: w)
    }
}

// MARK: one glass control
/// One floating control: a native NSGlassEffectView holding the control's content (like Motion Lab v2: the glass view itself is what moves during the compact <-> expanded
/// animation, so the system can follow it; wrapping the glass in another view made the buttons vanish and the glass lose its depth). It takes mouse events only while shown and
/// its content is visible (GlassHitRule): alpha 0 does not stop AppKit hit-testing, and collapsed controls are stacked under Play (the Motion Lab v1 Play -> fullscreen bug).
/// The Reduce Transparency surface is an opaque fill on the content layer, over the glass.
final class GlassItemView: NSGlassEffectView {
    let control: GlassControl
    let content: NSView
    private(set) var showsGlass = true
    private var hairline: CGFloat = 0

    init(control: GlassControl, content: NSView, size: CGSize) {
        self.control = control; self.content = content
        super.init(frame: NSRect(origin: .zero, size: size))
        identifier = NSUserInterfaceItemIdentifier(control.rawValue)
        cornerRadius = GlassMetrics.cornerRadius(for: size)
        effectIsInteractive = true
        content.frame = bounds; content.autoresizingMask = [.width, .height]
        contentView = content
    }
    required init?(coder: NSCoder) { fatalError() }

    override func layout() {
        super.layout()
        let r = GlassMetrics.cornerRadius(for: bounds.size)
        if cornerRadius != r { cornerRadius = r }
        if !showsGlass { content.layer?.cornerRadius = r }
    }
    override func hitTest(_ point: NSPoint) -> NSView? {
        guard GlassHitRule.accepts(isHidden: isHidden, contentAlpha: content.alphaValue) else { return nil }
        return super.hitTest(point)
    }

    func apply(_ t: GlassTreatment) {
        style = t.clearStyle ? .clear : .regular
        tintColor = t.tintOpacity > 0 ? GlassRGB.graphite.nsColor(alpha: CGFloat(t.tintOpacity)) : nil
        showsGlass = t.surface == .glass
        content.wantsLayer = true
        if showsGlass { content.layer?.backgroundColor = nil; content.layer?.borderWidth = 0 }
        else {
            content.layer?.backgroundColor = t.opaqueFill.nsColor().cgColor
            content.layer?.borderWidth = 1; content.layer?.borderColor = NSColor(white: 1, alpha: CGFloat(t.hairlineOpacity)).cgColor
            content.layer?.cornerRadius = GlassMetrics.cornerRadius(for: bounds.size); content.layer?.masksToBounds = true
        }
    }
    var isShowingGlass: Bool { showsGlass }
#if TP_TESTING
    var debugInfo: String { "glass view, content \(content.frame) alpha \(content.alphaValue) hidden \(isHidden) opaque layer \(content.layer?.backgroundColor != nil)" }
#endif
}

// MARK: the expand / collapse chevron
/// ONE SF Symbol (chevron.up) drawn on its own layer and rotated 180 degrees between the states, so it turns together with the toolbar morph instead of being replaced by another
/// symbol. The turn uses the same duration and easing as the frame animation; a new toggle starts from the angle currently on screen, so rapid toggles never snap.
final class ChevronButton: NSButton {
    private let glyph = CALayer()
    private(set) var targetExpanded = false
    private var configured = false
    static let animationKey = "turn"

    override init(frame: NSRect) {
        super.init(frame: frame)
        isBordered = false; imagePosition = .noImage; title = ""
        wantsLayer = true
        guard let sym = NSImage(systemSymbolName: GlassSymbol.expand, accessibilityDescription: nil)?.withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: GlassMetrics.expandSymbolPoints, weight: .semibold)) else { return }
        let tinted = NSImage(size: sym.size, flipped: false) { r in sym.draw(in: r); ControlsView.textColor.set(); r.fill(using: .sourceAtop); return true }
        var r = NSRect(x: 0, y: 0, width: sym.size.width * 3, height: sym.size.height * 3)
        glyph.contents = tinted.cgImage(forProposedRect: &r, context: nil, hints: nil)
        glyph.contentsScale = 3; glyph.bounds = CGRect(origin: .zero, size: sym.size); glyph.contentsGravity = .resize
        layer?.addSublayer(glyph)
    }
    required init?(coder: NSCoder) { fatalError() }
    override func layout() { super.layout(); CATransaction.begin(); CATransaction.setDisableActions(true); glyph.position = CGPoint(x: bounds.midX, y: bounds.midY); CATransaction.commit() }

    /// Angle on screen right now (the presentation value while a turn is running).
    var currentAngle: CGFloat {
        if isTurning, let p = glyph.presentation()?.value(forKeyPath: "transform.rotation.z") as? CGFloat { return p }       // mid-turn: what is on screen
        return (glyph.value(forKeyPath: "transform.rotation.z") as? CGFloat) ?? 0                                        // at rest: the model (the presentation layer can lag one frame)
    }
    var isTurning: Bool { glyph.animation(forKey: Self.animationKey) != nil }
    var targetAngle: CGFloat { targetExpanded ? .pi : 0 }

    /// expanded = pointing down (180 degrees), compact = pointing up (0). `animated` false (Reduce Motion, first layout) changes at once; a non-animated call that repeats the current target
    /// leaves a running turn alone (window notifications re-apply the layout while a turn is in progress).
    func setExpanded(_ on: Bool, animated: Bool, duration: CFTimeInterval) {
        let target: CGFloat = on ? .pi : 0
        if !animated && configured && on == targetExpanded { return }
        let from = currentAngle
        targetExpanded = on; configured = true
        CATransaction.begin(); CATransaction.setDisableActions(true)
        glyph.removeAnimation(forKey: Self.animationKey)
        glyph.transform = CATransform3DMakeRotation(target, 0, 0, 1)
        CATransaction.commit()
        guard animated else { return }
        let a = CABasicAnimation(keyPath: "transform.rotation.z")
        a.fromValue = from; a.toValue = target; a.duration = duration; a.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
        glyph.add(a, forKey: Self.animationKey)
    }
}

// MARK: the controls view (panel content)
final class ControlsView: NSView {
    var onPlayPause: (() -> Void)?, onBack10: (() -> Void)?, onFwd10: (() -> Void)?, onMute: (() -> Void)?, onFullscreen: (() -> Void)?, onToggleExpanded: (() -> Void)?
    let timeLabel = NSTextField(labelWithString: "0:00")
    let durationLabel = NSTextField(labelWithString: "0:00")
    let seekBar = SeekBar()
    private(set) var actionCounts: [GlassControl: Int] = [:]               // per control: how many times its action ran (interaction checks)
    private(set) var items: [GlassControl: GlassItemView] = [:]
    private let container = NSGlassEffectContainerView()
    private let holder = NSView()
    private let seekRow = NSView()
    private var buttons: [GlassControl: NSButton] = [:]
    private(set) var chevron: ChevronButton!
    private var hideToken = 0
    private(set) var expandedShown = false

    static let textColor = GlassRGB.text.nsColor(alpha: 0.96)
    static func symbol(_ name: String, _ points: CGFloat, _ label: String) -> NSImage? {
        NSImage(systemSymbolName: name, accessibilityDescription: label)?.withSymbolConfiguration(NSImage.SymbolConfiguration(pointSize: points, weight: .semibold))
    }

    override init(frame: NSRect) {
        super.init(frame: frame)
        container.spacing = 0                                              // batch only: neighbouring buttons never merge (approved)
        container.contentView = holder
        addSubview(container)
        func button(_ c: GlassControl, _ symbol: String, _ points: CGFloat, _ action: Selector) {
            let size = GlassMetrics.size(of: c)
            let b = NSButton(frame: NSRect(origin: .zero, size: size))
            b.isBordered = false; b.imagePosition = .imageOnly; b.target = self; b.action = action
            b.image = Self.symbol(symbol, points, c.accessibilityLabel); b.contentTintColor = Self.textColor
            b.identifier = NSUserInterfaceItemIdentifier(c.rawValue); b.setAccessibilityLabel(c.accessibilityLabel); b.toolTip = c.accessibilityLabel
            buttons[c] = b
            let item = GlassItemView(control: c, content: b, size: size)
            items[c] = item; holder.addSubview(item)
        }
        button(.back10, GlassSymbol.back10, GlassMetrics.transportSymbolPoints, #selector(tapped(_:)))
        button(.play, GlassSymbol.pause, GlassMetrics.transportSymbolPoints, #selector(tapped(_:)))
        button(.fwd10, GlassSymbol.fwd10, GlassMetrics.transportSymbolPoints, #selector(tapped(_:)))
        button(.volume, GlassSymbol.volume, GlassMetrics.utilitySymbolPoints, #selector(tapped(_:)))
        button(.fullscreen, GlassSymbol.enterFullscreen, GlassMetrics.utilitySymbolPoints, #selector(tapped(_:)))
        do {                                                               // the chevron is one rotating symbol, not two swapped images
            let c = GlassControl.expand, size = GlassMetrics.size(of: c)
            let b = ChevronButton(frame: NSRect(origin: .zero, size: size))
            b.target = self; b.action = #selector(tapped(_:)); b.contentTintColor = Self.textColor
            b.identifier = NSUserInterfaceItemIdentifier(c.rawValue); b.setAccessibilityLabel(c.accessibilityLabel); b.toolTip = c.accessibilityLabel
            buttons[c] = b; chevron = b
            let item = GlassItemView(control: c, content: b, size: size)
            items[c] = item; holder.addSubview(item)
        }
        // seek capsule: time | seek bar | duration (the existing SeekBar: coverage bands, drag, release)
        for l in [timeLabel, durationLabel] { l.font = GlassMetrics.timeFont.nsFont; l.textColor = Self.textColor; l.alignment = .center; l.setAccessibilityLabel(l === timeLabel ? "Elapsed time" : "Duration") }
        seekRow.addSubview(timeLabel); seekRow.addSubview(seekBar); seekRow.addSubview(durationLabel)
        let seekItem = GlassItemView(control: .seek, content: seekRow, size: GlassMetrics.size(of: .seek))
        items[.seek] = seekItem; holder.addSubview(seekItem)
        seekRow.postsFrameChangedNotifications = true
        NotificationCenter.default.addObserver(self, selector: #selector(layoutSeekRow), name: NSView.frameDidChangeNotification, object: seekRow)
        applyTreatment(GlassPolicy.resolve(GlassAccessibility.environment()).treatment)
    }
    required init?(coder: NSCoder) { fatalError() }
    deinit { NotificationCenter.default.removeObserver(self) }

    override func layout() { super.layout(); container.frame = bounds; holder.frame = container.bounds }
    override func hitTest(_ point: NSPoint) -> NSView? { let v = super.hitTest(point); return (v === self || v === container || v === holder) ? nil : v }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    @objc private func layoutSeekRow() {
        let w = seekRow.bounds.width, h = seekRow.bounds.height
        if w >= 220 {
            timeLabel.frame = NSRect(x: 8, y: (h - 16) / 2, width: 50, height: 16); durationLabel.isHidden = false
            durationLabel.frame = NSRect(x: w - 58, y: (h - 16) / 2, width: 50, height: 16); seekBar.frame = NSRect(x: 58, y: 0, width: max(20, w - 116), height: h)
        } else {                                                           // narrow capsule: the duration is dropped, the seek bar keeps most of the width
            timeLabel.frame = NSRect(x: 10, y: (h - 16) / 2, width: 44, height: 16); durationLabel.isHidden = true
            seekBar.frame = NSRect(x: 54, y: 0, width: max(20, w - 54 - 12), height: h)
        }
    }

    @objc private func tapped(_ sender: NSButton) {
        guard let id = sender.identifier?.rawValue, let c = GlassControl(rawValue: id) else { return }
        actionCounts[c, default: 0] += 1
        switch c {
        case .play: onPlayPause?()
        case .back10: onBack10?()
        case .fwd10: onFwd10?()
        case .volume: onMute?()
        case .fullscreen: onFullscreen?()
        case .expand: onToggleExpanded?()
        case .seek, .inspector: break
        }
    }

    // MARK: state shown by the controls (the truth stays in PlayerSession)
    func setPlaying(_ playing: Bool) {
        let label = playing ? "Pause" : "Play"
        buttons[.play]?.image = Self.symbol(GlassSymbol.playPause(paused: !playing), GlassMetrics.transportSymbolPoints, label)
        buttons[.play]?.setAccessibilityLabel(label); buttons[.play]?.toolTip = label
    }
    func setMuted(_ muted: Bool) {
        let label = muted ? "Unmute" : "Mute"
        buttons[.volume]?.image = Self.symbol(GlassSymbol.mute(muted: muted), GlassMetrics.utilitySymbolPoints, label)
        buttons[.volume]?.setAccessibilityLabel(label); buttons[.volume]?.toolTip = label
    }
    func setFullscreen(_ fs: Bool) {
        let label = fs ? "Exit full screen" : "Enter full screen"
        buttons[.fullscreen]?.image = Self.symbol(GlassSymbol.fullscreen(isFullscreen: fs), GlassMetrics.utilitySymbolPoints, label)
        buttons[.fullscreen]?.setAccessibilityLabel(label); buttons[.fullscreen]?.toolTip = label
    }
    func applyTreatment(_ t: GlassTreatment) { for i in items.values { i.apply(t) } }

    static func format(_ s: Double) -> String {
        guard s.isFinite, s >= 0 else { return "0:00" }
        let t = Int(s); let h = t / 3600, m = (t % 3600) / 60, sec = t % 60
        return h > 0 ? String(format: "%d:%02d:%02d", h, m, sec) : String(format: "%d:%02d", m, sec)
    }

    // MARK: layout and the compact <-> expanded motion
    /// Puts every control where `layout` says (frames are in content coordinates; `origin` is the panel's origin in the same space). Controls the layout does not show collapse
    /// onto Play: icon faded out, then hidden (so they leave hit-testing and the glass layout). Approved motion: frames ease in/out over the policy's expand duration, icons fade
    /// separately; with Reduce Motion every change is instant.
    func apply(layout: GlassLayout, origin: CGPoint, showsExpand: Bool, motion: GlassMotion, animated: Bool) {
        let on = layout.expanded
        let animate = animated && motion.animates
        hideToken += 1; let token = hideToken
        let collapseTo = (layout.frames[.play] ?? .zero).offsetBy(dx: -origin.x, dy: -origin.y)
        var hiddenAfter: [GlassItemView] = []
        struct Target { let item: GlassItemView; let frame: CGRect; let alpha: CGFloat }
        var targets: [Target] = []
        for (c, item) in items {
            if let f = layout.frames[c], c != .expand || showsExpand {
                targets.append(Target(item: item, frame: f.offsetBy(dx: -origin.x, dy: -origin.y), alpha: 1)); item.isHidden = false
            } else {
                targets.append(Target(item: item, frame: collapseTo, alpha: 0))
                if c == .expand { item.isHidden = true } else { hiddenAfter.append(item) }
            }
        }
        let wasExpanded = expandedShown; expandedShown = on
        if layout.frames[.expand] != nil, showsExpand {                    // the chevron says what it will do and turns with the morph (same duration and easing)
            let label = on ? "Show fewer controls" : "Show more controls"
            buttons[.expand]?.setAccessibilityLabel(label); buttons[.expand]?.toolTip = label
            chevron.setExpanded(on, animated: animate && wasExpanded != on, duration: motion.expandDuration)
        }
        let frames: () -> Void = { for t in targets { if animate { t.item.animator().frame = t.frame } else { t.item.frame = t.frame } } }
        let icons: () -> Void = { for t in targets { if animate { t.item.content.animator().alphaValue = t.alpha } else { t.item.content.alphaValue = t.alpha } } }
        let finish: () -> Void = { [weak self] in
            guard let self = self, self.hideToken == token else { return }
            for i in hiddenAfter { i.isHidden = true }
        }
        if animate && wasExpanded != on {
            NSAnimationContext.runAnimationGroup({ ctx in ctx.duration = motion.expandDuration; ctx.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut); ctx.allowsImplicitAnimation = true; frames() })
            let fade: () -> Void = {
                NSAnimationContext.runAnimationGroup({ ctx in ctx.duration = on ? motion.iconFadeDuration : motion.iconCollapseFadeDuration; ctx.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut); ctx.allowsImplicitAnimation = true; icons() })
            }
            // Collapsing: the icons' model alpha drops NOW (same turn as the frames), so the controls stacked under Play stop taking clicks at once; expanding: they fade in after a delay.
            if on { DispatchQueue.main.asyncAfter(deadline: .now() + motion.iconFadeDelay) { [weak self] in guard let self = self, self.hideToken == token else { return }; fade() } }
            else { fade() }
            if !on { DispatchQueue.main.asyncAfter(deadline: .now() + motion.expandDuration + 0.04, execute: finish) }
        } else {
            frames(); icons(); finish()
        }
    }
}

// MARK: readability gradient under the controls
final class GlassScrimView: NSView {
    var opacity: CGFloat = 0.55 { didSet { update() } }
    private let g = CAGradientLayer()
    override init(frame: NSRect) { super.init(frame: frame); wantsLayer = true; layer?.addSublayer(g); update() }
    required init?(coder: NSCoder) { fatalError() }
    override func layout() { super.layout(); g.frame = bounds }
    private func update() {                                               // full strength from the bottom edge up to the toolbar, fading over the top of the strip
        let c = GlassRGB.graphite
        g.colors = [c.nsColor(alpha: 0).cgColor, c.nsColor(alpha: opacity).cgColor, c.nsColor(alpha: opacity).cgColor]; g.locations = [0, 0.22, 1]
        g.startPoint = CGPoint(x: 0.5, y: 1); g.endPoint = CGPoint(x: 0.5, y: 0)
    }
}

// MARK: controller: panels, layout, visibility, accessibility
final class GlassControls: NSObject {
    let view = ControlsView(frame: .zero)
    let panel = OverlayPanel(size: NSSize(width: 100, height: 100))
    private let scrimPanel = OverlayPanel(size: NSSize(width: 100, height: 100))
    private let scrimView = GlassScrimView(frame: .zero)
    private weak var window: NSWindow?
    private(set) var expandedWanted = true
    private(set) var policy = GlassPolicy.resolve(GlassAccessibility.environment())
    private(set) var layout: GlassLayout?
    private(set) var shown = true
    private var observers: [NSObjectProtocol] = []
#if TP_TESTING
    var testState: () -> [String: Any] = { [:] }                         // set by the player; read only by the test driver (testing builds only)
#endif
    private static let panelPadding: CGFloat = 16
    /// Narrower than this the expanded toolbar cannot fit; the window gets it as its minimum content width so the compact fallback is not reachable by resizing.
    static let minimumContentWidth = GlassLayout.minimumExpandedContentWidth(includesInspector: false)

    override init() {
        super.init()
        panel.contentView = view
        panel.appearance = NSAppearance(named: .darkAqua)                 // glass stays dark (readability strategy), also in Light mode
        scrimPanel.contentView = scrimView; scrimPanel.ignoresMouseEvents = true
    }

    func attach(to w: NSWindow) {
        window = w
        w.contentMinSize = NSSize(width: Self.minimumContentWidth, height: 200)
        w.addChildWindow(scrimPanel, ordered: .above); w.addChildWindow(panel, ordered: .above)
        scrimPanel.orderFront(nil); panel.orderFront(nil)
        observers.append(NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.accessibilityDisplayOptionsDidChangeNotification, object: nil, queue: .main) { [weak self] _ in self?.refreshPolicy() })
        refreshPolicy()
#if TP_TESTING
        GlassTestDriver.startIfRequested(controls: self, window: w, state: { [weak self] in self?.testState() ?? [:] })
#endif
    }

    func teardown() {
        for o in observers { NSWorkspace.shared.notificationCenter.removeObserver(o) }
        observers.removeAll()
        window?.removeChildWindow(panel); window?.removeChildWindow(scrimPanel)
        panel.orderOut(nil); scrimPanel.orderOut(nil)
    }

    private func refreshPolicy() {
        let env = GlassAccessibility.environment()
        policy = GlassPolicy.resolve(env)
        view.applyTreatment(policy.treatment)
        scrimView.opacity = CGFloat(policy.treatment.gradientOpacity)
        scrimPanel.alphaValue = (shown && policy.treatment.surface == .glass) ? 1 : 0
        reposition(animated: false)
    }

    /// Frames for the current window size and state. Called on attach, resize, move, fullscreen and appearance / accessibility changes.
    func reposition(animated: Bool) {
        guard let w = window, let cv = w.contentView else { return }
        let fs = w.styleMask.contains(.fullScreen)
        let size = cv.bounds.size
        let l = GlassLayout.make(content: size, fullscreen: fs, expanded: expandedWanted, inspectorOpen: false, includesInspector: false)
        let full = GlassLayout.make(content: size, fullscreen: fs, expanded: true, inspectorOpen: false, includesInspector: false)
        layout = l
        let pad = Self.panelPadding
        let rect = (full.canExpand ? full.toolbar : l.toolbar).insetBy(dx: -pad, dy: -pad)      // the panel is as wide as the expanded toolbar so compact and expanded share it
        let origin = w.convertToScreen(cv.convert(cv.bounds, to: nil)).origin
        panel.setFrame(NSRect(x: origin.x + rect.minX, y: origin.y + rect.minY, width: rect.width, height: rect.height), display: true)
        scrimPanel.setFrame(NSRect(x: origin.x, y: origin.y, width: size.width, height: l.gradientHeight), display: true)
        view.setFullscreen(fs)
        view.apply(layout: l, origin: rect.origin, showsExpand: l.canExpand, motion: policy.motion, animated: animated)
    }

    /// Compact <-> expanded (button and the E key). Nothing happens when the window is too narrow to expand (the chevron is hidden then).
    func toggleExpanded() {
        guard layout?.canExpand == true else { return }
        expandedWanted.toggle()
        reposition(animated: true)
    }

    /// Fade the controls in or out. Hidden controls take no mouse events (the panel ignores them), so the video area stays click-through.
    func setVisible(_ show: Bool) {
        guard show != shown else { return }
        shown = show
        let d = policy.motion.revealDuration
        if show { panel.ignoresMouseEvents = false }
        else { panel.ignoresMouseEvents = true }
        let apply: () -> Void = {
            self.panel.alphaValue = show ? 1 : 0
            self.scrimPanel.alphaValue = (show && self.policy.treatment.surface == .glass) ? 1 : 0
        }
        if d > 0 { NSAnimationContext.runAnimationGroup({ ctx in ctx.duration = d; ctx.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut); ctx.allowsImplicitAnimation = true; self.panel.animator().alphaValue = show ? 1 : 0; self.scrimPanel.animator().alphaValue = (show && self.policy.treatment.surface == .glass) ? 1 : 0 }) }
        else { apply() }
    }

#if TP_TESTING
    /// Test driver: facts about the gradient panel (it sits BELOW the toolbar panel: the glass samples what it draws).
    var scrimPanelInfo: [String: Any] { ["frame": "\(scrimPanel.frame)", "alpha": Double(scrimPanel.alphaValue), "isOpaque": scrimPanel.isOpaque, "gradientOpacity": Double(scrimView.opacity), "ignoresMouse": scrimPanel.ignoresMouseEvents] }
#endif
    /// Whether the controls are actually shown to the user (the panel stays ordered in with alpha 0 while auto-hidden, so `panel.isVisible` alone is not the truth).
    var isShownToUser: Bool { shown && panel.isVisible }

    /// True while the pointer is over the toolbar (keeps it visible).
    var pointerIsOverControls: Bool { panel.isVisible && panel.frame.insetBy(dx: -8, dy: -8).contains(NSEvent.mouseLocation) }
}
