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
        // clicks resolve the CURRENT download at click time (a row whose state changed since it was built must not act on its old state)
        var a1 = item(.downloading, verified: 100_000, rate: 1000); a1.id = "a"; var b1 = item(.paused, verified: 200_000); b1.id = "b"
        check(DownloadClick.pauseResume(id: "a", in: [a1, b1]) == .pause("a"), "downloading -> pause")
        var a2 = a1; a2.state = .paused
        check(DownloadClick.pauseResume(id: "a", in: [a2, b1]) == .resume("a"), "after the pause the same row means resume (stale handler would pause again)")
        var a3 = a1; a3.state = .waitingNetwork
        check(DownloadClick.pauseResume(id: "a", in: [a3]) == .pause("a"), "waiting for network can still be paused by the user")
        var a4 = a1; a4.state = .failed; a4.error = "x"
        check(DownloadClick.pauseResume(id: "a", in: [a4]) == .resume("a"), "failed -> retry")
        var a5 = a1; a5.state = .completed; a5.verifiedBytes = a5.selectedBytes
        check(DownloadClick.pauseResume(id: "a", in: [a5]) == .ignore, "completed: nothing to pause or resume")
        check(DownloadClick.pauseResume(id: "a", in: [b1]) == .ignore, "removed download: ignored")
        check(DownloadClick.pauseResume(id: "b", in: [b1, a1]) == .resume("b"), "row reuse / reordering: the id decides, not the position")
        check(DownloadClick.playPath(id: "a", in: [a1]) == nil, "no play while downloading")
        check(DownloadClick.playPath(id: "a", in: [a5]) == a5.primaryFile, "play once completed")
        check(DownloadClick.playPath(id: "zz", in: [a5]) == nil, "play of a removed download")
        check(DownloadClick.revealTarget(id: "a", in: [a1])?.folder == a1.savePath && DownloadClick.revealTarget(id: "zz", in: [a1]) == nil, "reveal target")
        check(DownloadClick.cancelPrompt(id: "a", in: [a1]) == .partial(name: "Movie.mkv", verified: 100_000), "cancel of a running download offers keep/delete of partial data")
        check(DownloadClick.cancelPrompt(id: "a", in: [a5]) == .completed(name: "Movie.mkv"), "cancel of a finished download is a removal")
        check(DownloadClick.cancelPrompt(id: "a", in: []) == .gone, "cancel of a vanished download")
        // network path: unknown / no path / path present are three different things, and a path is never presented as proof of internet access
        let unknown = NetworkPathSummary()
        check(unknown.status == .unknown && unknown.downloaderOnline == nil && unknown.headline.hasPrefix("Checking"), "before the first system report nothing is claimed: not online, not offline")
        var wifi = NetworkPathSummary(); wifi.status = .available; wifi.physical = ["Wi-Fi"]; wifi.tunnels = 1
        check(wifi.downloaderOnline == true && wifi.headline.contains("Wi-Fi") && wifi.headline.contains("VPN") && !wifi.headline.contains("available"), "path with Wi-Fi and a tunnel")
        check(wifi.detail.contains("does not prove the internet or any torrent source can be reached"), "a present connection is not presented as proof of reachability")
        var gone = NetworkPathSummary(); gone.status = .unavailable
        check(gone.downloaderOnline == false && gone.headline == "No network connection." && gone.detail.contains("continue by themselves"), "no path -> offline")
        var tunnelOnly = NetworkPathSummary(); tunnelOnly.status = .available; tunnelOnly.tunnels = 2
        check(tunnelOnly.downloaderOnline == false && tunnelOnly.headline.contains("only a VPN or tunnel"), "Wi-Fi switched off but a VPN interface stays satisfied: not reported as 'Network available'")
        var wired = NetworkPathSummary(); wired.status = .available; wired.physical = ["Ethernet"]; wired.constrained = true
        check(wired.downloaderOnline == true && wired.detail.contains("constrained") && wired.detail.contains("continue anyway"), "constrained network is shown, downloads are not blocked")
        var exp = wired; exp.constrained = false; exp.expensive = true
        check(exp.detail.contains("expensive"), "expensive network is shown")
        check(!wifi.text.contains("Network available"), "the old unconditional wording is gone")
        // transitions: unknown -> Wi-Fi -> Wi-Fi off with VPN -> back
        let seq = [unknown, wifi, tunnelOnly, wifi].map { $0.downloaderOnline }
        check(seq == [nil, true, false, true], "transition sequence reported to the downloader")
        // waiting-for-sources text states facts about discovery
        var ws = item(.waitingSources, verified: 0); ws.trackersTotal = 10; ws.trackersOK = 0; ws.trackersFailing = 10
        check(DownloadRowModel.make(ws).detail.contains("0 of 10 trackers answered, 10 failing"), "tracker facts in the waiting text")
        ws.connecting = 2; check(DownloadFormat.discovery(ws).contains("2 connection attempts in progress"), "connection attempts")
        var nt = item(.waitingSources); nt.trackersTotal = 0
        check(DownloadFormat.discovery(nt).contains("lists no trackers"), "torrent without trackers")
        print("download_model_test: \(checks) checks, \(failures) failures")
        exit(failures == 0 ? 0 : 1)
    }
}
