# TorrentPlayer 0.2.1 — Download & Play

A native macOS player for local videos and for streaming torrents and magnet links while they download.

**Requires** a Mac with Apple silicon and macOS 27 or later. Self-contained: no Homebrew or other installs needed.

## Highlights
- Plays local video files (mp4, mkv, mov, webm, avi and more) with mpv; hardware-accelerated decoding.
- Streams from `.torrent` files and magnet links with a bounded, self-cleaning cache (2 GiB by default); pick the video file and play before the download finishes.
- Titanium Glass interface: a home screen with drop target, Open File / Open Torrent / magnet actions, recent local videos, Light / Dark / System appearance; native glass playback controls that respect Reduce Motion, Reduce Transparency and Increase Contrast.
- Keyboard shortcuts that work on the same physical keys in other keyboard layouts (for example Russian), Playback menu for every control.

## Earlier testing status (0.2.0)
- **Verified:** local video playback (also with Homebrew's runtime paths made unavailable to the app), and torrent streaming in two controlled end-to-end tests on one Mac with a small test video: from a real BitTorrent peer and from an HTTP web seed. Every piece was verified, playback started within seconds and advanced steadily without stalls or crashes.
- **Public internet: mixed.** Public-torrent playback has succeeded: an earlier build started the Internet Archive's Big Buck Bunny torrent after about 85 seconds. The latest build did not start it within 150 seconds, because one piece spanning two files in that torrent was never delivered by the web seed able to serve it.
- **Not yet done:** installation on a second Mac, and human confirmation of picture and sound (tests confirm that the decoder and audio output start and that playback time advances). Reliability on public torrents is limited and not comprehensively validated; public trackers and DHT discovery are not comprehensively tested, and some HTTPS tracker connections failed during the TLS handshake (cause not determined).

## What is new in 0.2.1
- **Download & Play:** a second way to watch a torrent or magnet link. Download the selected video (or the whole torrent) completely into a folder you choose, with piece-by-piece verification and honest progress, then open it in the normal player. Stream is unchanged and remains the default. Pause, resume and cancel are in **Window ▸ Downloads** (⌥⌘L); cancelling asks whether to keep or delete the partial data.
- Downloads survive quitting and restarting, continue by themselves after a network interruption (switchable; a manual pause is never overridden), and keep running with the Downloads window closed while the app runs. There is no background helper: nothing downloads when the app is not running.
- **Web-seed recovery:** when playback waits on a piece that spans two files and no source has requested it (a libtorrent limitation with web seeds that redirect each file), the web seeds are re-added, at most 8 times per piece, 5 seconds apart.
- Internal: a race in one Download & Play test was fixed (test-only).

## Testing status (0.2.1)
- **Automated:** repository checks, C++ unit tests, Swift tests and the Download & Play and web-seed scenarios pass on loopback torrents.
- **Confirmed by the project owner on one Mac** with development builds of this code: the new sheet and Downloads window; real BitTorrent downloads; recovery after network interruptions; manual pause preserved; resume after restart; downloading with the window closed; Cancel and keep partial data; finished downloads playing, also offline and after a restart.
- **Web-seed recovery is not validated against a real occurrence:** the 8-attempt recovery was tested only with local loopback servers (0 of 6 runs completed without it, 8 of 8 with it). A bounded run on the Internet Archive's Big Buck Bunny torrent never reached the stuck-piece condition, so it neither confirms nor refutes the fix. A web seed that never answers is not recovered.
- **Not done:** a second Mac, and a run with the VPN switched off. Behind some VPNs UDP trackers and DHT are unreachable, so a torrent may find no sources.

## Install
Unzip, move `TorrentPlayer.app` to Applications, and approve it once in **System Settings ▸ Privacy & Security ▸ Open Anyway** (the app is ad-hoc signed, **not notarized**). Verify the download with `SHA256SUMS`. Details are in the README.

## Licence
GNU GPL v3 or later. Third-party notices, licence texts and the corresponding source are provided with the release (`THIRD_PARTY_NOTICES.md`, `SOURCE_OFFER.txt`).

## Known limitations
Not notarized (ad-hoc signed); not yet verified on a second Mac; torrent startup depends on responsive peers or web seeds, and web seeds that redirect each file to another server can delay or prevent playback when a piece spans two files; torrent seeking is keyframe-accurate only; occasional slow window shutdown and fullscreen frame restoration quirks; no subtitle controls; limited accessibility testing. See the README for the full list.
