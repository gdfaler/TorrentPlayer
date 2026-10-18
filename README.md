# TorrentPlayer

Native macOS torrent streaming player (personal build for this Mac). Streams a video file from a torrent through libtorrent into mpv with a strictly bounded, self-reclaiming cache.

## Build (Command Line Tools + Homebrew only; no Xcode)
```bash
tools/scripts/build_app.sh          # builds core (CMake/Ninja), the Swift app (swiftc), ad-hoc signs build/TorrentPlayer.app
ctest --test-dir build -L unit      # fast unit tests
```
Prerequisites (Homebrew): libtorrent-rasterbar, boost, openssl@3, mpv, cmake, ninja, pkg-config. See `docs/DISTRIBUTION_NOTES.md` (own-Mac use only).

## Launch
```bash
open build/TorrentPlayer.app
```
Then drop a `.torrent` onto the window, press "Open Torrent…", or paste a magnet link (Cmd-L reads the clipboard). Pick a video file, press Play. Keys: Space pause, ←/→ seek 5 s (Shift: 30 s), ↑/↓ volume, M mute, F fullscreen, R retry after an error, Cmd-O open. The first launch may show a macOS firewall question about incoming connections (BitTorrent needs them; answering is your choice).

## Where things are
- Streaming cache: `~/Library/Caches/TorrentPlayer/stream` (2 GiB default limit). Telemetry: `~/Library/Logs/TorrentPlayer/` (File > Reveal Telemetry Log in Finder).
- Docs: `docs/ARCHITECTURE_M2.md`, `docs/MILESTONE1_STATUS.md`, `docs/MILESTONE2_STATUS.md`, `docs/DISTRIBUTION_NOTES.md`. Raw evidence: `docs/gate_results/`.

## Tests
Loopback integration: `ctest --test-dir build -L integration` (long). App acceptance autotests: `python3 -I tests/app/app_autotests.py [scenario...]`. M1 gates on the current build: `tools/scripts/run_m1_regression.sh`. First real-swarm playback: `tools/scripts/run_m2_realswarm.sh` (AC power, lid open).
