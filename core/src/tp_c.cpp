// tp_c.cpp: implementation of the C interface (core/include/tpcore/tp_c.h). Exceptions never cross the boundary.
#include "tpcore/tp_c.h"
#include <libtorrent/load_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/torrent_info.hpp>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include "tpcore/engine.hpp"
#include "tpcore/stream_session.hpp"

namespace {
constexpr std::int64_t kMaxTorrentBytes = 16LL << 20;        // hostile-input bound: a .torrent larger than this is refused
constexpr double kMagnetTimeoutS = 240.0;
constexpr int kMaxFiles = 20000;

void put(char* dst, size_t n, const std::string& s) {
  if (!dst || n == 0) return;
  const size_t k = std::min(n - 1, s.size());
  std::memcpy(dst, s.data(), k); dst[k] = 0;
}
bool is_video_name(const std::string& path) {
  auto dot = path.rfind('.');
  if (dot == std::string::npos) return false;
  std::string e = path.substr(dot + 1);
  for (auto& c : e) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
  static const char* exts[] = {"mkv", "mp4", "m4v", "avi", "webm", "mov", "ts", "m2ts", "mts", "wmv", "flv", "mpg", "mpeg", "ogv", "vob"};
  for (auto* x : exts) if (e == x) return true;
  return false;
}
}  // namespace

struct tp_engine { std::unique_ptr<tp::Engine> eng; };
struct tp_cancel { std::atomic<bool> flag{false}; };
struct tp_meta {
  tp_engine* owner = nullptr;
  std::mutex mu;
  std::shared_ptr<tp::MetadataJob> job;
  std::vector<char> bytes;
  std::shared_ptr<lt::torrent_info> ti;
  int state = TP_META_FETCHING;
  std::string error;
  int peers = 0;
  double elapsed = 0;
  std::vector<std::string> extra_peers, trackers, web_seeds;     // from the magnet link: x.pe / tr / ws
};
struct tp_stream { std::shared_ptr<tp::StreamSession> s; };

static bool parse_ti(tp_meta* m, std::string* err) {
  try {
    lt::load_torrent_limits lim;
    lim.max_buffer_size = static_cast<int>(kMaxTorrentBytes);
    m->ti = std::make_shared<lt::torrent_info>(lt::span<char const>(m->bytes.data(), static_cast<std::ptrdiff_t>(m->bytes.size())), lim, lt::from_span);
  } catch (std::exception const& e) { *err = std::string("not a valid torrent: ") + e.what(); m->ti.reset(); return false; }
  if (!m->ti || !m->ti->is_valid()) { *err = "not a valid torrent"; return false; }
  if (m->ti->num_files() <= 0 || m->ti->num_files() > kMaxFiles) { *err = "torrent has an unsupported number of files"; m->ti.reset(); return false; }
  return true;
}

extern "C" {

tp_engine* tp_engine_create(const tp_engine_config* cfg, char* err, size_t errlen) {
  try {
    if (!cfg || !cfg->cache_root) { put(err, errlen, "cache_root required"); return nullptr; }
    tp::EngineConfig ec;
    ec.cache_root = std::filesystem::absolute(cfg->cache_root).lexically_normal();      // libtorrent normalises save paths: always hand it an absolute one
    if (cfg->evidence_dir) ec.evidence_dir = std::filesystem::absolute(cfg->evidence_dir).lexically_normal();
    if (cfg->listen_interfaces) ec.listen_interfaces = cfg->listen_interfaces;
    ec.loopback_test_mode = cfg->loopback_test_mode != 0;
    if (cfg->loopback_test_mode) { ec.enable_dht = false; ec.enable_lsd = false; }
    else ec.enable_dht = cfg->enable_dht != 0 || cfg->listen_interfaces == nullptr;
    std::filesystem::create_directories(ec.cache_root);
    auto* e = new tp_engine;
    e->eng = std::make_unique<tp::Engine>(ec);
    return e;
  } catch (std::exception const& ex) { put(err, errlen, ex.what()); return nullptr; }
}
void tp_engine_destroy(tp_engine* e) { try { delete e; } catch (...) {} }

tp_meta* tp_meta_from_file(tp_engine* e, const char* path, char* err, size_t errlen) {
  try {
    if (!e || !path) { put(err, errlen, "bad arguments"); return nullptr; }
    std::ifstream f(path, std::ios::binary);
    if (!f) { put(err, errlen, std::string("cannot read ") + path); return nullptr; }
    f.seekg(0, std::ios::end); const std::int64_t sz = f.tellg(); f.seekg(0);
    if (sz <= 0 || sz > kMaxTorrentBytes) { put(err, errlen, "torrent file is empty or too large"); return nullptr; }
    auto* m = new tp_meta; m->owner = e;
    m->bytes.resize(static_cast<size_t>(sz));
    f.read(m->bytes.data(), sz);
    std::string why;
    if (!parse_ti(m, &why)) { put(err, errlen, why); delete m; return nullptr; }
    m->state = TP_META_READY;
    return m;
  } catch (std::exception const& ex) { put(err, errlen, ex.what()); return nullptr; }
}

tp_meta* tp_meta_from_magnet(tp_engine* e, const char* magnet, char* err, size_t errlen) {
  try {
    if (!e || !magnet || std::strncmp(magnet, "magnet:?", 8) != 0 || std::strlen(magnet) > 8192) { put(err, errlen, "not a magnet link"); return nullptr; }
    std::string why;
    auto job = e->eng->start_metadata(magnet, &why);
    if (!job) { put(err, errlen, why); return nullptr; }
    auto* m = new tp_meta; m->owner = e; m->job = job;
    lt::error_code pec; lt::add_torrent_params pp = lt::parse_magnet_uri(magnet, pec);
    if (!pec) {
      for (auto const& t : pp.trackers) if (m->trackers.size() < 64) m->trackers.push_back(t);
      for (auto const& u : pp.url_seeds) if (m->web_seeds.size() < 16) m->web_seeds.push_back(u);
      for (auto const& p : pp.peers) if (m->extra_peers.size() < 64) m->extra_peers.push_back(p.address().to_string() + ":" + std::to_string(p.port()));
    }
    return m;
  } catch (std::exception const& ex) { put(err, errlen, ex.what()); return nullptr; }
}

void tp_meta_status_get(tp_meta* m, tp_meta_status* out) {
  if (!m || !out) return;
  std::memset(out, 0, sizeof *out);
  try {
    std::lock_guard<std::mutex> g(m->mu);
    if (m->state == TP_META_FETCHING && m->job) {
      auto p = m->owner->eng->poll_metadata(*m->job);
      m->elapsed = p.elapsed_s; m->peers = p.peers;
      if (p.ready) {
        m->bytes = std::move(p.bytes);
        std::string why;
        if (m->bytes.size() > static_cast<size_t>(kMaxTorrentBytes) || !parse_ti(m, &why)) { m->state = TP_META_FAILED; m->error = why.empty() ? "metadata too large" : why; }
        else m->state = TP_META_READY;
      } else if (p.elapsed_s > kMagnetTimeoutS) {
        m->owner->eng->cancel_metadata(*m->job);
        m->state = TP_META_FAILED; m->error = m->peers == 0 ? "No peers found: could not fetch the torrent metadata" : "Timed out fetching the torrent metadata";
      }
    }
    out->state = m->state; out->peers = m->peers; out->elapsed_s = m->elapsed;
    put(out->error, sizeof out->error, m->error);
  } catch (...) { out->state = TP_META_FAILED; put(out->error, sizeof out->error, "internal error"); }
}

void tp_meta_cancel(tp_meta* m) {
  if (!m) return;
  try { std::lock_guard<std::mutex> g(m->mu); if (m->job) m->owner->eng->cancel_metadata(*m->job);
        if (m->state == TP_META_FETCHING) { m->state = TP_META_FAILED; m->error = "Cancelled"; } } catch (...) {}
}
void tp_meta_free(tp_meta* m) { if (!m) return; tp_meta_cancel(m); try { delete m; } catch (...) {} }

int tp_meta_name(tp_meta* m, char* out, size_t outlen) {
  if (!m) return 0;
  std::lock_guard<std::mutex> g(m->mu);
  if (!m->ti) return 0;
  put(out, outlen, m->ti->name()); return 1;
}
int tp_meta_file_count(tp_meta* m) { if (!m) return 0; std::lock_guard<std::mutex> g(m->mu); return m->ti ? m->ti->num_files() : 0; }
int tp_meta_file_info(tp_meta* m, int index, char* path_out, size_t pathlen, int64_t* size_out, int* is_video_out) {
  if (!m) return 0;
  std::lock_guard<std::mutex> g(m->mu);
  if (!m->ti || index < 0 || index >= m->ti->num_files()) return 0;
  const auto& fs = m->ti->files();
  const std::string p = std::string(fs.file_path(lt::file_index_t(index)));
  put(path_out, pathlen, p);
  if (size_out) *size_out = fs.file_size(lt::file_index_t(index));
  if (is_video_out) *is_video_out = is_video_name(p) ? 1 : 0;
  return 1;
}
int tp_meta_default_video(tp_meta* m) {
  if (!m) return -1;
  std::lock_guard<std::mutex> g(m->mu);
  if (!m->ti) return -1;
  const auto& fs = m->ti->files();
  int best = -1; std::int64_t bs = -1;
  for (int i = 0; i < fs.num_files(); ++i) {
    const std::string p = std::string(fs.file_path(lt::file_index_t(i)));
    if (is_video_name(p) && fs.file_size(lt::file_index_t(i)) > bs) { bs = fs.file_size(lt::file_index_t(i)); best = i; }
  }
  return best;
}
void tp_meta_add_peer(tp_meta* m, const char* ip_port) { if (!m || !ip_port) return; std::lock_guard<std::mutex> g(m->mu); m->extra_peers.emplace_back(ip_port); }

tp_stream* tp_stream_open(tp_engine* e, tp_meta* m, int file_index, const tp_stream_options* o, char* err, size_t errlen) {
  try {
    if (!e || !m) { put(err, errlen, "bad arguments"); return nullptr; }
    tp::TorrentSource src;
    { std::lock_guard<std::mutex> g(m->mu);
      if (m->state != TP_META_READY || m->bytes.empty()) { put(err, errlen, "metadata not ready"); return nullptr; }
      if (file_index < 0 || file_index >= m->ti->num_files()) { put(err, errlen, "file index out of range"); return nullptr; }
      src.torrent_bytes = m->bytes; src.extra_peers = m->extra_peers; src.trackers = m->trackers; src.web_seed_urls = m->web_seeds; }
    tp::CacheConfig cc;
    if (o) {
      if (o->cache_limit_bytes > 0) cc.limit_bytes = o->cache_limit_bytes;
      if (o->behind_seconds > 0) cc.behind_seconds = o->behind_seconds;
      if (o->ahead_seconds > 0) cc.ahead_seconds_auto = o->ahead_seconds;
    }
    std::string why;
    auto s = e->eng->open_stream(src, file_index, cc, &why);
    if (!s) { put(err, errlen, why); return nullptr; }
    const int mode = o ? o->web_seed_mode : 1;
    s->set_web_seed_mode(mode == 0 ? tp::WebSeedMode::Off : mode == 2 ? tp::WebSeedMode::Always : tp::WebSeedMode::Assist);
    auto* r = new tp_stream; r->s = std::move(s);
    return r;
  } catch (std::exception const& ex) { put(err, errlen, ex.what()); return nullptr; }
}
void tp_stream_close(tp_stream* s) { try { delete s; } catch (...) {} }
int64_t tp_stream_size(tp_stream* s) { return s ? s->s->file_size() : 0; }
int tp_stream_file_name(tp_stream* s, char* out, size_t outlen) { if (!s) return 0; put(out, outlen, s->s->file_name()); return 1; }

tp_cancel* tp_cancel_new(void) { return new tp_cancel; }
void tp_cancel_set(tp_cancel* c, int v) { if (c) c->flag.store(v != 0); }
void tp_cancel_free(tp_cancel* c) { delete c; }

int64_t tp_stream_read(tp_stream* s, int64_t offset, void* buf, int64_t len, tp_cancel* c) {
  if (!s || !buf || len <= 0) return -1;
  try { return s->s->read(offset, buf, len, c ? &c->flag : nullptr); } catch (...) { return -1; }
}
void tp_stream_seek_hint(tp_stream* s, int64_t off) { if (s) try { s->s->seek_hint(off); } catch (...) {} }
void tp_stream_set_bitrate(tp_stream* s, double bps) { if (s && bps > 0) try { s->s->set_bitrate(bps); } catch (...) {} }

void tp_stream_status_get(tp_stream* s, tp_stream_status* o) {
  if (!s || !o) return;
  std::memset(o, 0, sizeof *o);
  try {
    const tp::StreamStatus st = s->s->status();
    o->state = static_cast<int>(st.state);
    put(o->state_text, sizeof o->state_text, tp::to_string(st.state));
    o->peers = st.peers; o->seeds = st.seeds; o->web_connections = st.web_connections;
    o->download_rate = st.download_rate; o->upload_rate = st.upload_rate; o->peer_bytes = st.peer_bytes; o->web_bytes = st.web_bytes;
    o->blocked_readers = st.blocked_readers;
    o->piece_size = s->s->piece_size(); o->num_pieces = s->s->num_pieces();
    o->head_piece = st.head; o->window_lo = st.lo; o->window_hi_full = st.hi_full; o->window_hi_wanted = st.hi_wanted; o->contiguous_ahead_pieces = st.contiguous_ahead;
    o->contiguous_ahead_bytes = static_cast<int64_t>(st.contiguous_ahead) * s->s->piece_size();
    o->refilling = st.refilling ? 1 : 0;
    o->cache_limit = st.store.limit; o->cache_internal_cap = st.store.internal_cap; o->cache_charged = st.store.charged; o->cache_max_charged = st.store.max_charged;
    o->cap_violations = st.store.cap_violations; o->refused_writes = st.store.refused_writes; o->forced_evictions = st.store.forced_evictions;
    o->pieces_present = st.store.present;
    o->disk_state = static_cast<int>(st.disk_state);
    o->hash_failures = st.hash_failures; o->hash_storage = st.hash_storage; o->hash_arrived = st.hash_arrived; o->hash_undetermined = st.hash_undetermined;
    o->pieces_verified = st.pieces_verified; o->peer_bans = st.peer_bans;
    o->un_have_resets = st.un_have_resets; o->watchdog_kicks = st.connect_kicks; o->watchdog_resets = st.watchdog_resets;
    o->seconds_without_peers = st.seconds_without_peers; o->web_seed_active = st.web_seed_active ? 1 : 0;
    const auto& r = s->s->recovery_report();
    o->recovery_verified = r.verified_present; o->recovery_deleted_partial = r.deleted_partial; o->recovery_deleted_corrupt = r.deleted_corrupt; o->recovery_trimmed = r.trimmed;
    const auto& w = st.wait;
    o->wait_kind = static_cast<int>(w.kind); o->wait_offset = w.offset; o->wait_piece = w.piece; o->wait_piece_priority = w.priority; o->wait_piece_deadline_ms = w.deadline_ms;
    o->wait_blocks_total = w.blocks_total; o->wait_blocks_done = w.blocks_done; o->wait_blocks_writing = w.blocks_writing; o->wait_blocks_requested = w.blocks_requested; o->wait_blocks_pad = w.blocks_pad;
    o->wait_in_queue = w.in_queue ? 1 : 0; o->wait_peers_total = w.peers_total; o->wait_peers_with_piece = w.peers_with_piece; o->wait_peers_requesting = w.peers_requesting;
    o->wait_web_requesting = w.web_requesting; o->wait_no_progress_s = w.no_progress_s;
    o->head_pin_pieces = w.head_pin_pieces; o->head_pin_present = w.head_pin_present; o->tail_pin_pieces = w.tail_pin_pieces; o->tail_pin_present = w.tail_pin_present;
    o->head_pin_blocks_total = w.head_pin_blocks_total; o->head_pin_blocks_done = w.head_pin_blocks_done; o->tail_pin_blocks_total = w.tail_pin_blocks_total; o->tail_pin_blocks_done = w.tail_pin_blocks_done;
    o->web_seed_errors = st.web_seed_errors; o->web_disconnects = st.web_disconnects; o->request_drops = st.request_drops; o->block_timeouts = st.block_timeouts;
    put(o->web_seed_last_error, sizeof o->web_seed_last_error, st.web_seed_last_error);
    o->hash_incomplete = st.store.hash_incomplete;
  } catch (...) { o->state = TP_STATE_ERROR; put(o->state_text, sizeof o->state_text, "Error"); }
}

void tp_stream_coverage(tp_stream* s, int n, uint8_t* out) {
  if (!s || !out || n <= 0) return;
  try {
    const std::int64_t fsz = s->s->file_size();
    const int p0 = s->s->piece_of_file_offset(0), p1 = s->s->piece_of_file_offset(std::max<std::int64_t>(0, fsz - 1));
    const int np = p1 - p0 + 1;
    for (int i = 0; i < n; ++i) {
      const int a = p0 + static_cast<int>(static_cast<std::int64_t>(np) * i / n);
      const int b = std::max(a + 1, p0 + static_cast<int>(static_cast<std::int64_t>(np) * (i + 1) / n));
      int have = 0, tot = 0;
      for (int p = a; p < b && p <= p1; ++p) { ++tot; have += s->s->store().has(p) ? 1 : 0; }
      out[i] = static_cast<uint8_t>(tot ? (255 * have) / tot : 0);
    }
  } catch (...) { std::memset(out, 0, static_cast<size_t>(n)); }
}

int tp_stream_events(tp_stream* s, char* out, size_t outlen) {
  if (!s) return 0;
  std::string all;
  try { for (auto& e : s->s->recent_events()) { all += e; all += '\n'; } } catch (...) {}
  put(out, outlen, all);
  return static_cast<int>(std::min(all.size(), outlen ? outlen - 1 : 0));
}
int tp_stream_last_hash_report(tp_stream* s, char* out, size_t outlen) {
  if (!s) return 0;
  std::string r; try { r = s->s->shadow().last_report(); } catch (...) {}
  put(out, outlen, r); return static_cast<int>(r.size());
}

}  // extern "C"
