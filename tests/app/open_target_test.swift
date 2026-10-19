// Offline test for OpenTarget (open-request classification, cold-launch queue, session switching). No mpv, no UI, no network.
// Build and run:  swiftc -parse-as-library -swift-version 5 -target arm64-apple-macosx27.0 app/Sources/OpenTarget.swift tests/app/open_target_test.swift -o dist/<dir>/open_target_test && dist/<dir>/open_target_test dist/<dir>/tmp
// The directory argument is where the test creates its small files (keep it under dist/).
import Foundation

@main struct OpenTargetTest {
    static var failures = 0, checks = 0
    static func check(_ c: Bool, _ what: String, line: Int = #line) { checks += 1; if !c { failures += 1; print("FAILED (line \(line)): \(what)") } }

    static func main() {
        let base = CommandLine.arguments.count > 1 ? CommandLine.arguments[1] : NSTemporaryDirectory() + "open_target_test"
        let fm = FileManager.default
        try? fm.removeItem(atPath: base)
        try! fm.createDirectory(atPath: base, withIntermediateDirectories: true)
        let root = URL(fileURLWithPath: base).standardizedFileURL.path
        func touch(_ name: String, _ bytes: Int = 16) -> String {
            let p = root + "/" + name
            try! fm.createDirectory(atPath: (p as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
            fm.createFile(atPath: p, contents: Data(count: bytes))
            return p
        }
        func local(_ t: OpenTarget) -> String? { if case .localVideo(let p) = t { return p }; return nil }

        // 1. video containers (case-insensitive extension)
        for name in ["a.mp4", "b.MKV", "c.mov", "d.webm", "e.avi", "f.m4v", "g.mpg", "h.mpeg", "i.wmv", "j.ts", "k.m2ts", "l.flv", "m.ogv", "n.3gp"] {
            let p = touch(name)
            check(local(OpenClassifier.classify(p)) == p, "video: \(name)")
        }
        // 2. unicode, spaces, punctuation
        let u = touch("Фильм ёж 日本語 (2026) [1080p] #1.mp4")
        check(local(OpenClassifier.classify(u)) == u, "unicode + spaces + brackets")
        let sp = touch("dir with spaces/sub folder/my  video.MOV")
        check(local(OpenClassifier.classify(sp)) == sp, "spaces in directories and doubled spaces")
        // file:// URL string with percent-encoding
        let enc = "file://" + (u.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? u)
        check(local(OpenClassifier.classify(enc)) == u, "file URL with percent-encoded unicode: \(OpenClassifier.classify(enc))")
        check(local(OpenClassifier.classify("  " + u + "\n")) == u, "surrounding whitespace is trimmed")

        // 3. torrent and magnet
        let t = touch("x.torrent")
        check(OpenClassifier.classify(t) == .torrentFile(t), ".torrent file")
        check(OpenClassifier.classify(root + "/Y.TORRENT") == .missing("The file does not exist."), "missing upper-case torrent is missing")
        let m = "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567&dn=x"
        check(OpenClassifier.classify(m) == .magnet(m), "magnet link")
        check(OpenClassifier.classify("MAGNET:?xt=urn:btih:abc") == .magnet("MAGNET:?xt=urn:btih:abc"), "magnet scheme is case-insensitive")

        // 4. missing, directory, empty, foreign URLs
        if case .missing = OpenClassifier.classify(root + "/nope.mp4") {} else { check(false, "missing file") }
        if case .missing = OpenClassifier.classify("") {} else { check(false, "empty request") }
        if case .missing = OpenClassifier.classify("   ") {} else { check(false, "blank request") }
        try! fm.createDirectory(atPath: root + "/folder.mp4", withIntermediateDirectories: true)
        if case .unsupported = OpenClassifier.classify(root + "/folder.mp4") {} else { check(false, "a directory named like a video is not a video") }
        if case .unsupported = OpenClassifier.classify("https://example.com/a.mp4") {} else { check(false, "http URL refused") }
        if case .unsupported = OpenClassifier.classify("tp://stream") {} else { check(false, "foreign scheme refused") }

        // 5. unsupported kinds: other types, no extension, audio-only
        if case .unsupported = OpenClassifier.classify(touch("notes.txt")) {} else { check(false, ".txt") }
        if case .unsupported = OpenClassifier.classify(touch("noext")) {} else { check(false, "no extension") }
        if case .unsupported(let why) = OpenClassifier.classify(touch("song.mp3")) { check(why.contains("Audio"), "audio message: \(why)") } else { check(false, "audio-only file refused") }
        if case .unsupported = OpenClassifier.classify(touch("a.flac")) {} else { check(false, "flac refused") }

        // 6. symlinks
        let target = touch("real/target.mkv")
        try! fm.createSymbolicLink(atPath: root + "/link.mkv", withDestinationPath: target)
        check(local(OpenClassifier.classify(root + "/link.mkv")) == root + "/link.mkv", "symlink with a video extension keeps the path as given")
        try! fm.createSymbolicLink(atPath: root + "/linknoext", withDestinationPath: target)
        check(local(OpenClassifier.classify(root + "/linknoext")) == root + "/linknoext", "symlink without extension to a video: classified by its target")
        try! fm.createSymbolicLink(atPath: root + "/broken.mp4", withDestinationPath: root + "/gone.mp4")
        if case .missing = OpenClassifier.classify(root + "/broken.mp4") {} else { check(false, "dangling symlink is missing") }
        try! fm.createSymbolicLink(atPath: root + "/dirlink.mp4", withDestinationPath: root + "/real")
        if case .unsupported = OpenClassifier.classify(root + "/dirlink.mp4") {} else { check(false, "symlink to a folder refused") }
        let tt = touch("real/t.torrent")
        try! fm.createSymbolicLink(atPath: root + "/tlink", withDestinationPath: tt)
        check(OpenClassifier.classify(root + "/tlink") == .torrentFile(root + "/tlink"), "symlink to a torrent")

        // 7. unreadable file (skipped when running as root)
        let secret = touch("locked.mp4")
        try! fm.setAttributes([.posixPermissions: 0], ofItemAtPath: secret)
        if getuid() != 0 { if case .unsupported(let why) = OpenClassifier.classify(secret) { check(why.contains("permission"), "permission message: \(why)") } else { check(false, "unreadable file") } }
        try? fm.setAttributes([.posixPermissions: 0o644], ofItemAtPath: secret)

        // 8. relative paths and ~ resolve against the current directory / home
        let cwd = fm.currentDirectoryPath
        fm.changeCurrentDirectoryPath(root)
        check(local(OpenClassifier.classify("a.mp4")) == root + "/a.mp4", "relative path resolved against the working directory")
        fm.changeCurrentDirectoryPath(cwd)
        check(OpenClassifier.classify("~/definitely-not-here-\(getpid()).mp4") == .missing("The file does not exist."), "tilde expands (and is missing)")

        // 9. several items: the first OPENABLE one wins, in the order given
        check(OpenClassifier.first(of: [root + "/notes.txt", root + "/a.mp4", t]) == .localVideo(root + "/a.mp4"), "skips unsupported items, keeps the first openable")
        check(OpenClassifier.first(of: [t, root + "/a.mp4"]) == .torrentFile(t), "order is preserved: the torrent first")
        check(OpenClassifier.first(of: [root + "/a.mp4", root + "/c.mov"]) == .localVideo(root + "/a.mp4"), "no playlist: only the first video")
        if case .unsupported = OpenClassifier.first(of: [root + "/notes.txt", root + "/gone.mp4"])! {} else { check(false, "nothing openable: the first item's reason is reported") }
        check(OpenClassifier.first(of: []) == nil, "empty list")

        // 10. telemetry facts: extension and size only
        let facts = OpenClassifier.telemetryFacts(path: u)
        check((facts["ext"] as? String) == "mp4" && (facts["size"] as? Int64) == 16, "ext and size")
        check(Set(facts.keys) == ["ext", "size"] && !String(describing: facts).contains("Фильм"), "no name or path in the telemetry facts")
        check((OpenClassifier.telemetryFacts(path: root + "/nope.mp4")["size"] as? Int64) == -1, "unknown size is -1")

        // 11. Open panel types include movies and torrents
        let ids = OpenClassifier.panelContentTypes.map { $0.identifier }
        check(ids.contains("public.movie") && ids.contains("com.apple.quicktime-movie") && ids.contains("public.mpeg-4"), "panel video types")
        check(OpenClassifier.panelContentTypes.contains { $0.preferredFilenameExtension == "torrent" || $0.tags[.filenameExtension]?.contains("torrent") == true }, "panel accepts .torrent")
        check(Set(ids).count == ids.count, "no duplicate panel types")

        // 12. cold-launch pending-open queue
        var q = OpenQueue()
        check(q.submit(["/a.mp4"]) == nil && q.submit(["/b.mp4", "/c.mp4"]) == nil, "requests before ready are held")
        check(!q.ready, "not ready yet")
        check(q.markReady() == ["/a.mp4", "/b.mp4", "/c.mp4"], "held requests are released once, in arrival order")
        check(q.ready && q.markReady().isEmpty, "released only once")
        check(q.submit(["/d.mp4"]) == ["/d.mp4"], "after ready, requests pass straight through")
        var q2 = OpenQueue()
        check(q2.markReady().isEmpty && q2.submit(["/x"]) == ["/x"], "nothing held: ready immediately")

        // 13. session switching
        let a = OpenTarget.localVideo("/a.mp4"), b = OpenTarget.torrentFile("/b.torrent"), c = OpenTarget.localVideo("/c.mkv")
        var sw = SwitchCoordinator()
        check(sw.request(a, hasSession: false, sessionEnding: false) == .startNow(a), "nothing running: start now")
        check(sw.pending == nil, "no pending item")
        check(sw.request(b, hasSession: true, sessionEnding: false) == .endCurrentThenStart, "running: end the current session first")
        check(sw.pending == b, "the new item waits for the teardown")
        check(sw.request(c, hasSession: true, sessionEnding: true) == .waitForTeardown, "ending already: wait")
        check(sw.pending == c, "the latest request wins")
        check(sw.sessionDidEnd(otherSessionActive: false) == c, "after the teardown the pending item starts")
        check(sw.pending == nil && sw.sessionDidEnd(otherSessionActive: false) == nil, "started only once")
        // a session that ends by itself (window closed): nothing to start
        var sw2 = SwitchCoordinator()
        check(sw2.sessionDidEnd(otherSessionActive: false) == nil, "no pending item: nothing starts")
        // an older session ending while a newer one already runs (existing retry flow): never start anything
        _ = sw2.request(a, hasSession: true, sessionEnding: false)
        check(sw2.sessionDidEnd(otherSessionActive: true) == nil && sw2.pending == a, "a newer session is active: the pending item is kept, nothing starts")
        check(sw2.request(b, hasSession: false, sessionEnding: false) == .startNow(b) && sw2.pending == nil, "no session: a fresh request clears stale pending")

        try? fm.removeItem(atPath: base)
        print("open target: \(checks) checks, \(failures) failures")
        exit(failures == 0 ? 0 : 1)
    }
}
