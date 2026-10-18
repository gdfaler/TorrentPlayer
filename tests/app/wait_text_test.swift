// Offline test for WaitText (banner wording for the wait before playback). No network, no UI.
// Build and run:  swiftc -parse-as-library -swift-version 5 -target arm64-apple-macosx27.0 -import-objc-header app/Sources/bridge.h \
//                   -Xcc -Icore/include -Xcc -Iapp/Sources -Xcc -I/opt/homebrew/opt/mpv/include app/Sources/WaitText.swift tests/app/wait_text_test.swift -o dist/wait_text_test && dist/wait_text_test
import Foundation

@main struct WaitTextTest {
    static var failures = 0, checks = 0
    static func check(_ c: Bool, _ what: String, line: Int = #line) { checks += 1; if !c { failures += 1; print("FAILED (line \(line)): \(what)") } }

    static func base(_ state: tp_state) -> tp_stream_status {
        var r = tp_stream_status(); r.state = Int32(state.rawValue); r.wait_offset = -1; r.wait_piece = -1; r.wait_piece_priority = -1; r.wait_piece_deadline_ms = -1; return r
    }

    static func main() {
        let size: Int64 = 3_410_694_231
        // 1. the recorded failure: index piece at the end of the file, 1 of 128 blocks, 0 of 9 peers report having it, web seed connected, 25 s without a new block
        var r = base(TP_STATE_PREPARING)
        r.wait_kind = 2; r.wait_piece = 1626; r.wait_offset = 3_410_668_616; r.blocked_readers = 1
        r.wait_blocks_total = 128; r.wait_blocks_done = 1; r.wait_in_queue = 1; r.wait_peers_total = 9; r.wait_peers_with_piece = 0; r.web_connections = 1; r.wait_no_progress_s = 25
        var c = WaitText.beforePlayback(r, fileSize: size, rate: "100 KB/s", aheadText: "184 MB buffered ahead")!
        check(c.title == "Preparing playback…", "title is Preparing, not Buffering")
        check(c.detail == "Still waiting for the video's index at the end of the file", "prolonged wait names the place: \(c.detail)")
        check(c.note.contains("No new data for this part of the file for 25 s"), "prolonged wait states the seconds without a new block: \(c.note)")
        check(c.note.contains("1 of 128 blocks received"), "block progress: \(c.note)")
        check(c.note.contains("0 of 9 connected peers report having it"), "peer facts: \(c.note)")
        check(c.note.contains("web seed connected"), "web seed fact: \(c.note)")
        check(abs((c.progress ?? -1) - 1.0 / 128.0) < 1e-9, "progress is the real block ratio")
        for text in [c.title, c.detail, c.note] { check(!text.lowercased().contains("unavailable") && !text.lowercased().contains("missing") && !text.lowercased().contains("not available"), "no claim of unavailability: \(text)") }
        check(!c.note.contains("184 MB"), "the unrelated head-of-file buffer is not presented as progress")
        // 2. a fresh wait (3 s): no 'still waiting' wording yet
        r.wait_no_progress_s = 3
        c = WaitText.beforePlayback(r, fileSize: size, rate: "1 MB/s", aheadText: "")!
        check(c.detail == "Reading the video's index from the end of the file", "fresh wait text: \(c.detail)")
        // 3. start-of-file index
        r.wait_kind = 1
        c = WaitText.beforePlayback(r, fileSize: size, rate: "", aheadText: "")!
        check(c.detail.contains("start of the file"), "head index wording")
        // 4. ordinary data wait before playback: names the position
        var d = base(TP_STATE_BUFFERING)
        d.blocked_readers = 1; d.wait_kind = 3; d.wait_piece = 800; d.wait_offset = size / 2; d.wait_blocks_total = 128; d.wait_blocks_done = 64; d.wait_peers_total = 4; d.wait_peers_with_piece = 4
        c = WaitText.beforePlayback(d, fileSize: size, rate: "2 MB/s", aheadText: "10 MB buffered ahead")!
        check(c.title == "Buffering…" && c.detail == "Waiting for data at 50% of the file", "data wait wording: \(c.detail)")
        check(abs((c.progress ?? -1) - 0.5) < 1e-9, "data wait progress")
        check(c.note.contains("4 of 4 connected peers report having it") && c.note.contains("no web seed connected"), "data wait facts: \(c.note)")
        // 5. buffering without a blocked reader keeps the generic text
        d.blocked_readers = 0
        c = WaitText.beforePlayback(d, fileSize: size, rate: "2 MB/s", aheadText: "10 MB buffered ahead")!
        check(c.detail == "10 MB buffered ahead · 2 MB/s" && c.progress == nil, "generic buffering text")
        // 6. data available, decoder not started
        c = WaitText.beforePlayback(base(TP_STATE_READY), fileSize: size, rate: "", aheadText: "")!
        check(c.title == "Starting playback…", "ready-but-not-started wording")
        // 7. states handled elsewhere return nil
        for s in [TP_STATE_CONNECTING, TP_STATE_NO_PEERS, TP_STATE_DISK_PAUSED, TP_STATE_ERROR] { check(WaitText.beforePlayback(base(s), fileSize: size, rate: "", aheadText: "") == nil, "state \(s) is left to the existing banner") }
        // 7b. Stalled keeps the specific waiting reason (and the facts) instead of a generic message
        var st = base(TP_STATE_STALLED)
        st.wait_kind = 2; st.wait_piece = 1626; st.blocked_readers = 1; st.wait_blocks_total = 45; st.wait_blocks_done = 1; st.wait_peers_total = 9; st.wait_peers_with_piece = 1; st.web_connections = 3; st.wait_no_progress_s = 120
        c = WaitText.beforePlayback(st, fileSize: size, rate: "0 KB/s", aheadText: "")!
        check(c.title == "Stalled" && c.detail == "No data is arriving for the video's index at the end of the file", "stalled keeps the waiting reason: \(c.detail)")
        check(c.note.contains("1 of 45 blocks received") && c.note.contains("1 of 9 connected peers report having it") && c.note.contains("No new data for this part of the file for 120 s"), "stalled keeps the facts: \(c.note)")
        check(abs((c.progress ?? -1) - 1.0 / 45.0) < 1e-9, "stalled progress counts real blocks only")
        var st2 = base(TP_STATE_STALLED); st2.wait_kind = 3; st2.wait_piece = 900; st2.wait_offset = size / 4
        check(WaitText.beforePlayback(st2, fileSize: size, rate: "", aheadText: "")!.detail == "No data is arriving for the data at 25% of the file", "stalled data wait wording")
        // a Stalled state with nothing known to wait for stays with the existing generic banner
        check(WaitText.beforePlayback(base(TP_STATE_STALLED), fileSize: size, rate: "", aheadText: "") == nil, "stalled without wait facts is left to the existing banner")
        // 8. singular wording and telemetry fields
        var one = base(TP_STATE_PREPARING); one.wait_kind = 2; one.wait_piece = 5; one.wait_blocks_total = 1; one.wait_blocks_done = 0; one.wait_peers_total = 1; one.wait_peers_with_piece = 1
        check(WaitText.facts(one).contains("0 of 1 block received") && WaitText.facts(one).contains("1 of 1 connected peer report"), "singular forms: \(WaitText.facts(one))")
        let f = WaitText.fields(r)
        check((f["wait_kind"] as? String) == "head_index" && (f["wait_piece"] as? Int32) == 1626 && f["tail_pin_blocks_total"] != nil && f["wait_blocks_pad"] != nil && f["hash_incomplete"] != nil, "telemetry fields present")
        print("\(checks) checks, \(failures) failures")
        exit(failures == 0 ? 0 : 1)
    }
}
