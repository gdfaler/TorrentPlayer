// HomeView: the Titanium Glass home screen (the window shown before playback). One centred column: a stage card whose content follows the launcher state (drop target,
// progress, file list, error with actions), the Open File / Open Torrent buttons, the magnet-link row and the appearance control. Pure logic (what each state shows, palette,
// timing) lives in HomeModel.swift; AppController owns the flow and feeds this view.
import AppKit
import UniformTypeIdentifiers

private extension GlassRGB {
    var ns: NSColor { NSColor(srgbRed: r, green: g, blue: b, alpha: 1) }
}

/// What a drag or paste delivered: file paths (in the order macOS provides them) or one magnet link.
enum HomeDrop: Equatable {
    case files([String])
    case magnet(String)

    static func read(_ pb: NSPasteboard) -> HomeDrop? {
        if let urls = pb.readObjects(forClasses: [NSURL.self], options: nil) as? [URL], !urls.isEmpty {
            let files = urls.filter { $0.isFileURL }.map { $0.path }
            if !files.isEmpty { return .files(files) }
            if let u = urls.first, u.scheme == "magnet" { return .magnet(u.absoluteString) }
        }
        if let str = pb.string(forType: .string)?.trimmingCharacters(in: .whitespacesAndNewlines), str.hasPrefix("magnet:?") { return .magnet(str) }
        return nil
    }
}

/// Click-through view whose layer draws the card's outline and drag highlight.
final class HomeOutlineView: NSView {
    override init(frame: NSRect) { super.init(frame: frame); wantsLayer = true }
    required init?(coder: NSCoder) { fatalError() }
    override func hitTest(_ p: NSPoint) -> NSView? { nil }
    override var isOpaque: Bool { false }
}

/// The surface behind the stage content: native glass normally, a flat opaque surface under Reduce Transparency; a hairline (stronger under Increase Contrast) and an accent
/// outline while a drag is over the window.
final class HomeCard: NSView {
    private let glass = NSGlassEffectView()
    private let outlineView = HomeOutlineView()
    private var outline: CALayer { outlineView.layer! }
    private(set) var dragging = false
    private var flat = false, palette = HomePalette.resolve(dark: true, increaseContrast: false), reduceMotion = false

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        glass.style = .regular; glass.cornerRadius = 20; glass.translatesAutoresizingMaskIntoConstraints = false
        addSubview(glass)
        NSLayoutConstraint.activate([glass.leadingAnchor.constraint(equalTo: leadingAnchor), glass.trailingAnchor.constraint(equalTo: trailingAnchor),
                                     glass.topAnchor.constraint(equalTo: topAnchor), glass.bottomAnchor.constraint(equalTo: bottomAnchor)])
        outlineView.translatesAutoresizingMaskIntoConstraints = false          // above the glass (a layer sublayer would be painted under it), below the stage content
        addSubview(outlineView)
        NSLayoutConstraint.activate([outlineView.leadingAnchor.constraint(equalTo: leadingAnchor), outlineView.trailingAnchor.constraint(equalTo: trailingAnchor),
                                     outlineView.topAnchor.constraint(equalTo: topAnchor), outlineView.bottomAnchor.constraint(equalTo: bottomAnchor)])
        outline.cornerRadius = 20; outline.masksToBounds = true
        layer?.cornerRadius = 20
    }
    required init?(coder: NSCoder) { fatalError() }

    func apply(palette: HomePalette, opaque: Bool, reduceMotion: Bool) {
        self.palette = palette; self.flat = opaque; self.reduceMotion = reduceMotion
        glass.isHidden = flat
        layer?.backgroundColor = flat ? palette.surface.ns.cgColor : nil
        restyle(animated: false)
    }
    func setDragging(_ on: Bool) {
        guard on != dragging else { return }
        dragging = on
        restyle(animated: true)
    }
    private func restyle(animated: Bool) {
        let accent = palette.accent.ns.cgColor
        CATransaction.begin()
        CATransaction.setAnimationDuration(animated ? HomeMotion.duration(HomeMotion.hover, reduceMotion: reduceMotion) : 0)
        if dragging { outline.borderColor = accent; outline.borderWidth = 2 }
        else if flat || palette.borderWidth > 1 { outline.borderColor = palette.border.ns.cgColor; outline.borderWidth = palette.borderWidth }
        else { outline.borderColor = palette.border.ns.withAlphaComponent(0.45).cgColor; outline.borderWidth = 0.5 }
        outline.backgroundColor = dragging ? palette.accent.ns.withAlphaComponent(0.10).cgColor : nil
        CATransaction.commit()
    }
}

/// The Recent list: Return / Space open the selected row, Delete removes it.
final class RecentsTableView: NSTableView {
    var onOpen: ((Int) -> Void)?
    var onRemove: ((Int) -> Void)?
    override func keyDown(with e: NSEvent) {
        switch e.keyCode {
        case 36, 76, 49: if selectedRow >= 0 { onOpen?(selectedRow) } else { super.keyDown(with: e) }
        case 51, 117: if selectedRow >= 0 { onRemove?(selectedRow) } else { super.keyDown(with: e) }
        default: super.keyDown(with: e)
        }
    }
}

/// One row of the Recent list: file name, folder (or the problem), and a remove button.
final class RecentCell: NSTableCellView {
    let nameField = NSTextField(labelWithString: ""), subField = NSTextField(labelWithString: "")
    let removeButton = NSButton()
    var onRemove: (() -> Void)?
    override init(frame: NSRect) {
        super.init(frame: frame)
        nameField.font = .systemFont(ofSize: 13, weight: .medium); nameField.lineBreakMode = .byTruncatingMiddle
        subField.font = .systemFont(ofSize: 11); subField.lineBreakMode = .byTruncatingHead
        removeButton.isBordered = false; removeButton.imagePosition = .imageOnly; removeButton.target = self; removeButton.action = #selector(remove)
        removeButton.image = NSImage(systemSymbolName: "xmark.circle.fill", accessibilityDescription: nil)
        removeButton.toolTip = "Remove from Recents"
        let text = NSStackView(views: [nameField, subField]); text.orientation = .vertical; text.alignment = .leading; text.spacing = 1
        [text, removeButton].forEach { $0.translatesAutoresizingMaskIntoConstraints = false; addSubview($0) }
        NSLayoutConstraint.activate([
            text.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 10), text.centerYAnchor.constraint(equalTo: centerYAnchor), text.trailingAnchor.constraint(lessThanOrEqualTo: removeButton.leadingAnchor, constant: -8),
            removeButton.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8), removeButton.centerYAnchor.constraint(equalTo: centerYAnchor), removeButton.widthAnchor.constraint(equalToConstant: 22), removeButton.heightAnchor.constraint(equalToConstant: 22)])
    }
    required init?(coder: NSCoder) { fatalError() }
    @objc private func remove() { onRemove?() }
}

final class HomeView: NSView, NSTextFieldDelegate, NSTableViewDataSource, NSTableViewDelegate {
    // callbacks into the controller (every open path ends in AppController.open(requests:) / openMagnet)
    var onDrop: ((HomeDrop) -> Void)?
    var onChooseFile: (() -> Void)?
    var onChooseTorrent: (() -> Void)?
    var onPasteMagnet: (() -> Void)?
    var onOpenLink: ((String) -> Void)?
    var onAction: ((HomeAction) -> Void)?
    var onAppearance: ((HomeAppearance) -> Void)?
    var onOpenRecent: ((String) -> Void)?
    var onRemoveRecent: ((String) -> Void)?
    var onClearRecents: (() -> Void)?

    let table = NSTableView()
    let scroll = NSScrollView()
    let magnetField = NSTextField()
    private(set) var presentation = HomePresentation.make(.idle)
    private(set) var appearancePreference = HomeAppearance.load()
    private(set) var palette = HomePalette.resolve(dark: true, increaseContrast: false)
    private(set) var environment = GlassEnvironment()

    let card = HomeCard()
    private let wordmark = NSTextField(labelWithString: "TorrentPlayer")
    private let icon = NSImageView()
    private let titleLabel = NSTextField(wrappingLabelWithString: "")
    private let detailLabel = NSTextField(wrappingLabelWithString: "")
    private let hintLabel = NSTextField(labelWithString: "Drop a video, a .torrent file or a magnet link here")
    private let spinner = NSProgressIndicator()
    private let actionRow = NSStackView()
    private var actionButtons: [HomeAction: NSButton] = [:]
    private let openFileButton = NSButton(), openTorrentButton = NSButton(), pasteButton = NSButton(), openLinkButton = NSButton()
    private let appearanceControl = NSSegmentedControl(labels: HomeAppearance.allCases.map { $0.title }, trackingMode: .selectOne, target: nil, action: nil)
    private let entryRow = NSStackView(), magnetRow = NSStackView()
    private var observers: [NSObjectProtocol] = []
    private let recentsTable = RecentsTableView()
    private let recentsScroll = NSScrollView()
    private let recentsHeader = NSStackView()
    private var recentsHeight: NSLayoutConstraint!
    private let recentsLabel = NSTextField(labelWithString: "Recent")
    private let clearRecentsButton = NSButton()
    private(set) var recentRows: [RecentRowModel] = []
    private var recentsShown: Bool { presentation.mode == .empty && !recentRows.isEmpty }
    private var listFill: [NSLayoutConstraint] = []                      // active only while the file list is shown: the list takes the card's height

    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([.fileURL, .URL, .string])
        wantsLayer = true
        build()
        observers.append(NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.accessibilityDisplayOptionsDidChangeNotification, object: nil, queue: .main) { [weak self] _ in self?.refreshEnvironment() })
        apply(presentation)
        refreshEnvironment()
    }
    required init?(coder: NSCoder) { fatalError() }
    deinit { observers.forEach { NSWorkspace.shared.notificationCenter.removeObserver($0) } }

    // MARK: construction
    private func label(_ l: NSTextField, size: CGFloat, weight: NSFont.Weight, lines: Int = 0) {
        l.font = .systemFont(ofSize: size, weight: weight); l.alignment = .center; l.maximumNumberOfLines = lines
        l.lineBreakMode = .byWordWrapping; l.isSelectable = false
        l.translatesAutoresizingMaskIntoConstraints = false
    }

    private func button(_ b: NSButton, title: String, symbol: String?, action: Selector, large: Bool = false) {
        b.title = title; b.target = self; b.action = action
        b.bezelStyle = .glass; b.controlSize = large ? .large : .regular
        if let s = symbol { b.image = NSImage(systemSymbolName: s, accessibilityDescription: nil); b.imagePosition = .imageLeading }
        b.setAccessibilityLabel(title)
    }

    private func build() {
        wordmark.font = .systemFont(ofSize: 13, weight: .semibold); wordmark.alignment = .center
        label(titleLabel, size: 20, weight: .semibold, lines: 3)
        label(detailLabel, size: 13, weight: .regular, lines: 4)
        hintLabel.font = .systemFont(ofSize: 12, weight: .medium); hintLabel.alignment = .center
        icon.symbolConfiguration = NSImage.SymbolConfiguration(pointSize: 34, weight: .light)
        icon.setAccessibilityElement(false)
        spinner.style = .spinning; spinner.controlSize = .regular; spinner.isIndeterminate = true; spinner.isDisplayedWhenStopped = false

        let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("name")); col.title = "File"; col.width = 300
        let col2 = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("size")); col2.title = "Size"; col2.width = 90
        let col3 = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("kind")); col3.title = "Type"; col3.width = 70
        [col, col2, col3].forEach { table.addTableColumn($0) }
        table.style = .plain; table.columnAutoresizingStyle = .firstColumnOnlyAutoresizingStyle; table.backgroundColor = .clear; table.usesAlternatingRowBackgroundColors = false
        scroll.documentView = table; scroll.hasVerticalScroller = true; scroll.drawsBackground = false; scroll.borderType = .noBorder
        scroll.translatesAutoresizingMaskIntoConstraints = false; scroll.wantsLayer = true; scroll.layer?.cornerRadius = 10
        scroll.setAccessibilityLabel("Files in this torrent")

        // Recent files (idle state only, below the drop hint; secondary to the Open actions)
        recentsLabel.font = .systemFont(ofSize: 11, weight: .semibold); recentsLabel.setAccessibilityElement(false)
        clearRecentsButton.title = "Clear All"; clearRecentsButton.isBordered = false; clearRecentsButton.font = .systemFont(ofSize: 11, weight: .medium)
        clearRecentsButton.target = self; clearRecentsButton.action = #selector(clearRecents); clearRecentsButton.setAccessibilityLabel("Clear all recent files")
        let spacer = NSView(); spacer.setContentHuggingPriority(.init(1), for: .horizontal)
        recentsHeader.setViews([recentsLabel, spacer, clearRecentsButton], in: .leading); recentsHeader.orientation = .horizontal; recentsHeader.alignment = .centerY
        recentsHeader.translatesAutoresizingMaskIntoConstraints = false
        let rcol = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("recent")); rcol.resizingMask = .autoresizingMask
        recentsTable.addTableColumn(rcol); recentsTable.headerView = nil; recentsTable.style = .plain; recentsTable.rowHeight = 40; recentsTable.backgroundColor = .clear
        recentsTable.selectionHighlightStyle = .regular; recentsTable.allowsEmptySelection = true; recentsTable.intercellSpacing = NSSize(width: 0, height: 2)
        recentsTable.dataSource = self; recentsTable.delegate = self; recentsTable.target = self; recentsTable.action = #selector(recentClicked)
        recentsTable.onOpen = { [weak self] r in self?.openRecent(row: r) }; recentsTable.onRemove = { [weak self] r in self?.removeRecent(row: r) }
        recentsTable.setAccessibilityLabel("Recent files")
        recentsScroll.documentView = recentsTable; recentsScroll.hasVerticalScroller = true; recentsScroll.drawsBackground = false; recentsScroll.borderType = .noBorder
        recentsScroll.translatesAutoresizingMaskIntoConstraints = false; recentsScroll.wantsLayer = true; recentsScroll.layer?.cornerRadius = 10
        recentsHeight = recentsScroll.heightAnchor.constraint(equalToConstant: 40); recentsHeight.isActive = true       // fits the rows (3 visible, then it scrolls)

        actionRow.orientation = .horizontal; actionRow.spacing = 10; actionRow.alignment = .centerY; actionRow.setHuggingPriority(.required, for: .horizontal)

        let content = NSStackView(views: [icon, spinner, titleLabel, detailLabel, hintLabel, recentsHeader, recentsScroll, scroll, actionRow])
        content.orientation = .vertical; content.alignment = .centerX; content.spacing = 10; content.distribution = .fill
        scroll.setContentHuggingPriority(.init(1), for: .vertical)
        content.setCustomSpacing(14, after: icon); content.setCustomSpacing(18, after: hintLabel); content.setCustomSpacing(4, after: recentsHeader); content.setCustomSpacing(16, after: detailLabel)
        content.translatesAutoresizingMaskIntoConstraints = false
        card.translatesAutoresizingMaskIntoConstraints = false
        card.addSubview(content)
        card.setAccessibilityElement(false)

        button(openFileButton, title: "Open File…", symbol: "film", action: #selector(chooseFile), large: true)
        button(openTorrentButton, title: "Open Torrent…", symbol: "arrow.down.doc", action: #selector(chooseTorrent), large: true)
        entryRow.setViews([openFileButton, openTorrentButton], in: .center); entryRow.orientation = .horizontal; entryRow.spacing = 12; entryRow.distribution = .fillEqually

        magnetField.placeholderString = "Paste a magnet link  magnet:?xt=urn:btih:…"; magnetField.delegate = self; magnetField.target = self; magnetField.action = #selector(openLink)
        magnetField.controlSize = .large; magnetField.bezelStyle = .roundedBezel; magnetField.lineBreakMode = .byTruncatingMiddle; magnetField.usesSingleLineMode = true
        magnetField.setAccessibilityLabel("Magnet link")
        magnetField.setContentHuggingPriority(.defaultLow, for: .horizontal)
        button(pasteButton, title: "", symbol: "doc.on.clipboard", action: #selector(pasteLink), large: true)
        pasteButton.imagePosition = .imageOnly; pasteButton.toolTip = "Paste a magnet link from the clipboard and open it"
        pasteButton.setAccessibilityLabel("Paste magnet link from clipboard and open")
        button(openLinkButton, title: "Open Link", symbol: nil, action: #selector(openLink), large: true)
        magnetRow.setViews([magnetField, pasteButton, openLinkButton], in: .center); magnetRow.orientation = .horizontal; magnetRow.spacing = 8; magnetRow.alignment = .centerY

        appearanceControl.target = self; appearanceControl.action = #selector(appearanceChanged); appearanceControl.controlSize = .small
        appearanceControl.segmentStyle = .rounded; appearanceControl.setAccessibilityLabel("Appearance")
        appearanceControl.selectedSegment = HomeAppearance.allCases.firstIndex(of: appearancePreference) ?? 0
        appearanceControl.translatesAutoresizingMaskIntoConstraints = false

        // one centred column (a layout guide, not a stack view: the card must take ALL the spare height while the rows keep their size)
        let column = NSLayoutGuide(); addLayoutGuide(column)
        [wordmark, card, entryRow, magnetRow].forEach { $0.translatesAutoresizingMaskIntoConstraints = false }
        addSubview(wordmark); addSubview(card); addSubview(entryRow); addSubview(magnetRow); addSubview(appearanceControl)

        let width = column.widthAnchor.constraint(equalTo: widthAnchor, constant: -64); width.priority = .defaultHigh
        var cs: [NSLayoutConstraint] = [
            column.centerXAnchor.constraint(equalTo: centerXAnchor), column.widthAnchor.constraint(lessThanOrEqualToConstant: 540), width,
            wordmark.centerXAnchor.constraint(equalTo: column.centerXAnchor), wordmark.topAnchor.constraint(equalTo: safeAreaLayoutGuide.topAnchor, constant: 22),
            card.topAnchor.constraint(equalTo: wordmark.bottomAnchor, constant: 20), card.heightAnchor.constraint(greaterThanOrEqualToConstant: 236),
            entryRow.topAnchor.constraint(equalTo: card.bottomAnchor, constant: 16), entryRow.heightAnchor.constraint(equalToConstant: 40), magnetRow.topAnchor.constraint(equalTo: entryRow.bottomAnchor, constant: 16), magnetRow.heightAnchor.constraint(equalToConstant: 36),
            magnetRow.bottomAnchor.constraint(lessThanOrEqualTo: appearanceControl.topAnchor, constant: -16)]
        let pinBottom = magnetRow.bottomAnchor.constraint(equalTo: appearanceControl.topAnchor, constant: -16); pinBottom.priority = .defaultHigh     // the card absorbs the slack
        cs.append(pinBottom)
        for v in [card, entryRow, magnetRow] as [NSView] { cs += [v.leadingAnchor.constraint(equalTo: column.leadingAnchor), v.trailingAnchor.constraint(equalTo: column.trailingAnchor)] }
        cs += [
            content.centerXAnchor.constraint(equalTo: card.centerXAnchor), content.centerYAnchor.constraint(equalTo: card.centerYAnchor),
            content.leadingAnchor.constraint(greaterThanOrEqualTo: card.leadingAnchor, constant: 24), content.trailingAnchor.constraint(lessThanOrEqualTo: card.trailingAnchor, constant: -24),
            content.topAnchor.constraint(greaterThanOrEqualTo: card.topAnchor, constant: 22), content.bottomAnchor.constraint(lessThanOrEqualTo: card.bottomAnchor, constant: -22),
            titleLabel.widthAnchor.constraint(lessThanOrEqualTo: card.widthAnchor, constant: -56), detailLabel.widthAnchor.constraint(lessThanOrEqualTo: card.widthAnchor, constant: -56),
            scroll.widthAnchor.constraint(equalTo: card.widthAnchor, constant: -40), scroll.heightAnchor.constraint(greaterThanOrEqualToConstant: 120),
            recentsScroll.widthAnchor.constraint(equalTo: card.widthAnchor, constant: -40), recentsHeader.widthAnchor.constraint(equalTo: recentsScroll.widthAnchor, constant: -20),
            appearanceControl.centerXAnchor.constraint(equalTo: centerXAnchor), appearanceControl.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -16)]
        NSLayoutConstraint.activate(cs)
        listFill = [content.topAnchor.constraint(equalTo: card.topAnchor, constant: 22), content.bottomAnchor.constraint(equalTo: card.bottomAnchor, constant: -22),
                    content.leadingAnchor.constraint(equalTo: card.leadingAnchor, constant: 24), content.trailingAnchor.constraint(equalTo: card.trailingAnchor, constant: -24)]
    }

    // MARK: state
    func apply(_ p: HomePresentation) {
        let changedMode = p.mode != presentation.mode
        presentation = p
        titleLabel.stringValue = p.title; detailLabel.stringValue = p.detail
        detailLabel.isHidden = p.detail.isEmpty
        hintLabel.isHidden = p.mode != .empty
        icon.image = NSImage(systemSymbolName: p.symbol, accessibilityDescription: nil)
        icon.isHidden = p.mode == .progress || p.mode == .list || recentsShown                  // the list of recents takes the icon's place
        if recentsShown { detailLabel.isHidden = true }                                         // the explanation returns when there is nothing recent to show
        scroll.isHidden = p.mode != .list
        recentsHeader.isHidden = !recentsShown; recentsScroll.isHidden = !recentsShown
        if p.mode == .list { NSLayoutConstraint.activate(listFill) } else { NSLayoutConstraint.deactivate(listFill) }
        if p.spinner { spinner.startAnimation(nil) } else { spinner.stopAnimation(nil) }
        spinner.isHidden = !p.spinner
        rebuildActions(p.actions)
        card.setAccessibilityElement(true); card.setAccessibilityRole(.group); card.setAccessibilityLabel(p.accessibilityLabel)
        restyleText()
        if p.mode == .list { table.reloadData() }
        if changedMode, p.mode == .error {
            NSAccessibility.post(element: NSApp as Any, notification: .announcementRequested, userInfo: [.announcement: p.title + ". " + p.detail, .priority: NSAccessibilityPriorityLevel.high.rawValue])
        }
    }

    // MARK: recent files
    func setRecents(_ rows: [RecentRowModel]) {
        let changed = rows != recentRows
        recentRows = rows
        recentsHeight.constant = rows.count <= 3 ? CGFloat(max(rows.count, 1)) * 40 : 140         // 3 rows, or 3.5 when more exist so the scrolling is visible
        if changed { recentsTable.reloadData() }
        apply(presentation)                                                                     // shows or hides the section for the current state
        restyleText()
    }
    private func openRecent(row: Int) { if row >= 0, row < recentRows.count { onOpenRecent?(recentRows[row].id) } }
    private func removeRecent(row: Int) { if row >= 0, row < recentRows.count { onRemoveRecent?(recentRows[row].id) } }
    @objc private func recentClicked() { openRecent(row: recentsTable.clickedRow) }            // the remove button handles its own clicks
    @objc private func clearRecents() { onClearRecents?() }
    func numberOfRows(in tableView: NSTableView) -> Int { tableView === recentsTable ? recentRows.count : 0 }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard tableView === recentsTable, row < recentRows.count else { return nil }
        let m = recentRows[row]
        let id = NSUserInterfaceItemIdentifier("recentCell")
        let cell = (tableView.makeView(withIdentifier: id, owner: self) as? RecentCell) ?? { let c = RecentCell(frame: .zero); c.identifier = id; return c }()
        cell.nameField.stringValue = m.title; cell.subField.stringValue = m.subtitle
        cell.nameField.textColor = m.available ? palette.text.ns : palette.secondaryText.ns
        cell.subField.textColor = m.available ? palette.secondaryText.ns : palette.danger.ns
        cell.removeButton.contentTintColor = palette.secondaryText.ns
        cell.removeButton.setAccessibilityLabel("Remove \(m.title) from Recents")
        let rowID = m.id; cell.onRemove = { [weak self] in self?.onRemoveRecent?(rowID) }
        cell.setAccessibilityElement(true); cell.setAccessibilityRole(.row); cell.setAccessibilityLabel(m.accessibilityLabel)
        return cell
    }

    private func rebuildActions(_ actions: [HomeAction]) {
        actionRow.arrangedSubviews.forEach { actionRow.removeArrangedSubview($0); $0.removeFromSuperview() }
        for (i, a) in actions.enumerated() {
            let b = actionButtons[a] ?? { let nb = NSButton(); button(nb, title: a.title, symbol: nil, action: #selector(actionPressed(_:)), large: true); actionButtons[a] = nb; return nb }()
            b.identifier = NSUserInterfaceItemIdentifier(String(describing: a))
            b.keyEquivalent = (i == 0 && (a == .play || a == .tryAgain || a == .openFile || a == .editLink)) ? "\r" : ""
            b.bezelColor = i == 0 ? palette.accent.ns : nil
            actionRow.addArrangedSubview(b)
        }
        actionRow.isHidden = actions.isEmpty
    }
    func setPlayEnabled(_ on: Bool) { actionButtons[.play]?.isEnabled = on }

    // MARK: appearance / accessibility environment
    func setAppearance(_ pref: HomeAppearance) {
        appearancePreference = pref; pref.save()
        appearanceControl.selectedSegment = HomeAppearance.allCases.firstIndex(of: pref) ?? 0
        switch pref {
        case .system: window?.appearance = nil
        case .light: window?.appearance = NSAppearance(named: .aqua)
        case .dark: window?.appearance = NSAppearance(named: .darkAqua)
        }
        refreshEnvironment()
    }
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if let w = window { w.initialFirstResponder = openFileButton; setAppearance(appearancePreference) }          // the magnet field does not grab the keyboard on launch
    }
    override func viewDidChangeEffectiveAppearance() { super.viewDidChangeEffectiveAppearance(); refreshEnvironment() }

    func refreshEnvironment() {
        environment = GlassAccessibility.environment()
        let dark = effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
        palette = HomePalette.resolve(dark: dark, increaseContrast: environment.increaseContrast)
        layer?.backgroundColor = palette.background.ns.cgColor
        window?.backgroundColor = palette.background.ns
        card.apply(palette: palette, opaque: environment.reduceTransparency, reduceMotion: environment.reduceMotion)
        restyleText()
        actionButtons.values.forEach { $0.bezelColor = nil }
        rebuildActions(presentation.actions)
        for b in [openFileButton, openTorrentButton] { b.bezelColor = nil }
        openFileButton.bezelColor = palette.accent.ns
        recentsTable.reloadData()
    }
    private func restyleText() {
        wordmark.textColor = palette.secondaryText.ns
        recentsLabel.textColor = palette.secondaryText.ns
        clearRecentsButton.attributedTitle = NSAttributedString(string: "Clear All", attributes: [.font: NSFont.systemFont(ofSize: 11, weight: .medium), .foregroundColor: palette.secondaryText.ns])
        titleLabel.textColor = presentation.mode == .error ? palette.danger.ns : palette.text.ns
        detailLabel.textColor = palette.secondaryText.ns; hintLabel.textColor = palette.secondaryText.ns
        icon.contentTintColor = presentation.mode == .error ? palette.danger.ns : palette.secondaryText.ns
    }

    // MARK: actions
    @objc private func chooseFile() { onChooseFile?() }
    @objc private func chooseTorrent() { onChooseTorrent?() }
    @objc private func pasteLink() { onPasteMagnet?() }
    @objc private func openLink() { onOpenLink?(magnetField.stringValue.trimmingCharacters(in: .whitespacesAndNewlines)) }
    @objc private func actionPressed(_ b: NSButton) { if let a = actionButtons.first(where: { $0.value === b })?.key { onAction?(a) } }
    @objc private func appearanceChanged() {
        let i = appearanceControl.selectedSegment
        guard i >= 0, i < HomeAppearance.allCases.count else { return }
        setAppearance(HomeAppearance.allCases[i]); onAppearance?(appearancePreference)
    }
    func focusMagnetField() { window?.makeFirstResponder(magnetField); magnetField.selectText(nil) }

    // MARK: drag and drop (the whole window is the drop target; the card shows it)
    func accepts(_ pb: NSPasteboard) -> Bool { HomeDrop.read(pb) != nil }
    override func draggingEntered(_ s: NSDraggingInfo) -> NSDragOperation { guard accepts(s.draggingPasteboard) else { return [] }; card.setDragging(true); return .copy }
    override func draggingExited(_ s: NSDraggingInfo?) { card.setDragging(false) }
    override func draggingEnded(_ s: NSDraggingInfo) { card.setDragging(false) }
    override func performDragOperation(_ s: NSDraggingInfo) -> Bool { handleDrop(s.draggingPasteboard) }
    @discardableResult func handleDrop(_ pb: NSPasteboard) -> Bool {
        card.setDragging(false)
        guard let d = HomeDrop.read(pb) else { return false }
        onDrop?(d); return true
    }
    var isDragHighlighted: Bool { card.dragging }
#if TP_TESTING
    var recentsVisible: Bool { !recentsScroll.isHidden }
    func testClickRecent(_ i: Int) { openRecent(row: i) }
    func testRemoveRecent(_ i: Int) { removeRecent(row: i) }
    func testPressClearAll() { clearRecentsButton.performClick(nil) }
    func testButton(_ name: String) -> NSButton? { ["file": openFileButton, "torrent": openTorrentButton, "paste": pasteButton, "link": openLinkButton][name] ?? actionButtons.first(where: { "\($0.key)".lowercased() == name })?.value }
    var layoutFrames: [String: String] { ["card": NSStringFromRect(card.frame), "entry": NSStringFromRect(entryRow.frame), "magnet": NSStringFromRect(magnetRow.frame), "appearance": NSStringFromRect(appearanceControl.frame), "wordmark": NSStringFromRect(wordmark.frame), "self": NSStringFromRect(bounds)] }
#endif
}
