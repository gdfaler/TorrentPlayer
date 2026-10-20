// GlassMetrics: the numbers behind the Titanium Glass playback chrome, with NO UI, NO AppKit and NO mpv: control sizes, spacing, radii, type and symbol names,
// the pure layout of the floating controls and the inspector, and the accessibility / readability policy. Unit-tested on its own.
// Milestone 1 of the polish work: nothing in the app uses this yet.
// PROVISIONAL: every visual constant and every layout threshold in this file (sizes, margins, gaps, the narrow-window limits, durations, scrim/tint/hairline
// values, type sizes) is a starting value to be adjusted after visual testing; the tests pin the CURRENT values and the invariants (no overlap, margins, bounds,
// policy rules), so a deliberate change of a constant means updating the exact-number checks, nothing else.
// CONTRAST NUMBERS are MODELLED estimates (a white or black frame darkened by the scrim and tint, composited in gamma space), NOT measured guarantees: the real
// NSGlassEffectView samples the video itself (its tone follows the backdrop, the system adds its own highlights and shadows) and nothing here can know the video.
// The only real measurements are the Motion Lab composites (white icons over a pure white frame: 6.2:1 for the default combination); readability over arbitrary
// video must be confirmed by eye and, later, by captures of the integrated controls.
// Coordinates: the playback window's content area, origin bottom-left (AppKit), points.
import Foundation
import CoreGraphics

// MARK: controls and symbols
enum GlassControl: String, CaseIterable {
    case back10, play, fwd10, seek, volume, inspector, fullscreen, expand
    var accessibilityLabel: String {
        switch self {
        case .back10: return "Back 10 seconds"
        case .play: return "Play or pause"
        case .fwd10: return "Forward 10 seconds"
        case .seek: return "Seek"
        case .volume: return "Mute or unmute"
        case .inspector: return "Show or hide the torrent inspector"
        case .fullscreen: return "Enter or exit full screen"
        case .expand: return "Show or hide more controls"
        }
    }
}

/// SF Symbol identifiers (one cohesive family, semibold weight).
enum GlassSymbol {
    static let play = "play.fill", pause = "pause.fill"
    static let back10 = "gobackward.10", fwd10 = "goforward.10"
    static let volume = "speaker.wave.2.fill", muted = "speaker.slash.fill"
    static let inspector = "sidebar.right"
    static let enterFullscreen = "arrow.up.left.and.arrow.down.right", exitFullscreen = "arrow.down.right.and.arrow.up.left"
    static let expand = "chevron.up", collapse = "chevron.down"
    static func playPause(paused: Bool) -> String { paused ? play : pause }
    static func mute(muted m: Bool) -> String { m ? muted : volume }
    static func fullscreen(isFullscreen f: Bool) -> String { f ? exitFullscreen : enterFullscreen }
}

struct GlassFont: Equatable {
    enum Weight { case regular, medium, semibold }
    let size: CGFloat
    let weight: Weight
    let monospacedDigits: Bool
}

// MARK: metrics
enum GlassMetrics {
    // control sizes (circles use size/2 as the corner radius; the seek capsule uses height/2)
    static let transportSize: CGFloat = 52          // back 10, play/pause, forward 10
    static let utilitySize: CGFloat = 44            // volume, inspector, fullscreen
    static let expandSize: CGFloat = 36             // the expand / collapse chevron
    static let seekHeight: CGFloat = 48
    static let seekWidthMin: CGFloat = 140
    static let seekWidthRange: ClosedRange<CGFloat> = 280...420
    static let seekWidthFraction: CGFloat = 0.28    // of the content width, clamped to the range
    // spacing: glass buttons stay visibly separate (approved; container spacing 0 + gaps this large)
    static let buttonGap: CGFloat = 14
    static let groupGap: CGFloat = 24
    static let compactGap: CGFloat = 8
    static let sideMargin: CGFloat = 16             // minimum distance from the window's left and right edges
    static let bottomMargin: CGFloat = 28, bottomMarginFullscreen: CGFloat = 44
    static let topMargin: CGFloat = 24, topMarginFullscreen: CGFloat = 48
    static let rightMargin: CGFloat = 24
    // inspector (floating glass panel at the right edge; the video is never resized)
    static let inspectorWidthRange: ClosedRange<CGFloat> = 240...320
    static let inspectorWidthFraction: CGFloat = 0.32
    static let inspectorHeightMax: CGFloat = 380
    static let inspectorHeightMin: CGFloat = 160    // below this the inspector is not shown rather than squeezed
    static let inspectorGap: CGFloat = 16           // vertical gap above the toolbar
    static let inspectorCornerRadius: CGFloat = 24
    // readability gradient under the controls
    static let gradientHeightMax: CGFloat = 300, gradientHeightFraction: CGFloat = 0.45
    // symbols and type
    static let transportSymbolPoints: CGFloat = 21, utilitySymbolPoints: CGFloat = 17, expandSymbolPoints: CGFloat = 15
    static let timeFont = GlassFont(size: 11, weight: .medium, monospacedDigits: true)
    static let inspectorTitleFont = GlassFont(size: 15, weight: .semibold, monospacedDigits: false)
    static let inspectorLabelFont = GlassFont(size: 12, weight: .regular, monospacedDigits: false)
    static let inspectorValueFont = GlassFont(size: 12, weight: .medium, monospacedDigits: true)

    static func cornerRadius(for size: CGSize) -> CGFloat { min(size.width, size.height) / 2 }
    static func size(of c: GlassControl) -> CGSize {
        switch c {
        case .back10, .play, .fwd10: return CGSize(width: transportSize, height: transportSize)
        case .volume, .inspector, .fullscreen: return CGSize(width: utilitySize, height: utilitySize)
        case .expand: return CGSize(width: expandSize, height: expandSize)
        case .seek: return CGSize(width: seekWidthMin, height: seekHeight)
        }
    }
}

// MARK: layout
struct GlassLayout: Equatable {
    var frames: [GlassControl: CGRect]      // visible controls only
    var toolbar: CGRect                     // union of the visible controls
    var expanded: Bool                      // EFFECTIVE state: false when the window is too narrow for the expanded toolbar
    var canExpand: Bool                     // whether the expanded toolbar fits at this width (false: hide/disable the chevron)
    var hidden: [GlassControl]              // controls dropped for lack of width (volume first)
    var inspector: CGRect?                  // nil when closed or when there is not enough height
    var gradientHeight: CGFloat             // height of the readability gradient from the bottom edge

    private static let expandedUtilitySets: [[GlassControl]] = [[.volume, .inspector, .fullscreen], [.inspector, .fullscreen]]
    /// Width of the expanded toolbar without the seek bar.
    private static func expandedFixedWidth(utilityCount n: Int) -> CGFloat {
        typealias M = GlassMetrics
        let transportW = 3 * M.transportSize + 2 * M.buttonGap
        let utilW = CGFloat(n) * M.utilitySize + CGFloat(n - 1) * M.buttonGap
        return transportW + M.groupGap + M.groupGap + utilW + M.groupGap + M.expandSize
    }
    /// The expanded toolbar for this content width: all utility buttons if they fit (seek bar shrinking down to its minimum), else without volume, else nil.
    private static func expandedPlan(contentWidth: CGFloat) -> (utility: [GlassControl], seekWidth: CGFloat)? {
        typealias M = GlassMetrics
        let avail = contentWidth - 2 * M.sideMargin
        let preferred = min(max(contentWidth * M.seekWidthFraction, M.seekWidthRange.lowerBound), M.seekWidthRange.upperBound)
        for utility in expandedUtilitySets {
            let seekW = min(preferred, avail - expandedFixedWidth(utilityCount: utility.count))
            if seekW >= M.seekWidthMin { return (utility, seekW) }
        }
        return nil
    }
    /// Narrowest content width at which the expanded toolbar fits (without volume, seek bar at its minimum).
    static var minimumExpandedContentWidth: CGFloat { expandedFixedWidth(utilityCount: 2) + GlassMetrics.seekWidthMin + 2 * GlassMetrics.sideMargin }

    /// NARROW WINDOWS: when the expanded toolbar cannot fit (content narrower than `GlassLayout.minimumExpandedContentWidth`), the layout falls back to the
    /// compact toolbar (play/pause + chevron) and `canExpand` is false: the integration must then HIDE or DISABLE the chevron (it could not do anything) and keep
    /// every action reachable some other way. Today these actions are reachable from the keyboard and menus regardless of the toolbar: Space (play/pause),
    /// Left/Right (seek 5 s, Shift 30 s), Up/Down (volume), M (mute), F or View > Enter Full Screen (fullscreen), Esc (leave fullscreen). NOT reachable when
    /// compact: the +/-10 s buttons and the mouse seek bar; the inspector has no shortcut yet (it does not exist yet: the milestone that adds it must add a menu
    /// item / shortcut). Recommended for the integration: give the playback window a minimum content width of `GlassLayout.minimumExpandedContentWidth` so the fallback is
    /// practically unreachable.
    /// `content`: the playback window's content size (fullscreen: the screen-sized content). Precondition: at least about 160 pt wide.
    static func make(content: CGSize, fullscreen: Bool, expanded wantsExpanded: Bool, inspectorOpen: Bool) -> GlassLayout {
        typealias M = GlassMetrics
        let bottom = fullscreen ? M.bottomMarginFullscreen : M.bottomMargin
        let centerY = bottom + M.transportSize / 2
        func rect(_ w: CGFloat, _ h: CGFloat, _ x: CGFloat) -> CGRect { CGRect(x: x, y: centerY - h / 2, width: w, height: h) }

        var frames: [GlassControl: CGRect] = [:]
        var hidden: [GlassControl] = []
        var isExpanded = false

        if wantsExpanded, let plan = expandedPlan(contentWidth: content.width) {
            let utility = plan.utility
            let seekW = plan.seekWidth
            let fixed = expandedFixedWidth(utilityCount: utility.count)
            var x = (content.width - (fixed + seekW)) / 2
            for c in [GlassControl.back10, .play, .fwd10] { frames[c] = rect(M.transportSize, M.transportSize, x); x += M.transportSize + M.buttonGap }
            x += M.groupGap - M.buttonGap
            frames[.seek] = rect(seekW, M.seekHeight, x); x += seekW + M.groupGap
            for c in utility { frames[c] = rect(M.utilitySize, M.utilitySize, x); x += M.utilitySize + M.buttonGap }
            x += M.groupGap - M.buttonGap
            frames[.expand] = rect(M.expandSize, M.expandSize, x)
            hidden = utility.count == 3 ? [] : [.volume]
            isExpanded = true
        }
        if !isExpanded {                                    // compact: play + chevron (also the fallback for very narrow windows)
            let w = M.transportSize + M.compactGap + M.expandSize
            let x = (content.width - w) / 2
            frames = [.play: rect(M.transportSize, M.transportSize, x), .expand: rect(M.expandSize, M.expandSize, x + M.transportSize + M.compactGap)]
            hidden = []
        }
        let toolbar = frames.values.reduce(CGRect.null) { $0.union($1) }

        var inspector: CGRect?
        if inspectorOpen {
            let top = content.height - (fullscreen ? M.topMarginFullscreen : M.topMargin)
            let minY = toolbar.maxY + M.inspectorGap
            let room = top - minY
            if room >= M.inspectorHeightMin {
                let w = min(max(content.width * M.inspectorWidthFraction, M.inspectorWidthRange.lowerBound), M.inspectorWidthRange.upperBound, content.width - 2 * M.sideMargin)
                inspector = CGRect(x: content.width - M.rightMargin - w, y: minY, width: w, height: min(M.inspectorHeightMax, room))
            }
        }
        let gradient = min(M.gradientHeightMax, content.height * M.gradientHeightFraction)
        return GlassLayout(frames: frames, toolbar: toolbar, expanded: isExpanded, canExpand: expandedPlan(contentWidth: content.width) != nil, hidden: hidden, inspector: inspector, gradientHeight: gradient)
    }
}

// MARK: colour helpers (sRGB; the same relative-luminance contrast as WCAG)
struct GlassRGB: Equatable {
    let r: Double, g: Double, b: Double          // 0...1, gamma encoded
    init(hex: Int) { r = Double((hex >> 16) & 255) / 255; g = Double((hex >> 8) & 255) / 255; b = Double(hex & 255) / 255 }
    init(r: Double, g: Double, b: Double) { self.r = r; self.g = g; self.b = b }
    static let white = GlassRGB(hex: 0xFFFFFF), black = GlassRGB(hex: 0x000000)
    // Titanium Glass dark tokens (initial values from the specification; used here only by the readability maths)
    static let graphite = GlassRGB(hex: 0x101318), surface = GlassRGB(hex: 0x1B2028), elevated = GlassRGB(hex: 0x262D37)
    static let text = GlassRGB(hex: 0xF0F3F8), secondaryText = GlassRGB(hex: 0xA5AFBE)
    /// This colour composited over `base` at `alpha` (gamma-space blend, as the compositor does).
    func over(_ base: GlassRGB, alpha a: Double) -> GlassRGB { GlassRGB(r: base.r * (1 - a) + r * a, g: base.g * (1 - a) + g * a, b: base.b * (1 - a) + b * a) }
    var luminance: Double {
        func lin(_ c: Double) -> Double { c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4) }
        return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)
    }
    static func contrast(_ a: GlassRGB, _ b: GlassRGB) -> Double { let x = a.luminance, y = b.luminance; return (max(x, y) + 0.05) / (min(x, y) + 0.05) }
}

// MARK: accessibility and readability policy
struct GlassEnvironment: Equatable {
    var reduceMotion = false
    var reduceTransparency = false
    var increaseContrast = false
}

enum GlassSurface: Equatable { case glass, opaque }

/// How the floating controls are drawn so the icons stay legible. The video's brightness is NOT known (no luminance detection, no frame access):
/// every treatment is designed for the WORST case, a pure white frame, and must still work over black.
struct GlassTreatment: Equatable {
    var surface: GlassSurface
    var clearStyle: Bool                 // glass style: clear (true) or regular (false); ignored for .opaque
    var gradientOpacity: Double          // graphite gradient under the controls (strongest at the bottom edge)
    var tintOpacity: Double              // graphite tint inside the glass
    var hairlineOpacity: Double          // white edge line
    var opaqueFill: GlassRGB             // fill used when surface == .opaque
    var primaryText: GlassRGB
    var secondaryText: GlassRGB

    /// Worst case: white icon/text over a pure WHITE video frame (modelled: glass ~ the frame, darkened by the gradient and the tint).
    func modelledBackgroundOverWhiteFrame() -> GlassRGB {
        if surface == .opaque { return opaqueFill }
        let darken = 1 - (1 - gradientOpacity) * (1 - tintOpacity)
        return GlassRGB.graphite.over(.white, alpha: darken)
    }
    func modelledBackgroundOverBlackFrame() -> GlassRGB {
        if surface == .opaque { return opaqueFill }
        let darken = 1 - (1 - gradientOpacity) * (1 - tintOpacity)
        return GlassRGB.graphite.over(.black, alpha: darken)
    }
    func primaryContrastOverWhiteFrame() -> Double { GlassRGB.contrast(primaryText, modelledBackgroundOverWhiteFrame()) }
    func primaryContrastOverBlackFrame() -> Double { GlassRGB.contrast(primaryText, modelledBackgroundOverBlackFrame()) }
    func secondaryContrastOverWhiteFrame() -> Double { GlassRGB.contrast(secondaryText, modelledBackgroundOverWhiteFrame()) }
    /// Edge line against the surface it sits on (opaque surfaces only: on glass the edge is a highlight, not a hairline).
    func hairlineContrast() -> Double { GlassRGB.contrast(GlassRGB.white.over(opaqueFill, alpha: hairlineOpacity), opaqueFill) }
}

struct GlassMotion: Equatable {
    var expandDuration: Double           // compact <-> expanded (approved 380 ms)
    var iconFadeDuration: Double, iconFadeDelay: Double, iconCollapseFadeDuration: Double
    var revealDuration: Double           // controls show / hide, hover
    var inspectorDuration: Double
    var animates: Bool { expandDuration > 0 }
}

struct GlassPolicy: Equatable {
    var treatment: GlassTreatment
    var motion: GlassMotion

    /// Starting-point values; measured over a pure white frame in the Motion Lab (white icons): clear + graphite tint 25 % + gradient 55 % = 6.2:1, the
    /// same with 20 % tint and 50 % gradient on regular glass = 4.67:1. Treat as provisional until the user approves the look.
    static func resolve(_ env: GlassEnvironment) -> GlassPolicy {
        var t = GlassTreatment(surface: .glass, clearStyle: true, gradientOpacity: 0.55, tintOpacity: 0.25, hairlineOpacity: 0x24 / 255.0,
                               opaqueFill: .surface, primaryText: .text, secondaryText: .secondaryText)
        if env.increaseContrast {                       // stronger scrim and edge, secondary text promoted to primary
            t.gradientOpacity = 0.70; t.tintOpacity = 0.35; t.hairlineOpacity = 0.50; t.secondaryText = .text
        }
        if env.reduceTransparency {                     // no glass: an opaque graphite surface (stays dark in Light mode too)
            t.surface = .opaque
        }
        if t.surface == .glass { t.secondaryText = .text }      // no dimmed text on glass: the video behind it is unknown
        let m: GlassMotion = env.reduceMotion
            ? GlassMotion(expandDuration: 0, iconFadeDuration: 0, iconFadeDelay: 0, iconCollapseFadeDuration: 0, revealDuration: 0, inspectorDuration: 0)
            : GlassMotion(expandDuration: 0.38, iconFadeDuration: 0.22, iconFadeDelay: 0.14, iconCollapseFadeDuration: 0.12, revealDuration: 0.20, inspectorDuration: 0.30)
        return GlassPolicy(treatment: t, motion: m)
    }
}
