# TorrentPlayer 0.2.0 — first public MVP

A native macOS player for local videos and for streaming torrents and magnet links while they download.

**Requires** a Mac with Apple silicon and macOS 27 or later. Self-contained: no Homebrew or other installs needed.

## Highlights
- Plays local video files (mp4, mkv, mov, webm, avi and more) with mpv; hardware-accelerated decoding.
- Streams from `.torrent` files and magnet links with a bounded, self-cleaning cache (2 GiB by default); pick the video file and play before the download finishes.
- Titanium Glass interface: a home screen with drop target, Open File / Open Torrent / magnet actions, recent local videos, Light / Dark / System appearance; native glass playback controls that respect Reduce Motion, Reduce Transparency and Increase Contrast.
- Keyboard shortcuts that work on the same physical keys in other keyboard layouts (for example Russian), Playback menu for every control.

## Testing status
- **Verified:** local video playback (also with Homebrew's runtime paths made unavailable to the app), and torrent streaming in two controlled end-to-end tests on one Mac with a small test video: from a real BitTorrent peer and from an HTTP web seed. Every piece was verified, playback started within seconds and advanced steadily without stalls or crashes.
- **Public internet: mixed.** Public-torrent playback has succeeded: an earlier build started the Internet Archive's Big Buck Bunny torrent after about 85 seconds. The latest build did not start it within 150 seconds, because one piece spanning two files in that torrent was never delivered by the web seed able to serve it.
- **Not yet done:** installation on a second Mac, and human confirmation of picture and sound (tests confirm that the decoder and audio output start and that playback time advances). Reliability on public torrents is limited and not comprehensively validated; public trackers and DHT discovery are not comprehensively tested, and some HTTPS tracker connections failed during the TLS handshake (cause not determined).

## Next release (in development, not part of 0.2.0): Download & Play
- A second way to watch a torrent or magnet link: download the selected video (or the whole torrent) completely into a folder you choose, with piece-by-piece verification and honest progress, then open it in the normal player.
- Downloads survive quitting and restarting, continue by themselves after a network interruption (switchable; a manual pause is never overridden), and keep running with the window closed while the app runs.
- Tested with local loopback torrents only; not yet checked on public torrents, on real network switching or by a person using the new screens.

## Install
Unzip, move `TorrentPlayer.app` to Applications, and approve it once in **System Settings ▸ Privacy & Security ▸ Open Anyway** (the app is ad-hoc signed, **not notarized**). Verify the download with `SHA256SUMS`. Details are in the README.

## Licence
GNU GPL v3 or later. Third-party notices, licence texts and the corresponding source are provided with the release (`THIRD_PARTY_NOTICES.md`, `SOURCE_OFFER.txt`).

## Known limitations
Not notarized (ad-hoc signed); not yet verified on a second Mac; torrent startup depends on responsive peers or web seeds, and web seeds that redirect each file to another server can delay or prevent playback when a piece spans two files; torrent seeking is keyframe-accurate only; occasional slow window shutdown and fullscreen frame restoration quirks; no subtitle controls; limited accessibility testing. See the README for the full list.
