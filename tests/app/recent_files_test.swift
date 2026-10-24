// Offline tests for RecentsStore / RecentRowModel with an isolated UserDefaults suite and real temp files. Build:
// swiftc -parse-as-library app/Sources/RecentFiles.swift tests/app/recent_files_test.swift -o /tmp/x && /tmp/x <scratch-dir>
import Foundation
var failures = 0, checks = 0
func check(_ ok: Bool, _ what: @autoclosure () -> String) { checks += 1; if !ok { failures += 1; print("FAIL: \(what())") } }

@main struct RecentFilesTest {
    static func main() {
        let fm = FileManager.default
        guard CommandLine.arguments.count > 1 else { print("usage: recent_files_test <scratch-dir>"); exit(2) }
        let dir = CommandLine.arguments[1] + "/rf_\(UUID().uuidString)"
        try! fm.createDirectory(atPath: dir, withIntermediateDirectories: true)
        let suite = "tp.recents.test.\(UUID().uuidString)"
        let d = UserDefaults(suiteName: suite)!
        func mk(_ n: String, _ body: String = "x") -> String { let p = dir + "/" + n; try! body.write(toFile: p, atomically: false, encoding: .utf8); return p }
        let canon = RecentsStore.canonical

        // first launch: empty
        var s = RecentsStore(defaults: d)
        check(s.entries.isEmpty && d.data(forKey: RecentsStore.key) == nil, "empty on first launch")

        // only readable regular files are recorded
        check(!s.record(path: dir + "/nope.mp4"), "missing file is not recorded")
        check(!s.record(path: dir), "a folder is not recorded")
        check(s.entries.isEmpty, "still empty")

        // record + newest first
        let a = mk("a.mp4"), b = mk("b.mkv"), c = mk("c.mov")
        let t0 = Date(timeIntervalSince1970: 1_000_000)
        check(s.record(path: a, now: t0) && s.record(path: b, now: t0 + 1) && s.record(path: c, now: t0 + 2), "record")
        check(s.entries.map { $0.name } == ["c.mov", "b.mkv", "a.mp4"], "newest first")
        check(s.entries.allSatisfy { $0.bookmark != nil && !$0.id.isEmpty && $0.fileNumber > 0 }, "bookmark, id and file number stored")

        // persistence across "launches"
        let idsBefore = s.entries.map { $0.id }
        s = RecentsStore(defaults: d)
        check(s.entries.map { $0.name } == ["c.mov", "b.mkv", "a.mp4"] && s.entries.map { $0.id } == idsBefore, "persisted after relaunch")

        // reopening moves to the top, no duplicate (also via a different spelling of the same path)
        check(s.record(path: a, now: t0 + 3), "reopen a")
        check(s.entries.map { $0.name } == ["a.mp4", "c.mov", "b.mkv"], "a moved to the top without a duplicate")
        check(s.record(path: dir + "/../" + (dir as NSString).lastPathComponent + "/b.mkv", now: t0 + 4), "reopen b via ../")
        check(s.entries.map { $0.name } == ["b.mkv", "a.mp4", "c.mov"] && s.entries.count == 3, "same file, other spelling: still one entry")
        let link = dir + "/link_to_c.mov"; try! fm.createSymbolicLink(atPath: link, withDestinationPath: c)
        check(s.record(path: link, now: t0 + 5) && s.entries.map { $0.name } == ["c.mov", "b.mkv", "a.mp4"], "a symlink to the same file is the same entry")

        // maximum 10, oldest dropped
        var many: [String] = []
        for i in 0..<12 { let p = mk("m\(i).mp4"); many.append(p); s.record(path: p, now: t0 + 10 + Double(i)) }
        check(s.entries.count == 10, "capped at 10: \(s.entries.count)")
        check(s.entries.first?.name == "m11.mp4" && s.entries.last?.name == "m2.mp4", "newest first, oldest dropped: \(s.entries.map { $0.name })")
        check(RecentsStore(defaults: d).entries.count == 10, "cap persisted")

        // removal and clear
        let victim = s.entries[3]
        s.remove(id: victim.id)
        check(s.entries.count == 9 && s.entry(id: victim.id) == nil && !s.entries.contains { $0.id == victim.id }, "individual removal")
        s.remove(id: "no-such-id"); check(s.entries.count == 9, "removing an unknown id changes nothing")
        check(RecentsStore(defaults: d).entries.count == 9, "removal persisted")
        s.clear()
        check(s.entries.isEmpty && d.data(forKey: RecentsStore.key) == nil && RecentsStore(defaults: d).entries.isEmpty, "clear all, persisted, key removed")

        // availability: available, deleted, moved/renamed (same file followed), replaced by another file, unreadable, unmounted-like (bookmark gone)
        let v = mk("v.mp4", "video-v"); s.record(path: v)
        var e = s.entries[0]
        check(s.availability(of: e) == .available(path: canon(v)), "available")
        let renamed = dir + "/renamed.mp4"; try! fm.moveItem(atPath: v, toPath: renamed)
        check(s.availability(of: e) == .available(path: canon(renamed)), "renamed file is followed: \(s.availability(of: e))")
        e = s.entries[0]
        check(e.path == canon(renamed) && e.name == "renamed.mp4", "entry updated to the new location")
        check(RecentsStore(defaults: d).entries[0].path == canon(renamed), "new location persisted")
        let sub = dir + "/sub"; try! fm.createDirectory(atPath: sub, withIntermediateDirectories: true)
        try! fm.moveItem(atPath: renamed, toPath: sub + "/deep.mp4")
        check(s.availability(of: s.entries[0]) == .available(path: canon(sub + "/deep.mp4")), "moved to another folder is followed")
        try! fm.removeItem(atPath: sub + "/deep.mp4")
        check(s.availability(of: s.entries[0]) == .missing, "deleted: missing")
        _ = mk("sub/deep.mp4", "a DIFFERENT file at the old path")                                  // another file now sits at the last known path
        check(s.availability(of: s.entries[0]) == .missing, "a different file at the old path is never accepted: \(s.availability(of: s.entries[0]))")

        s.clear()
        let locked = mk("locked.mp4"); s.record(path: locked)
        try! fm.setAttributes([.posixPermissions: 0o000], ofItemAtPath: locked)
        check(s.availability(of: s.entries[0]) == .inaccessible, "unreadable file")
        try! fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: locked)
        check(s.availability(of: s.entries[0]) == .available(path: canon(locked)), "readable again")

        // an entry whose bookmark is unusable never falls back to the old path
        var broken = s.entries[0]; broken.bookmark = Data([1, 2, 3])
        check(s.availability(of: broken) == .missing, "damaged bookmark: missing, no guessing")
        var nobm = s.entries[0]; nobm.bookmark = nil
        check(s.availability(of: nobm) == .available(path: canon(locked)), "no bookmark: stored path with the same file number")

        // damaged stored data means an empty list; an over-long list is trimmed
        d.set(Data("not json".utf8), forKey: RecentsStore.key); check(RecentsStore(defaults: d).entries.isEmpty, "damaged data")
        let fakes = (0..<15).map { RecentEntry(id: "i\($0)", name: "n\($0)", path: "/x/n\($0)", fileNumber: 1, bookmark: nil, lastOpened: Date()) }
        d.set(try! JSONEncoder().encode(fakes), forKey: RecentsStore.key); check(RecentsStore(defaults: d).entries.count == 10, "trimmed on load")

        // row model
        let ent = RecentEntry(id: "r", name: "old.mp4", path: "/Users/x/Movies/old.mp4", fileNumber: 1, bookmark: nil, lastOpened: Date())
        let ok = RecentRowModel.make(entry: ent, availability: .available(path: "/Users/x/Movies/New Name.mp4"), home: "/Users/x")
        check(ok.title == "New Name.mp4" && ok.subtitle == "~/Movies" && ok.available && ok.accessibilityLabel.contains("Open"), "available row: \(ok)")
        let miss = RecentRowModel.make(entry: ent, availability: .missing, home: "/Users/x")
        check(miss.title == "old.mp4" && !miss.available && miss.subtitle.contains("not found") && miss.accessibilityLabel.contains("not found"), "missing row")
        check(!RecentRowModel.make(entry: ent, availability: .inaccessible, home: "/Users/x").available, "inaccessible row")

        d.removePersistentDomain(forName: suite)
        try? fm.removeItem(atPath: dir)
        print("recent_files_test: \(checks) checks, \(failures) failures")
        exit(failures == 0 ? 0 : 1)
    }
}
