# Milestone 1 architecture notes

## Data path
libtorrent (custom `disk_interface` = `SegmentDisk`) -> `SegmentStore` (one file per piece) -> `StreamSession::read()` (blocking, never crosses a piece boundary) -> (milestone 2) mpv stream callbacks.

## SegmentStore invariants (enforced and tested)
1. A piece with any bytes on disk is charged one **full piece slot**. `charged <= internal_cap` after every write, because a write first **reserves**: victims are evicted until the slot fits; if none exists the write is **refused** (`NoSpace`), never executed over the cap.
2. Victims are never pinned pieces (2 MiB at EACH end, rounded up to whole pieces) and never pieces in the protected ahead-window `[head, hi)`. Outside-window first (farthest from the playhead), then behind-playhead oldest.
3. `present` implies "file exists and is charged": `mark_verified` is serialised with eviction and returns false if the file is gone.
4. Accounting is reconciled against measured `st_blocks` (`max(slot, measured)`) incrementally; if that pushes the total over the internal cap it evicts. `internal_cap = limit - max(2 pieces, 10 % of limit)`; the user-visible `limit` is what the independent auditors check.
5. Recovery: delete partial/foreign files, **trim first** (keep pieces nearest the head hint within the cap, before hashing or charging anything), then hash-verify what remains, delete corrupt.
6. Orphan partial piece files are purged only when libtorrent forgets partial state (re-add).

## Scheduler (`StreamSession::tick`, 100 ms)
playhead estimate (reads + app hints; jumps into head/tail zones ignored) -> `WindowPlanner` (sizes from the internal cap budget; hysteresis refill below 50 %) -> protection snapshot -> sweep outside window (30 s touch guard) -> reconcile 256 files -> stale-have detection for the demanded piece -> **un-have = remove + re-add with `have_pieces`, window priorities and known peers** -> priorities/deadlines -> connectivity watchdog (re-dial known peers, then re-add) -> web-seed policy (1 Hz) -> disk policy (2 s) -> status snapshot.

## Threads
libtorrent's threads; Engine alert thread (dispatches per-torrent alerts to the session: block sources, hash failures, file errors, bans); one scheduler thread per `StreamSession`; reader threads (callers of `read`). Lock order: `io_mu_` -> `mu_`. Auditors take no engine/store lock.

## Known limitations (milestone 1)
- v1 and hybrid torrents only (v1 hashes); no v2-only support.
- Magnet links resolve metadata first (`Engine::fetch_metadata`), then stream.
- Real-swarm latency (startup, cold seeks) is dominated by the swarm; see the spec for observed numbers.
- The cause of APFS allocation beyond logical size on the system volume is unexplained; the margin + reconciliation exist because of it (risk R3, hard release gate).
