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

        print("seek tracker: \(checks) checks, \(failures) failures")
        exit(failures == 0 ? 0 : 1)
    }
}
