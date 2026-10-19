// Offline test for SeekTracker (seek request <-> completion bookkeeping). No mpv, no UI, no network.
// Build and run:  swiftc -parse-as-library -swift-version 5 -target arm64-apple-macosx27.0 app/Sources/SeekTracker.swift tests/app/seek_tracker_test.swift -o dist/seek_tracker_test && dist/seek_tracker_test
import Foundation

@main struct SeekTrackerTest {
    static var failures = 0, checks = 0
    static func check(_ c: Bool, _ what: String, line: Int = #line) { checks += 1; if !c { failures += 1; print("FAILED (line \(line)): \(what)") } }

    static func main() {
        // 1. normal seek: start event, restart, then the position update; landed is the NEW position, latency is issue -> restart
        var t = SeekTracker()
        check(t.issue(target: 420, now: 145.857, fromPos: 60.0, coveredAtTarget: 0) == nil, "no previous seek")
        check(t.pending?.target == 420, "pending recorded")
        t.seekStarted()
        let lat = t.restarted(now: 159.488)
        check(lat != nil && abs(lat! - 13.631) < 1e-6, "latency measured at the restart: \(String(describing: lat))")
        check(t.pending == nil, "seek no longer pending after the restart")
        check(t.positionUpdated(420.0)?.landed == 420.0, "landed is the first position after the restart")
        check(t.positionUpdated(421.0) == nil, "only one completion per seek")

        // 2. the recorded defect: seek to 30 s issued while the player sat at 708.08 s paused for cache.
        //    A restart that belongs to the buffering resume arrives BEFORE the seek was reported started: it must not complete the seek.
        t = SeekTracker()
        _ = t.issue(target: 30, now: 225.857, fromPos: 708.08, coveredAtTarget: 0)
        check(t.restarted(now: 225.878) == nil, "a restart without a seek-start event does not complete the seek")
        check(t.pending != nil, "the seek is still pending")
        t.seekStarted()
        check(t.restarted(now: 226.10) != nil, "the real restart completes it")
        let c = t.positionUpdated(24.79)
        check(c?.landed == 24.79 && c?.landedSource == "time-pos", "landed is the position AFTER the seek, not the stale 708.08: \(String(describing: c?.landed))")
        check(c?.matchedSeekEvent == true, "associated through the seek-start event")

        // 3. the position update never arrives (target equals the old position): fallback after positionWait
        t = SeekTracker()
        _ = t.issue(target: 50, now: 10, fromPos: 50, coveredAtTarget: 255)
        t.seekStarted(); _ = t.restarted(now: 10.05)
        check(t.flush(now: 10.5, lastPos: 50) == nil, "waits for the position update")
        let f = t.flush(now: 11.1, lastPos: 50)
        check(f?.landedSource == "fallback" && f?.landed == 50 && abs((f?.latency ?? 0) - 0.05) < 1e-9, "fallback completion keeps the real latency")
        check(t.flush(now: 12, lastPos: 50) == nil, "completes once")

        // 4. lost seek-start event: a restart much later still completes the seek and says so
        t = SeekTracker()
        _ = t.issue(target: 100, now: 0, fromPos: 0, coveredAtTarget: 0)
        check(t.restarted(now: 1.0) == nil, "too early without the start event")
        check(t.restarted(now: 2.5) != nil, "completes after the grace period")
        check(t.positionUpdated(100)?.matchedSeekEvent == false, "reports that no start event was seen")

        // 5. a new seek while the previous one waits for its position: the previous one is completed first, with the position known then
        t = SeekTracker()
        _ = t.issue(target: 200, now: 0, fromPos: 0, coveredAtTarget: 0)
        t.seekStarted(); _ = t.restarted(now: 0.5)
        let prev = t.issue(target: 300, now: 0.6, fromPos: 12, coveredAtTarget: 0)
        check(prev?.record.target == 200 && prev?.landed == 12 && prev?.landedSource == "fallback", "previous seek completed on the next issue")
        check(t.pending?.target == 300, "new seek is pending")

        // 6. position updates and restarts with nothing pending are ignored
        t = SeekTracker()
        check(t.restarted(now: 5) == nil && t.positionUpdated(3) == nil && t.flush(now: 9, lastPos: 3) == nil, "idle tracker ignores events")
        t.seekStarted()
        check(t.pending == nil, "seek-start without a request is ignored")

        // ---- seek bar hold (snap-back regression): the thumb must not follow a stale time-pos after the slider is released
        func shown(_ t: inout SeekTracker, _ now: Double, actual: Double, seeking: Bool) -> Double {
            SeekDisplay.seconds(actual: actual, held: t.heldTarget(now: now, mpvSeeking: seeking))
        }
        // 14. normal local seek: released at 10.00 s (playing at 12 s, target 30); the old position is still reported for a moment
        t = SeekTracker()
        check(shown(&t, 9.9, actual: 11.9, seeking: false) == 11.9, "no seek: the display follows mpv")
        _ = t.issue(target: 30, now: 10.0, fromPos: 12.0, coveredAtTarget: 0)
        check(shown(&t, 10.01, actual: 12.0, seeking: false) == 30, "right after release the thumb stays at the target, not at the stale 12.0")
        check(shown(&t, 10.05, actual: 12.04, seeking: true) == 30, "a stale time-pos while mpv is seeking is ignored")
        t.seekStarted()
        check(t.positionUpdated(12.08) == nil, "a position update before the restart does not complete the seek")
        check(shown(&t, 10.06, actual: 12.08, seeking: true) == 30, "still held after a stale update")
        _ = t.restarted(now: 10.07)
        check(shown(&t, 10.08, actual: 12.08, seeking: false) == 30, "restarted but the new position has not arrived yet: still the target")
        check(t.positionUpdated(30.04)?.landed == 30.04, "the first update after the restart completes the seek")
        check(shown(&t, 10.1, actual: 30.04, seeking: false) == 30.04, "afterwards the display follows mpv again (landed position)")
        // 15. delayed mpv position update: held up to positionWait, then released
        t = SeekTracker()
        _ = t.issue(target: 50, now: 0, fromPos: 5, coveredAtTarget: 0); t.seekStarted(); _ = t.restarted(now: 0.1)
        check(shown(&t, 0.9, actual: 5, seeking: false) == 50, "update 0.8 s late: still held")
        check(shown(&t, 1.2, actual: 5, seeking: false) == 5, "after positionWait the display follows mpv again")
        // 16. repeated seeks: the newest target wins and nothing stays stuck
        t = SeekTracker()
        _ = t.issue(target: 30, now: 0, fromPos: 1, coveredAtTarget: 0); t.seekStarted()
        check(shown(&t, 0.1, actual: 1, seeking: true) == 30, "first seek held")
        _ = t.issue(target: 60, now: 0.2, fromPos: 1, coveredAtTarget: 0)
        check(shown(&t, 0.25, actual: 1, seeking: true) == 60, "second seek replaces the first")
        t.seekStarted(); _ = t.restarted(now: 0.3)
        check(t.positionUpdated(60.0)?.record.target == 60, "only the second seek completes")
        check(shown(&t, 0.4, actual: 60.0, seeking: false) == 60.0 && t.pending == nil, "nothing stuck afterwards")
        // seek issued while a restarted one still waits for its position: the old one is completed, the new one is held
        t = SeekTracker()
        _ = t.issue(target: 30, now: 0, fromPos: 1, coveredAtTarget: 0); t.seekStarted(); _ = t.restarted(now: 0.1)
        let flushed = t.issue(target: 70, now: 0.2, fromPos: 1, coveredAtTarget: 0)
        check(flushed?.record.target == 30, "the previous seek is completed on the next issue")
        check(shown(&t, 0.3, actual: 1, seeking: false) == 70, "the new target is shown")
        // 17. failed / ignored seek: mpv never reports seeking and never restarts: the hold is released and stays released
        t = SeekTracker()
        _ = t.issue(target: 90, now: 0, fromPos: 20, coveredAtTarget: 0)
        check(shown(&t, 1.9, actual: 20.5, seeking: false) == 90, "within the grace period the target is shown")
        check(shown(&t, 2.1, actual: 21.0, seeking: false) == 21.0, "after the grace period the bar returns to mpv's real position")
        check(shown(&t, 2.5, actual: 21.4, seeking: true) == 21.4, "released for good, even if mpv later reports seeking")
        check(t.pending != nil, "the request itself is still tracked (banner/telemetry)")
        // interrupted: mpv started the seek, then something else resets it; a new seek takes over cleanly
        t = SeekTracker()
        _ = t.issue(target: 40, now: 0, fromPos: 2, coveredAtTarget: 0); t.seekStarted()
        _ = shown(&t, 0.1, actual: 2, seeking: true)
        _ = t.issue(target: 10, now: 0.5, fromPos: 2, coveredAtTarget: 0)
        check(shown(&t, 0.6, actual: 2, seeking: false) == 10, "the interrupting seek is held")
        check(shown(&t, 2.7, actual: 3, seeking: false) == 3, "and released if mpv never confirms it")
        // 18. torrent seek waiting for data: mpv keeps reporting seeking for a long time; the thumb stays at the target until the data arrives
        t = SeekTracker()
        _ = t.issue(target: 420, now: 100, fromPos: 60, coveredAtTarget: 0); t.seekStarted()
        var always = true
        for sec in stride(from: 100.2, through: 160, by: 3.7) { if shown(&t, sec, actual: 60.2, seeking: true) != 420 { always = false } }
        check(always, "a 60 s wait for torrent data: the thumb stays at the target")
        check(t.pending != nil, "still pending (banner keeps explaining the wait)")
        _ = t.restarted(now: 161)
        check(shown(&t, 161.1, actual: 60.2, seeking: false) == 420, "restarted: held until the position arrives")
        _ = t.positionUpdated(420.04)
        check(shown(&t, 161.2, actual: 420.04, seeking: false) == 420.04, "then it follows mpv")
        // the torrent hold lasts only while mpv says it is seeking: a seek mpv drops mid-wait is released after the grace period
        t = SeekTracker()
        _ = t.issue(target: 300, now: 0, fromPos: 10, coveredAtTarget: 0)
        check(shown(&t, 0.5, actual: 10, seeking: false) == 300, "not yet reported by mpv: held")
        // 19. display helpers
        check(SeekDisplay.seconds(actual: 5, held: nil) == 5 && SeekDisplay.seconds(actual: 5, held: 9) == 9, "held wins over actual")
        check(SeekDisplay.fraction(seconds: 30, duration: 120) == 0.25 && SeekDisplay.fraction(seconds: 500, duration: 120) == 1 && SeekDisplay.fraction(seconds: -3, duration: 120) == 0 && SeekDisplay.fraction(seconds: 3, duration: 0) == 0, "fraction is clamped")
        t = SeekTracker()
        check(t.heldTarget(now: 5, mpvSeeking: true) == nil, "nothing held without a seek")

        print("seek tracker: \(checks) checks, \(failures) failures")
        exit(failures == 0 ? 0 : 1)
    }
}
