// Launcher.swift: the app-level flow (open -> metadata -> file choice -> player session -> back). The window it drives is the Titanium Glass home screen (HomeView.swift;
// what each state shows is decided by HomeModel.swift): open a video, a .torrent or a magnet link, watch metadata fetch (with a named "No peers found" state), choose the file, press Play.
import AppKit
import UniformTypeIdentifiers

struct AppOptions {
    var openTarget: String?                 // --open <path-or-magnet>
    var autoplay = false                    // --autoplay: pick the default video without asking
    var fileIndex: Int?                     // --file-index N
    var extraPeers: [String] = []           // --peer ip:port (repeatable; tests)
    var cacheRoot: String?                  // --cache-dir
    var limitMiB: Int64?                    // --limit-mib
    var exitAfter: Double?                  // --exit-after seconds of PLAYBACK (autotest)
    var seekScript: [(after: Double, to: Double)] = []   // --seek-script "10:120,25:30": N seconds after playback starts, seek to M seconds
    var loopback = false                    // --loopback: loopback test engine on 127.0.0.1:<port>
    var listen: String?                     // --listen
    var webSeedMode = 1                     // --web-seeds off|assist|always
    var mute = false                        // --mute
    var audioOutput: String?                // --ao null
    var behindSeconds: Double?, aheadSeconds: Double?
    var startTimeoutGiven = false
    var startTimeout = 300.0                // --start-timeout S: end an autotest if playback never starts
    var openScript: [(after: Double, path: String)] = []   // --open-script "6:/path/b.mkv": N seconds after playback starts, open another item (acceptance runs of source switching)
    var autotestMode: Bool { exitAfter != nil || !seekScript.isEmpty || startTimeoutGiven || !openScript.isEmpty }
}

final class AppController: NSObject, NSTableViewDataSource, NSTableViewDelegate, PlayerSessionDelegate {
    enum Phase { case idle, fetching, choosing, playing, failed(String) }

    let options: AppOptions
    var engine: TPEngine?
    var meta: TPMeta?
    var session: PlayerSession?
    var phase: Phase = .idle { didSet { refreshUI() } }
    var files: [TPMeta.FileEntry] = []
    var telemetry: Telemetry
    var settings = PlayerSettings()
    var autotestStartedAt: Date?
    var autotestSeeksDone = 0
    var autotestOpensDone = 0
    var autotestTimer: Timer?
    var lastSeedAction: Double = 0
    var metaTimer: Timer?
    var fetchStartedAt = Date()
    var openedSource = ""
    var lastLauncherTitle = ""
    var failureTitle = "Could not open this torrent"
    var failureKind = HomeFailureKind.torrent
    var failureRetryable = true
    var failureDetailHasFileName = false                           // the shown message names a recent file: it stays out of telemetry
    private var fadeGeneration = 0
    var switcher = SwitchCoordinator()
    let recents = RecentsStore(defaults: AppDefaults.shared)       // recently played LOCAL videos (never torrents or magnet links); isolated suite in automated tests

    // UI
    let window: NSWindow
    let home = HomeView(frame: NSRect(x: 0, y: 0, width: 640, height: 580))
    var table: NSTableView { home.table }

    init(options: AppOptions) {
        self.options = options
        telemetry = Telemetry()
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 640, height: 580), styleMask: [.titled, .closable, .miniaturizable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
        super.init()
        settings.cacheLimit = (options.limitMiB ?? 2048) << 20
        if let b = options.behindSeconds { settings.behindSeconds = b }
        if let a = options.aheadSeconds { settings.aheadSeconds = a }
        settings.webSeedMode = options.webSeedMode; settings.mute = options.mute; settings.audioOutput = options.audioOutput
        buildUI()
    }

    // MARK: engine
    func ensureEngine() throws -> TPEngine {
        if let e = engine { return e }
        let home = FileManager.default.homeDirectoryForCurrentUser
        let cache = options.cacheRoot ?? home.appendingPathComponent("Library/Caches/TorrentPlayer/stream").path     // approved default; separate from persistent downloads
        let e = try TPEngine(cacheRoot: cache, evidenceDir: nil, listen: options.listen ?? (options.loopback ? "127.0.0.1:42901" : nil), loopback: options.loopback)
        engine = e
        telemetry.emit("engine_started", ["cache_root": cache, "loopback": options.loopback])
        return e
    }

    // MARK: UI construction
    func buildUI() {
        window.title = "TorrentPlayer — Open"; window.isReleasedWhenClosed = false          // the title stays for Mission Control / accessibility; the mpv window is found by its own title "TorrentPlayer"
        window.titlebarAppearsTransparent = true; window.titleVisibility = .hidden; window.isMovableByWindowBackground = true
        window.minSize = NSSize(width: 560, height: 540)
        window.contentView = home
        home.onDrop = { [weak self] d in switch d { case .files(let f): self?.open(requests: f); case .magnet(let m): self?.openMagnet(m) } }
        home.onChooseFile = { [weak self] in self?.chooseVideo() }
        home.onChooseTorrent = { [weak self] in self?.chooseTorrent() }
        home.onPasteMagnet = { [weak self] in self?.pasteMagnet() }
        home.onOpenLink = { [weak self] link in self?.openMagnet(link) }
        home.onAction = { [weak self] a in self?.perform(a) }
        home.onOpenRecent = { [weak self] id in self?.openRecent(id: id) }
        home.onRemoveRecent = { [weak self] id in self?.removeRecent(id: id) }
        home.onClearRecents = { [weak self] in self?.confirmClearRecents() }
        NotificationCenter.default.addObserver(forName: NSWindow.didBecomeKeyNotification, object: window, queue: .main) { [weak self] _ in self?.refreshRecents() }      // files may have been moved while the app was in the background
        home.table.dataSource = self; home.table.delegate = self; home.table.doubleAction = #selector(playSelected); home.table.target = self
        window.center()
        refreshUI()
        refreshRecents()
    }

    /// The launcher phase as the presentation sees it.
    var homeState: HomeState {
        switch phase {
        case .idle: return .idle
        case .fetching: let st = meta?.status(); return .fetching(fromMagnet: meta?.fromMagnet == true, peers: st?.peers ?? 0, elapsed: st?.elapsed ?? 0)
        case .choosing: return .choosing(name: meta?.name, hasVideo: files.contains(where: { $0.isVideo }))
        case .playing: return .playing
        case .failed(let m): return .failed(kind: failureKind, title: failureTitle, message: m, retryable: failureRetryable)
        }
    }

    func refreshUI() {
        let p = HomePresentation.make(homeState)
        if p.title != lastLauncherTitle { lastLauncherTitle = p.title; telemetry.emit("ui_launcher", ["title": p.title, "detail": (p.mode == .error && failureDetailHasFileName) ? "" : p.detail]) }
        home.apply(p)
        if p.mode == .list { home.setPlayEnabled(table.selectedRow >= 0) }
    }

    // MARK: recent files
    func refreshRecents() {
        home.setRecents(recents.entries.map { RecentRowModel.make(entry: $0, availability: recents.availability(of: $0)) })
    }

    /// Opens a recent through the normal route (OpenClassifier -> SwitchCoordinator). Only the very file the entry stands for is opened; if it cannot be found the user is told and nothing else is opened.
    func openRecent(id: String) {
        guard let e = recents.entry(id: id) else { refreshRecents(); return }
        switch recents.availability(of: e) {
        case .available(let path):
            telemetry.emit("recent_open", [:])                                           // no file name or path in telemetry
            open(requests: [path])
        case .missing, .inaccessible:
            let why = recents.availability(of: e)
            telemetry.emit("recent_unavailable", ["reason": why == .inaccessible ? "inaccessible" : "missing"])
            openedSource = ""; setFailure(.file, title: "Could not open this file", retryable: false); failureDetailHasFileName = true
            showHome()
            phase = .failed(why == .inaccessible ? "“\(e.name)” cannot be read (permission denied). Check its permissions, or remove it from Recents."
                                                 : "“\(e.name)” is no longer at its last location. It may have been moved to another disk, renamed or deleted. Open it again with Open File…, or remove it from Recents.")
            refreshRecents()
        }
    }
    func removeRecent(id: String) {
        let name = recents.entry(id: id)?.name
        recents.remove(id: id); refreshRecents()
        if let n = name { NSAccessibility.post(element: NSApp as Any, notification: .announcementRequested, userInfo: [.announcement: "Removed \(n) from Recents"]) }
    }
    /// Clear All asks first: it removes the list only, never the files.
    func confirmClearRecents() {
        guard !recents.entries.isEmpty else { return }
        let a = NSAlert()
        a.messageText = "Clear all recent files?"
        a.informativeText = "This removes the list only. Your video files are not deleted."
        a.alertStyle = .warning
        let clear = a.addButton(withTitle: "Clear All"); clear.hasDestructiveAction = true
        a.addButton(withTitle: "Cancel")
        showHome()
        a.beginSheetModal(for: window) { [weak self] r in
            guard r == .alertFirstButtonReturn, let self = self else { return }
            self.telemetry.emit("recents_cleared", ["count": self.recents.entries.count])
            self.recents.clear(); self.refreshRecents()
        }
    }

    func perform(_ a: HomeAction) {
        switch a {
        case .cancel: cancelFetch()
        case .play: playSelected()
        case .tryAgain: retryOpen()
        case .openFile: chooseVideo()
        case .editLink: phase = .idle; home.focusMagnetField()
        case .startOver: phase = .idle
        }
    }

    // MARK: actions
    /// File > Open… (any supported item).
    @objc func chooseFile() { presentOpenPanel(types: OpenClassifier.panelContentTypes, message: "Choose a video or a .torrent file") }
    /// The home screen's Open File… button: videos only.
    func chooseVideo() {
        var types: [UTType] = [.movie, .video, .mpeg4Movie, .quickTimeMovie, .avi]
        for e in OpenClassifier.videoExtensions.sorted() { if let t = UTType(filenameExtension: e), !types.contains(t) { types.append(t) } }
        presentOpenPanel(types: types, message: "Choose a video to play")
    }
    /// The home screen's Open Torrent… button (and File > Open Torrent…): .torrent files only.
    @objc func chooseTorrent() {
        presentOpenPanel(types: UTType(filenameExtension: "torrent").map { [$0] } ?? [.data], message: "Choose a .torrent file")
    }
    private func presentOpenPanel(types: [UTType], message: String) {
        showHome()
        let p = NSOpenPanel(); p.allowedContentTypes = types; p.allowsMultipleSelection = false; p.canChooseDirectories = false; p.message = message
        p.beginSheetModal(for: window) { [weak self] r in if r == .OK, let u = p.url { self?.open(requests: [u.path]) } }
    }
    @objc func pasteMagnet() { openMagnet((NSPasteboard.general.string(forType: .string) ?? "").trimmingCharacters(in: .whitespacesAndNewlines), fromClipboard: true) }
    @objc func cancelFetch() { metaTimer?.invalidate(); meta?.cancel(); telemetry.emit("metadata_cancelled", [:]); meta = nil; phase = .idle }
    @objc func retryOpen() { if !openedSource.isEmpty { open(source: openedSource) } else { phase = .idle } }
    @objc func playSelected() { let r = table.selectedRow; guard r >= 0, r < files.count else { return }; startPlayback(file: files[r]) }

    // MARK: home <-> playback transition (a short fade of the home window; instant under Reduce Motion)
    func hideHome() {
        fadeGeneration += 1; let gen = fadeGeneration
        let d = HomeMotion.duration(HomeMotion.fadeOut, reduceMotion: home.environment.reduceMotion)
        guard window.isVisible, d > 0 else { window.orderOut(nil); return }
        window.level = .floating; window.ignoresMouseEvents = true                      // stays above the appearing video window while it fades
        NSAnimationContext.runAnimationGroup({ c in c.duration = d; c.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut); self.window.animator().alphaValue = 0 }, completionHandler: { [weak self] in
            guard let self = self, gen == self.fadeGeneration else { return }
            self.window.orderOut(nil); self.window.alphaValue = 1; self.window.level = .normal; self.window.ignoresMouseEvents = false
        })
    }
    func showHome() {
        fadeGeneration += 1                                                             // cancels a fade-out still running
        window.level = .normal; window.ignoresMouseEvents = false
        refreshRecents()
        let d = HomeMotion.duration(HomeMotion.fadeIn, reduceMotion: home.environment.reduceMotion)
        if !window.isVisible { window.alphaValue = d > 0 ? 0 : 1 }
        window.makeKeyAndOrderFront(nil)
        if window.alphaValue < 1 { NSAnimationContext.runAnimationGroup { c in c.duration = d; c.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut); self.window.animator().alphaValue = 1 } }
    }

    private func setFailure(_ kind: HomeFailureKind, title: String, retryable: Bool) { failureKind = kind; failureTitle = title; failureRetryable = retryable; failureDetailHasFileName = false }

    func open(source: String) { open(requests: [source]) }

    /// The one entry for every open request (menu, button, drop, Finder, Dock, --open): classify, keep the first openable item, then start it
    /// (replacing a running playback only after that session's asynchronous teardown).
    func open(requests: [String]) {
        guard let target = OpenClassifier.first(of: requests) else { return }
        switch target {
        case .unsupported(let why), .missing(let why):
            telemetry.emit("open_failed", ["source": "request", "message": why])
            if session != nil { NSSound.beep(); return }                              // keep playing: a bad request must not stop the current video
            openedSource = requests.first ?? ""; setFailure(.file, title: "Could not open this file", retryable: false)
            showHome(); phase = .failed(why)
        case .torrentFile, .magnet, .localVideo:
            switch switcher.request(target, hasSession: session != nil, sessionEnding: session?.isEnding ?? false) {
            case .startNow(let t): begin(t)
            case .endCurrentThenStart: telemetry.emit("session_replace", ["reason": "new item opened"]); session?.end(reason: "replaced by a new item")
            case .waitForTeardown: break
            }
        }
    }

    /// Starts an item. Only ever called when no session exists.
    func begin(_ t: OpenTarget) {
        switch t {
        case .torrentFile(let p): openTorrentFile(p)
        case .magnet(let m): openMagnet(m)
        case .localVideo(let p): startLocal(path: p)
        case .unsupported, .missing: break
        }
    }

    func startLocal(path: String) {
        if case .fetching = phase { cancelFetch() }
        openedSource = path; setFailure(.file, title: "Could not open this file", retryable: true)
        var facts = OpenClassifier.telemetryFacts(path: path); facts["source"] = "local_file"
        telemetry.emit("open_local", facts)                                            // extension and size only
        let s = PlayerSession(localFile: path, settings: settings, telemetry: telemetry)
        s.delegate = self
        s.onRetryRequested = { [weak self] in self?.open(requests: [path]) }
        session = s; phase = .playing
        hideHome()
        s.start()
        if options.autotestMode { armAutotest() }
    }

    func openTorrentFile(_ path: String) {
        showHome()
        openedSource = path; setFailure(.torrent, title: "Could not open this torrent", retryable: true)
        do {
            let e = try ensureEngine()
            let m = try TPMeta(engine: e, file: path)
            options.extraPeers.forEach { m.addPeer($0) }
            telemetry.emit("open_torrent_file", ["file": (path as NSString).lastPathComponent])
            metaReady(m)
        } catch { telemetry.emit("open_failed", ["source": "file", "message": "\(error)"]); phase = .failed("\(error)") }
    }

    func openMagnet(_ link: String, fromClipboard: Bool = false) {
        showHome()
        if fromClipboard, !link.isEmpty { home.magnetField.stringValue = link }
        guard link.hasPrefix("magnet:?") else {
            setFailure(.magnetSyntax, title: HomePresentation.magnetSyntaxTitle, retryable: false)
            phase = .failed(HomePresentation.magnetSyntaxMessage); return
        }
        openedSource = link; setFailure(.torrent, title: "Could not open this torrent", retryable: true)
        do {
            let e = try ensureEngine()
            let m = try TPMeta(engine: e, magnet: link)
            options.extraPeers.forEach { m.addPeer($0) }
            meta = m; fetchStartedAt = Date(); phase = .fetching
            telemetry.emit("open_magnet", [:])
            metaTimer?.invalidate()
            metaTimer = Timer.scheduledTimer(withTimeInterval: 0.3, repeats: true) { [weak self] t in
                guard let self = self, let m = self.meta else { t.invalidate(); return }
                let st = m.status()
                switch st.state {
                case .fetching: self.refreshUI()
                case .ready: t.invalidate(); self.metaReady(m)
                case .failed: t.invalidate(); self.telemetry.emit("metadata_failed", ["error": st.error, "peers": st.peers, "elapsed_s": st.elapsed]); self.phase = .failed(st.error)
                }
            }
        } catch { telemetry.emit("open_failed", ["source": "magnet", "message": "\(error)"]); phase = .failed("\(error)") }
    }

    func metaReady(_ m: TPMeta) {
        meta = m; files = m.files()
        telemetry.emit("metadata_ready", ["name": m.name, "files": files.count, "fetch_s": Date().timeIntervalSince(fetchStartedAt), "from_magnet": m.fromMagnet])
        let defIdx = options.fileIndex ?? m.defaultVideo
        if options.autoplay, defIdx >= 0, defIdx < files.count { phase = .choosing; startPlayback(file: files[defIdx]); return }
        phase = .choosing
        if let i = files.firstIndex(where: { $0.index == defIdx }) { table.selectRowIndexes(IndexSet(integer: i), byExtendingSelection: false); table.scrollRowToVisible(i) }
        home.setPlayEnabled(table.selectedRow >= 0)
        showHome()
    }

    func startPlayback(file: TPMeta.FileEntry) {
        guard let e = engine, let m = meta else { return }
        telemetry.emit("play_requested", ["file": file.path, "size": file.size, "is_video": file.isVideo])
        let s = PlayerSession(engine: e, meta: m, fileIndex: file.index, settings: settings, displayName: (file.path as NSString).lastPathComponent, telemetry: telemetry)
        s.delegate = self
        s.onRetryRequested = { [weak self] in self?.startPlayback(file: file) }
        session = s; phase = .playing
        hideHome()
        s.start()
        if options.autotestMode { armAutotest() }
    }

    /// The first frame of a LOCAL video was decoded: only now does it count as successfully opened.
    func playerSessionDidStartPlayback(_ s: PlayerSession) {
        guard s.isLocal, let path = s.localPath, recents.record(path: path) else { return }
        refreshRecents()
    }

    func playerSessionDidEnd(_ s: PlayerSession, reason: String) {
        if session === s { session = nil }
        if case .playing = phase { phase = s.isLocal ? .idle : .choosing }           // after a local video there is no torrent file list to go back to
        if let next = switcher.sessionDidEnd(otherSessionActive: session != nil) { begin(next); return }   // a queued replacement starts only now, after the teardown
        if reason == "retry" { return }
        if options.autotestMode { autotestFinish(reason: reason); return }
        showHome()
    }

    // MARK: autotest (command-line driven acceptance runs; no UI automation needed)
    func armAutotest() {
        autotestStartedAt = nil; autotestSeeksDone = 0
        autotestTimer?.invalidate()
        autotestTimer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] t in
            guard let self = self, let s = self.session else { t.invalidate(); return }
            if self.autotestStartedAt == nil { if s.playbackStarted { self.autotestStartedAt = Date() } else { return } }
            let el = Date().timeIntervalSince(self.autotestStartedAt!)
            if self.autotestOpensDone < self.options.openScript.count, el >= self.options.openScript[self.autotestOpensDone].after {
                let item = self.options.openScript[self.autotestOpensDone]; self.autotestOpensDone += 1
                self.open(requests: [item.path]); return
            }
            if self.autotestSeeksDone < self.options.seekScript.count, el >= self.options.seekScript[self.autotestSeeksDone].after {
                s.seek(to: self.options.seekScript[self.autotestSeeksDone].to, exact: false); self.autotestSeeksDone += 1
            }
            if let ex = self.options.exitAfter, el >= ex { t.invalidate(); s.end(reason: "autotest finished") }
        }
        // a session that never starts must not hang an unattended run
        DispatchQueue.main.asyncAfter(deadline: .now() + options.startTimeout) { [weak self] in
            if let s = self?.session, self?.autotestStartedAt == nil { s.end(reason: "autotest: playback never started") }
        }
    }
    func autotestFinish(reason: String) {
        print("AUTOTEST DONE reason=\(reason) telemetry=\(telemetry.url.path)")
        fflush(stdout)
        telemetry.close()
        NSApp.terminate(nil)
    }

    // MARK: table
    func numberOfRows(in tableView: NSTableView) -> Int { files.count }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let f = files[row]
        let id = tableColumn?.identifier.rawValue ?? ""
        let text: String
        switch id { case "size": text = ByteCountFormatter.string(fromByteCount: f.size, countStyle: .file); case "kind": text = f.isVideo ? "Video" : "Other"; default: text = f.path }
        let cell = NSTextField(labelWithString: text); cell.lineBreakMode = .byTruncatingMiddle
        cell.textColor = f.isVideo ? .labelColor : .secondaryLabelColor
        return cell
    }
    func tableViewSelectionDidChange(_ n: Notification) { home.setPlayEnabled(table.selectedRow >= 0) }
}
