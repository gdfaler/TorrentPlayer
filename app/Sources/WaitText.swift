// WaitText.swift: turns the core's "what is the player waiting for" facts into banner text. Pure functions (no UI, no I/O) so they can be tested offline.
// Rule: state facts (blocks received, peers that report having the piece, seconds without new data); never claim that data is "unavailable".
import Foundation

struct BannerContent {
    var title: String
    var detail: String
    var spinning: Bool
    var retry: Bool
    var note: String = ""            // second, smaller line: the facts behind the wait
    var progress: Double? = nil      // 0...1 when a real progress figure exists
    var cancel: Bool = false         // show a Cancel button
}

enum WaitText {
    /// seconds without any new block for the needed piece before the banner says so (a statement of fact)
    static let slowAfter = 20.0

    static func size(_ b: Int64) -> String { ByteCountFormatter.string(fromByteCount: b, countStyle: .binary) }
    static func plural(_ n: Int, _ w: String) -> String { "\(n) \(w)\(n == 1 ? "" : "s")" }

    /// The facts about the piece the player needs next, e.g. "12 of 128 blocks received · 0 of 9 connected peers report having it · web seed connected".
    static func facts(_ r: tp_stream_status) -> String {
        guard r.wait_piece >= 0 else { return "" }
        var parts: [String] = []
        if r.wait_blocks_total > 0 { parts.append("\(r.wait_blocks_done) of \(plural(Int(r.wait_blocks_total), "block")) received") }
        parts.append("\(r.wait_peers_with_piece) of \(plural(Int(r.wait_peers_total), "connected peer")) report having it")
        parts.append(r.web_connections > 0 ? "web seed connected" : "no web seed connected")
        if r.wait_in_queue == 0 && r.wait_blocks_done == 0 { parts.append("not requested yet") }
        return parts.joined(separator: " · ")
    }

    static func progress(_ r: tp_stream_status) -> Double? {
        guard r.wait_piece >= 0, r.wait_blocks_total > 0 else { return nil }
        return min(1, max(0, Double(r.wait_blocks_done) / Double(r.wait_blocks_total)))
    }

    static func idleText(_ r: tp_stream_status) -> String? {
        r.wait_no_progress_s >= slowAfter ? "No new data for this part of the file for \(Int(r.wait_no_progress_s)) s" : nil
    }

    /// Banner for the time BEFORE playback has started, for the states where the core knows what is being waited for.
    /// nil = not one of those states; the caller keeps its existing text (connecting, no peers, disk, error, ...).
    static func beforePlayback(_ r: tp_stream_status, fileSize: Int64, rate: String, aheadText: String) -> BannerContent? {
        let facts = WaitText.facts(r)
        let prog = WaitText.progress(r)
        func note(_ extra: String? = nil) -> String {
            [facts.isEmpty ? nil : facts, extra, rate].compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: " · ")
        }
        switch Int(r.state) {
        case Int(TP_STATE_PREPARING.rawValue):
            let where_: String
            switch Int(r.wait_kind) {
            case 2: where_ = "the end of the file"
            case 1: where_ = "the start of the file"
            default: where_ = "the file"
            }
            let idle = WaitText.idleText(r)
            let detail = idle != nil ? "Still waiting for the video's index at \(where_)" : "Reading the video's index from \(where_)"
            return BannerContent(title: "Preparing playback…", detail: detail, spinning: true, retry: false, note: note(idle), progress: prog)
        case Int(TP_STATE_BUFFERING.rawValue):
            if r.blocked_readers > 0, r.wait_piece >= 0, fileSize > 0, r.wait_offset >= 0 {
                let pct = Int((Double(r.wait_offset) / Double(fileSize) * 100).rounded())
                let idle = WaitText.idleText(r)
                let detail = idle != nil ? "Still waiting for data at \(pct)% of the file" : "Waiting for data at \(pct)% of the file"
                return BannerContent(title: "Buffering…", detail: detail, spinning: true, retry: false, note: note([idle, aheadText].compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: " · ")), progress: prog)
            }
            return BannerContent(title: "Buffering…", detail: "\(aheadText) · \(rate)", spinning: true, retry: false)
        case Int(TP_STATE_READY.rawValue):
            return BannerContent(title: "Starting playback…", detail: "Data is available; waiting for the video decoder to start", spinning: true, retry: false)
        default:
            return nil
        }
    }

    /// Fields recorded in telemetry (samples, cancel, prolonged waits) so a stuck start can be diagnosed afterwards.
    static func fields(_ r: tp_stream_status) -> [String: Any] {
        let kind = ["none", "head_index", "tail_index", "data"][max(0, min(3, Int(r.wait_kind)))]
        return ["wait_kind": kind, "wait_offset": r.wait_offset, "wait_piece": r.wait_piece, "wait_piece_priority": r.wait_piece_priority,
                "wait_piece_deadline_ms": r.wait_piece_deadline_ms, "wait_blocks_total": r.wait_blocks_total, "wait_blocks_done": r.wait_blocks_done,
                "wait_blocks_writing": r.wait_blocks_writing, "wait_blocks_requested": r.wait_blocks_requested, "wait_in_queue": r.wait_in_queue != 0,
                "wait_peers_total": r.wait_peers_total, "wait_peers_with_piece": r.wait_peers_with_piece, "wait_peers_requesting": r.wait_peers_requesting,
                "wait_web_requesting": r.wait_web_requesting, "wait_no_progress_s": r.wait_no_progress_s,
                "head_pin_pieces": r.head_pin_pieces, "head_pin_present": r.head_pin_present, "head_pin_blocks_total": r.head_pin_blocks_total, "head_pin_blocks_done": r.head_pin_blocks_done,
                "tail_pin_pieces": r.tail_pin_pieces, "tail_pin_present": r.tail_pin_present, "tail_pin_blocks_total": r.tail_pin_blocks_total, "tail_pin_blocks_done": r.tail_pin_blocks_done,
                "web_seed_errors": r.web_seed_errors, "web_disconnects": r.web_disconnects, "request_drops": r.request_drops, "block_timeouts": r.block_timeouts]
    }
}
