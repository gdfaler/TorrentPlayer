// Overlay.swift: borderless, non-activating child panels over mpv's own window (architecture A from the research spikes).
// Two small panels (state banner, transport controls) instead of one full-size panel, so the video area stays click-through to mpv's window.
import AppKit

final class OverlayPanel: NSPanel {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
    init(size: NSSize) {
        super.init(contentRect: NSRect(origin: .zero, size: size), styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        isOpaque = false; backgroundColor = .clear; hasShadow = false; hidesOnDeactivate = false
        collectionBehavior = [.fullScreenAuxiliary]
        isReleasedWhenClosed = false
    }
}

// MARK: state banner
final class BannerView: NSView {
    let title = NSTextField(labelWithString: "")
    let detail = NSTextField(labelWithString: "")
    let spinner = NSProgressIndicator()
    let retryButton = NSButton(title: "Retry", target: nil, action: nil)
    var onRetry: (() -> Void)?

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = NSColor(white: 0, alpha: 0.62).cgColor
        layer?.cornerRadius = 14
        title.font = .systemFont(ofSize: 17, weight: .semibold); title.textColor = .white; title.alignment = .center; title.lineBreakMode = .byTruncatingTail
        detail.font = .systemFont(ofSize: 12); detail.textColor = NSColor(white: 1, alpha: 0.78); detail.alignment = .center; detail.lineBreakMode = .byTruncatingTail
        spinner.style = .spinning; spinner.controlSize = .regular; spinner.isIndeterminate = true; spinner.appearance = NSAppearance(named: .darkAqua)
        retryButton.bezelStyle = .rounded; retryButton.target = self; retryButton.action = #selector(retryTapped); retryButton.isHidden = true
        [spinner, title, detail, retryButton].forEach { addSubview($0) }
    }
    required init?(coder: NSCoder) { fatalError() }
    @objc func retryTapped() { onRetry?() }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    /// Returns the size the panel needs for this content.
    func set(title t: String, detail d: String, spinning: Bool, retry: Bool) -> NSSize {
        title.stringValue = t; detail.stringValue = d
        retryButton.isHidden = !retry
        if spinning { spinner.isHidden = false; spinner.startAnimation(nil) } else { spinner.isHidden = true; spinner.stopAnimation(nil) }
        let w = min(560, max(280, max(title.intrinsicContentSize.width, detail.intrinsicContentSize.width) + 56))
        var h: CGFloat = 20 + 22 + 6 + 16 + 18
        if spinning { h += 36 }
        if retry { h += 34 }
        let size = NSSize(width: w, height: h)
        setFrameSize(size)
        var y = size.height - 18
        if spinning { y -= 28; spinner.frame = NSRect(x: (w - 28) / 2, y: y, width: 28, height: 28); y -= 8 }
        y -= 22; title.frame = NSRect(x: 16, y: y, width: w - 32, height: 22)
        y -= 20; detail.frame = NSRect(x: 16, y: y, width: w - 32, height: 16)
        if retry { retryButton.frame = NSRect(x: (w - 90) / 2, y: 12, width: 90, height: 26) }
        return size
    }
}

// MARK: seek bar with buffer-coverage bands
final class SeekBar: NSView {
    var fraction: Double = 0 { didSet { if !dragging { needsDisplay = true } } }
    var coverage: [UInt8] = [] { didSet { needsDisplay = true } }
    var onSeek: ((Double) -> Void)?
    var onScrub: ((Double) -> Void)?
    private(set) var dragging = false
    private var dragFraction = 0.0
    override var isFlipped: Bool { false }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    private func frac(_ e: NSEvent) -> Double { let p = convert(e.locationInWindow, from: nil); return Double(max(0, min(1, (p.x - 6) / max(1, bounds.width - 12)))) }
    override func mouseDown(with e: NSEvent) { dragging = true; dragFraction = frac(e); onScrub?(dragFraction); needsDisplay = true }
    override func mouseDragged(with e: NSEvent) { dragFraction = frac(e); onScrub?(dragFraction); needsDisplay = true }
    override func mouseUp(with e: NSEvent) { dragFraction = frac(e); dragging = false; fraction = dragFraction; onSeek?(dragFraction); needsDisplay = true }

    override func draw(_ dirtyRect: NSRect) {
        let trackH: CGFloat = 5, y = (bounds.height - trackH) / 2
        let track = NSRect(x: 6, y: y, width: bounds.width - 12, height: trackH)
        NSColor(white: 1, alpha: 0.22).setFill(); NSBezierPath(roundedRect: track, xRadius: 2.5, yRadius: 2.5).fill()
        // cached (downloaded) bands
        if !coverage.isEmpty {
            NSColor(white: 1, alpha: 0.5).setFill()
            let n = coverage.count; let bw = track.width / CGFloat(n)
            var i = 0
            while i < n {
                if coverage[i] > 127 { var j = i; while j < n && coverage[j] > 127 { j += 1 }
                    NSBezierPath(rect: NSRect(x: track.minX + CGFloat(i) * bw, y: y, width: CGFloat(j - i) * bw + 0.5, height: trackH)).fill(); i = j } else { i += 1 }
            }
        }
        let f = dragging ? dragFraction : fraction
        let px = track.minX + CGFloat(f) * track.width
        NSColor.controlAccentColor.setFill(); NSBezierPath(roundedRect: NSRect(x: track.minX, y: y, width: max(0, px - track.minX), height: trackH), xRadius: 2.5, yRadius: 2.5).fill()
        NSColor.white.setFill(); NSBezierPath(ovalIn: NSRect(x: px - 6, y: bounds.midY - 6, width: 12, height: 12)).fill()
    }
}

// MARK: transport controls
final class ControlsView: NSView {
    let playButton = NSButton()
    let timeLabel = NSTextField(labelWithString: "0:00")
    let durationLabel = NSTextField(labelWithString: "0:00")
    let seekBar = SeekBar()
    var onPlayPause: (() -> Void)?

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = NSColor(white: 0, alpha: 0.58).cgColor
        layer?.cornerRadius = 12
        playButton.bezelStyle = .regularSquare; playButton.isBordered = false
        playButton.image = NSImage(systemSymbolName: "pause.fill", accessibilityDescription: "Pause")
        playButton.contentTintColor = .white; playButton.target = self; playButton.action = #selector(tapped)
        for l in [timeLabel, durationLabel] { l.font = .monospacedDigitSystemFont(ofSize: 12, weight: .regular); l.textColor = .white; l.alignment = .center }
        [playButton, timeLabel, seekBar, durationLabel].forEach { addSubview($0) }
    }
    required init?(coder: NSCoder) { fatalError() }
    @objc func tapped() { onPlayPause?() }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
    override func layout() {
        super.layout()
        let h = bounds.height, w = bounds.width
        playButton.frame = NSRect(x: 12, y: (h - 28) / 2, width: 28, height: 28)
        timeLabel.frame = NSRect(x: 46, y: (h - 16) / 2, width: 56, height: 16)
        durationLabel.frame = NSRect(x: w - 70, y: (h - 16) / 2, width: 58, height: 16)
        seekBar.frame = NSRect(x: 104, y: 0, width: max(40, w - 104 - 72), height: h)
    }
    func setPlaying(_ playing: Bool) {
        playButton.image = NSImage(systemSymbolName: playing ? "pause.fill" : "play.fill", accessibilityDescription: playing ? "Pause" : "Play")
    }
    static func format(_ s: Double) -> String {
        guard s.isFinite, s >= 0 else { return "0:00" }
        let t = Int(s); let h = t / 3600, m = (t % 3600) / 60, sec = t % 60
        return h > 0 ? String(format: "%d:%02d:%02d", h, m, sec) : String(format: "%d:%02d", m, sec)
    }
}
