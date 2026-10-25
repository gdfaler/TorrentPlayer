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
    var completed: Bool { state == .completed }
    /// Verified fraction, never above 1 and never a rounded-up 100% before the download is really complete.
    var fraction: Double {
        if state == .completed { return 1 }
        guard selectedBytes > 0 else { return 0 }
        return min(Double(verifiedBytes) / Double(selectedBytes), 0.9999)
    }
}

enum DownloadFormat {
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
            detail = "Waiting for sources · \(DownloadFormat.percent(d.fraction)) · \(amount). No peer or web seed is connected right now; TorrentPlayer keeps looking."
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
