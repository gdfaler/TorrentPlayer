/* tp_mpv_bridge: libmpv custom stream protocol "tp://" backed by tp_stream_read (the verified behaviour from the research spikes):
 *  - cancel_fn is mandatory (mpv calls it to abort a blocked read on seek/stop); it sets a ONE-SHOT token that releases the blocked read with -1;
 *  - read errors / EOF are NEVER injected by the app (that poisoned the MP4 demuxer in the spike); only mpv's own cancel releases a read;
 *  - seek_fn only repositions; size_fn reports the file size;
 *  - the mpv handle must be destroyed (mpv_terminate_destroy, off the main thread) BEFORE tp_stream_close. */
#ifndef TP_MPV_BRIDGE_H
#define TP_MPV_BRIDGE_H
#include <mpv/client.h>
#include <stdint.h>
#include "tpcore/tp_c.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Registers protocol "tp" on h; url to load: "tp://stream". Returns 0 on success (mpv error code otherwise). */
int tpb_register(mpv_handle* h, tp_stream* s);

typedef struct { int64_t opens, closes, reads, bytes, cancels, cancelled_reads, seeks, verify_mismatches; } tpb_counters;
void tpb_counters_get(tpb_counters* out);
void tpb_counters_reset(void);
/* tests only: compare every byte served with this file (env TPB_VERIFY_PATH is read at register time too). */
void tpb_set_verify_path(const char* path, int64_t file_offset_in_torrent_file);
#ifdef __cplusplus
}
#endif
#endif
