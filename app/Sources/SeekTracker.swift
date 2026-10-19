// SeekTracker: pure bookkeeping that associates a seek request with its completion (no AppKit, no mpv).
// mpv reports a seek with MPV_EVENT_SEEK (it started) and MPV_EVENT_PLAYBACK_RESTART (it finished); the `time-pos` property is updated asynchronously
// and can arrive AFTER the restart event. A restart that arrives without a seek start belongs to something else (for example playback resuming after a
// buffering pause) and must not complete a pending seek. The landed position is the first `time-pos` update after the restart, never a cached older value.
import Foundation

struct SeekTracker {
    struct Record {
        var target: Double; var issued: Double; var fromPos: Double; var coveredAtTarget: Int; var sawSeek = false
        var mpvSawSeeking = false      // mpv's own `seeking` flag was seen true for this request (display hold)
        var released = false           // the display hold ended without a completion (the request looks failed or ignored); sticky
    }
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
    /// A seek mpv never reports as seeking is considered failed/ignored after this many seconds: the seek bar goes back to following mpv.
    static let noSeekGrace = 2.0

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

    /// The position (seconds) the seek bar and time label must keep showing while a seek is in flight, or nil when they should follow mpv's `time-pos`.
    /// Without it an old `time-pos` still in the pipeline overwrites the thumb right after the slider is released (it snaps back, then jumps forward).
    ///  - pending (issued, mpv has not restarted): held while mpv reports `seeking` (a torrent seek can legitimately wait a long time for data);
    ///    if mpv never reports it within `noSeekGrace` the request is treated as failed/ignored and the hold is released for good;
    ///  - restarted, waiting for the new position: held until that update arrives or `positionWait` passes.
    mutating func heldTarget(now: Double, mpvSeeking: Bool) -> Double? {
        if var p = pending {
            if mpvSeeking { p.mpvSawSeeking = true }
            if !p.released && !p.mpvSawSeeking && now - p.issued >= SeekTracker.noSeekGrace { p.released = true }
            pending = p
            return p.released ? nil : p.target
        }
        if let a = awaiting, now - a.restartedAt < SeekTracker.positionWait { return a.record.target }
        return nil
    }
}

/// What the controls display (pure, so the snap-back rules are testable).
enum SeekDisplay {
    /// While a seek is held the target is shown; otherwise mpv's position.
    static func seconds(actual: Double, held: Double?) -> Double { held ?? actual }
    static func fraction(seconds: Double, duration: Double) -> Double { duration > 0 ? max(0, min(1, seconds / duration)) : 0 }
}

/// Which mpv seek mode each kind of request uses (pure, so the policy is testable).
enum SeekMode {
    /// Slider release: precise only for local files. Keyframe seeks land on the keyframe before the target (up to a whole keyframe interval early), which made the thumb jump.
    static func exact(forSliderRelease isLocal: Bool) -> Bool { isLocal }
    /// Relative (keyboard) seeks of 10 s or less are precise, longer ones use keyframes: unchanged behaviour.
    static func exact(forRelative delta: Double) -> Bool { abs(delta) <= 10 }
}
