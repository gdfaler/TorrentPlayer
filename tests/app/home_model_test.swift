// Offline tests for HomeModel: presentation per launcher state, offered actions, palette contrast in all appearance / Increase Contrast combinations,
// the persisted appearance preference and the motion timing. Build: swiftc -parse-as-library app/Sources/GlassMetrics.swift app/Sources/HomeModel.swift tests/app/home_model_test.swift
import Foundation

var failures = 0, checks = 0
func check(_ ok: Bool, _ what: @autoclosure () -> String) { checks += 1; if !ok { failures += 1; print("FAIL: \(what())") } }

@main struct HomeModelTest {
    static func main() {
        // idle: the empty state is the drop target and offers no stray actions
        let idle = HomePresentation.make(.idle)
        check(idle.mode == .empty && idle.actions.isEmpty && !idle.spinner, "idle")
        check(idle.title == HomePresentation.idleTitle && idle.detail == HomePresentation.idleDetail && !idle.accessibilityLabel.isEmpty, "idle text")

        // fetching: progress with Cancel, the named "No peers found" state after 15 s
        let f0 = HomePresentation.make(.fetching(fromMagnet: true, peers: 0, elapsed: 3))
        check(f0.mode == .progress && f0.spinner && f0.actions == [.cancel] && f0.title == "Looking for peers…", "fetch looking")
        check(HomePresentation.make(.fetching(fromMagnet: true, peers: 0, elapsed: 15)).title == "Looking for peers…", "15 s boundary is not yet no-peers")
        let np = HomePresentation.make(.fetching(fromMagnet: true, peers: 0, elapsed: 16))
        check(np.title == "No peers found" && np.detail.contains("16 s") && np.actions == [.cancel], "no peers")
        check(HomePresentation.make(.fetching(fromMagnet: true, peers: 1, elapsed: 20)).detail == "1 peer found (20 s)", "singular peer")
        check(HomePresentation.make(.fetching(fromMagnet: true, peers: 4, elapsed: 2)).detail == "4 peers found (2 s)", "plural peers")
        let rd = HomePresentation.make(.fetching(fromMagnet: false, peers: 0, elapsed: 100))
        check(rd.title == "Reading torrent…" && rd.detail.isEmpty, "torrent file never says no peers")

        // choosing
        let c1 = HomePresentation.make(.choosing(name: "Sintel", hasVideo: true))
        check(c1.mode == .list && c1.title == "Sintel" && c1.actions == [.play] && c1.detail == "Choose a video file and press Play.", "choosing video")
        let c2 = HomePresentation.make(.choosing(name: nil, hasVideo: false))
        check(c2.title == "Choose a file" && c2.actions.isEmpty && c2.detail.contains("No playable video"), "choosing without video")
        check(HomePresentation.make(.choosing(name: "", hasVideo: true)).title == "Choose a file", "empty name")

        // failures: every kind is actionable
        let m = HomePresentation.make(.failed(kind: .magnetSyntax, title: HomePresentation.magnetSyntaxTitle, message: HomePresentation.magnetSyntaxMessage, retryable: false))
        check(m.mode == .error && m.actions == [.editLink, .startOver] && m.accessibilityLabel.hasPrefix("Error."), "magnet syntax")
        let fl = HomePresentation.make(.failed(kind: .file, title: "Could not open this file", message: "The file does not exist.", retryable: false))
        check(fl.actions == [.openFile, .startOver], "file not retryable")
        check(HomePresentation.make(.failed(kind: .file, title: "t", message: "m", retryable: true)).actions == [.openFile, .tryAgain, .startOver], "file retryable")
        let tr = HomePresentation.make(.failed(kind: .torrent, title: "Could not open this torrent", message: "boom", retryable: true))
        check(tr.actions == [.tryAgain, .startOver] && tr.detail == "boom", "torrent retryable")
        check(HomePresentation.make(.failed(kind: .torrent, title: "t", message: "m", retryable: false)).actions == [.startOver], "torrent not retryable")
        for k in [HomeFailureKind.magnetSyntax, .file, .torrent] { for r in [true, false] {
            let p = HomePresentation.make(.failed(kind: k, title: "T", message: "M", retryable: r))
            check(!p.actions.isEmpty && p.actions.contains(.startOver) && Set(p.actions.map { $0.title }).count == p.actions.count, "error \(k) \(r) always has a way out, unique titles")
        } }
        check(HomePresentation.make(.playing).mode == .playing, "playing")
        for a in [HomeAction.cancel, .play, .tryAgain, .editLink, .openFile, .startOver] { check(!a.title.isEmpty, "action title") }

        // palette: text contrast in Light / Dark x Increase Contrast, against both the window and the card surface
        for dark in [true, false] { for ic in [false, true] {
            let p = HomePalette.resolve(dark: dark, increaseContrast: ic)
            for bg in [p.background, p.surface] {
                let t = GlassRGB.contrast(p.text, bg), s = GlassRGB.contrast(p.secondaryText, bg), d = GlassRGB.contrast(p.danger, bg), a = GlassRGB.contrast(p.accent, bg)
                check(t >= 7, "text \(dark) \(ic) \(t)")
                check(s >= 4.5, "secondary text \(dark) \(ic) \(s)")
                check(d >= 4.5, "danger text \(dark) \(ic) \(d)")
                check(a >= 3, "accent (non-text) \(dark) \(ic) \(a)")
                if ic { check(s >= 7, "Increase Contrast secondary text reaches AAA \(dark) \(s)"); check(GlassRGB.contrast(p.border, bg) >= 3, "IC border \(dark) \(GlassRGB.contrast(p.border, bg))") }
            }
            check(p.borderWidth == (ic ? 2 : 1), "border width")
        } }
        let dn = HomePalette.resolve(dark: true, increaseContrast: false), ln = HomePalette.resolve(dark: false, increaseContrast: false)
        check(dn.background == GlassRGB(hex: 0x101318) && dn.surface == GlassRGB(hex: 0x1B2028) && ln.background == GlassRGB(hex: 0xF4F5F7) && ln.surface == GlassRGB(hex: 0xFFFFFF), "spec tokens")
        check(HomePalette.resolve(dark: true, increaseContrast: true) != dn && HomePalette.resolve(dark: false, increaseContrast: true) != ln, "IC changes the palette")

        // appearance preference: System by default, persisted, unknown values fall back to System
        let suite = "tp.home.test.\(UUID().uuidString)"
        let d = UserDefaults(suiteName: suite)!
        check(HomeAppearance.load(d) == .system, "default is System")
        for a in HomeAppearance.allCases { a.save(d); check(HomeAppearance.load(d) == a, "round trip \(a)") }
        d.set("sepia", forKey: HomeAppearance.defaultsKey); check(HomeAppearance.load(d) == .system, "unknown value")
        d.removePersistentDomain(forName: suite)
        check(HomeAppearance.system.isDark(systemIsDark: true) && !HomeAppearance.system.isDark(systemIsDark: false), "system follows")
        check(!HomeAppearance.light.isDark(systemIsDark: true) && HomeAppearance.dark.isDark(systemIsDark: false), "forced")
        check(HomeAppearance.allCases.map { $0.title } == ["System", "Light", "Dark"], "titles / order")

        // motion
        check(HomeMotion.duration(HomeMotion.fadeOut, reduceMotion: true) == 0 && HomeMotion.duration(HomeMotion.fadeIn, reduceMotion: true) == 0, "Reduce Motion is instant")
        check(HomeMotion.fadeOut >= 0.3 && HomeMotion.fadeOut <= 0.5 && HomeMotion.duration(HomeMotion.fadeOut, reduceMotion: false) == HomeMotion.fadeOut, "fade-out within the design guideline")

        print("home_model_test: \(checks) checks, \(failures) failures")
        exit(failures == 0 ? 0 : 1)
    }
}
