// Offline test for GlassMetrics (layout, accessibility and readability policy). No UI, no mpv, no network.
// Build and run:  swiftc -parse-as-library -swift-version 5 -target arm64-apple-macosx27.0 app/Sources/GlassMetrics.swift tests/app/glass_metrics_test.swift -o dist/glass_metrics_test && dist/glass_metrics_test
import Foundation
import CoreGraphics

@main struct GlassMetricsTest {
    static var failures = 0, checks = 0
    static func check(_ c: Bool, _ what: String, line: Int = #line) { checks += 1; if !c { failures += 1; print("FAILED (line \(line)): \(what)") } }
    static func near(_ a: CGFloat, _ b: CGFloat, _ tol: CGFloat = 0.01) -> Bool { abs(a - b) <= tol }
    typealias M = GlassMetrics

    /// Invariants that must hold for every layout.
    static func invariants(_ l: GlassLayout, _ content: CGSize, fullscreen: Bool, expandedAsked: Bool, inspectorAsked: Bool, _ tag: String) {
        let bottom = fullscreen ? M.bottomMarginFullscreen : M.bottomMargin
        check(l.frames[.play] != nil && l.frames[.expand] != nil, "\(tag): play and expand always present")
        // bounds and margins
        for (c, f) in l.frames {
            check(f.minX >= M.sideMargin - 0.01 && f.maxX <= content.width - M.sideMargin + 0.01, "\(tag): \(c) inside the side margins")
            check(f.minY >= bottom - 0.01 && f.maxY <= content.height, "\(tag): \(c) above the bottom margin and inside the window")
        }
        // no overlap and a minimum gap between neighbours
        let items = l.frames.sorted { $0.value.minX < $1.value.minX }
        for i in 1..<max(1, items.count) { check(items[i].value.minX - items[i - 1].value.maxX >= M.compactGap - 0.01, "\(tag): gap between \(items[i - 1].key) and \(items[i].key)") }
        if l.expanded { for i in 1..<items.count { check(items[i].value.minX - items[i - 1].value.maxX >= M.buttonGap - 0.01, "\(tag): expanded gap \(items[i - 1].key)/\(items[i].key) >= button gap") } }
        // centred, one centre line, toolbar = union
        check(near(l.toolbar.midX, content.width / 2, 0.5), "\(tag): toolbar centred (\(l.toolbar.midX))")
        let centres = Set(l.frames.values.map { ($0.midY * 100).rounded() })
        check(centres.count == 1, "\(tag): one common centre line")
        check(near(l.toolbar.minY, bottom, 0.01), "\(tag): bottom margin")
        // effective state
        if !expandedAsked { check(!l.expanded && l.frames.count == 2, "\(tag): compact when not asked") }
        check(!l.expanded == (l.frames[.seek] == nil), "\(tag): seek present exactly when expanded")
        // inspector
        if let i = l.inspector {
            check(inspectorAsked, "\(tag): inspector only when asked")
            check(i.maxX <= content.width - M.rightMargin + 0.01 && i.minX >= M.sideMargin, "\(tag): inspector horizontal margins")
            check(i.maxY <= content.height - (fullscreen ? M.topMarginFullscreen : M.topMargin) + 0.01, "\(tag): inspector top margin")
            check(i.minY - l.toolbar.maxY >= M.inspectorGap - 0.01, "\(tag): inspector clear of the toolbar by the gap (\(i.minY - l.toolbar.maxY))")
            check(!i.intersects(l.toolbar.insetBy(dx: -1, dy: -1)), "\(tag): inspector does not overlap the toolbar")
            check(M.inspectorWidthRange.contains(i.width) || i.width == content.width - 2 * M.sideMargin, "\(tag): inspector width in range")
            check(i.height >= M.inspectorHeightMin - 0.01 && i.height <= M.inspectorHeightMax + 0.01, "\(tag): inspector height in range")
        } else if inspectorAsked {
            let room = content.height - (fullscreen ? M.topMarginFullscreen : M.topMargin) - (l.toolbar.maxY + M.inspectorGap)
            check(room < M.inspectorHeightMin, "\(tag): inspector omitted only for lack of height (room \(room))")
        }
        check(l.gradientHeight > 0 && l.gradientHeight <= M.gradientHeightMax && l.gradientHeight <= content.height * 0.5, "\(tag): gradient height bounded")
    }

    static func main() {
        // 1. every combination on the sizes that matter (windowed 800x480, 1000x600, a small one, fullscreen on two displays)
        let sizes: [(CGSize, Bool, String)] = [(CGSize(width: 800, height: 480), false, "800x480"), (CGSize(width: 1000, height: 600), false, "1000x600"), (CGSize(width: 640, height: 400), false, "640x400"),
                                              (CGSize(width: 1470, height: 956), true, "fs1470x956"), (CGSize(width: 2560, height: 1440), true, "fs2560x1440"), (CGSize(width: 1000, height: 600), true, "fs1000x600")]
        for (size, fs, name) in sizes { for ex in [false, true] { for ins in [false, true] {
            invariants(GlassLayout.make(content: size, fullscreen: fs, expanded: ex, inspectorOpen: ins), size, fullscreen: fs, expandedAsked: ex, inspectorAsked: ins, "\(name) ex=\(ex) ins=\(ins)")
        } } }

        // 2. exact numbers, 800x480 and 1000x600 windowed
        let a = GlassLayout.make(content: CGSize(width: 800, height: 480), fullscreen: false, expanded: true, inspectorOpen: true)
        check(a.expanded && a.hidden.isEmpty && a.frames.count == 8, "800x480 expanded shows all 8 controls")
        check(near(a.frames[.seek]!.width, 280) && near(a.frames[.seek]!.height, 48), "800x480 seek 280x48")
        check(near(a.frames[.play]!.width, 52) && near(a.frames[.volume]!.width, 44) && near(a.frames[.expand]!.width, 36), "control sizes")
        check(near(a.toolbar.width, 732) && near(a.toolbar.minX, 34) && near(a.toolbar.maxX, 766), "800x480 toolbar 732 wide, 34 pt margins")
        check(a.inspector == CGRect(x: 520, y: 96, width: 256, height: 360), "800x480 inspector frame \(String(describing: a.inspector))")
        check(near(a.gradientHeight, 216), "800x480 gradient 216")
        let b = GlassLayout.make(content: CGSize(width: 1000, height: 600), fullscreen: false, expanded: true, inspectorOpen: true)
        check(near(b.frames[.seek]!.width, 280) && b.hidden.isEmpty, "1000x600 seek 280")
        check(b.inspector == CGRect(x: 656, y: 96, width: 320, height: 380), "1000x600 inspector frame \(String(describing: b.inspector))")
        // compact: play and chevron, centred
        let c = GlassLayout.make(content: CGSize(width: 800, height: 480), fullscreen: false, expanded: false, inspectorOpen: false)
        check(c.frames.count == 2 && near(c.toolbar.width, 96) && near(c.frames[.expand]!.minX - c.frames[.play]!.maxX, M.compactGap), "compact toolbar: play + chevron, 8 pt apart")
        check(near(c.frames[.play]!.minY, a.frames[.play]!.minY), "play stays on the same line in compact and expanded")
        // the same transport circle in every state keeps its place vertically: expanded vs compact
        check(c.inspector == nil, "no inspector when closed")

        // 3. fullscreen inputs: larger bottom/top margins, seek grows with the width up to its cap
        let f1 = GlassLayout.make(content: CGSize(width: 1470, height: 956), fullscreen: true, expanded: true, inspectorOpen: true)
        check(near(f1.toolbar.minY, M.bottomMarginFullscreen), "fullscreen bottom margin")
        check(near(f1.frames[.seek]!.width, 1470 * 0.28, 0.01) && f1.frames[.seek]!.width <= 420, "fullscreen 1470 seek width follows the fraction")
        check(f1.inspector != nil && near(f1.inspector!.minY, 96 + M.inspectorGap) && near(f1.inspector!.maxY, 96 + M.inspectorGap + 380) && f1.inspector!.maxY <= 956 - M.topMarginFullscreen, "fullscreen inspector: height capped at 380 above the toolbar, below the 48 pt top margin")
        let f2 = GlassLayout.make(content: CGSize(width: 2560, height: 1440), fullscreen: true, expanded: true, inspectorOpen: true)
        check(near(f2.frames[.seek]!.width, 420), "fullscreen 2560 seek capped at 420")
        check(near(f2.inspector!.width, 320) && near(f2.inspector!.height, 380), "fullscreen 2560 inspector 320x380")
        check(near(f2.gradientHeight, 300), "gradient capped at 300")

        // 4. narrow windows: seek shrinks, then volume is dropped, then the toolbar falls back to compact
        let n1 = GlassLayout.make(content: CGSize(width: 640, height: 400), fullscreen: false, expanded: true, inspectorOpen: false)
        check(n1.expanded && n1.hidden.isEmpty && near(n1.frames[.seek]!.width, 156), "640 wide: seek shrinks to 156 (\(n1.frames[.seek]!.width))")
        let n2 = GlassLayout.make(content: CGSize(width: 580, height: 400), fullscreen: false, expanded: true, inspectorOpen: false)
        check(n2.expanded && n2.hidden == [.volume] && n2.frames[.volume] == nil && near(n2.frames[.seek]!.width, 154), "580 wide: volume dropped, seek 154")
        let n3 = GlassLayout.make(content: CGSize(width: 540, height: 400), fullscreen: false, expanded: true, inspectorOpen: false)
        check(!n3.expanded && !n3.canExpand && n3.frames.count == 2, "540 wide: falls back to compact although expanded was asked")
        for w in stride(from: 540.0, through: 1400.0, by: 10) {      // every width: bounds respected, seek never below its minimum
            let l = GlassLayout.make(content: CGSize(width: w, height: 500), fullscreen: false, expanded: true, inspectorOpen: true)
            invariants(l, CGSize(width: w, height: 500), fullscreen: false, expandedAsked: true, inspectorAsked: true, "w=\(w)")
            if let s = l.frames[.seek] { check(s.width >= M.seekWidthMin - 0.01 && s.width <= M.seekWidthRange.upperBound + 0.01, "w=\(w): seek width bounded") }
        }
        // short windows: the inspector disappears instead of overlapping the toolbar
        let s1 = GlassLayout.make(content: CGSize(width: 1000, height: 300), fullscreen: false, expanded: true, inspectorOpen: true)
        check(s1.inspector != nil && near(s1.inspector!.height, 180), "300 high: inspector squeezed to the available 180")
        let s2 = GlassLayout.make(content: CGSize(width: 1000, height: 250), fullscreen: false, expanded: true, inspectorOpen: true)
        check(s2.inspector == nil, "250 high: no inspector (not enough room)")
        for h in stride(from: 250.0, through: 1000.0, by: 25) {
            let l = GlassLayout.make(content: CGSize(width: 900, height: h), fullscreen: false, expanded: true, inspectorOpen: true)
            invariants(l, CGSize(width: 900, height: h), fullscreen: false, expandedAsked: true, inspectorAsked: true, "h=\(h)")
        }

        // 4b. canExpand / the compact fallback is never a dead end: the threshold is explicit and the chevron can be hidden by the integration
        let th = GlassLayout.minimumExpandedContentWidth
        check(near(th, 566), "minimum expanded content width is 566 (\(th))")
        for ask in [false, true] {
            check(GlassLayout.make(content: CGSize(width: th - 0.5, height: 500), fullscreen: false, expanded: ask, inspectorOpen: false).canExpand == false, "just below the threshold: canExpand false (asked \(ask))")
            check(GlassLayout.make(content: CGSize(width: th, height: 500), fullscreen: false, expanded: ask, inspectorOpen: false).canExpand, "at the threshold: canExpand true (asked \(ask))")
        }
        let atTh = GlassLayout.make(content: CGSize(width: th, height: 500), fullscreen: false, expanded: true, inspectorOpen: false)
        check(atTh.expanded && near(atTh.frames[.seek]!.width, M.seekWidthMin), "at the threshold the seek bar is at its minimum")
        let belowTh = GlassLayout.make(content: CGSize(width: th - 0.5, height: 500), fullscreen: false, expanded: true, inspectorOpen: false)
        check(!belowTh.expanded && !belowTh.canExpand && belowTh.frames.count == 2 && belowTh.frames[.play] != nil, "below the threshold: compact with play/pause, canExpand false")
        check(GlassLayout.make(content: CGSize(width: 800, height: 480), fullscreen: false, expanded: false, inspectorOpen: false).canExpand, "800 wide compact: can expand")
        for w in stride(from: 160.0, through: 1600.0, by: 7) {
            let l = GlassLayout.make(content: CGSize(width: w, height: 500), fullscreen: false, expanded: true, inspectorOpen: false)
            check(l.canExpand == l.expanded, "w=\(w): when expansion was asked, canExpand matches the effective state")
            check(l.frames[.play] != nil, "w=\(w): play/pause is always present")
        }

        // 4c. production variant without the inspector button (Milestone 2A): same invariants, narrower thresholds
        let thNo = GlassLayout.minimumExpandedContentWidth(includesInspector: false)
        check(near(thNo, 508), "without the inspector button the expanded toolbar needs 508 pt (\(thNo))")
        for (size, fs, name) in sizes { for ex in [false, true] {
            let l = GlassLayout.make(content: size, fullscreen: fs, expanded: ex, inspectorOpen: false, includesInspector: false)
            invariants(l, size, fullscreen: fs, expandedAsked: ex, inspectorAsked: false, "noinsp \(name) ex=\(ex)")
            check(l.frames[.inspector] == nil, "noinsp \(name): no inspector button")
            if ex { check(l.expanded && l.hidden.isEmpty && l.frames.count == 7, "noinsp \(name): back10 play fwd10 seek volume fullscreen expand") }
        } }
        let ni = GlassLayout.make(content: CGSize(width: 800, height: 480), fullscreen: false, expanded: true, inspectorOpen: false, includesInspector: false)
        check(near(ni.toolbar.width, 732 - 58) && near(ni.frames[.seek]!.width, 280), "800 wide, no inspector: toolbar 674 wide, seek 280 (\(ni.toolbar.width))")
        let nn = GlassLayout.make(content: CGSize(width: 520, height: 400), fullscreen: false, expanded: true, inspectorOpen: false, includesInspector: false)
        check(nn.expanded && nn.hidden == [.volume] && nn.frames[.fullscreen] != nil, "520 wide, no inspector: volume dropped, fullscreen kept")
        let nb = GlassLayout.make(content: CGSize(width: thNo - 0.5, height: 400), fullscreen: false, expanded: true, inspectorOpen: false, includesInspector: false)
        check(!nb.expanded && !nb.canExpand, "just below 508: compact, canExpand false")

        // 4d. the hit rule that fixed the Play -> fullscreen bug
        check(!GlassHitRule.accepts(isHidden: true, contentAlpha: 1) && !GlassHitRule.accepts(isHidden: false, contentAlpha: 0) && !GlassHitRule.accepts(isHidden: false, contentAlpha: 0.49), "hidden or faded-out surfaces take no mouse events")
        check(GlassHitRule.accepts(isHidden: false, contentAlpha: 0.5) && GlassHitRule.accepts(isHidden: false, contentAlpha: 1), "visible surfaces take mouse events")

        // 5. accessibility: Reduce Motion
        let base = GlassPolicy.resolve(GlassEnvironment())
        check(base.motion.animates && near(CGFloat(base.motion.expandDuration), 0.38, 0.0001), "default: the approved 380 ms expansion")
        check(base.motion.revealDuration >= 0.15 && base.motion.revealDuration <= 0.25, "reveal/hover within 150-250 ms")
        check(base.motion.inspectorDuration >= 0.25 && base.motion.inspectorDuration <= 0.35, "inspector within 250-350 ms")
        let rm = GlassPolicy.resolve(GlassEnvironment(reduceMotion: true))
        check(!rm.motion.animates && rm.motion.expandDuration == 0 && rm.motion.iconFadeDuration == 0 && rm.motion.iconFadeDelay == 0 && rm.motion.iconCollapseFadeDuration == 0 && rm.motion.revealDuration == 0 && rm.motion.inspectorDuration == 0, "Reduce Motion: every duration 0")
        check(rm.treatment == base.treatment, "Reduce Motion does not change the surface treatment")

        // 6. accessibility: Reduce Transparency -> opaque fallback; Increase Contrast -> stronger
        check(base.treatment.surface == .glass, "default surface is glass")
        let rt = GlassPolicy.resolve(GlassEnvironment(reduceTransparency: true))
        check(rt.treatment.surface == .opaque && rt.treatment.opaqueFill == .surface, "Reduce Transparency: opaque graphite surface")
        check(rt.motion == base.motion, "Reduce Transparency does not change motion")
        check(rt.treatment.primaryContrastOverWhiteFrame() >= 4.5 && rt.treatment.primaryContrastOverBlackFrame() >= 4.5, "opaque: primary text >= 4.5:1 on any frame (\(rt.treatment.primaryContrastOverWhiteFrame()))")
        check(rt.treatment.secondaryContrastOverWhiteFrame() >= 4.5, "opaque: secondary text >= 4.5:1 (\(rt.treatment.secondaryContrastOverWhiteFrame()))")
        check(rt.treatment.hairlineContrast() >= 1.3, "opaque: hairline >= 1.3:1 (\(rt.treatment.hairlineContrast()))")
        let ic = GlassPolicy.resolve(GlassEnvironment(increaseContrast: true))
        check(ic.treatment.surface == .glass && ic.treatment.gradientOpacity > base.treatment.gradientOpacity && ic.treatment.tintOpacity > base.treatment.tintOpacity && ic.treatment.hairlineOpacity > base.treatment.hairlineOpacity, "Increase Contrast: stronger scrim, tint and edge")
        check(ic.treatment.primaryContrastOverWhiteFrame() > base.treatment.primaryContrastOverWhiteFrame(), "Increase Contrast raises the contrast")
        let both = GlassPolicy.resolve(GlassEnvironment(reduceMotion: true, reduceTransparency: true, increaseContrast: true))
        check(both.treatment.surface == .opaque && both.treatment.secondaryText == .text && both.treatment.hairlineContrast() > rt.treatment.hairlineContrast() && !both.motion.animates, "all three together")

        // 7. readability: the default is native regular glass, no tint, gradient 10 % (provisional). KNOWN limitation: BELOW 4.5:1 over bright video (modelled ~1.8:1 over a pure white frame);
        // Increase Contrast and the opaque surface reach >= 4.5:1.
        let def = GlassPolicy.resolve(GlassEnvironment()).treatment
        check(!def.clearStyle && def.tintOpacity == 0 && def.gradientOpacity == 0.10 && def.surface == .glass, "default treatment = regular glass, no tint, gradient 10 %")
        check(def.primaryContrastOverWhiteFrame() < 4.5, "default: contrast over a pure white frame is below 4.5:1 (documented limitation; modelled \(def.primaryContrastOverWhiteFrame()))")
        check(def.primaryContrastOverBlackFrame() >= 4.5, "default: over a black frame >= 4.5:1 (\(def.primaryContrastOverBlackFrame()))")
        for (name, env) in [("increase contrast", GlassEnvironment(increaseContrast: true)), ("reduce transparency", GlassEnvironment(reduceTransparency: true)), ("IC+RT", GlassEnvironment(reduceTransparency: true, increaseContrast: true))] {
            let t = GlassPolicy.resolve(env).treatment
            check(t.primaryContrastOverWhiteFrame() >= 4.5, "\(name): primary text over a white frame >= 4.5:1 (\(t.primaryContrastOverWhiteFrame()))")
            check(t.secondaryContrastOverWhiteFrame() >= 4.5, "\(name): secondary text over a white frame >= 4.5:1 (\(t.secondaryContrastOverWhiteFrame()))")
            check(t.primaryContrastOverBlackFrame() >= 4.5, "\(name): primary text over a black frame >= 4.5:1")
        }
        for env in [GlassEnvironment(), GlassEnvironment(increaseContrast: true), GlassEnvironment(reduceMotion: true)] {
            let t = GlassPolicy.resolve(env).treatment
            check(t.gradientOpacity <= 0.9 && t.tintOpacity <= 0.5, "the scrim does not hide the video")
            check(GlassPolicy.resolve(env).treatment.secondaryText == .text, "no dimmed text on glass")
        }
        check(GlassRGB.contrast(.white, .black) > 20.9 && GlassRGB.contrast(.white, .white) < 1.01, "contrast helper sanity")
        check(near(CGFloat(GlassRGB(hex: 0x1B2028).r * 255), 27, 0.01), "hex parsing")

        // 8. symbols and metrics sanity
        check(GlassSymbol.playPause(paused: true) == "play.fill" && GlassSymbol.playPause(paused: false) == "pause.fill" && GlassSymbol.mute(muted: true) == "speaker.slash.fill" && GlassSymbol.fullscreen(isFullscreen: true) == GlassSymbol.exitFullscreen, "symbol helpers")
        check(Set(GlassControl.allCases.map { $0.accessibilityLabel }).count == GlassControl.allCases.count, "accessibility labels are unique")
        check(near(M.cornerRadius(for: CGSize(width: 52, height: 52)), 26) && near(M.cornerRadius(for: CGSize(width: 280, height: 48)), 24), "circle and capsule radii")
        check(M.size(of: .seek).height == M.seekHeight && M.size(of: .play).width == M.transportSize, "size lookup")
        check(M.timeFont.monospacedDigits && M.inspectorValueFont.monospacedDigits && !M.inspectorLabelFont.monospacedDigits, "type: tabular digits for times and values")

        print("glass_metrics_test: \(checks) checks, \(failures) failures")
        exit(failures == 0 ? 0 : 1)
    }
}
