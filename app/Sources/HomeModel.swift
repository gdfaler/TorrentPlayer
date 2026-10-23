// HomeModel: the pure (Foundation-only) logic behind the home screen: what each launcher state shows, which actions it offers, the Titanium palette per appearance /
// Increase Contrast, the persisted appearance preference and the transition timing. No AppKit: unit-tested on its own (tests/app/home_model_test.swift).
import Foundation

// MARK: appearance preference
enum HomeAppearance: String, CaseIterable {
    case system, light, dark
    static let defaultsKey = "homeAppearance"
    var title: String { switch self { case .system: return "System"; case .light: return "Light"; case .dark: return "Dark" } }

    /// Reads the stored choice; anything missing or unknown means System (the default).
    static func load(_ d: UserDefaults = .standard) -> HomeAppearance { d.string(forKey: defaultsKey).flatMap(HomeAppearance.init(rawValue:)) ?? .system }
    func save(_ d: UserDefaults = .standard) { d.set(rawValue, forKey: Self.defaultsKey) }
    /// Whether the dark palette applies, given what the system currently uses.
    func isDark(systemIsDark: Bool) -> Bool { switch self { case .system: return systemIsDark; case .light: return false; case .dark: return true } }
}

// MARK: palette (Titanium tokens; values are the specification's initial tokens)
struct HomePalette: Equatable {
    var background: GlassRGB, surface: GlassRGB, border: GlassRGB
    var text: GlassRGB, secondaryText: GlassRGB, accent: GlassRGB, danger: GlassRGB
    var borderWidth: Double

    static func resolve(dark: Bool, increaseContrast: Bool) -> HomePalette {
        if dark {
            return HomePalette(background: GlassRGB(hex: 0x101318), surface: GlassRGB(hex: 0x1B2028),
                               border: increaseContrast ? GlassRGB(hex: 0xA5AFBE) : GlassRGB(hex: 0x3A4452),
                               text: GlassRGB(hex: 0xF0F3F8), secondaryText: increaseContrast ? GlassRGB(hex: 0xD3DAE5) : GlassRGB(hex: 0xA5AFBE),
                               accent: GlassRGB(hex: 0xB7C4D8), danger: GlassRGB(hex: 0xFF9C94), borderWidth: increaseContrast ? 2 : 1)
        }
        return HomePalette(background: GlassRGB(hex: 0xF4F5F7), surface: GlassRGB(hex: 0xFFFFFF),
                           border: increaseContrast ? GlassRGB(hex: 0x4A5568) : GlassRGB(hex: 0xC9CFD8),
                           text: GlassRGB(hex: 0x202630), secondaryText: increaseContrast ? GlassRGB(hex: 0x3E485A) : GlassRGB(hex: 0x5A6678),
                           accent: GlassRGB(hex: 0x53657D), danger: GlassRGB(hex: 0xB3261E), borderWidth: increaseContrast ? 2 : 1)
    }
}

// MARK: motion
enum HomeMotion {
    static let fadeOut = 0.35                    // home -> playback: design guideline 350-500 ms, kept at the restrained end
    static let fadeIn = 0.25                     // playback -> home
    static let hover = 0.12
    static func duration(_ base: Double, reduceMotion: Bool) -> Double { reduceMotion ? 0 : base }
}

// MARK: what the launcher shows
enum HomeFailureKind: Equatable {
    case magnetSyntax              // text that is not a magnet link
    case file                      // a local file that cannot be opened (missing, unreadable, unsupported)
    case torrent                   // the engine or the metadata fetch failed
}

enum HomeAction: Equatable {
    case cancel, play, tryAgain, editLink, openFile, startOver
    var title: String {
        switch self {
        case .cancel: return "Cancel"
        case .play: return "Play"
        case .tryAgain: return "Try Again"
        case .editLink: return "Edit Link"
        case .openFile: return "Choose Another File…"
        case .startOver: return "Start Over"
        }
    }
}

/// The launcher phases as the presentation sees them (the controller maps its own `Phase` onto this).
enum HomeState: Equatable {
    case idle
    case fetching(fromMagnet: Bool, peers: Int, elapsed: Double)
    case choosing(name: String?, hasVideo: Bool)
    case playing
    case failed(kind: HomeFailureKind, title: String, message: String, retryable: Bool)
}

enum HomeMode: Equatable { case empty, progress, list, error, playing }

struct HomePresentation: Equatable {
    var mode: HomeMode
    var title: String
    var detail: String
    var symbol: String                // SF Symbol shown on the card
    var spinner: Bool
    var actions: [HomeAction]         // the first is the primary action
    var accessibilityLabel: String

    static let idleTitle = "Open a video or a torrent to start"
    static let idleDetail = "Local videos play straight from disk. Torrents stream: nothing is downloaded to your Downloads folder, the cache is bounded and reclaimed automatically."
    static let noPeersAfter = 15.0

    static func make(_ s: HomeState) -> HomePresentation {
        switch s {
        case .idle:
            return HomePresentation(mode: .empty, title: idleTitle, detail: idleDetail, symbol: "play.rectangle", spinner: false, actions: [], accessibilityLabel: "Drop zone. Drop a video, a torrent file or a magnet link here.")
        case .fetching(let fromMagnet, let peers, let elapsed):
            var title = "Reading torrent…", detail = ""
            if fromMagnet {
                if peers == 0 && elapsed > noPeersAfter { title = "No peers found"; detail = "Still searching for peers that have this torrent's metadata (\(Int(elapsed)) s). The link may be dead or you may be offline." }
                else if peers == 0 { title = "Looking for peers…"; detail = "Fetching torrent metadata from the swarm" }
                else { title = "Fetching torrent metadata…"; detail = "\(peers) peer\(peers == 1 ? "" : "s") found (\(Int(elapsed)) s)" }
            }
            return HomePresentation(mode: .progress, title: title, detail: detail, symbol: "antenna.radiowaves.left.and.right", spinner: true, actions: [.cancel], accessibilityLabel: detail.isEmpty ? title : title + ". " + detail)
        case .choosing(let name, let hasVideo):
            let title = (name?.isEmpty == false ? name! : "Choose a file")
            let detail = hasVideo ? "Choose a video file and press Play." : "No playable video found in this torrent. The files are listed below."
            return HomePresentation(mode: .list, title: title, detail: detail, symbol: "film.stack", spinner: false, actions: hasVideo ? [.play] : [], accessibilityLabel: title + ". " + detail)
        case .playing:
            return HomePresentation(mode: .playing, title: "Playing", detail: "", symbol: "play.fill", spinner: false, actions: [], accessibilityLabel: "Playing")
        case .failed(let kind, let title, let message, let retryable):
            var actions: [HomeAction]
            switch kind {
            case .magnetSyntax: actions = [.editLink, .startOver]
            case .file: actions = [.openFile, .startOver]
            case .torrent: actions = retryable ? [.tryAgain, .startOver] : [.startOver]
            }
            if kind == .file && retryable { actions.insert(.tryAgain, at: 1) }
            return HomePresentation(mode: .error, title: title, detail: message, symbol: "exclamationmark.triangle", spinner: false, actions: actions, accessibilityLabel: "Error. " + title + ". " + message)
        }
    }

    static let magnetSyntaxTitle = "That is not a magnet link"
    static let magnetSyntaxMessage = "A magnet link starts with “magnet:?”. Copy it again from its source, or choose a file instead."
}
