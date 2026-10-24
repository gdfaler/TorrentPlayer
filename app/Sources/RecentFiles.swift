// RecentFiles: the list of recently played LOCAL videos shown on the home screen (not a media library: at most 10 entries, no thumbnails, no metadata, no torrents or magnet links).
// Pure Foundation, unit-tested on its own (tests/app/recent_files_test.swift).
// Persistence: UserDefaults (the app's own preferences domain, or an injected suite in tests). Each entry keeps the file name, the last known path, the file number (inode) and a
// plain bookmark. The app is NOT sandboxed (ad-hoc signed, no entitlements), so security-scoped bookmarks are not needed; a bookmark still follows a file that was moved or renamed.
// A recent is never opened by guessing: if the bookmark no longer resolves, or resolves to a file with a different file number, the entry is reported as missing.
// Privacy: paths stay in the local preferences file and the UI; nothing from this file goes to telemetry.
import Foundation

struct RecentEntry: Codable, Equatable {
    var id: String
    var name: String               // file name as last seen
    var path: String               // last known standardized path
    var fileNumber: UInt64         // inode: tells "the same file, moved" from "another file at the same path"
    var bookmark: Data?
    var lastOpened: Date
}

enum RecentAvailability: Equatable {
    case available(path: String)
    case missing                   // moved to an unknown place, deleted, replaced by another file, volume not mounted
    case inaccessible              // exists but cannot be read
}

final class RecentsStore {
    static let maxEntries = 10
    static let key = "recentLocalVideos.v1"
    private let defaults: UserDefaults
    private let fm: FileManager
    private(set) var entries: [RecentEntry] = []

    init(defaults: UserDefaults, fileManager: FileManager = .default) {
        self.defaults = defaults; fm = fileManager
        if let d = defaults.data(forKey: Self.key), let e = try? JSONDecoder().decode([RecentEntry].self, from: d) { entries = Array(e.prefix(Self.maxEntries)) }     // damaged data simply means an empty list
    }

    private func save() {
        if entries.isEmpty { defaults.removeObject(forKey: Self.key) } else if let d = try? JSONEncoder().encode(entries) { defaults.set(d, forKey: Self.key) }
    }

    static func canonical(_ path: String) -> String { URL(fileURLWithPath: path).resolvingSymlinksInPath().standardizedFileURL.path }

    private func fileNumber(_ path: String) -> UInt64? {
        ((try? fm.attributesOfItem(atPath: path))?[.systemFileNumber] as? NSNumber)?.uint64Value
    }
    private func isReadableRegularFile(_ path: String) -> Bool {
        var isDir: ObjCBool = false
        return fm.fileExists(atPath: path, isDirectory: &isDir) && !isDir.boolValue && fm.isReadableFile(atPath: path)
    }

    /// Where an entry's bookmark points now (without changing anything), or nil when it no longer resolves.
    private func resolvedPath(of e: RecentEntry) -> String? {
        guard let b = e.bookmark else { return e.path }
        var stale = false
        guard let u = try? URL(resolvingBookmarkData: b, options: [.withoutUI, .withoutMounting], relativeTo: nil, bookmarkDataIsStale: &stale) else { return nil }
        return Self.canonical(u.path)
    }

    /// Records a local video that has just started playing: newest first, no duplicates (the same file under another path counts as the same), at most 10. Returns false when
    /// the path is not a readable regular file (nothing is recorded).
    @discardableResult
    func record(path: String, now: Date = Date()) -> Bool {
        let p = Self.canonical(path)
        guard isReadableRegularFile(p), let number = fileNumber(p) else { return false }
        let bookmark = try? URL(fileURLWithPath: p).bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil)
        entries.removeAll { $0.path == p || ($0.fileNumber == number && resolvedPath(of: $0) == p) }
        entries.insert(RecentEntry(id: UUID().uuidString, name: (p as NSString).lastPathComponent, path: p, fileNumber: number, bookmark: bookmark, lastOpened: now), at: 0)
        if entries.count > Self.maxEntries { entries.removeLast(entries.count - Self.maxEntries) }
        save()
        return true
    }

    func remove(id: String) { entries.removeAll { $0.id == id }; save() }
    func clear() { entries = []; save() }
    func entry(id: String) -> RecentEntry? { entries.first { $0.id == id } }

    /// Resolves an entry for display or opening. A moved or renamed file is followed (the entry is updated); a different file at the old path is NOT accepted.
    func availability(of e: RecentEntry) -> RecentAvailability {
        guard let path = resolvedPath(of: e) else { return .missing }
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: path, isDirectory: &isDir), !isDir.boolValue else { return .missing }
        guard let number = fileNumber(path), number == e.fileNumber else { return .missing }           // another file now sits there
        guard fm.isReadableFile(atPath: path) else { return .inaccessible }
        if path != e.path, let i = entries.firstIndex(where: { $0.id == e.id }) {                       // followed a move / rename: remember the new location
            entries[i].path = path; entries[i].name = (path as NSString).lastPathComponent
            entries[i].bookmark = (try? URL(fileURLWithPath: path).bookmarkData(options: [], includingResourceValuesForKeys: nil, relativeTo: nil)) ?? entries[i].bookmark
            save()
        }
        return .available(path: path)
    }
}

/// What one row of the Recent list shows.
struct RecentRowModel: Equatable {
    var id: String
    var title: String
    var subtitle: String           // folder (home abbreviated as ~), or the problem
    var available: Bool
    var accessibilityLabel: String

    static func make(entry e: RecentEntry, availability a: RecentAvailability, home: String = NSHomeDirectory()) -> RecentRowModel {
        let current: String
        if case .available(let p) = a { current = p } else { current = e.path }
        let name = (current as NSString).lastPathComponent
        let folder = ((current as NSString).deletingLastPathComponent as NSString).abbreviatingWithTildeInPath
        let folderShown = home.isEmpty ? folder : folder.replacingOccurrences(of: home, with: "~")
        switch a {
        case .available: return RecentRowModel(id: e.id, title: name, subtitle: folderShown, available: true, accessibilityLabel: "\(name), in \(folderShown). Open")
        case .missing: return RecentRowModel(id: e.id, title: name, subtitle: "File not found. It was moved, renamed or deleted.", available: false, accessibilityLabel: "\(name), file not found")
        case .inaccessible: return RecentRowModel(id: e.id, title: name, subtitle: "Cannot be read (permission denied).", available: false, accessibilityLabel: "\(name), cannot be read")
        }
    }
}
