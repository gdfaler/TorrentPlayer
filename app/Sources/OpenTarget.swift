// OpenTarget: the single place that decides what an "open" request is (local video, .torrent file, magnet link, or something we cannot open), plus two small
// pure helpers: the queue for open requests that arrive before the app is ready (Finder "Open With" on a cold launch) and the coordinator that replaces one
// playback session with another only after the first one's asynchronous teardown. No AppKit, no mpv, no libtorrent: unit-tested on their own.
import Foundation
import UniformTypeIdentifiers

enum OpenTarget: Equatable {
    case torrentFile(String)          // absolute path of a .torrent file
    case magnet(String)               // magnet link
    case localVideo(String)           // absolute path of a video file played straight from disk
    case unsupported(String)          // user-readable reason
    case missing(String)              // user-readable reason (file does not exist, empty request, ...)

    /// True for the targets that start something.
    var isOpenable: Bool {
        switch self { case .torrentFile, .magnet, .localVideo: return true; case .unsupported, .missing: return false }
    }
}

enum OpenClassifier {
    /// Video containers accepted for local playback (the Info.plist "Video file" document type plus a few common transport formats).
    static let videoExtensions: Set<String> = ["mp4", "m4v", "mov", "mkv", "webm", "avi", "mpg", "mpeg", "wmv", "ts", "m2ts", "mts", "flv", "ogv", "3gp"]
    /// Audio-only formats are outside the MVP: refused with a clear message instead of being handed to the player.
    static let audioExtensions: Set<String> = ["mp3", "flac", "m4a", "aac", "wav", "ogg", "oga", "opus", "wma", "aiff", "aif"]

    /// Content types offered by the Open panel: videos and .torrent files.
    static var panelContentTypes: [UTType] {
        var types: [UTType] = [.movie, .video, .mpeg4Movie, .quickTimeMovie, .avi]
        for e in videoExtensions.sorted() { if let t = UTType(filenameExtension: e) { types.append(t) } }
        if let t = UTType(filenameExtension: "torrent") { types.append(t) }
        var seen = Set<String>(), out: [UTType] = []
        for t in types where seen.insert(t.identifier).inserted { out.append(t) }
        return out
    }

    /// Classify one open request: a magnet link, a path, or a file:// URL string.
    static func classify(_ request: String, fileManager fm: FileManager = .default) -> OpenTarget {
        let s = request.trimmingCharacters(in: .whitespacesAndNewlines)
        if s.isEmpty { return .missing("Nothing to open.") }
        if s.lowercased().hasPrefix("magnet:") { return .magnet(s) }
        var path = s
        if s.lowercased().hasPrefix("file://") {
            guard let u = URL(string: s), u.isFileURL else { return .unsupported("That address is not a file on this Mac.") }
            path = u.path
        } else if s.contains("://") {
            return .unsupported("Only files on this Mac and magnet links can be opened.")
        }
        path = (path as NSString).expandingTildeInPath
        if !path.hasPrefix("/") { path = fm.currentDirectoryPath + "/" + path }
        path = URL(fileURLWithPath: path).standardizedFileURL.path

        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: path, isDirectory: &isDir) else { return .missing("The file does not exist.") }   // follows symlinks: a broken link is missing
        if isDir.boolValue { return .unsupported("That is a folder, not a video file.") }
        guard fm.isReadableFile(atPath: path) else { return .unsupported("The file cannot be read (permission denied).") }

        // by extension: the name as given first, then the symlink target (a link without a useful extension)
        let target = URL(fileURLWithPath: path).resolvingSymlinksInPath().path
        for p in [path, target] {
            let ext = (p as NSString).pathExtension.lowercased()
            if ext == "torrent" { return .torrentFile(path) }
            if videoExtensions.contains(ext) { return .localVideo(path) }
            if audioExtensions.contains(ext) { return .unsupported("Audio files are not supported yet. Open a video file or a .torrent.") }
        }
        let ext = (path as NSString).pathExtension.lowercased()
        if !ext.isEmpty, let t = UTType(filenameExtension: ext), t.conforms(to: .movie) { return .localVideo(path) }
        return .unsupported("This kind of file is not supported. Open a video file or a .torrent.")
    }

    /// Several items arrive together (several files selected or dropped): only the FIRST openable one is used, in the order given (no playlist).
    /// When none is openable the first item's classification is returned so the user sees why.
    static func first(of requests: [String], fileManager fm: FileManager = .default) -> OpenTarget? {
        var firstResult: OpenTarget?
        for r in requests {
            let t = classify(r, fileManager: fm)
            if t.isOpenable { return t }
            if firstResult == nil { firstResult = t }
        }
        return firstResult
    }

    /// Size and extension only: local-file telemetry never carries the file name or path.
    static func telemetryFacts(path: String, fileManager fm: FileManager = .default) -> [String: Any] {
        let size = ((try? fm.attributesOfItem(atPath: path))?[.size] as? NSNumber)?.int64Value ?? -1
        return ["ext": (path as NSString).pathExtension.lowercased(), "size": size]
    }
}

/// Open requests received before the app is ready (macOS can deliver "open file" events before applicationDidFinishLaunching).
struct OpenQueue {
    private(set) var ready = false
    private var held: [String] = []

    /// Returns the requests to handle NOW (everything when ready), or nil when they were held for later.
    mutating func submit(_ requests: [String]) -> [String]? {
        if ready { return requests }
        held.append(contentsOf: requests)
        return nil
    }
    /// The app is ready: returns everything held, in arrival order (once).
    mutating func markReady() -> [String] {
        ready = true
        let out = held
        held = []
        return out
    }
}

/// Replaces one playback session with another: the new item starts only after the current session has finished its asynchronous teardown.
struct SwitchCoordinator {
    enum Action: Equatable {
        case startNow(OpenTarget)            // nothing is running
        case endCurrentThenStart             // the current session must be ended; the target starts when it has ended
        case waitForTeardown                 // an end is already in progress; the target (latest request wins) starts when it finishes
    }
    private(set) var pending: OpenTarget?

    mutating func request(_ t: OpenTarget, hasSession: Bool, sessionEnding: Bool) -> Action {
        guard hasSession else { pending = nil; return .startNow(t) }
        pending = t
        return sessionEnding ? .waitForTeardown : .endCurrentThenStart
    }
    /// A session finished its teardown. `otherSessionActive`: a different, newer session already exists (existing retry flow): then nothing is started.
    mutating func sessionDidEnd(otherSessionActive: Bool) -> OpenTarget? {
        if otherSessionActive { return nil }
        let t = pending
        pending = nil
        return t
    }
}
