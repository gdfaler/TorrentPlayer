// Player.swift: one playback session = one TPStream + one mpv instance (its own window, architecture A) + overlay panels + telemetry.
// Rules carried over from the research spikes (see tp_mpv_bridge.h): never inject read errors/EOF, mpv_terminate_destroy off the main thread and BEFORE
// the stream is closed, seek completion = PLAYBACK_RESTART after SEEK, no synchronous mpv calls on the main thread after startup (state comes from observed
// properties and async commands, so a stream blocked on the network can never freeze the UI).
import AppKit

struct PlayerSettings {
    var cacheLimit: Int64 = 2 << 30                  // 2 GiB default (tested configuration; the limit is enforced by reservation + margin + reconciliation, measured, not guaranteed)
    var behindSeconds = 300.0
    var aheadSeconds = 75.0
    var webSeedMode = 1                              // assist
    var audioOutput: String? = nil                   // tests: "null"
    var mute = false
}

protocol PlayerSessionDelegate: AnyObject {
    func playerSessionDidEnd(_ s: PlayerSession, reason: String)
}

final class PlayerSession: NSObject {
    let engine: TPEngine, meta: TPMeta, fileIndex: Int, settings: PlayerSettings
    weak var delegate: PlayerSessionDelegate?
    let telemetry: Telemetry
    private(set) var stream: TPStream?
    private var mpv: OpaquePointer?
    private var mpvWindow: NSWindow?
    private let banner = OverlayPanel(size: NSSize(width: 360, height: 120))
    private let controlsPanel = OverlayPanel(size: NSSize(width: 600, height: 48))
    private let bannerView = BannerView(frame: NSRect(x: 0, y: 0, width: 360, height: 120))
    private let controlsView = ControlsView(frame: NSRect(x: 0, y: 0, width: 600, height: 48))
    private let mpvQueue = DispatchQueue(label: "tp.mpv.setup")
    private var pollTimer: Timer?, uiTimer: Timer?, sampleTimer: Timer?
    private var keyMonitor: Any?
    private var observers: [NSObjectProtocol] = []
    private let displayName: String

    // ---- observed mpv state (main thread only)
    private(set) var timePos = 0.0, duration = 0.0
    private var paused = false, pausedForCache = false, seeking = false, coreIdle = true
    private(set) var playbackStarted = false
    private var mpvError: String?
    private var ending = false
    private var fileLoaded = false

    // ---- bookkeeping for telemetry / banner
    private let openedAt = Date()
    private var lastTPState = -1
    private var firstPeerLogged = false, firstPieceLogged = false
    private var stallStart: Double?, stallStartState = "", stallStartPeers = 0
    private var seekTracker = SeekTracker()
    private var seekPending: SeekTracker.Record? { seekTracker.pending }
    private(set) var seekLatencies: [Double] = []
    private(set) var stallDurations: [Double] = []
    private var lastMouseMove = Date(), lastMouse = NSPoint.zero
    private var coverageCache: [UInt8] = []
    private var lastSystemEvent = ""

    init(engine: TPEngine, meta: TPMeta, fileIndex: Int, settings: PlayerSettings, displayName: String, telemetry: Telemetry) {
        self.engine = engine; self.meta = meta; self.fileIndex = fileIndex; self.settings = settings; self.displayName = displayName; self.telemetry = telemetry
        super.init()
        bannerView.onRetry = { [weak self] in self?.retry() }
        bannerView.onCancel = { [weak self] in self?.cancel() }
        banner.contentView = bannerView
        controlsPanel.contentView = controlsView
        controlsView.onPlayPause = { [weak self] in self?.togglePause() }
        controlsView.seekBar.onSeek = { [weak self] f in self?.seekFraction(f) }
        controlsView.seekBar.onScrub = { [weak self] f in guard let s = self else { return }; s.controlsView.timeLabel.stringValue = ControlsView.format(f * s.duration) }
    }

    var onRetryRequested: (() -> Void)?

    // MARK: start
    func start() {
        telemetry.emit("session_start", ["torrent": meta.name, "file": displayName, "file_index": fileIndex, "source": meta.fromMagnet ? "magnet" : "torrent_file",
                                         "cache_limit_bytes": settings.cacheLimit, "web_seed_mode": settings.webSeedMode])
        do {
            stream = try TPStream(engine: engine, meta: meta, fileIndex: fileIndex, cacheLimit: settings.cacheLimit, behindSeconds: settings.behindSeconds, aheadSeconds: settings.aheadSeconds, webSeedMode: settings.webSeedMode)
        } catch {
            mpvError = "Could not open the torrent stream: \(error)"
            telemetry.emit("error", ["where": "stream_open", "message": "\(error)"])
            showBannerWindowless(); return
        }
        let st = stream!.status()
        telemetry.emit("stream_open", ["file_size": stream!.size, "piece_size": st.raw.piece_size, "pieces": st.raw.num_pieces,
                                       "recovery_verified": st.raw.recovery_verified, "recovery_deleted_partial": st.raw.recovery_deleted_partial,
                                       "recovery_deleted_corrupt": st.raw.recovery_deleted_corrupt, "recovery_trimmed": st.raw.recovery_trimmed])
        let s = stream!
        let settings = self.settings, name = displayName
        mpvQueue.async { [weak self] in
            guard let h = mpv_create() else { DispatchQueue.main.async { self?.fail("Could not create the video player (mpv).") }; return }
            func opt(_ k: String, _ v: String) { mpv_set_option_string(h, k, v) }
            // verified configuration: Vulkan-via-Metal only (the Homebrew mpv has no OpenGL render API), VideoToolbox decode, mpv's own window.
            opt("vo", "gpu-next"); opt("gpu-api", "vulkan"); opt("gpu-context", "macvk"); opt("hwdec", "videotoolbox")
            opt("config", "no"); opt("ytdl", "no"); opt("load-scripts", "no"); opt("terminal", "no"); opt("osc", "no"); opt("osd-level", "0")
            opt("input-default-bindings", "no"); opt("input-vo-keyboard", "no"); opt("input-media-keys", "no")
            opt("keep-open", "yes"); opt("idle", "yes"); opt("force-window", "immediate"); opt("title", "TorrentPlayer"); opt("autofit-larger", "90%x90%"); opt("autofit", "50%x50%")
            opt("cache", "yes"); opt("demuxer-readahead-secs", "10"); opt("cache-pause", "yes"); opt("cache-pause-wait", "2"); opt("interpolation", "no")
            if let ao = settings.audioOutput { opt("ao", ao) }
            if settings.mute { opt("mute", "yes") }
            if mpv_initialize(h) < 0 { DispatchQueue.main.async { self?.fail("Could not initialise the video player (mpv).") }; mpv_destroy(h); return }
            for (n, f) in [("time-pos", MPV_FORMAT_DOUBLE), ("duration", MPV_FORMAT_DOUBLE), ("pause", MPV_FORMAT_FLAG), ("paused-for-cache", MPV_FORMAT_FLAG),
                           ("seeking", MPV_FORMAT_FLAG), ("core-idle", MPV_FORMAT_FLAG), ("mute", MPV_FORMAT_FLAG), ("volume", MPV_FORMAT_DOUBLE)] { mpv_observe_property(h, 0, n, f) }
            mpv_request_log_messages(h, "warn")
            let rc = tpb_register(h, s.ptr)
            if rc < 0 { DispatchQueue.main.async { self?.fail("Could not register the torrent stream with the player.") }; mpv_destroy(h); return }
            DispatchQueue.main.async { guard let self = self else { return }; self.mpv = h; self.startTimers() }
            var args: [UnsafePointer<CChar>?] = [UnsafePointer(strdup("loadfile")), UnsafePointer(strdup("tp://stream")), nil]
            _ = mpv_command(h, &args)
            _ = name
        }
        installKeyMonitor()
        observeSystem()
    }

    private func startTimers() {
        pollTimer = Timer.scheduledTimer(withTimeInterval: 0.03, repeats: true) { [weak self] _ in self?.pollMpv() }
        uiTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in self?.uiTick() }
        sampleTimer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in self?.sample() }
        RunLoop.main.add(pollTimer!, forMode: .common); RunLoop.main.add(uiTimer!, forMode: .common)
    }

    private func fail(_ msg: String) {
        mpvError = msg
        telemetry.emit("error", ["where": "setup", "message": msg])
        showBannerWindowless()
    }

    // The failure happened before mpv's window exists: show the banner as its own small window.
    private var fallbackWindow: NSWindow?
    private func showBannerWindowless() {
        let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 520, height: 200), styleMask: [.titled, .closable], backing: .buffered, defer: false)
        w.title = "TorrentPlayer"; w.isReleasedWhenClosed = false; w.center()
        let v = BannerView(frame: NSRect(x: 0, y: 0, width: 400, height: 140))
        v.onRetry = { [weak self] in self?.retry() }
        let c = composeBanner() ?? BannerContent(title: "Playback error", detail: mpvError ?? "", spinning: false, retry: true)
        let size = v.set(title: c.title, detail: c.detail, spinning: false, retry: true)
        v.frame.origin = NSPoint(x: (520 - size.width) / 2, y: 30)
        w.contentView?.addSubview(v); fallbackWindow = w
        w.makeKeyAndOrderFront(nil); NSApp.activate(ignoringOtherApps: true)
    }

    // MARK: mpv events (main thread, non-blocking)
    private func pollMpv() {
        guard let h = mpv, !ending else { return }
        for _ in 0..<64 {
            guard let ev = mpv_wait_event(h, 0) else { break }
            let id = ev.pointee.event_id
            if id == MPV_EVENT_NONE { break }
            handle(ev, id)
        }
    }

    private func handle(_ ev: UnsafeMutablePointer<mpv_event>, _ id: mpv_event_id) {
        switch id {
        case MPV_EVENT_PROPERTY_CHANGE:
            guard let d = ev.pointee.data else { return }
            let p = d.assumingMemoryBound(to: mpv_event_property.self).pointee
            guard p.format != MPV_FORMAT_NONE, let data = p.data else { return }
            let name = String(cString: p.name)
            if p.format == MPV_FORMAT_DOUBLE {
                let v = data.assumingMemoryBound(to: Double.self).pointee
                if name == "time-pos" { timePos = v; if let c = seekTracker.positionUpdated(v) { emitSeekDone(c) } } else if name == "duration" { if v > 0 && duration != v { duration = v; durationKnown() } }
            } else if p.format == MPV_FORMAT_FLAG {
                let v = data.assumingMemoryBound(to: Int32.self).pointee != 0
                switch name {
                case "pause": paused = v; controlsView.setPlaying(!v)
                case "paused-for-cache": setPausedForCache(v)
                case "seeking": seeking = v
                case "core-idle": coreIdle = v
                default: break
                }
            }
        case MPV_EVENT_FILE_LOADED:
            fileLoaded = true; telemetry.emit("file_loaded", ["since_open_s": since()])
        case MPV_EVENT_SEEK:
            seekTracker.seekStarted()
        case MPV_EVENT_PLAYBACK_RESTART:
            if !playbackStarted {
                playbackStarted = true
                telemetry.emit("playback_started", ["since_open_s": since(), "note": "first PLAYBACK_RESTART (decoder running); approximates first frame"])
                DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) { [weak self] in self?.reportGeometry() }
            }
            // Completes the pending seek only when mpv reported that seek's start; `seek_done` is written once the position after the restart is known.
            if let lat = seekTracker.restarted(now: telemetry.now) { seekLatencies.append(lat) }
        case MPV_EVENT_END_FILE:
            guard let d = ev.pointee.data else { return }
            let ef = d.assumingMemoryBound(to: mpv_event_end_file.self).pointee
            if ef.reason == MPV_END_FILE_REASON_ERROR {
                let m = String(cString: mpv_error_string(ef.error))
                mpvError = "The video could not be played (\(m))."
                telemetry.emit("error", ["where": "mpv_end_file", "message": m, "code": Int(ef.error)])
            } else if ef.reason == MPV_END_FILE_REASON_EOF {
                telemetry.emit("end_of_file", ["time_pos_s": timePos])
            }
        case MPV_EVENT_SHUTDOWN:
            end(reason: "player closed")
        case MPV_EVENT_LOG_MESSAGE:
            if let d = ev.pointee.data { let m = d.assumingMemoryBound(to: mpv_event_log_message.self).pointee
                if logCount < 40 { logCount += 1; telemetry.emit("mpv_log", ["level": String(cString: m.level), "prefix": String(cString: m.prefix), "text": String(cString: m.text).trimmingCharacters(in: .whitespacesAndNewlines)]) } }
        default: break
        }
    }
    private var logCount = 0
    private var hungStream: TPStream?

    private func since() -> Double { Date().timeIntervalSince(openedAt) }

    private func durationKnown() {
        guard let s = stream, duration > 1 else { return }
        s.setBitrate(Double(s.size) / duration)
        telemetry.emit("duration_known", ["duration_s": duration, "avg_bitrate_bytes_per_s": Double(s.size) / duration])
    }

    private func setPausedForCache(_ v: Bool) {
        pausedForCache = v
        guard playbackStarted else { return }
        let st = stream?.status()
        if v && stallStart == nil {
            stallStart = telemetry.now; stallStartState = st?.stateName ?? ""; stallStartPeers = Int(st?.raw.peers ?? 0)
            telemetry.emit("stall_start", ["state": stallStartState, "peers": stallStartPeers, "time_pos_s": timePos, "during_seek": seekPending != nil])
        } else if !v, let t = stallStart {
            let d = telemetry.now - t; stallDurations.append(d); stallStart = nil
            telemetry.emit("stall_end", ["duration_s": d, "peers_at_start": stallStartPeers, "state_at_start": stallStartState])
        }
    }

    // MARK: commands (async: the main thread never blocks on mpv)
    private func cmd(_ a: [String]) {
        guard let h = mpv, !ending else { return }
        var cs: [UnsafeMutablePointer<CChar>?] = a.map { strdup($0) } + [nil]
        var cp: [UnsafePointer<CChar>?] = cs.map { UnsafePointer($0) }
        _ = mpv_command_async(h, 0, &cp)
        for p in cs { free(p) }
        cs.removeAll()
    }
    func togglePause() { cmd(["cycle", "pause"]) }
    func seek(to target: Double, exact: Bool) {
        guard duration > 1, let s = stream else { return }
        let t = max(0, min(duration - 1.0, target))
        let cov = coverageValue(at: t / duration)
        if let previous = seekTracker.issue(target: t, now: telemetry.now, fromPos: timePos, coveredAtTarget: cov) { emitSeekDone(previous) }
        s.seekHint(Int64(t / duration * Double(s.size)))
        telemetry.emit("seek_issued", ["target_s": t, "from_s": timePos, "exact": exact, "cache_coverage_at_target_0_255": cov])
        cmd(["seek", String(format: "%.3f", t), exact ? "absolute+exact" : "absolute+keyframes"])
    }
    private func emitSeekDone(_ c: SeekTracker.Completed) {
        telemetry.emit("seek_done", ["target_s": c.record.target, "latency_s": c.latency, "landed_s": c.landed, "landed_source": c.landedSource, "from_s": c.record.fromPos,
                                     "cache_coverage_at_target_0_255": c.record.coveredAtTarget, "seek_event_seen": c.matchedSeekEvent])
    }
    func seekRelative(_ d: Double) { seek(to: timePos + d, exact: abs(d) <= 10) }
    func seekFraction(_ f: Double) { seek(to: f * duration, exact: false) }
    private func coverageValue(at f: Double) -> Int {
        guard !coverageCache.isEmpty else { return -1 }
        return Int(coverageCache[max(0, min(coverageCache.count - 1, Int(f * Double(coverageCache.count))))])
    }
    func adjustVolume(_ d: Int) { cmd(["add", "volume", String(d)]) }
    func toggleMute() { cmd(["cycle", "mute"]) }
    func toggleFullScreen() { mpvWindow?.toggleFullScreen(nil) }

    /// User cancelled a start that has not produced playback: tear the session down (same safe order as any end) and return to the file list.
    func cancel() {
        guard !ending else { return }
        var f: [String: Any] = ["since_open_s": since(), "playback_started": playbackStarted]
        if let s = stream { f.merge(WaitText.fields(s.status().raw)) { a, _ in a } }
        telemetry.emit("cancel_requested", f)
        end(reason: "cancelled by user")
    }

    func retry() {
        telemetry.emit("retry_requested", ["error": mpvError ?? ""])
        end(reason: "retry")
        onRetryRequested?()
    }

    // MARK: banner composition: every wait gets a named, explained state
    func composeBanner() -> BannerContent? {
        if let e = mpvError { return BannerContent(title: "Playback error", detail: e, spinning: false, retry: true) }
        guard let s = stream else { return nil }
        let st = s.status(); let raw = st.raw
        let rate = ByteCountFormatter.string(fromByteCount: Int64(raw.download_rate), countStyle: .binary) + "/s"
        let bitrate = duration > 1 ? Double(s.size) / duration : 0
        let ahead = bitrate > 0 ? Double(raw.contiguous_ahead_bytes) / bitrate : 0
        let aheadText = bitrate > 0 ? String(format: "%.0f s buffered ahead", ahead) : ByteCountFormatter.string(fromByteCount: raw.contiguous_ahead_bytes, countStyle: .binary) + " buffered ahead"
        let peers = "\(raw.peers) peer\(raw.peers == 1 ? "" : "s")"
        func waiting(_ prefix: String) -> BannerContent {
            switch Int(raw.state) {
            case Int(TP_STATE_NO_PEERS.rawValue): return BannerContent(title: "No peers found", detail: "\(prefix)Still searching. The swarm may be empty or unreachable.", spinning: true, retry: false)
            case Int(TP_STATE_CONNECTING.rawValue):
                // after data has already arrived the peers are being RE-connected (they close idle connections), which is a different thing to tell the user
                if raw.peer_bytes > 0 { return BannerContent(title: "Reconnecting to peers…", detail: "\(prefix)\(peers) connected · \(ByteCountFormatter.string(fromByteCount: raw.peer_bytes, countStyle: .binary)) received so far", spinning: true, retry: false) }
                return BannerContent(title: "Connecting to peers…", detail: "\(prefix)\(peers) found so far", spinning: true, retry: false)
            case Int(TP_STATE_PREPARING.rawValue): return BannerContent(title: "Preparing playback…", detail: "\(prefix)Reading the container index · \(peers) · \(rate)", spinning: true, retry: false)
            case Int(TP_STATE_STALLED.rawValue): return BannerContent(title: "Stalled", detail: "\(prefix)\(peers) connected but no data is arriving", spinning: true, retry: false)
            case Int(TP_STATE_DISK_PAUSED.rawValue): return BannerContent(title: "Paused: disk space is low", detail: "Free some space on the cache volume to continue", spinning: false, retry: false)
            case Int(TP_STATE_ERROR.rawValue): return BannerContent(title: "Playback error", detail: "The torrent session reported an error", spinning: false, retry: true)
            default: return BannerContent(title: "Buffering…", detail: "\(prefix)\(aheadText) · \(peers) · \(rate)", spinning: true, retry: false)
            }
        }
        if !playbackStarted {
            var c = WaitText.beforePlayback(raw, fileSize: s.size, rate: rate, aheadText: aheadText) ?? waiting("")
            c.cancel = !c.retry                               // a Cancel button for every wait before playback has started
            return c
        }
        if let r = seekPending {
            let age = telemetry.now - r.issued
            let to = ControlsView.format(r.target)
            if age < 1.0 { return nil }                      // in-window / cached seeks finish well under a second: no flashing banner
            if age < 3.0 { return BannerContent(title: "Seeking to \(to)…", detail: "", spinning: true, retry: false) }
            if Int(raw.state) == Int(TP_STATE_NO_PEERS.rawValue) || Int(raw.state) == Int(TP_STATE_STALLED.rawValue) { return waiting("Seek to \(to) pending · ") }
            return BannerContent(title: "Seek pending: \(to)", detail: "Waiting for data at the new position · \(peers) · \(rate)", spinning: true, retry: false)
        }
        if pausedForCache { return waiting("") }
        if raw.blocked_readers > 0 && (Int(raw.state) == Int(TP_STATE_NO_PEERS.rawValue) || Int(raw.state) == Int(TP_STATE_STALLED.rawValue) || Int(raw.state) == Int(TP_STATE_DISK_PAUSED.rawValue)) { return waiting("") }
        if Int(raw.state) == Int(TP_STATE_DISK_PAUSED.rawValue) { return waiting("") }
        return nil
    }

    // MARK: UI tick
    private func uiTick() {
        guard !ending else { return }
        if mpvWindow == nil {
            if let w = NSApp.windows.first(where: { $0.title == "TorrentPlayer" && $0 !== fallbackWindow && $0.contentView != nil }) { attach(to: w) }
        }
        if let s = stream, tick % 4 == 0 { coverageCache = s.coverage(400); controlsView.seekBar.coverage = coverageCache }
        tick += 1
        controlsView.timeLabel.stringValue = ControlsView.format(timePos)
        controlsView.durationLabel.stringValue = ControlsView.format(duration)
        if duration > 0 { controlsView.seekBar.fraction = max(0, min(1, timePos / duration)) }
        controlsView.setPlaying(!paused)
        // banner
        let content = composeBanner()
        let bannerKey = content.map { $0.title + (retryShownKey($0)) } ?? ""
        let nowT = telemetry.now
        if bannerKey != lastBannerKey || (content != nil && nowT - lastBannerEmit >= 5.0) {
            lastBannerKey = bannerKey; lastBannerEmit = nowT
            telemetry.emit("ui_banner", ["title": content?.title ?? "(hidden)", "detail": content?.detail ?? "", "note": content?.note ?? "", "progress": content?.progress ?? -1,
                                         "retry_button": content?.retry ?? false, "cancel_button": content?.cancel ?? false, "playing": playbackStarted])
        }
        if let c = content {
            let size = bannerView.set(title: c.title, detail: c.detail, spinning: c.spinning, retry: c.retry, note: c.note, progress: c.progress, cancel: c.cancel)
            banner.setContentSize(size)
            positionOverlays()
            if !banner.isVisible, let w = mpvWindow { if banner.parent == nil { w.addChildWindow(banner, ordered: .above) }; banner.orderFront(nil) }
            bannerShown = true
        } else if bannerShown { mpvWindow?.removeChildWindow(banner); banner.orderOut(nil); bannerShown = false }
        // controls auto-hide: visible while paused, banner showing, or the mouse moved in the last 3 s
        let loc = NSEvent.mouseLocation
        if loc != lastMouse { lastMouse = loc; if let f = mpvWindow?.frame, f.contains(loc) { lastMouseMove = Date() } }
        let show = paused || content != nil || Date().timeIntervalSince(lastMouseMove) < 3.0 || !playbackStarted
        if mpvWindow != nil {
            if show && !controlsPanel.isVisible, let w = mpvWindow { if controlsPanel.parent == nil { w.addChildWindow(controlsPanel, ordered: .above) }; controlsPanel.orderFront(nil) }
            if !show && controlsPanel.isVisible { mpvWindow?.removeChildWindow(controlsPanel); controlsPanel.orderOut(nil) }
        }
        // the hash-failure classifier result is surfaced as a telemetry event the first time it changes
        if let s = stream, tick % 4 == 0 { let st = s.status().raw; if Int(st.hash_failures) != lastHashFailures { lastHashFailures = Int(st.hash_failures)
            telemetry.emit("hash_failure", ["total": lastHashFailures, "storage": st.hash_storage, "arrived": st.hash_arrived, "undetermined": st.hash_undetermined, "report": s.lastHashReport()]) } }
    }
    private var tick = 0, bannerShown = false, lastHashFailures = 0, lastBannerKey = "", lastBannerEmit = -100.0, lastWaitSnapshot = -100.0
    private func retryShownKey(_ c: BannerContent) -> String { c.retry ? "|retry" : "" }

    private func attach(to w: NSWindow) {
        mpvWindow = w
        w.acceptsMouseMovedEvents = true
        w.addChildWindow(controlsPanel, ordered: .above)
        positionOverlays()
        for n in [NSWindow.didResizeNotification, NSWindow.didMoveNotification, NSWindow.didEnterFullScreenNotification, NSWindow.didExitFullScreenNotification, NSWindow.didEndLiveResizeNotification, NSWindow.didChangeScreenNotification] {
            observers.append(NotificationCenter.default.addObserver(forName: n, object: w, queue: .main) { [weak self] _ in self?.positionOverlays() })
        }
        observers.append(NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: w, queue: .main) { [weak self] _ in self?.end(reason: "window closed") })
        telemetry.emit("window_attached", ["since_open_s": since()])
        NSApp.activate(ignoringOtherApps: true)
    }

    /// Evidence that architecture A works in the real app: window/overlay geometry, backing scale, Metal drawable size, child-window relationship.
    private func reportGeometry() {
        guard let w = mpvWindow, !ending else { return }
        func metal(_ l: CALayer?) -> CAMetalLayer? { if let m = l as? CAMetalLayer { return m }; for s in l?.sublayers ?? [] { if let m = metal(s) { return m } }; return nil }
        func find(_ v: NSView?) -> CAMetalLayer? { guard let v = v else { return nil }; if let m = metal(v.layer) { return m }; for s in v.subviews { if let m = find(s) { return m } }; return nil }
        let ml = find(w.contentView); let scale = w.backingScaleFactor; let cs = w.contentView?.bounds.size ?? .zero
        let ds = ml?.drawableSize ?? .zero
        telemetry.emit("window_geometry", ["window": NSStringFromRect(w.frame), "content_pt": NSStringFromSize(cs), "scale": scale, "drawable_px": NSStringFromSize(ds),
            "drawable_matches_backing": abs(ds.width - cs.width * scale) <= 1 && abs(ds.height - cs.height * scale) <= 1, "has_metal_layer": ml != nil,
            "controls_frame": NSStringFromRect(controlsPanel.frame), "controls_is_child": w.childWindows?.contains(controlsPanel) ?? false,
            "banner_visible": banner.isVisible, "banner_is_child": w.childWindows?.contains(banner) ?? false, "controls_visible": controlsPanel.isVisible, "screens": NSScreen.screens.count, "fullscreen": w.styleMask.contains(.fullScreen)])
        if let dir = ProcessInfo.processInfo.environment["TP_SNAPSHOT_DIR"] {            // own views rendered into PNGs (no screen capture, no permission prompt)
            try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
            for (name, v) in [("controls", controlsView as NSView), ("banner", bannerView as NSView)] {
                if let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds) { v.cacheDisplay(in: v.bounds, to: rep); try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: dir + "/\(name).png")) }
            }
        }
    }

    private func positionOverlays() {
        guard let w = mpvWindow, let cv = w.contentView else { return }
        let v = w.convertToScreen(cv.convert(cv.bounds, to: nil))
        let cw = max(260, min(900, v.width - 40))
        controlsPanel.setFrame(NSRect(x: v.midX - cw / 2, y: v.minY + 20, width: cw, height: 48), display: true)
        let bs = banner.frame.size
        banner.setFrame(NSRect(x: v.midX - bs.width / 2, y: v.midY - bs.height / 2 + 20, width: bs.width, height: bs.height), display: true)
    }

    // MARK: telemetry sampling (1 Hz)
    private func sample() {
        guard let s = stream, !ending else { return }
        let st = s.status(); let r = st.raw
        if Int(r.state) != lastTPState {
            telemetry.emit("state_change", ["from": lastTPState, "to": st.stateName, "to_qualified": qualifiedState(st), "peers": r.peers, "web_connections": r.web_connections,
                                            "blocked_readers": r.blocked_readers, "ahead_bytes": r.contiguous_ahead_bytes, "since_open_s": since()])
            lastTPState = Int(r.state)
            if r.state == Int32(TP_STATE_NO_PEERS.rawValue) || r.state == Int32(TP_STATE_STALLED.rawValue) { telemetry.emit("core_events", ["state": st.stateName, "events": s.events()]) }   // the core's own log explains waits
        }
        if !firstPeerLogged && r.peers > 0 { firstPeerLogged = true; telemetry.emit("first_peer", ["since_open_s": since(), "peers": r.peers]) }
        if !firstPieceLogged && r.pieces_verified > 0 { firstPieceLogged = true; telemetry.emit("first_piece", ["since_open_s": since()]) }
        if let c = seekTracker.flush(now: telemetry.now, lastPos: timePos) { emitSeekDone(c) }   // the position update after a restart never arrived (target equals the old position)
        let bitrate = duration > 1 ? Double(s.size) / duration : 0
        telemetry.emit("sample", [
            "state": st.stateName, "state_qualified": qualifiedState(st), "peers": r.peers, "seeds": r.seeds, "web_connections": r.web_connections, "download_Bps": r.download_rate,
            "ahead_bytes": r.contiguous_ahead_bytes, "ahead_s": bitrate > 0 ? Double(r.contiguous_ahead_bytes) / bitrate : -1, "blocked_readers": r.blocked_readers,
            "cache_charged": r.cache_charged, "cache_limit": r.cache_limit, "pieces_present": r.pieces_present, "pieces_verified": r.pieces_verified,
            "hash_failures": r.hash_failures, "hash_incomplete": r.hash_incomplete, "un_have_resets": r.un_have_resets, "watchdog_kicks": r.watchdog_kicks, "cap_violations": r.cap_violations,
            "time_pos_s": timePos, "paused": paused, "paused_for_cache": pausedForCache, "seeking": seekPending != nil, "peer_bytes": r.peer_bytes, "web_bytes": r.web_bytes]
            .merging(WaitText.fields(r)) { a, _ in a })
        // a prolonged wait for one piece is recorded with the core's own event log and the last web-seed error (facts for diagnosis; no conclusion is drawn here)
        if r.wait_no_progress_s >= WaitText.slowAfter && telemetry.now - lastWaitSnapshot >= 60.0 {
            lastWaitSnapshot = telemetry.now
            telemetry.emit("wait_prolonged", WaitText.fields(r).merging(["state": st.stateName, "web_seed_last_error": st.webSeedLastError,
                                                                          "core_events_tail": String(s.events().suffix(4000))]) { a, _ in a })
        }
    }

    /// The core's state names describe connectivity ("No peers found") even when nothing is waiting for data. For analysis the telemetry therefore also carries a
    /// qualified label: the raw core name stays in "to"/"state"; this one says whether anything is actually waiting and whether a web seed is the only source.
    private func qualifiedState(_ st: StreamStatus) -> String {
        let r = st.raw
        let waiting = r.blocked_readers > 0
        switch Int(r.state) {
        case Int(TP_STATE_NO_PEERS.rawValue), Int(TP_STATE_CONNECTING.rawValue):
            if r.web_connections > 0 && r.peers == 0 { return "\(st.stateName) (web seed connected: it is the only source)" }
            return waiting ? "\(st.stateName) (a reader is waiting for data)" : "\(st.stateName) (idle: nothing is waiting for data; buffer satisfied)"
        default: return st.stateName
        }
    }

    // MARK: keyboard (one app-level local monitor; mpv's own bindings are disabled)
    private func installKeyMonitor() {
        keyMonitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) { [weak self] e in
            guard let self = self, !self.ending else { return e }
            let cmdDown = e.modifierFlags.contains(.command)
            if cmdDown { return e }                                        // menu shortcuts
            let shift = e.modifierFlags.contains(.shift)
            switch e.keyCode {
            case 49: self.togglePause(); return nil                        // space
            case 123: self.seekRelative(shift ? -30 : -5); return nil     // left
            case 124: self.seekRelative(shift ? 30 : 5); return nil       // right
            case 126: self.adjustVolume(5); return nil                     // up
            case 125: self.adjustVolume(-5); return nil                    // down
            case 53:                                                       // escape: leave full screen; before playback has started it cancels the start
                if self.mpvWindow?.styleMask.contains(.fullScreen) == true { self.toggleFullScreen(); return nil }
                if !self.playbackStarted && self.mpvError == nil && self.mpvWindow != nil { self.cancel(); return nil }
                return e
            default: break
            }
            switch e.charactersIgnoringModifiers?.lowercased() {
            case "f": self.toggleFullScreen(); return nil
            case "m": self.toggleMute(); return nil
            case "r": if self.mpvError != nil { self.retry(); return nil }
            default: break
            }
            return e
        }
    }

    private func observeSystem() {
        let nc = NSWorkspace.shared.notificationCenter
        observers.append(nc.addObserver(forName: NSWorkspace.willSleepNotification, object: nil, queue: .main) { [weak self] _ in self?.telemetry.emit("system_sleep", ["note": "this session is affected by sleep and is not valid for performance analysis"]) })
        observers.append(nc.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { [weak self] _ in self?.telemetry.emit("system_wake") })
    }

    // MARK: teardown (verified order: mpv destroyed off the main thread, then the stream)
    func end(reason: String) {
        guard !ending else { return }
        ending = true
        pollTimer?.invalidate(); uiTimer?.invalidate(); sampleTimer?.invalidate()
        if let m = keyMonitor { NSEvent.removeMonitor(m); keyMonitor = nil }
        for o in observers { NotificationCenter.default.removeObserver(o); NSWorkspace.shared.notificationCenter.removeObserver(o) }
        observers.removeAll()
        controlsPanel.orderOut(nil); banner.orderOut(nil)
        mpvWindow?.removeChildWindow(controlsPanel); mpvWindow?.removeChildWindow(banner)
        fallbackWindow?.close(); fallbackWindow = nil
        let h = mpv; mpv = nil
        let t0 = Date()
        let finish: () -> Void = { [weak self] in
            guard let self = self else { return }
            let td = Date().timeIntervalSince(t0)
            if let s = self.stream {
                var c = tpb_counters(); tpb_counters_get(&c)
                self.telemetry.emit("bridge_counters", ["reads": c.reads, "bytes": c.bytes, "cancels": c.cancels, "cancelled_reads": c.cancelled_reads, "seeks": c.seeks, "opens": c.opens, "closes": c.closes])
                let r = s.status().raw
                self.telemetry.emit("session_end", ["reason": reason, "teardown_s": td, "duration_open_s": self.since(), "playback_started": self.playbackStarted,
                                                   "seeks": self.seekLatencies.count, "seek_latency_median_s": self.median(self.seekLatencies), "seek_latency_max_s": self.seekLatencies.max() ?? -1,
                                                   "stalls": self.stallDurations.count, "stall_total_s": self.stallDurations.reduce(0, +), "pieces_verified": r.pieces_verified, "hash_failures": r.hash_failures,
                                                   "hash_storage": r.hash_storage, "cap_violations": r.cap_violations, "cache_max_charged": r.cache_max_charged, "cache_limit": r.cache_limit])
                s.close()
            }
            self.stream = nil
            self.delegate?.playerSessionDidEnd(self, reason: reason)
        }
        if let h = h {
            DispatchQueue.global().async {
                let sema = DispatchSemaphore(value: 0)
                DispatchQueue.global().async { mpv_terminate_destroy(h); sema.signal() }
                if sema.wait(timeout: .now() + 10) == .timedOut {
                    // never close the stream under a live mpv: keep it (leak) and report; the app stays usable
                    NSLog("TorrentPlayer: mpv_terminate_destroy did not finish within 10 s; the stream is kept open to stay safe")
                    DispatchQueue.main.async { [weak self] in guard let self = self else { return }; self.telemetry.emit("error", ["where": "teardown", "message": "mpv_terminate_destroy hung >10 s"]); self.hungStream = self.stream; self.stream = nil; self.delegate?.playerSessionDidEnd(self, reason: reason) }
                    return
                }
                DispatchQueue.main.async(execute: finish)
            }
        } else { DispatchQueue.main.async(execute: finish) }
    }

    private func median(_ v: [Double]) -> Double { v.isEmpty ? -1 : v.sorted()[v.count / 2] }
    var isEnding: Bool { ending }
}
