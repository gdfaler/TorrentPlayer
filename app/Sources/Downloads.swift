// Downloads.swift: Download & Play in the app. The C++ core (tp_dl_* in tp_c.h) does the downloading, verification and persistence; this file owns the Swift side:
// the wrapper, the network path monitor, the Downloads window and the "How do you want to watch" sheet. The finished file plays through the existing local playback path.
import AppKit
import Network

private func cstr<T>(_ t: T) -> String {
    var t = t
    return withUnsafePointer(to: &t) { $0.withMemoryRebound(to: CChar.self, capacity: MemoryLayout<T>.size) { String(cString: $0) } }
}

final class TPDownloads {
    private var ptr: OpaquePointer?
    let stateDir: String

    init(stateDir: String, listen: String?, loopback: Bool, autoResume: Bool) throws {
        self.stateDir = stateDir
        var err = [CChar](repeating: 0, count: 256)
        var p: OpaquePointer?
        func make(_ l: UnsafePointer<CChar>?) {
            stateDir.withCString { sd in
                var cfg = tp_dl_config(state_dir: sd, listen_interfaces: l, loopback_test_mode: loopback ? 1 : 0, enable_dht: loopback ? 0 : 1, auto_resume_on_network: autoResume ? 1 : 0)
                p = tp_dl_create(&cfg, &err, err.count)
            }
        }
        if let l = listen { l.withCString { make($0) } } else { make(nil) }
        guard let h = p else { throw TPError(String(cString: err)) }
        ptr = h
    }
    deinit { shutdown() }
    /// Graceful: the core writes the resume data of every download first (up to a few seconds).
    func shutdown() { if let p = ptr { ptr = nil; tp_dl_destroy(p) } }

    func restore() -> Int { ptr.map { Int(tp_dl_restore($0)) } ?? 0 }
    func add(meta: TPMeta, destination: String, files: [Int], autoPlay: Bool) -> Result<String, TPError> {
        guard let p = ptr else { return .failure(TPError("Downloads are not available.")) }
        var id = [CChar](repeating: 0, count: 64), err = [CChar](repeating: 0, count: 300)
        let f = files.map { Int32($0) }
        let ok = f.withUnsafeBufferPointer { tp_dl_add(p, meta.ptr, destination, $0.baseAddress, Int32(f.count), autoPlay ? 1 : 0, &id, id.count, &err, err.count) }
        return ok == 1 ? .success(String(cString: id)) : .failure(TPError(String(cString: err)))
    }
    func list() -> [DownloadItem] {
        guard let p = ptr else { return [] }
        var buf = [tp_dl_status](repeating: tp_dl_status(), count: 64)
        let n = Int(tp_dl_list(p, &buf, 64))
        return buf.prefix(n).map { s in
            DownloadItem(id: cstr(s.id), name: cstr(s.name), savePath: cstr(s.save_path), primaryFile: cstr(s.primary_file), error: cstr(s.error),
                         state: DownloadState(rawValue: Int(s.state)) ?? .failed, selectedBytes: s.selected_bytes, verifiedBytes: s.verified_bytes,
                         downloadRate: Int(s.download_rate), uploadRate: Int(s.upload_rate), peers: Int(s.peers), webSeeds: Int(s.web_seeds), etaSeconds: s.eta_s,
                         userPaused: s.user_paused != 0, autoPlay: s.auto_play != 0, checkingProgress: s.checking_progress)
        }
    }
    func pause(_ id: String) { if let p = ptr { _ = tp_dl_pause(p, id) } }
    func resume(_ id: String) { if let p = ptr { _ = tp_dl_resume(p, id) } }
    func cancel(_ id: String, deleteFiles: Bool) { if let p = ptr { _ = tp_dl_cancel(p, id, deleteFiles ? 1 : 0) } }
    func setNetwork(_ online: Bool) { if let p = ptr { tp_dl_set_network(p, online ? 1 : 0) } }
    func setAutoResume(_ on: Bool) { if let p = ptr { tp_dl_set_auto_resume(p, on ? 1 : 0) } }
    func saveAll() { if let p = ptr { tp_dl_save_all(p) } }
}

/// Reports whether the Mac has a usable network path (one monitor for the whole app). A satisfied path is only a hint: the core still waits for real progress before it says "Downloading".
final class NetworkMonitor {
    private let monitor = NWPathMonitor()
    private let queue = DispatchQueue(label: "TorrentPlayer.network-path")
    private(set) var online = true, expensive = false, constrained = false
    var onChange: ((Bool) -> Void)?
    func start() {
        monitor.pathUpdateHandler = { [weak self] path in
            DispatchQueue.main.async {
                guard let self = self else { return }
                self.expensive = path.isExpensive; self.constrained = path.isConstrained
                let now = path.status == .satisfied
                if now != self.online { self.online = now; self.onChange?(now) } else if self.onChange != nil && !self.started { self.started = true; self.onChange?(now) }
            }
        }
        monitor.start(queue: queue)
    }
    private var started = false
    func stop() { monitor.cancel() }
}

// MARK: - Downloads window

final class DownloadRowView: NSTableCellView {
    let title = NSTextField(labelWithString: ""), detail = NSTextField(wrappingLabelWithString: ""), bar = NSProgressIndicator()
    let pauseButton = NSButton(title: "Pause", target: nil, action: nil), playButton = NSButton(title: "Play", target: nil, action: nil)
    let revealButton = NSButton(title: "Show in Finder", target: nil, action: nil), cancelButton = NSButton(title: "Cancel", target: nil, action: nil)
    /// The download this (reused) row currently shows. Handlers receive it at click time and look the download up in the latest list.
    var itemID = ""
    var onPauseResume: ((String) -> Void)?, onPlay: ((String) -> Void)?, onReveal: ((String) -> Void)?, onCancel: ((String) -> Void)?
    private var lastModel: DownloadRowModel?

    override init(frame: NSRect) {
        super.init(frame: frame)
        title.font = .systemFont(ofSize: 13, weight: .semibold); title.lineBreakMode = .byTruncatingMiddle
        detail.font = .systemFont(ofSize: 11); detail.textColor = .secondaryLabelColor; detail.maximumNumberOfLines = 2
        bar.style = .bar; bar.isIndeterminate = false; bar.minValue = 0; bar.maxValue = 1
        for b in [pauseButton, playButton, revealButton, cancelButton] { b.bezelStyle = .rounded; b.controlSize = .small; b.target = self }
        pauseButton.action = #selector(pr); playButton.action = #selector(pl); revealButton.action = #selector(rv); cancelButton.action = #selector(cn)
        let buttons = NSStackView(views: [pauseButton, playButton, revealButton, cancelButton]); buttons.orientation = .horizontal; buttons.spacing = 6
        let col = NSStackView(views: [title, bar, detail, buttons]); col.orientation = .vertical; col.alignment = .leading; col.spacing = 4
        col.translatesAutoresizingMaskIntoConstraints = false; addSubview(col)
        bar.translatesAutoresizingMaskIntoConstraints = false
        NSLayoutConstraint.activate([col.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 14), col.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -14),
                                     col.topAnchor.constraint(equalTo: topAnchor, constant: 8), bar.widthAnchor.constraint(equalTo: col.widthAnchor), detail.widthAnchor.constraint(equalTo: col.widthAnchor)])
    }
    required init?(coder: NSCoder) { fatalError() }
    @objc private func pr() { onPauseResume?(itemID) }
    @objc private func pl() { onPlay?(itemID) }
    @objc private func rv() { onReveal?(itemID) }
    @objc private func cn() { onCancel?(itemID) }

    func configure(_ m: DownloadRowModel) {
        guard m != lastModel else { return }
        lastModel = m
        title.stringValue = m.title; detail.stringValue = m.detail
        bar.isIndeterminate = m.indeterminate
        if m.indeterminate { bar.startAnimation(nil) } else { bar.stopAnimation(nil); bar.doubleValue = m.fraction }
        pauseButton.title = m.canResume ? "Resume" : "Pause"; pauseButton.isEnabled = m.canPause || m.canResume
        playButton.isEnabled = m.canPlay; playButton.isHidden = !m.canPlay
        revealButton.isEnabled = m.canReveal; cancelButton.title = m.cancelTitle
    }
}

final class DownloadsController: NSObject, NSTableViewDataSource, NSTableViewDelegate {
    let window: NSWindow
    private let table = NSTableView()
    private let footer = NSTextField(labelWithString: "")
    private let autoResume = NSButton(checkboxWithTitle: "Automatically resume downloads when the network returns", target: nil, action: nil)
    private var items: [DownloadItem] = []
    private var timer: Timer?
    private var handledAutoPlay: Set<String>
    private let defaults: UserDefaults
    var host: () -> TPDownloads?                       // nil until the first download exists
    var playerBusy: () -> Bool = { false }
    var onPlay: (DownloadItem) -> Void = { _ in }
    var onAutoResumeChanged: (Bool) -> Void = { _ in }
    var networkNote: () -> String = { "" }
    var telemetry: ((String, [String: Any]) -> Void)?

    init(defaults: UserDefaults, host: @escaping () -> TPDownloads?) {
        self.defaults = defaults; self.host = host
        handledAutoPlay = Set(defaults.stringArray(forKey: "dl.handledAutoPlay") ?? [])
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 640, height: 420), styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
        super.init()
        window.title = "Downloads"; window.isReleasedWhenClosed = false; window.minSize = NSSize(width: 520, height: 260)
        let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("d")); col.resizingMask = .autoresizingMask
        table.addTableColumn(col); table.headerView = nil; table.rowHeight = 104; table.dataSource = self; table.delegate = self; table.selectionHighlightStyle = .none
        table.intercellSpacing = NSSize(width: 0, height: 1)
        let scroll = NSScrollView(); scroll.documentView = table; scroll.hasVerticalScroller = true; scroll.drawsBackground = false
        autoResume.target = self; autoResume.action = #selector(autoResumeToggled); autoResume.controlSize = .small
        autoResume.state = DownloadPreferences.load(defaults).autoResumeOnNetwork ? .on : .off
        footer.font = .systemFont(ofSize: 11); footer.textColor = .secondaryLabelColor
        let bottom = NSStackView(views: [autoResume, footer]); bottom.orientation = .vertical; bottom.alignment = .leading; bottom.spacing = 2
        let root = NSStackView(views: [scroll, bottom]); root.orientation = .vertical; root.spacing = 8; root.edgeInsets = NSEdgeInsets(top: 0, left: 14, bottom: 10, right: 14)
        scroll.translatesAutoresizingMaskIntoConstraints = false
        window.contentView = root
        scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 150).isActive = true
        window.center()
    }

    func show() { refresh(); window.makeKeyAndOrderFront(nil); startTimer() }
    func startTimer() { if timer == nil { timer = Timer.scheduledTimer(withTimeInterval: 1.0, repeats: true) { [weak self] _ in self?.refresh() } } }
    var activeCount: Int { items.filter { [.checking, .downloading, .waitingNetwork, .waitingSources, .resuming].contains($0.state) }.count }

    @objc private func autoResumeToggled() {
        var p = DownloadPreferences.load(defaults); p.autoResumeOnNetwork = autoResume.state == .on; p.save(defaults)
        onAutoResumeChanged(p.autoResumeOnNetwork)
    }

    func refresh() {
        let new = host()?.list() ?? []
        let sameShape = new.map { $0.id } == items.map { $0.id }
        let old = items
        items = new
        if sameShape {
            for (i, it) in new.enumerated() where it != old[i] { if let v = table.view(atColumn: 0, row: i, makeIfNecessary: false) as? DownloadRowView { v.itemID = it.id; v.configure(DownloadRowModel.make(it)) } }
        } else { table.reloadData() }
        for (i, it) in new.enumerated() where i < old.count && old[i].id == it.id && old[i].state != it.state { telemetry?("download_state", ["state": "\(it.state)"]) }
        for it in new where DownloadAutoPlay.shouldStart(it, alreadyHandled: handledAutoPlay, playerBusy: playerBusy()) {
            handledAutoPlay.insert(it.id); defaults.set(Array(handledAutoPlay), forKey: "dl.handledAutoPlay")
            telemetry?("download_autoplay", [:])
            onPlay(it)
            break
        }
        footer.stringValue = items.isEmpty ? "No downloads. Open a torrent or magnet link and choose Download & Play." : networkNote()
        if items.isEmpty && !window.isVisible { timer?.invalidate(); timer = nil }
    }

    // MARK: table
    func numberOfRows(in tableView: NSTableView) -> Int { items.count }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let id = NSUserInterfaceItemIdentifier("row")
        let v = (tableView.makeView(withIdentifier: id, owner: nil) as? DownloadRowView) ?? { let r = DownloadRowView(frame: .zero); r.identifier = id; return r }()
        guard row < items.count else { return v }
        let it = items[row]
        v.itemID = it.id; v.configure(DownloadRowModel.make(it))
        // No handler captures a download: each one is given the row's current id and resolves the current state from the latest list when the button is pressed.
        v.onPauseResume = { [weak self] id in self?.pauseOrResume(id) }
        v.onPlay = { [weak self] id in self?.play(id) }
        v.onReveal = { [weak self] id in self?.reveal(id) }
        v.onCancel = { [weak self] id in self?.confirmCancel(id) }
        return v
    }

    private func pauseOrResume(_ id: String) {
        guard let h = host() else { return }
        let current = h.list()                                                    // the very latest state, not the last one drawn
        switch DownloadClick.pauseResume(id: id, in: current) {
        case .pause(let i): h.pause(i)
        case .resume(let i): h.resume(i)
        case .ignore: break
        }
        refresh()
    }
    private func play(_ id: String) {
        guard let h = host() else { return }
        let current = h.list()
        if DownloadClick.playPath(id: id, in: current) != nil, let it = DownloadClick.item(id, in: current) { onPlay(it) }
    }
    private func reveal(_ id: String) {
        guard let h = host() else { return }
        let current = h.list()
        guard let t = DownloadClick.revealTarget(id: id, in: current) else { return }
        if let f = t.file, FileManager.default.fileExists(atPath: f) { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: f)]) }
        else { NSWorkspace.shared.open(URL(fileURLWithPath: t.folder)) }
    }

    /// Cancelling is deliberate and never deletes anything unless the user says so.
    private func confirmCancel(_ id: String) {
        guard let h = host() else { return }
        let current = h.list()
        let a = NSAlert()
        switch DownloadClick.cancelPrompt(id: id, in: current) {
        case .gone: return
        case .completed(let name):
            a.messageText = "Remove “\(name)” from the list?"; a.informativeText = "The downloaded file stays where it is, unless you choose to delete it. Files that were already in the folder before this download started are never deleted."
            a.addButton(withTitle: "Remove from List"); a.addButton(withTitle: "Remove and Delete File"); a.addButton(withTitle: "Keep")
        case .partial(let name, let verified):
            a.messageText = "Cancel the download of “\(name)”?"
            a.informativeText = "\(DownloadFormat.bytes(verified)) are already downloaded. You can keep that partial data (a later download of the same torrent into the same folder continues from it) or delete it. Only files this download created are deleted; files that were already in the folder are never touched."
            a.addButton(withTitle: "Cancel and Keep Partial Data"); a.addButton(withTitle: "Cancel and Delete Partial Data"); a.addButton(withTitle: "Continue Downloading")
        }
        a.alertStyle = .informational
        a.beginSheetModal(for: window) { [weak self] r in
            guard let self = self, let h = self.host() else { return }
            guard r == .alertFirstButtonReturn || r == .alertSecondButtonReturn else { return }
            let delete = r == .alertSecondButtonReturn
            self.handledAutoPlay.remove(id); self.defaults.set(Array(self.handledAutoPlay), forKey: "dl.handledAutoPlay")
            h.cancel(id, deleteFiles: delete); self.telemetry?("download_cancelled", ["deleted": delete])
            self.refresh()
        }
    }
}

// MARK: - the choice sheet

final class DownloadChoiceSheet: NSObject {
    private let mode = NSSegmentedControl(labels: ["Stream", "Download & Play"], trackingMode: .selectOne, target: nil, action: nil)
    private let scope = NSPopUpButton(frame: .zero, pullsDown: false)
    private let destLabel = NSTextField(labelWithString: ""), chooseButton = NSButton(title: "Choose…", target: nil, action: nil)
    private let autoPlay = NSButton(checkboxWithTitle: "Play automatically when the download finishes", target: nil, action: nil)
    private let note = NSTextField(wrappingLabelWithString: "")
    private var prefs: DownloadPreferences
    private var optionViews: [NSView] = []
    private weak var parent: NSWindow?

    init(prefs: DownloadPreferences) { self.prefs = prefs; super.init() }

    /// Asks how to watch `fileName`. The completion receives the final choice, or nil when the user cancelled.
    func present(over window: NSWindow, fileName: String, wholeTorrentLabel: String, completion: @escaping (DownloadPreferences?) -> Void) {
        parent = window
        let a = NSAlert()
        a.messageText = "How do you want to watch “\(fileName)”?"
        a.informativeText = ""
        a.addButton(withTitle: "Continue"); a.addButton(withTitle: "Cancel")
        mode.selectedSegment = prefs.downloadMode ? 1 : 0; mode.target = self; mode.action = #selector(modeChanged)
        scope.addItems(withTitles: ["Selected video only", wholeTorrentLabel]); scope.selectItem(at: prefs.wholeTorrent ? 1 : 0)
        destLabel.stringValue = (prefs.destination as NSString).abbreviatingWithTildeInPath; destLabel.lineBreakMode = .byTruncatingMiddle
        destLabel.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        chooseButton.target = self; chooseButton.action = #selector(chooseFolder); chooseButton.bezelStyle = .rounded; chooseButton.controlSize = .small
        autoPlay.state = prefs.autoPlay ? .on : .off
        note.font = .systemFont(ofSize: 11); note.textColor = .secondaryLabelColor
        let destRow = NSStackView(views: [NSTextField(labelWithString: "Save to:"), destLabel, chooseButton]); destRow.orientation = .horizontal; destRow.spacing = 6
        optionViews = [scope, destRow, autoPlay]
        let stack = NSStackView(views: [mode] + optionViews + [note]); stack.orientation = .vertical; stack.alignment = .leading; stack.spacing = 8
        stack.frame = NSRect(x: 0, y: 0, width: 400, height: 150)
        a.accessoryView = stack
        modeChanged()
        a.beginSheetModal(for: window) { [weak self] r in
            guard let self = self, r == .alertFirstButtonReturn else { completion(nil); return }
            var p = self.prefs
            p.downloadMode = self.mode.selectedSegment == 1; p.wholeTorrent = self.scope.indexOfSelectedItem == 1; p.autoPlay = self.autoPlay.state == .on
            completion(p)
        }
    }

    @objc private func modeChanged() {
        let dl = mode.selectedSegment == 1
        for v in optionViews { v.isHidden = !dl }
        note.stringValue = dl ? "The file is downloaded completely, checked piece by piece, and kept in the folder you choose. The download continues if the connection drops, and after a restart."
                              : "Plays while it downloads. Nothing is kept in your folders; the cache is bounded and cleaned automatically."
        mode.window?.layoutIfNeeded()
    }
    @objc private func chooseFolder() {
        let p = NSOpenPanel(); p.canChooseDirectories = true; p.canChooseFiles = false; p.canCreateDirectories = true; p.prompt = "Choose"
        p.directoryURL = URL(fileURLWithPath: prefs.destination, isDirectory: true).deletingLastPathComponent()
        guard let w = mode.window else { return }
        p.beginSheetModal(for: w) { [weak self] r in if r == .OK, let u = p.url { self?.prefs.destination = u.path; self?.destLabel.stringValue = (u.path as NSString).abbreviatingWithTildeInPath } }
    }
}
