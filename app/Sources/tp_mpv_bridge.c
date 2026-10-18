#include "tp_mpv_bridge.h"
#include <mpv/stream_cb.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static atomic_llong c_opens, c_closes, c_reads, c_bytes, c_cancels, c_cancelled_reads, c_seeks, c_mismatch;
static atomic_int g_verify_fd = -1;
static int64_t g_verify_base = 0;

typedef struct { tp_stream* stream; int64_t pos; tp_cancel* cancel; } Cookie;

static int64_t cb_read(void* cookie, char* buf, uint64_t n) {
  Cookie* c = cookie;
  const int64_t r = tp_stream_read(c->stream, c->pos, buf, (int64_t)n, c->cancel);
  if (r < 0) { atomic_fetch_add(&c_cancelled_reads, 1); tp_cancel_set(c->cancel, 0); return -1; }   /* released by mpv's own cancel: one-shot token */
  if (r == 0) return 0;                                                                           /* genuine EOF only */
  atomic_fetch_add(&c_reads, 1); atomic_fetch_add(&c_bytes, r);
  int fd = atomic_load(&g_verify_fd);
  if (fd >= 0) { char* ref = malloc((size_t)r); if (ref) { ssize_t k = pread(fd, ref, (size_t)r, (off_t)(g_verify_base + c->pos)); if (k != r || memcmp(ref, buf, (size_t)r) != 0) atomic_fetch_add(&c_mismatch, 1); free(ref); } }
  c->pos += r;
  return r;
}
static int64_t cb_seek(void* cookie, int64_t off) {
  Cookie* c = cookie; atomic_fetch_add(&c_seeks, 1);
  if (off < 0 || off > tp_stream_size(c->stream)) return MPV_ERROR_GENERIC;
  c->pos = off; return off;
}
static int64_t cb_size(void* cookie) { return tp_stream_size(((Cookie*)cookie)->stream); }
static void cb_close(void* cookie) { Cookie* c = cookie; atomic_fetch_add(&c_closes, 1); tp_cancel_free(c->cancel); free(c); }
static void cb_cancel(void* cookie) { Cookie* c = cookie; atomic_fetch_add(&c_cancels, 1); tp_cancel_set(c->cancel, 1); }
static int cb_open(void* user_data, char* uri, mpv_stream_cb_info* info) {
  (void)uri;
  Cookie* c = calloc(1, sizeof *c); if (!c) return MPV_ERROR_NOMEM;
  c->stream = user_data; c->cancel = tp_cancel_new();
  atomic_fetch_add(&c_opens, 1);
  info->cookie = c; info->read_fn = cb_read; info->seek_fn = cb_seek; info->size_fn = cb_size; info->close_fn = cb_close; info->cancel_fn = cb_cancel;
  return 0;
}
int tpb_register(mpv_handle* h, tp_stream* s) {
  const char* vp = getenv("TPB_VERIFY_PATH");
  if (vp && *vp) { int fd = open(vp, O_RDONLY); atomic_store(&g_verify_fd, fd); g_verify_base = 0; }
  return mpv_stream_cb_add_ro(h, "tp", s, cb_open);
}
void tpb_set_verify_path(const char* path, int64_t base) { int fd = path ? open(path, O_RDONLY) : -1; atomic_store(&g_verify_fd, fd); g_verify_base = base; }
void tpb_counters_get(tpb_counters* o) {
  o->opens = c_opens; o->closes = c_closes; o->reads = c_reads; o->bytes = c_bytes; o->cancels = c_cancels; o->cancelled_reads = c_cancelled_reads; o->seeks = c_seeks; o->verify_mismatches = c_mismatch;
}
void tpb_counters_reset(void) { c_opens = c_closes = c_reads = c_bytes = c_cancels = c_cancelled_reads = c_seeks = c_mismatch = 0; }
