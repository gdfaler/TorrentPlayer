/* tp_c.h: the narrow, C-compatible interface between the native app (Swift/AppKit) and the C++ core (tpcore).
 * No C++ types cross this boundary. All functions are thread-safe unless stated; none throw. Strings are UTF-8, caller-owned buffers with explicit sizes.
 * Hostile input (torrent files, magnet links, peers) is bounds-checked inside; failures are reported through the err buffer / return codes. */
#ifndef TP_C_H
#define TP_C_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct tp_engine tp_engine;
typedef struct tp_meta tp_meta;       /* torrent metadata: loaded from a .torrent file or fetched for a magnet link */
typedef struct tp_stream tp_stream;   /* one streamed file of one torrent */
typedef struct tp_cancel tp_cancel;   /* cancellation token for blocking reads */

typedef struct {
  const char* cache_root;             /* streaming cache root (per-torrent directories live here) */
  const char* evidence_dir;           /* hash-failure evidence; NULL = <cache_root>/evidence */
  const char* listen_interfaces;      /* NULL = "0.0.0.0:6881,[::]:6881" */
  int loopback_test_mode;             /* tests only */
  int enable_dht;                     /* 1 default behaviour when listen_interfaces is NULL; tests set 0 */
} tp_engine_config;

tp_engine* tp_engine_create(const tp_engine_config* cfg, char* err, size_t errlen);
void tp_engine_destroy(tp_engine*);

/* ---- metadata */
typedef enum { TP_META_FETCHING = 0, TP_META_READY = 1, TP_META_FAILED = 2 } tp_meta_state;
typedef struct { int state; int peers; double elapsed_s; char error[160]; } tp_meta_status;

tp_meta* tp_meta_from_file(tp_engine*, const char* path, char* err, size_t errlen);
tp_meta* tp_meta_from_magnet(tp_engine*, const char* magnet_uri, char* err, size_t errlen);  /* starts fetching; poll tp_meta_status_get */
void tp_meta_status_get(tp_meta*, tp_meta_status* out);       /* fetching / ready / failed (+ peers seen while fetching) */
void tp_meta_cancel(tp_meta*);
void tp_meta_free(tp_meta*);                                  /* cancels if still fetching */
int tp_meta_name(tp_meta*, char* out, size_t outlen);         /* torrent name; 0 if not ready */
int tp_meta_file_count(tp_meta*);                             /* 0 until ready */
int tp_meta_file_info(tp_meta*, int index, char* path_out, size_t pathlen, int64_t* size_out, int* is_video_out);
int tp_meta_default_video(tp_meta*);                          /* index of the largest video file, or -1 if there is none */
/* tests / known peers: "ip:port" added to the next stream opened from this metadata */
void tp_meta_add_peer(tp_meta*, const char* ip_port);

/* ---- streaming */
typedef struct {
  int64_t cache_limit_bytes;          /* user-visible limit; 0 = 2 GiB */
  double behind_seconds;              /* 0 = 300 */
  double ahead_seconds;               /* 0 = 75 (automatic target) */
  int web_seed_mode;                  /* 0 off, 1 assist (default), 2 always */
} tp_stream_options;

tp_stream* tp_stream_open(tp_engine*, tp_meta*, int file_index, const tp_stream_options*, char* err, size_t errlen);
void tp_stream_close(tp_stream*);
int64_t tp_stream_size(tp_stream*);
int tp_stream_file_name(tp_stream*, char* out, size_t outlen);

tp_cancel* tp_cancel_new(void);
void tp_cancel_set(tp_cancel*, int cancelled);
void tp_cancel_free(tp_cancel*);
/* Blocking read. Returns bytes copied (>0), 0 at EOF, -1 if cancelled. Never copies across a piece boundary. cancel may be NULL. */
int64_t tp_stream_read(tp_stream*, int64_t offset, void* buf, int64_t len, tp_cancel* cancel);
void tp_stream_seek_hint(tp_stream*, int64_t offset);        /* the user is about to go there */
void tp_stream_set_bitrate(tp_stream*, double bytes_per_second);

typedef enum {
  TP_STATE_CONNECTING = 0, TP_STATE_NO_PEERS, TP_STATE_PREPARING, TP_STATE_BUFFERING, TP_STATE_READY, TP_STATE_STALLED, TP_STATE_DISK_PAUSED, TP_STATE_ERROR
} tp_state;

typedef struct {
  int state;                          /* tp_state */
  char state_text[64];                /* human-readable, e.g. "No peers found" */
  int peers, seeds, web_connections;
  double download_rate, upload_rate;  /* bytes/s */
  int64_t peer_bytes, web_bytes;
  int blocked_readers;                /* reads waiting for data right now */
  int64_t piece_size; int num_pieces;
  int head_piece, window_lo, window_hi_full, window_hi_wanted, contiguous_ahead_pieces;
  int64_t contiguous_ahead_bytes;
  int refilling;
  /* cache accounting (the tested strict-cap machinery) */
  int64_t cache_limit, cache_internal_cap, cache_charged, cache_max_charged;
  int64_t cap_violations, refused_writes, forced_evictions;
  int pieces_present;
  int disk_state;                     /* 0 ok, 1 warning, 2 paused */
  /* integrity */
  int hash_failures, hash_storage, hash_arrived, hash_undetermined, pieces_verified, peer_bans;
  /* recovery / resilience */
  int un_have_resets, watchdog_kicks, watchdog_resets;
  double seconds_without_peers;
  int web_seed_active;
  int recovery_verified, recovery_deleted_partial, recovery_deleted_corrupt, recovery_trimmed;
  /* what the reader is waiting for right now (startup / buffering diagnostics; facts only, ~1 Hz) */
  int wait_kind;                      /* 0 nothing, 1 start-of-file index, 2 end-of-file index (container metadata), 3 file data */
  int64_t wait_offset;                /* file offset of the blocked read; -1 when no read is blocked */
  int wait_piece;                     /* piece needed next; -1 none */
  int wait_piece_priority;            /* libtorrent priority 0..7; -1 unknown */
  int wait_piece_deadline_ms;         /* deadline TorrentPlayer asked for; -1 none */
  int wait_blocks_total, wait_blocks_done, wait_blocks_writing, wait_blocks_requested;   /* REAL data blocks only */
  int wait_blocks_pad;                /* blocks of that piece that are entirely BEP 47 padding (never downloaded) */
  int wait_in_queue;                  /* 1 if libtorrent has the piece in its download queue */
  int wait_peers_total;               /* connected non-web peers */
  int wait_peers_with_piece;          /* of those, peers whose bitfield has the needed piece */
  int wait_peers_requesting;          /* of those, peers with open block requests */
  int wait_web_requesting;            /* web-seed connections with open requests */
  double wait_no_progress_s;          /* seconds since the needed piece last gained a block (0 when nothing is blocked) */
  int head_pin_pieces, head_pin_present, tail_pin_pieces, tail_pin_present;
  int head_pin_blocks_total, head_pin_blocks_done, tail_pin_blocks_total, tail_pin_blocks_done;
  int web_seed_errors, web_disconnects, request_drops, block_timeouts;
  char web_seed_last_error[96];
  int64_t hash_incomplete;            /* verification attempts that found real data missing (not a SHA-1 mismatch) */
} tp_stream_status;
void tp_stream_status_get(tp_stream*, tp_stream_status* out);

/* Coverage for the seek bar: out[i] in 0..255 = fraction of the i-th of n equal file buckets that is present in the cache. */
void tp_stream_coverage(tp_stream*, int n, uint8_t* out);
/* Recent events (newline separated), most recent last; returns bytes written. */
int tp_stream_events(tp_stream*, char* out, size_t outlen);
/* Hash-failure evidence is classified by the core; this returns a one-line summary of the last report ("" if none). */
int tp_stream_last_hash_report(tp_stream*, char* out, size_t outlen);

/* ---- Download & Play: complete, hash-verified downloads into a user-chosen folder; state survives restarts (see tpcore/download.hpp) */
typedef struct tp_dlmgr tp_dlmgr;
typedef struct {
  const char* state_dir;              /* per-download bookkeeping (torrent, resume data, state); required */
  const char* listen_interfaces;      /* NULL = "0.0.0.0:6882,[::]:6882" */
  int loopback_test_mode;             /* tests only */
  int enable_dht;                     /* 0 disables (tests) */
  int auto_resume_on_network;         /* 1 = continue automatically when the network returns */
} tp_dl_config;
typedef enum {
  TP_DL_CHECKING = 0, TP_DL_DOWNLOADING, TP_DL_WAITING_NETWORK, TP_DL_WAITING_SOURCES, TP_DL_PAUSED, TP_DL_RESUMING, TP_DL_COMPLETED, TP_DL_FAILED
} tp_dl_state;
typedef struct {
  char id[48]; char name[256]; char save_path[1024]; char primary_file[1536]; char error[256]; char state_text[32];
  int state;                          /* tp_dl_state */
  int64_t selected_bytes, verified_bytes; double progress;          /* verified = bytes of hash-verified pieces inside the selected files */
  int download_rate, upload_rate, peers, web_seeds;
  int64_t eta_s;                      /* -1 = unknown */
  int user_paused, network_available, auto_play, completed;
  double checking_progress;
  int trackers_total, trackers_ok, trackers_failing, connecting;   /* announce results (answered / failed; the rest not tried yet) and connection attempts in progress */
} tp_dl_status;

tp_dlmgr* tp_dl_create(const tp_dl_config*, char* err, size_t errlen);
void tp_dl_destroy(tp_dlmgr*);        /* graceful: writes resume data first */
int tp_dl_restore(tp_dlmgr*);         /* re-adds persisted downloads; returns how many exist */
/* files/nfiles: selected indices (nfiles 0 = whole torrent). The metadata's known peers/trackers/web seeds are used. Returns 1 and the id on success. */
int tp_dl_add(tp_dlmgr*, tp_meta*, const char* dest_folder, const int* files, int nfiles, int auto_play, char* id_out, size_t idlen, char* err, size_t errlen);
int tp_dl_list(tp_dlmgr*, tp_dl_status* out, int max);            /* returns the number of entries written */
int tp_dl_get(tp_dlmgr*, const char* id, tp_dl_status* out);      /* 1 if found */
int tp_dl_pause(tp_dlmgr*, const char* id);
int tp_dl_resume(tp_dlmgr*, const char* id);
int tp_dl_cancel(tp_dlmgr*, const char* id, int delete_files);    /* files are kept unless delete_files != 0 */
void tp_dl_set_network(tp_dlmgr*, int online);                    /* from the OS path monitor */
void tp_dl_set_auto_resume(tp_dlmgr*, int on);
void tp_dl_save_all(tp_dlmgr*);

#ifdef __cplusplus
}
#endif
#endif
