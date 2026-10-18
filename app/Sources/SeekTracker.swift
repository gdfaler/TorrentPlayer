// SeekTracker: pure bookkeeping that associates a seek request with its completion (no AppKit, no mpv).
// mpv reports a seek with MPV_EVENT_SEEK (it started) and MPV_EVENT_PLAYBACK_RESTART (it finished); the `time-pos` property is updated asynchronously
// and can arrive AFTER the restart event. A restart that arrives without a seek start belongs to something else (for example playback resuming after a
// buffering pause) and must not complete a pending seek. The landed position is the first `time-pos` update after the restart, never a cached older value.
import Foundation

struct SeekTracker {
    struct Record { var target: Double; var issued: Double; var fromPos: Double; var coveredAtTarget: Int; var sawSeek = false }
    struct Completed {
        var record: Record
        var latency: Double          // issue -> PLAYBACK_RESTART
        var landed: Double           // position reported after the restart
        var landedSource: String     // "time-pos" (updated after the restart) or "fallback" (no update arrived in time: the last known position)
        var matchedSeekEvent: Bool   // false: completed by a restart without a seek-start event after `unmatchedGrace`
    }
    /// A restart without a seek-start event only completes a seek this old (seconds); protects against a lost start event.
    static let unmatchedGrace = 2.0
    /// How long to wait for the position update that follows a restart before falling back to the last known position.
    static let positionWait = 1.0

    private(set) var pending: Record?
    private var awaiting: (record: Record, latency: Double, restartedAt: Double, matched: Bool)?

    /// A new seek was issued. A previous seek still waiting for its position is completed first with the position known now.
    mutating func issue(target: Double, now: Double, fromPos: Double, coveredAtTarget: Int) -> Completed? {
        let previous = flush(now: now, lastPos: fromPos, force: true)
        pending = Record(target: target, issued: now, fromPos: fromPos, coveredAtTarget: coveredAtTarget)
        return previous
    }
    /// MPV_EVENT_SEEK: mpv started the seek that is pending.
    mutating func seekStarted() { if pending != nil { pending!.sawSeek = true } }
    /// MPV_EVENT_PLAYBACK_RESTART. Returns the completion latency if this restart finished the pending seek.
    mutating func restarted(now: Double) -> Double? {
        guard let p = pending else { return nil }
        let matched = p.sawSeek
        guard matched || now - p.issued >= SeekTracker.unmatchedGrace else { return nil }
        let latency = now - p.issued
        awaiting = (p, latency, now, matched)
        pending = nil
        return latency
    }
    /// A new `time-pos` value arrived: it completes a seek that already restarted.
    mutating func positionUpdated(_ pos: Double) -> Completed? {
        guard let a = awaiting else { return nil }
        awaiting = nil
        return Completed(record: a.record, latency: a.latency, landed: pos, landedSource: "time-pos", matchedSeekEvent: a.matched)
    }
    /// Periodic call: completes a restarted seek whose position update did not arrive (the target equals the old position, or events were dropped).
    mutating func flush(now: Double, lastPos: Double, force: Bool = false) -> Completed? {
        guard let a = awaiting, force || now - a.restartedAt >= SeekTracker.positionWait else { return nil }
        awaiting = nil
        return Completed(record: a.record, latency: a.latency, landed: lastPos, landedSource: "fallback", matchedSeekEvent: a.matched)
    }
}
