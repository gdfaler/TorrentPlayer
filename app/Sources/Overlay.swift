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
    let note = NSTextField(wrappingLabelWithString: "")
    let spinner = NSProgressIndicator()
    let bar = NSProgressIndicator()
    let retryButton = NSButton(title: "Retry", target: nil, action: nil)
    let cancelButton = NSButton(title: "Cancel", target: nil, action: nil)
    var onRetry: (() -> Void)?
    var onCancel: (() -> Void)?

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = NSColor(white: 0, alpha: 0.62).cgColor
        layer?.cornerRadius = 14
        title.font = .systemFont(ofSize: 17, weight: .semibold); title.textColor = .white; title.alignment = .center; title.lineBreakMode = .byTruncatingTail
        detail.font = .systemFont(ofSize: 12); detail.textColor = NSColor(white: 1, alpha: 0.78); detail.alignment = .center; detail.lineBreakMode = .byTruncatingTail
        note.font = .systemFont(ofSize: 11); note.textColor = NSColor(white: 1, alpha: 0.6); note.alignment = .center; note.maximumNumberOfLines = 3
        spinner.style = .spinning; spinner.controlSize = .regular; spinner.isIndeterminate = true; spinner.appearance = NSAppearance(named: .darkAqua)
        bar.style = .bar; bar.controlSize = .small; bar.isIndeterminate = false; bar.minValue = 0; bar.maxValue = 1; bar.appearance = NSAppearance(named: .darkAqua); bar.isHidden = true
        retryButton.bezelStyle = .rounded; retryButton.target = self; retryButton.action = #selector(retryTapped); retryButton.isHidden = true
        cancelButton.bezelStyle = .rounded; cancelButton.target = self; cancelButton.action = #selector(cancelTapped); cancelButton.isHidden = true
        cancelButton.setAccessibilityLabel("Cancel and go back to the file list")
        [spinner, title, detail, note, bar, retryButton, cancelButton].forEach { addSubview($0) }
    }
    required init?(coder: NSCoder) { fatalError() }
    @objc func retryTapped() { onRetry?() }
    @objc func cancelTapped() { onCancel?() }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    /// Returns the size the panel needs for this content.
    func set(title t: String, detail d: String, spinning: Bool, retry: Bool, note n: String = "", progress: Double? = nil, cancel: Bool = false) -> NSSize {
        title.stringValue = t; detail.stringValue = d; note.stringValue = n
        retryButton.isHidden = !retry; cancelButton.isHidden = !cancel
        if spinning { spinner.isHidden = false; spinner.startAnimation(nil) } else { spinner.isHidden = true; spinner.stopAnimation(nil) }
        if let p = progress { bar.isHidden = false; bar.doubleValue = p } else { bar.isHidden = true }
        let w = min(560, max(300, max(title.intrinsicContentSize.width, detail.intrinsicContentSize.width) + 56))
        var noteH: CGFloat = 0
        if !n.isEmpty {
            note.preferredMaxLayoutWidth = w - 32
            noteH = min(48, ceil((n as NSString).boundingRect(with: NSSize(width: w - 32, height: 200), options: [.usesLineFragmentOrigin], attributes: [.font: note.font as Any]).height) + 4)
        }
        var h: CGFloat = 20 + 22 + 6 + 16 + 18
        if spinning { h += 36 }
        if progress != nil { h += 14 }
        if !n.isEmpty { h += noteH + 4 }
        if retry || cancel { h += 34 }
        let size = NSSize(width: w, height: h)
        setFrameSize(size)
        var y = size.height - 18
        if spinning { y -= 28; spinner.frame = NSRect(x: (w - 28) / 2, y: y, width: 28, height: 28); y -= 8 }
        y -= 22; title.frame = NSRect(x: 16, y: y, width: w - 32, height: 22)
        y -= 20; detail.frame = NSRect(x: 16, y: y, width: w - 32, height: 16)
        if progress != nil { y -= 14; bar.frame = NSRect(x: 24, y: y + 2, width: w - 48, height: 10) }
        if !n.isEmpty { y -= noteH + 4; note.frame = NSRect(x: 16, y: y, width: w - 32, height: noteH) }
        let buttons = [retry ? retryButton : nil, cancel ? cancelButton : nil].compactMap { $0 }
        if !buttons.isEmpty {
            let total = CGFloat(buttons.count) * 90 + CGFloat(buttons.count - 1) * 10
            var x = (w - total) / 2
            for b in buttons { b.frame = NSRect(x: x, y: 12, width: 90, height: 26); x += 100 }
        }
        return size
    }
}

// MARK: seek bar with buffer-coverage bands
final class SeekBar: NSView {
    var fraction: Double = 0 { didSet { if !dragging { needsDisplay = true } } }
    var coverage: [UInt8] = [] { didSet { needsDisplay = true } }
    var onSeek: ((Double) -> Void)?
    var onScrub: ((Double) -> Void)?
    var onStep: ((Double) -> Void)?                     // accessibility increment / decrement: seconds relative to the current position
    var valueText = "0:00"                              // spoken value ("elapsed of total")
    var shadowOpacity: CGFloat = 0                      // soft shadow behind the knob (local contrast on bright video)
    private(set) var dragging = false
    private var dragFraction = 0.0
    override var isFlipped: Bool { false }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    private func frac(_ e: NSEvent) -> Double { let p = convert(e.locationInWindow, from: nil); return Double(max(0, min(1, (p.x - 6) / max(1, bounds.width - 12)))) }
    override func mouseDown(with e: NSEvent) { dragging = true; dragFraction = frac(e); onScrub?(dragFraction); needsDisplay = true }
    override func mouseDragged(with e: NSEvent) { dragFraction = frac(e); onScrub?(dragFraction); needsDisplay = true }
    override func mouseUp(with e: NSEvent) { dragFraction = frac(e); dragging = false; fraction = dragFraction; onSeek?(dragFraction); needsDisplay = true }

    // accessibility: a slider whose value is the playback position; increment / decrement step by 10 s
    override func isAccessibilityElement() -> Bool { true }
    override func accessibilityRole() -> NSAccessibility.Role? { .slider }
    override func accessibilityLabel() -> String? { "Seek" }
    override func accessibilityValue() -> Any? { valueText }
    override func accessibilityPerformIncrement() -> Bool { onStep?(10); return onStep != nil }
    override func accessibilityPerformDecrement() -> Bool { onStep?(-10); return onStep != nil }

    override func draw(_ dirtyRect: NSRect) {
        let trackH: CGFloat = 5, y = (bounds.height - trackH) / 2
        let track = NSRect(x: 6, y: y, width: bounds.width - 12, height: trackH)
        NSColor(white: 1, alpha: 0.26).setFill(); NSBezierPath(roundedRect: track, xRadius: 2.5, yRadius: 2.5).fill()
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
        NSColor(srgbRed: 0xB7 / 255, green: 0xC4 / 255, blue: 0xD8 / 255, alpha: 1).setFill()      // Titanium accent (provisional token #B7C4D8)
        NSBezierPath(roundedRect: NSRect(x: track.minX, y: y, width: max(0, px - track.minX), height: trackH), xRadius: 2.5, yRadius: 2.5).fill()
        NSGraphicsContext.saveGraphicsState()
        if shadowOpacity > 0 { let sh = NSShadow(); sh.shadowColor = NSColor(white: 0, alpha: shadowOpacity); sh.shadowBlurRadius = 2.5; sh.shadowOffset = NSSize(width: 0, height: -0.5); sh.set() }
        NSColor.white.setFill(); NSBezierPath(ovalIn: NSRect(x: px - 7, y: bounds.midY - 7, width: 14, height: 14)).fill()
        NSGraphicsContext.restoreGraphicsState()
    }
}
