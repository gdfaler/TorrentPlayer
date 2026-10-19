// Launcher.swift: the window shown before playback: open a .torrent or magnet link, watch metadata fetch (with a named "No peers found" state),
// choose the video file, press Play. Also owns the app-level flow (open -> metadata -> file choice -> player session -> back).
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

final class DropView: NSView {
    var onFiles: (([String]) -> Void)?                // dropped files, in the order macOS provides them
    var onMagnet: ((String) -> Void)?
    let label = NSTextField(labelWithString: "Drop a video, a .torrent file or a magnet link here")
    private var hover = false { didSet { needsDisplay = true } }
    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([.fileURL, .string, .URL])
        label.font = .systemFont(ofSize: 14); label.textColor = .secondaryLabelColor; label.alignment = .center
        addSubview(label)
    }
    required init?(coder: NSCoder) { fatalError() }
    override func layout() { super.layout(); label.frame = NSRect(x: 10, y: bounds.midY - 10, width: bounds.width - 20, height: 20) }
    override func draw(_ dirtyRect: NSRect) {
        let r = bounds.insetBy(dx: 1, dy: 1)
        let p = NSBezierPath(roundedRect: r, xRadius: 12, yRadius: 12); p.setLineDash([6, 4], count: 2, phase: 0); p.lineWidth = 1.5
        (hover ? NSColor.controlAccentColor : NSColor.tertiaryLabelColor).setStroke(); p.stroke()
        if hover { NSColor.controlAccentColor.withAlphaComponent(0.08).setFill(); p.fill() }
    }
    override func draggingEntered(_ s: NSDraggingInfo) -> NSDragOperation { hover = true; return .copy }
    override func draggingExited(_ s: NSDraggingInfo?) { hover = false }
    override func performDragOperation(_ s: NSDraggingInfo) -> Bool {
        hover = false
        let pb = s.draggingPasteboard
        if let urls = pb.readObjects(forClasses: [NSURL.self], options: nil) as? [URL], !urls.isEmpty {
            let files = urls.filter { $0.isFileURL }.map { $0.path }
            if !files.isEmpty { onFiles?(files); return true }
            if let u = urls.first, u.scheme == "magnet" { onMagnet?(u.absoluteString); return true }
        }
        if let str = pb.string(forType: .string)?.trimmingCharacters(in: .whitespacesAndNewlines), str.hasPrefix("magnet:?") { onMagnet?(str); return true }
        return false
    }
}

final class AppController: NSObject, NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate, PlayerSessionDelegate {
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
    var switcher = SwitchCoordinator()

    // UI
    let window: NSWindow
    let drop = DropView(frame: .zero)
    let magnetField = NSTextField()
    let statusLabel = NSTextField(labelWithString: "")
    let detailLabel = NSTextField(labelWithString: "")
    let spinner = NSProgressIndicator()
    let cancelButton = NSButton(title: "Cancel", target: nil, action: nil)
    let table = NSTableView()
    let scroll = NSScrollView()
    let playButton = NSButton(title: "Play", target: nil, action: nil)
    let retryButton = NSButton(title: "Try again", target: nil, action: nil)
    var openButton = NSButton(title: "Open…", target: nil, action: nil)

    init(options: AppOptions) {
        self.options = options
        telemetry = Telemetry()
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 620, height: 470), styleMask: [.titled, .closable, .miniaturizable], backing: .buffered, defer: false)
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
        window.title = "TorrentPlayer — Open"; window.isReleasedWhenClosed = false
        let content = window.contentView!
        drop.onFiles = { [weak self] files in self?.open(requests: files) }
        drop.onMagnet = { [weak self] m in self?.openMagnet(m) }
        openButton.target = self; openButton.action = #selector(chooseFile); openButton.bezelStyle = .rounded
        let pasteButton = NSButton(title: "Paste Magnet Link", target: self, action: #selector(pasteMagnet)); pasteButton.bezelStyle = .rounded
        magnetField.placeholderString = "magnet:?xt=urn:btih:…"; magnetField.delegate = self; magnetField.target = self; magnetField.action = #selector(magnetEntered)
        let goButton = NSButton(title: "Open", target: self, action: #selector(magnetEntered)); goButton.bezelStyle = .rounded
        statusLabel.font = .systemFont(ofSize: 15, weight: .semibold); statusLabel.lineBreakMode = .byTruncatingTail
        detailLabel.font = .systemFont(ofSize: 12); detailLabel.textColor = .secondaryLabelColor; detailLabel.lineBreakMode = .byTruncatingTail
        spinner.style = .spinning; spinner.controlSize = .small; spinner.isIndeterminate = true; spinner.isDisplayedWhenStopped = false
        cancelButton.target = self; cancelButton.action = #selector(cancelFetch); cancelButton.bezelStyle = .rounded
        retryButton.target = self; retryButton.action = #selector(retryOpen); retryButton.bezelStyle = .rounded
        playButton.target = self; playButton.action = #selector(playSelected); playButton.bezelStyle = .rounded; playButton.keyEquivalent = "\r"
        let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("name")); col.title = "File"; col.width = 380
        let col2 = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("size")); col2.title = "Size"; col2.width = 90
        let col3 = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("kind")); col3.title = "Type"; col3.width = 80
        [col, col2, col3].forEach { table.addTableColumn($0) }
        table.dataSource = self; table.delegate = self; table.doubleAction = #selector(playSelected); table.target = self
        scroll.documentView = table; scroll.hasVerticalScroller = true; scroll.borderType = .bezelBorder

        let buttons = NSStackView(views: [openButton, pasteButton]); buttons.spacing = 8
        let magnetRow = NSStackView(views: [magnetField, goButton]); magnetRow.spacing = 8
        magnetField.setContentHuggingPriority(.defaultLow, for: .horizontal)
        let statusRow = NSStackView(views: [spinner, statusLabel]); statusRow.spacing = 8
        let actionRow = NSStackView(views: [cancelButton, retryButton, playButton]); actionRow.spacing = 8
        let stack = NSStackView(views: [drop, buttons, magnetRow, statusRow, detailLabel, scroll, actionRow])
        stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 10
        stack.edgeInsets = NSEdgeInsets(top: 18, left: 20, bottom: 16, right: 20)
        stack.translatesAutoresizingMaskIntoConstraints = false
        content.addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: content.leadingAnchor), stack.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            stack.topAnchor.constraint(equalTo: content.topAnchor), stack.bottomAnchor.constraint(equalTo: content.bottomAnchor),
            drop.heightAnchor.constraint(equalToConstant: 84), drop.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -40),
            magnetRow.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -40),
            scroll.widthAnchor.constraint(equalTo: stack.widthAnchor, constant: -40), scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 110),
            statusLabel.widthAnchor.constraint(lessThanOrEqualToConstant: 520), detailLabel.widthAnchor.constraint(lessThanOrEqualToConstant: 560)])
        window.center()
        refreshUI()
    }

    func refreshUI() {
        let busy: Bool, showList: Bool
        var title = "", detail = ""
        var cancelShown = false, retryShown = false
        switch phase {
        case .idle: busy = false; showList = false; title = "Open a video or a torrent to start"; detail = "Local videos play straight from disk. Torrents stream: nothing is downloaded to your Downloads folder, the cache is bounded and reclaimed automatically."
        case .fetching:
            busy = true; showList = false; cancelShown = true
            let st = meta?.status(); let p = st?.peers ?? 0; let e = st?.elapsed ?? 0
            if meta?.fromMagnet == true {
                if p == 0 && e > 15 { title = "No peers found"; detail = "Still searching for peers that have this torrent's metadata (\(Int(e)) s). The link may be dead or you may be offline." }
                else if p == 0 { title = "Looking for peers…"; detail = "Fetching torrent metadata from the swarm" }
                else { title = "Fetching torrent metadata…"; detail = "\(p) peer\(p == 1 ? "" : "s") found (\(Int(e)) s)" }
            } else { title = "Reading torrent…"; detail = "" }
        case .choosing: busy = false; showList = true; title = meta?.name ?? "Choose a file"
            detail = files.contains(where: { $0.isVideo }) ? "Choose a video file and press Play." : "No playable video found in this torrent. The files are listed below."
        case .playing: busy = false; showList = false; title = "Playing"; detail = ""
        case .failed(let m): busy = false; showList = false; title = failureTitle; detail = m; retryShown = true
        }
        if title != lastLauncherTitle { lastLauncherTitle = title; telemetry.emit("ui_launcher", ["title": title, "detail": detail]) }
        statusLabel.stringValue = title; detailLabel.stringValue = detail
        if busy { spinner.startAnimation(nil) } else { spinner.stopAnimation(nil) }
        scroll.isHidden = !showList; playButton.isHidden = !showList; cancelButton.isHidden = !cancelShown; retryButton.isHidden = !retryShown
        if showList { table.reloadData(); playButton.isEnabled = table.selectedRow >= 0 }
    }

    // MARK: actions
    @objc func chooseFile() {
        window.makeKeyAndOrderFront(nil)
        let p = NSOpenPanel(); p.allowedContentTypes = OpenClassifier.panelContentTypes; p.allowsMultipleSelection = false; p.canChooseDirectories = false
        p.beginSheetModal(for: window) { [weak self] r in if r == .OK, let u = p.url { self?.open(requests: [u.path]) } }
    }
    @objc func pasteMagnet() { if let s = NSPasteboard.general.string(forType: .string)?.trimmingCharacters(in: .whitespacesAndNewlines) { magnetField.stringValue = s; openMagnet(s) } }
    @objc func magnetEntered() { openMagnet(magnetField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)) }
    @objc func cancelFetch() { metaTimer?.invalidate(); meta?.cancel(); telemetry.emit("metadata_cancelled", [:]); meta = nil; phase = .idle }
    @objc func retryOpen() { if !openedSource.isEmpty { open(source: openedSource) } else { phase = .idle } }
    @objc func playSelected() { let r = table.selectedRow; guard r >= 0, r < files.count else { return }; startPlayback(file: files[r]) }

    func open(source: String) { open(requests: [source]) }

    /// The one entry for every open request (menu, button, drop, Finder, Dock, --open): classify, keep the first openable item, then start it
    /// (replacing a running playback only after that session's asynchronous teardown).
    func open(requests: [String]) {
        guard let target = OpenClassifier.first(of: requests) else { return }
        switch target {
        case .unsupported(let why), .missing(let why):
            telemetry.emit("open_failed", ["source": "request", "message": why])
            if session != nil { NSSound.beep(); return }                              // keep playing: a bad request must not stop the current video
            openedSource = requests.first ?? ""; failureTitle = "Could not open this file"
            window.makeKeyAndOrderFront(nil); phase = .failed(why)
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
        case .torrentFile(let p): failureTitle = "Could not open this torrent"; openTorrentFile(p)
        case .magnet(let m): failureTitle = "Could not open this torrent"; openMagnet(m)
        case .localVideo(let p): startLocal(path: p)
        case .unsupported, .missing: break
        }
    }

    func startLocal(path: String) {
        if case .fetching = phase { cancelFetch() }
        openedSource = path; failureTitle = "Could not open this file"
        var facts = OpenClassifier.telemetryFacts(path: path); facts["source"] = "local_file"
        telemetry.emit("open_local", facts)                                            // extension and size only
        let s = PlayerSession(localFile: path, settings: settings, telemetry: telemetry)
        s.delegate = self
        s.onRetryRequested = { [weak self] in self?.open(requests: [path]) }
        session = s; phase = .playing
        window.orderOut(nil)
        s.start()
        if options.autotestMode { armAutotest() }
    }

    func openTorrentFile(_ path: String) {
        window.makeKeyAndOrderFront(nil)
        openedSource = path
        do {
            let e = try ensureEngine()
            let m = try TPMeta(engine: e, file: path)
            options.extraPeers.forEach { m.addPeer($0) }
            telemetry.emit("open_torrent_file", ["file": (path as NSString).lastPathComponent])
            metaReady(m)
        } catch { telemetry.emit("open_failed", ["source": "file", "message": "\(error)"]); phase = .failed("\(error)") }
    }

    func openMagnet(_ link: String) {
        window.makeKeyAndOrderFront(nil)
        guard link.hasPrefix("magnet:?") else { phase = .failed("That is not a magnet link. It should start with “magnet:?”."); return }
        openedSource = link
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
        if let i = files.firstIndex(where: { $0.index == defIdx }) { table.selectRowIndexes(IndexSet(integer: i), byExtendingSelection: false) }
        playButton.isEnabled = table.selectedRow >= 0
        window.makeKeyAndOrderFront(nil)
    }

    func startPlayback(file: TPMeta.FileEntry) {
        guard let e = engine, let m = meta else { return }
        telemetry.emit("play_requested", ["file": file.path, "size": file.size, "is_video": file.isVideo])
        let s = PlayerSession(engine: e, meta: m, fileIndex: file.index, settings: settings, displayName: (file.path as NSString).lastPathComponent, telemetry: telemetry)
        s.delegate = self
        s.onRetryRequested = { [weak self] in self?.startPlayback(file: file) }
        session = s; phase = .playing
        window.orderOut(nil)
        s.start()
        if options.autotestMode { armAutotest() }
    }

    func playerSessionDidEnd(_ s: PlayerSession, reason: String) {
        if session === s { session = nil }
        if case .playing = phase { phase = s.isLocal ? .idle : .choosing }           // after a local video there is no torrent file list to go back to
        if let next = switcher.sessionDidEnd(otherSessionActive: session != nil) { begin(next); return }   // a queued replacement starts only now, after the teardown
        if reason == "retry" { return }
        if options.autotestMode { autotestFinish(reason: reason); return }
        window.makeKeyAndOrderFront(nil)
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
    func tableViewSelectionDidChange(_ n: Notification) { playButton.isEnabled = table.selectedRow >= 0 }
    func controlTextDidEndEditing(_ obj: Notification) {}
}
