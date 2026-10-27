// DownloadModel.swift: the pure (Foundation-only) presentation logic of Download & Play: what a download row says, which buttons it offers and when a finished download starts playing by itself.
// No AppKit, no C core: tests/app/download_model_test.swift compiles this file on its own.
import Foundation

enum DownloadState: Int {
    case checking = 0, downloading, waitingNetwork, waitingSources, paused, resuming, completed, failed
}

struct DownloadItem: Equatable {
    var id = ""
    var name = ""
    var savePath = ""
    var primaryFile = ""
    var error = ""
    var state: DownloadState = .checking
    var selectedBytes: Int64 = 0
    var verifiedBytes: Int64 = 0
    var downloadRate = 0
    var uploadRate = 0
    var peers = 0
    var webSeeds = 0
    var etaSeconds: Int64 = -1              // -1: unknown (never invented)
    var userPaused = false
    var autoPlay = true
    var checkingProgress = 0.0
    var trackersTotal = 0, trackersOK = 0, trackersFailing = 0, connecting = 0
    var completed: Bool { state == .completed }
    /// Verified fraction, never above 1 and never a rounded-up 100% before the download is really complete.
    var fraction: Double {
        if state == .completed { return 1 }
        guard selectedBytes > 0 else { return 0 }
        return min(Double(verifiedBytes) / Double(selectedBytes), 0.9999)
    }
}

enum DownloadFormat {
    /// What peer discovery has achieved so far, from facts only ("0 of 10 trackers answered, 10 failing").
    static func discovery(_ d: DownloadItem) -> String {
        if d.trackersTotal == 0 { return "This torrent lists no trackers; only DHT and local discovery can find peers." }
        var s = "\(d.trackersOK) of \(d.trackersTotal) trackers answered"
        if d.trackersFailing > 0 { s += ", \(d.trackersFailing) failing" }
        if d.connecting > 0 { s += "; \(d.connecting) connection attempt\(d.connecting == 1 ? "" : "s") in progress" }
        return s + "."
    }
    static func bytes(_ n: Int64) -> String { ByteCountFormatter.string(fromByteCount: max(0, n), countStyle: .file) }
    static func percent(_ f: Double) -> String { "\(Int((min(max(f, 0), 0.9999) * 100).rounded(.down)))%" }
    /// "about 5 min left"; nil when there is no meaningful estimate.
    static func remaining(_ s: Int64) -> String? {
        guard s >= 0 else { return nil }
        if s < 60 { return "less than a minute left" }
        if s < 3600 { return "about \(Int((Double(s) / 60).rounded())) min left" }
        let h = s / 3600, m = (s % 3600) / 60
        return m == 0 ? "about \(h) h left" : "about \(h) h \(m) min left"
    }
}

struct DownloadRowModel: Equatable {
    var title: String
    var detail: String
    var fraction: Double
    var indeterminate: Bool
    var canPause: Bool
    var canResume: Bool
    var canPlay: Bool
    var canReveal: Bool
    var cancelTitle: String

    static func make(_ d: DownloadItem) -> DownloadRowModel {
        let amount = "\(DownloadFormat.bytes(d.verifiedBytes)) of \(DownloadFormat.bytes(d.selectedBytes)) verified"
        var detail: String
        var indeterminate = false
        switch d.state {
        case .checking:
            detail = "Checking existing files… \(DownloadFormat.percent(d.checkingProgress))"; indeterminate = d.checkingProgress <= 0
        case .downloading:
            var parts = ["Downloading", DownloadFormat.percent(d.fraction), amount]
            parts.append(d.downloadRate > 0 ? DownloadFormat.bytes(Int64(d.downloadRate)) + "/s" : "no data yet")
            if let r = DownloadFormat.remaining(d.etaSeconds) { parts.append(r) }
            let sources = d.peers + d.webSeeds
            parts.append(sources == 1 ? "1 source" : "\(sources) sources")
            detail = parts.joined(separator: " · ")
        case .waitingNetwork:
            detail = "Waiting for network · \(DownloadFormat.percent(d.fraction)) · \(amount). It continues by itself when the connection returns."
        case .waitingSources:
            detail = "Waiting for sources · \(DownloadFormat.percent(d.fraction)) · \(amount). No peer or web seed is connected; TorrentPlayer keeps looking. " + DownloadFormat.discovery(d)
        case .paused:
            detail = "Paused · \(DownloadFormat.percent(d.fraction)) · \(amount)"
        case .resuming:
            detail = "Resuming… \(DownloadFormat.percent(d.fraction)) · \(amount)"; indeterminate = d.verifiedBytes == 0
        case .completed:
            detail = "Completed · \(DownloadFormat.bytes(d.selectedBytes)) verified · saved in \((d.savePath as NSString).abbreviatingWithTildeInPath)"
        case .failed:
            detail = "Failed · \(d.error.isEmpty ? "unknown error" : d.error)"
        }
        let active: Set<DownloadState> = [.checking, .downloading, .waitingNetwork, .waitingSources, .resuming]
        return DownloadRowModel(title: d.name.isEmpty ? "Download" : d.name, detail: detail, fraction: d.fraction, indeterminate: indeterminate,
                                canPause: active.contains(d.state), canResume: d.state == .paused || d.state == .failed, canPlay: d.state == .completed && !d.primaryFile.isEmpty,
                                canReveal: !d.savePath.isEmpty, cancelTitle: d.state == .completed ? "Remove" : "Cancel")
    }
}

/// What a click means, decided from the CURRENT list at click time (never from a snapshot captured when the row was built).
enum DownloadClick {
    enum PauseResume: Equatable { case pause(String), resume(String), ignore }
    static func item(_ id: String, in items: [DownloadItem]) -> DownloadItem? { items.first { $0.id == id } }
    static func pauseResume(id: String, in items: [DownloadItem]) -> PauseResume {
        guard let d = item(id, in: items) else { return .ignore }                  // the row's download is gone
        let m = DownloadRowModel.make(d)
        return m.canResume ? .resume(id) : (m.canPause ? .pause(id) : .ignore)
    }
    static func playPath(id: String, in items: [DownloadItem]) -> String? {
        guard let d = item(id, in: items), d.state == .completed, !d.primaryFile.isEmpty else { return nil }
        return d.primaryFile
    }
    /// The file to select in Finder (nil: open the folder instead) and the folder; nil when the download no longer exists.
    static func revealTarget(id: String, in items: [DownloadItem]) -> (file: String?, folder: String)? {
        guard let d = item(id, in: items) else { return nil }
        return (d.primaryFile.isEmpty ? nil : d.primaryFile, d.savePath)
    }
    enum CancelPrompt: Equatable { case gone, completed(name: String), partial(name: String, verified: Int64) }
    static func cancelPrompt(id: String, in items: [DownloadItem]) -> CancelPrompt {
        guard let d = item(id, in: items) else { return .gone }
        return d.state == .completed ? .completed(name: d.name) : .partial(name: d.name, verified: d.verifiedBytes)
    }
}

/// What the operating system tells us about the network, kept apart from what the downloader can reach. A usable path is NOT proof that the internet or any
/// torrent source is reachable (a VPN or tunnel interface stays up when Wi-Fi is switched off, and UDP or BitTorrent traffic may be blocked on a working path).
struct NetworkPathSummary: Equatable {
    enum Status: Equatable { case unknown, unavailable, requiresConnection, available }
    var status = Status.unknown
    var physical: [String] = []             // "Wi-Fi", "Ethernet", "Cellular" interfaces that are available (informational only)
    var tunnels = 0                         // VPN / tunnel / other interfaces (informational only)
    var expensive = false, constrained = false

    /// What the downloader is told: nil while the path is unknown or only "requires a connection" (no pause or resume decision is taken), false only when the system
    /// reports no usable path, true for every satisfied path. Which interfaces are listed never changes this (a VPN keeps the path satisfied with Wi-Fi off), and
    /// neither does it say anything about whether trackers or peers can be reached: that is judged per download from real announce and connection results.
    var downloaderOnline: Bool? {
        switch status {
        case .unknown, .requiresConnection: return nil
        case .unavailable: return false
        case .available: return true
        }
    }
    var headline: String {
        switch status {
        case .unknown: return "Checking the network…"
        case .unavailable: return "The system reports no network path."
        case .requiresConnection: return "The system says a connection must be established first."
        case .available:
            if physical.isEmpty { return "The system reports a usable network path" + (tunnels > 0 ? " (through a VPN or tunnel interface)." : ".") }
            return "The system reports a usable network path (" + physical.joined(separator: ", ") + (tunnels > 0 ? ", with a VPN or tunnel" : "") + ")."
        }
    }
    var detail: String {
        switch status {
        case .unknown: return "Waiting for the system to report the connection."
        case .unavailable: return "Downloads wait and continue by themselves when a path returns."
        case .requiresConnection: return "Downloads are left as they are until the system reports a path."
        case .available:
            var s = "This does not prove the internet, a tracker or any peer can be reached; each download shows that itself."
            if constrained { s += " The connection is marked constrained (Low Data Mode)." } else if expensive { s += " The connection is marked expensive." }
            return s
        }
    }
    var text: String { headline + " " + detail }
}

enum DownloadAutoPlay {
    /// A finished download starts playing by itself only if the user asked for that, it has not done so before and the player is not busy with something else.
    static func shouldStart(_ d: DownloadItem, alreadyHandled: Set<String>, playerBusy: Bool) -> Bool {
        d.state == .completed && d.autoPlay && !d.primaryFile.isEmpty && !alreadyHandled.contains(d.id) && !playerBusy
    }
}

/// The remembered choices of the Download & Play sheet.
struct DownloadPreferences: Equatable {
    var downloadMode = false                 // false = Stream (the long-standing behaviour)
    var wholeTorrent = false
    var destination = ""
    var autoPlay = true
    var autoResumeOnNetwork = true

    static let keys = (mode: "dl.mode", whole: "dl.whole", dest: "dl.dest", play: "dl.autoplay", resume: "dl.autoResume")
    static func load(_ d: UserDefaults, home: String = NSHomeDirectory()) -> DownloadPreferences {
        var p = DownloadPreferences()
        p.downloadMode = d.object(forKey: keys.mode) as? Bool ?? false
        p.wholeTorrent = d.object(forKey: keys.whole) as? Bool ?? false
        p.destination = (d.string(forKey: keys.dest)).flatMap { $0.isEmpty ? nil : $0 } ?? (home as NSString).appendingPathComponent("Movies/TorrentPlayer")
        p.autoPlay = d.object(forKey: keys.play) as? Bool ?? true
        p.autoResumeOnNetwork = d.object(forKey: keys.resume) as? Bool ?? true      // default: enabled
        return p
    }
    func save(_ d: UserDefaults) {
        d.set(downloadMode, forKey: DownloadPreferences.keys.mode); d.set(wholeTorrent, forKey: DownloadPreferences.keys.whole)
        d.set(destination, forKey: DownloadPreferences.keys.dest); d.set(autoPlay, forKey: DownloadPreferences.keys.play)
        d.set(autoResumeOnNetwork, forKey: DownloadPreferences.keys.resume)
    }
}
