# TorrentPlayer

A native macOS video player that can play a local video file, or stream a video from a torrent or magnet link while it downloads. It is written in Swift/AppKit with a C++ streaming core ([libtorrent](https://libtorrent.org)) and [mpv](https://mpv.io) for playback.

> **Status: first public MVP.** It is free software, but it is **not notarized** by Apple and has so far only been run on the developer's own Mac. Local video playback is the most thoroughly tested part; torrent streaming passed controlled tests and has worked once on a public torrent, but its reliability on the public internet is limited and has not been comprehensively validated. See [What has been tested](#what-has-been-tested) and [Known limitations](#known-limitations).

## Requirements
- A Mac with **Apple silicon** (arm64). Intel Macs are not supported.
- **macOS 27** or later.
- Nothing else: the download is self-contained (mpv, FFmpeg, libtorrent and the Vulkan/Metal driver are inside the app). Homebrew is **not** needed to run it.

## Install and first launch
1. Download `TorrentPlayer-<version>-macos-arm64.zip` from the [Releases](https://github.com/gdfaler/TorrentPlayer/releases) page, and check it against `SHA256SUMS`:
   ```bash
   shasum -a 256 -c SHA256SUMS
   ```
2. Double-click the ZIP to unpack it and drag **TorrentPlayer.app** to your Applications folder.
3. **The app is not notarized**, because the project has no Apple Developer ID. It is signed ad hoc, so macOS Gatekeeper will refuse the first launch ("cannot be opened because Apple cannot check it for malicious software"). To allow *this one app*:
   - Open TorrentPlayer once and dismiss the warning, then open **System Settings ▸ Privacy & Security**, scroll to the Security section and choose **Open Anyway** next to TorrentPlayer, then confirm. (The exact wording and layout differ between macOS versions; it has not been checked on macOS 27.)
   - Or, if you prefer the Terminal, remove the download quarantine flag from this app only:
     ```bash
     xattr -dr com.apple.quarantine /Applications/TorrentPlayer.app
     ```
   Do not turn Gatekeeper off globally. If you do not trust a download, build the app yourself (below).
4. The first time you open a torrent, macOS may ask whether TorrentPlayer may accept incoming network connections. BitTorrent works best if you allow it; the choice is yours.

## Using it
- **Local video:** *Open File…*, ⌘O, drag a video onto the window, or double-click a video with *Open With ▸ TorrentPlayer*. Supported containers include mp4, m4v, mov, mkv, webm, avi, mpg/mpeg, wmv, ts and flv. Audio-only files are not supported yet.
- **Torrent:** *Open Torrent…* (⇧⌘O), drop a `.torrent` file, paste a magnet link into the field (or ⌘L to open the one on the clipboard), then pick a video file and press Play. Playback starts while the file is still downloading.
- **Download & Play** *(new on this branch, not part of the 0.2.0 release)*: when you press Play for a torrent file or magnet link, choose **Stream** (as before) or **Download & Play**. Download & Play saves the selected video (or the whole torrent) in a folder you choose, checks every piece, shows real progress, and opens the finished file in the normal player. Pause, resume and cancel are in **Window ▸ Downloads** (⌥⌘L); cancelling asks whether to keep or delete the partial data. Downloads survive quitting and restarting: progress is saved and they continue by themselves the next time TorrentPlayer starts. If the network drops, a download waits and continues automatically when it returns (a setting in the Downloads window can turn this off); a download you paused yourself stays paused. Closing the window does not stop downloads; quitting does (progress is kept). Details and limits are under *What has been tested* and *Known limitations*.
- **Recent files:** the last 10 local videos you played are listed on the home screen (nothing is stored for torrents). *Clear All* removes the list; your files are never touched.
- **Keys:** Space pause · ←/→ seek 5 s (Shift: 30 s) · ↑/↓ volume · M mute · F fullscreen · E show more/fewer controls · R retry after an error · Esc leave fullscreen. The Playback menu offers the same actions.
- **Appearance:** System, Light or Dark for the home screen (bottom of the window or View ▸ Home Appearance).

## What has been tested
**Verified**
- Local video playback, including with Homebrew's runtime paths made unavailable to the app (the app bundles everything it needs).
- Torrent streaming in two controlled end-to-end tests on one Mac, using a small test video and only the local machine: once from a real BitTorrent peer and once from an HTTP web seed. In both, every piece was downloaded and checked, playback started within a few seconds and the position advanced steadily, with no crash and no stall.

**Download & Play (this branch)**
- Verified by automated tests on one Mac with local test torrents only: complete download of a selected file and of a whole multi-file torrent from a real BitTorrent peer, magnet metadata, piece-checked completion (wrong data already on disk is detected and replaced), resume in a brand-new session, missing/garbage/modified resume data, cancel without deleting data, unwritable folder and too little disk space, network loss and restore (including rapid on/off flapping and a user pause that network changes never override), a download that starts with no source at all, and the finished file opening in the normal player.
- Not verified: public torrents, a second Mac, and real Wi-Fi or interface changes (the network monitor is wired to the downloader, but the automated tests inject the network state instead of switching Wi-Fi). The new sheet and Downloads window have not been reviewed by a person.

**Public internet: mixed**
- Playback of a public torrent has succeeded: an earlier build started the Internet Archive's Big Buck Bunny torrent, but only after about 85 seconds.
- The latest build did **not** start the same torrent within 150 seconds, because one piece of the video, which spans two files in the torrent, was never delivered by the web seed that could serve it.
- Reliability on public torrents is therefore limited and has not been comprehensively validated. Public trackers, DHT peer discovery and HTTPS trackers have not been comprehensively tested; in one test some HTTPS tracker connections failed during the TLS handshake.

**Still to do**
- Installing and running on a second Mac.
- A person confirming the picture and sound. The automated tests confirm that the video decoder and audio output start and that playback time advances, not what is seen or heard.

## Where TorrentPlayer keeps things
- Streaming cache: `~/Library/Caches/TorrentPlayer/stream` (bounded, 2 GiB by default; it is not your Downloads folder).
- Download & Play: the files go to the folder you choose (default `~/Movies/TorrentPlayer`) and are never deleted unless you ask; resume bookkeeping is in `~/Library/Application Support/TorrentPlayer/Downloads`.
- Local log of what the app did, for troubleshooting: `~/Library/Logs/TorrentPlayer/` (File ▸ Reveal Telemetry Log in Finder). It stays on your Mac, is never sent anywhere, and can contain torrent and file names.
- Preferences and the recent-files list: the app's standard preferences file.

## Known limitations
- Not notarized (ad-hoc signed), and not yet verified on any Mac other than the developer's. Please report what you find.
- Download & Play downloads only while TorrentPlayer is running (the window may be closed or minimized). There is no background helper or login item, and a sleeping Mac does not download. After a download finishes, TorrentPlayer stops transferring that torrent (it does not keep seeding). Only the selected files are downloaded, plus the few bytes of neighbouring files that share a piece with them. After a restart, a finished download is trusted only if its files still have the size and modification time recorded when they were verified; anything else is re-checked against the torrent's piece hashes first (unchanged data is accepted without downloading again, damaged pieces are fetched again, and nothing plays automatically until it is verified). "Delete" removes only files the download itself created: files that were already in the folder are never deleted, and two downloads cannot share a file path. A file changed without changing its size or modification time is not detected until it is re-checked.
- Torrent startup needs at least one responsive BitTorrent peer or web seed. If the only source is slow or silent, the player can sit on "Preparing playback" for a long time or never start.
- Some web seeds (the Internet Archive is one) redirect each file to a different server. The torrent library then handles pieces that span two files poorly, which can delay or prevent the start of playback.
- Dragging the seek bar in a torrent jumps to the nearest keyframe (local files seek exactly).
- Rarely, closing a window can leave the player's shutdown waiting for several seconds before the home screen returns.
- After leaving fullscreen the window occasionally does not return to its previous size or position.
- Contrast of the on-screen controls over very bright video is low (a design trade-off); keyboard Tab navigation of the controls and VoiceOver support have not been verified.
- HTTPS tracker connectivity has not been fully validated: some tested connections failed during the TLS handshake, and the cause was not determined.
- No subtitle controls yet.
- BitTorrent is a tool: you are responsible for what you download or stream and for complying with the law where you live.

## Troubleshooting
- *"TorrentPlayer can't be opened"*: see step 3 above.
- *A torrent finds no peers, or stays on "Preparing playback"*: the link may be dead, its web seed may be unresponsive, or your network or VPN may block BitTorrent or tracker traffic. Try another link or network.
- *Video does not start*: check File ▸ Reveal Telemetry Log in Finder and open an issue with the last lines (remove anything private).

## Build from source
Requirements: macOS 27 on Apple silicon, the Xcode Command Line Tools (no full Xcode needed) and Homebrew packages `cmake ninja pkg-config boost libtorrent-rasterbar openssl@3 mpv molten-vk`.
```bash
tools/scripts/build_release.sh my-build      # builds the C++ core and the Swift app into dist/my-build/TorrentPlayer.app, bundles every library, signs ad hoc
tools/scripts/package_release.sh my-build    # ZIP, checksums, build metadata, notices, source archive (dist/release/)
```
The build bundles the libraries *your* Homebrew has installed; the release ZIP was built against the versions listed in `THIRD_PARTY_NOTICES.md`. Developer build for your own Mac: `tools/scripts/build_app.sh`. Unit tests: see `tests/app/*_test.swift` (each file lists its build command) and `ctest --test-dir build -L unit` for the core.

## Licence
Copyright © 2026 gdfaler. TorrentPlayer is licensed under the **GNU General Public License, version 3 or (at your option) any later version** — see [LICENSE](LICENSE). The app bundles third-party libraries, among them FFmpeg, mpv, x264, x265 and rubberband (GPL) and several LGPL libraries; each keeps its own licence and copyright. Their notices and licence texts are in `THIRD_PARTY_NOTICES.md` and, inside the app, in `TorrentPlayer.app/Contents/Resources/Licenses`. The corresponding source for a release (TorrentPlayer and the bundled libraries) is attached to that release; see `SOURCE_OFFER.txt`. The software comes with **no warranty**. Audio and video codecs may be covered by patents in some countries; this project gives no patent licence.
