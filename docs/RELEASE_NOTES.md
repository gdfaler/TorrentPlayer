# TorrentPlayer 0.2.0 — first public MVP

A native macOS player for local videos and for streaming torrents and magnet links while they download.

**Requires** a Mac with Apple silicon and macOS 27 or later. Self-contained: no Homebrew or other installs needed.

## Highlights
- Plays local video files (mp4, mkv, mov, webm, avi and more) with mpv; hardware-accelerated decoding.
- Streams from `.torrent` files and magnet links with a bounded, self-cleaning cache (2 GiB by default); pick the video file and play before the download finishes.
- Titanium Glass interface: a home screen with drop target, Open File / Open Torrent / magnet actions, recent local videos, Light / Dark / System appearance; native glass playback controls that respect Reduce Motion, Reduce Transparency and Increase Contrast.
- Keyboard shortcuts that work on the same physical keys in other keyboard layouts (for example Russian), Playback menu for every control.

## Install
Unzip, move `TorrentPlayer.app` to Applications, and approve it once in **System Settings ▸ Privacy & Security ▸ Open Anyway** (the app is ad-hoc signed, **not notarized**). Verify the download with `SHA256SUMS`. Details are in the README.

## Licence
GNU GPL v3 or later. Third-party notices, licence texts and the corresponding source are provided with the release (`THIRD_PARTY_NOTICES.md`, `SOURCE_OFFER.txt`).

## Known limitations
Not notarized; not yet verified on a second Mac; torrent seeking is keyframe-accurate only; occasional slow window shutdown and fullscreen frame restoration quirks; no subtitle controls; limited accessibility testing. See the README for the full list.
