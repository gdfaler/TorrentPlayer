// Offline tests for DownloadModel.swift (Download & Play presentation logic). Build: swiftc -parse-as-library app/Sources/DownloadModel.swift tests/app/download_model_test.swift
import Foundation
var failures = 0, checks = 0
func check(_ ok: Bool, _ what: @autoclosure () -> String) { checks += 1; if !ok { failures += 1; print("FAIL: \(what())") } }

func item(_ s: DownloadState, verified: Int64 = 0, total: Int64 = 1_000_000, rate: Int = 0, eta: Int64 = -1, peers: Int = 0, web: Int = 0) -> DownloadItem {
    var d = DownloadItem(); d.id = "abc"; d.name = "Movie.mkv"; d.state = s; d.verifiedBytes = verified; d.selectedBytes = total; d.downloadRate = rate; d.etaSeconds = eta; d.peers = peers; d.webSeeds = web
    d.savePath = "/Users/x/Movies"; d.primaryFile = "/Users/x/Movies/Movie.mkv"; return d
}

@main struct DownloadModelTest {
    static func main() {
        // progress is honest: never 100% before completion, never above 1, zero for an empty selection
        check(item(.downloading, verified: 1_000_000).fraction < 1, "all bytes verified but not yet completed is not 100%")
        check(DownloadFormat.percent(1.0) == "99%", "percent text never shows 100% for a running download")
        check(item(.completed, verified: 1_000_000).fraction == 1, "completed is 1")
        check(item(.downloading, verified: 5_000_000).fraction <= 1, "verified above total is clamped")
        check(item(.downloading, verified: 10, total: 0).fraction == 0, "empty selection")
        check(DownloadFormat.percent(0.456) == "45%", "floor, not round")
        // ETA is shown only when known
        check(DownloadFormat.remaining(-1) == nil, "unknown ETA is not invented")
        check(DownloadFormat.remaining(30) == "less than a minute left", "short ETA")
        check(DownloadFormat.remaining(600) == "about 10 min left", "minutes")
        check(DownloadFormat.remaining(3 * 3600 + 20 * 60) == "about 3 h 20 min left", "hours")
        check(!DownloadRowModel.make(item(.downloading, verified: 400_000, rate: 100_000)).detail.contains("left"), "no ETA text without an estimate")
        check(DownloadRowModel.make(item(.downloading, verified: 400_000, rate: 100_000, eta: 120)).detail.contains("about 2 min left"), "ETA text with an estimate")
        // states and buttons
        let down = DownloadRowModel.make(item(.downloading, verified: 400_000, rate: 100_000, peers: 3, web: 1))
        check(down.detail.hasPrefix("Downloading · 40%") && down.detail.contains("4 sources") && down.canPause && !down.canResume && !down.canPlay && down.cancelTitle == "Cancel", "downloading row")
        let net = DownloadRowModel.make(item(.waitingNetwork, verified: 400_000))
        check(net.detail.hasPrefix("Waiting for network") && net.detail.contains("continues by itself") && net.canPause, "waiting for network is distinct from paused")
        let src = DownloadRowModel.make(item(.waitingSources, verified: 400_000))
        check(src.detail.hasPrefix("Waiting for sources") && !src.detail.hasPrefix("Waiting for network"), "waiting for sources")
        let paused = DownloadRowModel.make(item(.paused, verified: 400_000))
        check(paused.detail.hasPrefix("Paused") && paused.canResume && !paused.canPause, "paused row offers resume only")
        let done = DownloadRowModel.make(item(.completed, verified: 1_000_000))
        check(done.detail.hasPrefix("Completed") && done.detail.contains("saved in") && done.detail.contains("Movies") && done.canPlay && done.canReveal && done.cancelTitle == "Remove" && !done.canPause, "completed row")
        var failed = item(.failed, verified: 1); failed.error = "The destination disk is full."
        let fr = DownloadRowModel.make(failed)
        check(fr.detail == "Failed · The destination disk is full." && fr.canResume, "failed row shows the reason and allows a retry")
        check(DownloadRowModel.make(item(.checking)).indeterminate, "checking with unknown progress is indeterminate")
        check(DownloadRowModel.make(item(.resuming)).detail.hasPrefix("Resuming"), "resuming row")
        var noFile = item(.completed, verified: 1_000_000); noFile.primaryFile = ""
        check(!DownloadRowModel.make(noFile).canPlay, "no Play button without a file")
        // auto-play: once, only when asked, never over a running video
        let finished = item(.completed, verified: 1_000_000)
        check(DownloadAutoPlay.shouldStart(finished, alreadyHandled: [], playerBusy: false), "auto-play starts")
        check(!DownloadAutoPlay.shouldStart(finished, alreadyHandled: ["abc"], playerBusy: false), "auto-play happens once")
        check(!DownloadAutoPlay.shouldStart(finished, alreadyHandled: [], playerBusy: true), "never interrupts a running video")
        var off = finished; off.autoPlay = false
        check(!DownloadAutoPlay.shouldStart(off, alreadyHandled: [], playerBusy: false), "respects the option")
        check(!DownloadAutoPlay.shouldStart(item(.downloading, verified: 1_000_000), alreadyHandled: [], playerBusy: false), "not before completion")
        // preferences: defaults and round trip in an isolated suite
        let suite = UserDefaults(suiteName: "tp.test.download.\(UUID().uuidString)")!
        let def = DownloadPreferences.load(suite, home: "/Users/x")
        check(!def.downloadMode && !def.wholeTorrent && def.autoPlay && def.autoResumeOnNetwork && def.destination == "/Users/x/Movies/TorrentPlayer", "defaults: Stream mode, resume on network enabled")
        var p = def; p.downloadMode = true; p.wholeTorrent = true; p.destination = "/Volumes/Big/Films"; p.autoPlay = false; p.autoResumeOnNetwork = false; p.save(suite)
        check(DownloadPreferences.load(suite, home: "/Users/x") == p, "preferences round trip")
        print("download_model_test: \(checks) checks, \(failures) failures")
        exit(failures == 0 ? 0 : 1)
    }
}
