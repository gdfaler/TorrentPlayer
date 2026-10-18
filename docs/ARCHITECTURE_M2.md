# TorrentPlayer: Milestone 2 architecture (vertical slice)

```
 AppKit app (Swift, app/Sources)              C interface                C++ core (tpcore, Milestone 1, unchanged logic)
 ┌───────────────────────────────┐   tp_c.h   ┌───────────────┐         ┌─────────────────────────────────────────────┐
 │ Launcher (open / drop / list) │──────────▶│ tp_c.cpp       │────────▶│ Engine (libtorrent session, SegmentDisk)     │
 │ PlayerSession (mpv + overlays)│   plain C  │ no exceptions, │         │ StreamSession (window planner, un-have,     │
 │ Telemetry (JSONL)             │            │ bounds checks  │         │   watchdog, disk policy, web-seed policy)    │
 └──────────────┬────────────────┘            └───────────────┘         │ SegmentStore (slot charging, reservation,    │
                │ mpv client API                                         │   eviction, reconciliation, recovery)        │
                ▼                                                        │ ShadowVerifier (ARRIVED / STORAGE / UNDET.)  │
 ┌───────────────────────────────┐   tp:// protocol (C)                  └─────────────────────────────────────────────┘
 │ libmpv (own window, Vulkan/   │◀─── tp_mpv_bridge.c: read/seek/size/close/cancel callbacks over tp_stream_read
 │ Metal, VideoToolbox decode)   │
 └───────────────────────────────┘
```

## Layers and rules
- **Core (`core/`)**: the Milestone-1 library. M2 only ADDED: `Engine::start_metadata/poll_metadata/cancel_metadata` (observable, cancellable magnet fetch), read-only getters on `StreamSession` (`piece_size`, `piece_of_file_offset`, `has_file_offset`), and `TorrentSource::trackers/web_seed_urls` (magnet `tr=` / `ws=` carried into the stream; without them a magnet-opened stream would lose its trackers). The cache accounting, pre-write reservation, reconciliation, integrity classifier, recovery, disk policy and watchdog code was not edited. The M1 gates are re-run against this build (`docs/gate_results/M2_m1_regression_*`).
- **C interface (`core/include/tpcore/tp_c.h`, `core/src/tp_c.cpp`)**: the only thing Swift sees. Opaque handles (`tp_engine`, `tp_meta`, `tp_stream`, `tp_cancel`), fixed-size POD status, caller-owned buffers with sizes. Hostile-input bounds: `.torrent` files > 16 MiB, > 20,000 files, magnets > 8 KiB or without `magnet:?` are refused; metadata is parsed with libtorrent load limits; the cache root is made absolute (libtorrent normalises save paths).
- **mpv bridge (`app/Sources/tp_mpv_bridge.c`)**: custom protocol `tp://stream`. Verified rules carried over from the spikes: `cancel_fn` is mandatory and sets a ONE-SHOT token that releases the blocked read with -1; the app never injects read errors or EOF (it poisoned the MP4 demuxer); `seek_fn` only repositions; the mpv handle is destroyed (`mpv_terminate_destroy`, off the main thread, 10 s guard) BEFORE the stream is closed; if mpv ever fails to terminate the stream is deliberately leaked rather than closed under a live player.
- **App (`app/Sources/*.swift`, AppKit only, built with `swiftc`, no Xcode)**: no synchronous mpv calls on the main thread after startup (state comes from observed properties; commands are `mpv_command_async`), so a stream blocked on the network cannot freeze the UI.

## Window architecture (A, verified in the research spikes)
mpv owns its window (title "TorrentPlayer", `force-window=immediate`). Two borderless, non-activating overlay panels are attached as child windows: the **state banner** (centre) and the **transport controls** (bottom). Keyboard input goes through one app-level local event monitor (mpv's own bindings are disabled). Child attachment is checked at runtime (`window_geometry` telemetry event). Not verified: multiple displays, VoiceOver, IME, final visual appearance on screen (snapshots of the overlay views are rendered in-app for review).

## Named states (what the user sees)
Launcher: "Open a torrent…", "Reading torrent…", "Looking for peers…" → "Fetching torrent metadata… (n peers)", "No peers found" (magnet, no peer after 15 s), error + "Try again", file list ("No playable video found" when none), Play.
Player banner (hidden only when playing normally): "Connecting to peers…", "Reconnecting to peers…" (data already received), "No peers found", "Preparing playback…" (container index), "Buffering…" (seconds buffered ahead, peers, rate), "Stalled" (peers connected, no data), "Seeking to h:mm:ss…" (after 1 s), "Seek pending: h:mm:ss" (after 3 s, with peers and rate), "Paused: disk space is low", "Playback error" + Retry (key R). Every banner change is also written to telemetry (`ui_banner`).

## Telemetry (JSONL, `~/Library/Logs/TorrentPlayer/telemetry-<time>-<pid>.jsonl`, local only)
Common fields: `event`, `t` (monotonic seconds since the app's telemetry started), `wall`. Events: `engine_started`, `open_torrent_file`, `open_magnet`, `metadata_ready` (fetch_s), `metadata_failed`, `play_requested`, `session_start`, `stream_open` (recovery report), `window_attached`, `file_loaded`, `duration_known`, `first_peer`, `first_piece`, `playback_started` (first PLAYBACK_RESTART; approximates the first frame, it is not a frame-presented timestamp), `state_change` (core state), `ui_launcher`, `ui_banner`, `stall_start`/`stall_end` (paused-for-cache; state and peers at start), `seek_issued` (target, whether the target was cached at issue time: coverage 0-255), `seek_done` (latency from issue to PLAYBACK_RESTART), `sample` (1 Hz: state, peers, seeds, download B/s, contiguous bytes and seconds ahead, blocked readers, cache charged/limit, pieces present/verified, hash failures, un-have resets, watchdog kicks, cap violations, playback position, paused flags), `core_events` (the core's own event log when waiting), `hash_failure` (classifier report), `mpv_log` (first 40 warnings), `error`, `system_sleep`/`system_wake` (a session with a sleep event is not valid for performance analysis), `bridge_counters`, `session_end` (totals: seeks, median/max seek latency, stalls, stall seconds, cache peak). `window_geometry` records the window/overlay geometry evidence.

## Paths (approved)
- Streaming cache: `~/Library/Caches/TorrentPlayer/stream` (limit 2 GiB by default; override with `--cache-dir`, `--limit-mib`).
- Telemetry/logs: `~/Library/Logs/TorrentPlayer/` (override `TP_LOG_DIR`).
- Persistent downloads (Milestone 5, not implemented): `~/Downloads/TorrentPlayer`, a different store with no eviction. Nothing in M2 writes there and the streaming cleanup code never touches it.

## Command-line options (acceptance runs)
`--open <.torrent|magnet>`, `--autoplay`, `--file-index N`, `--peer ip:port` (repeatable), `--cache-dir`, `--limit-mib`, `--ahead-seconds`, `--behind-seconds`, `--web-seeds off|assist|always`, `--mute`, `--ao null`, `--seek-script "after_s:to_s,..."`, `--exit-after S` (seconds of playback), `--start-timeout S`, `--loopback` (loopback test engine on 127.0.0.1:42901).
