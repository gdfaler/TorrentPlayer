// M2 acceptance support: drives the C interface (tp_c.h) exactly as the app does, against the loopback swarm. Covers .torrent and magnet opening, file listing,
// verified streaming reads and seeks (every byte), cancellation, status/coverage/events, and hostile-input handling. No C++ core types are used except the test fixture.
#include <libtorrent/torrent_info.hpp>
#include "loopback_fixture.hpp"
#include "tpcore/tp_c.h"
using namespace tpit;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; ITLOG("FAIL: " __VA_ARGS__); } else ITLOG("ok:   " __VA_ARGS__); } while (0)

int main() {
  Payload pl = Payload::create(data_dir(), 192LL << 20, 262144, 11);
  Seeder s1(pl, 42101), s2(pl, 42102);
  const fs::path dir = fresh_dir("capi");
  const fs::path tfile = dir / "t.torrent";
  { std::ofstream f(tfile, std::ios::binary); f.write(pl.torrent_bytes.data(), static_cast<std::streamsize>(pl.torrent_bytes.size())); }

  char err[256] = {0};
  tp_engine_config ec{}; std::string cache = (dir / "cache").string(); ec.cache_root = cache.c_str(); ec.listen_interfaces = "127.0.0.1:42001"; ec.loopback_test_mode = 1;
  tp_engine* eng = tp_engine_create(&ec, err, sizeof err);
  CHECK(eng != nullptr, "engine created (%s)", err);
  if (!eng) return 2;

  // ---- hostile / invalid input never crashes and reports an error
  { char e[200] = {0};
    CHECK(tp_meta_from_file(eng, "/nonexistent/x.torrent", e, sizeof e) == nullptr && e[0], "missing .torrent file refused: '%s'", e);
    const fs::path junk = dir / "junk.torrent"; { std::ofstream f(junk, std::ios::binary); f << "this is not a torrent"; }
    e[0] = 0; CHECK(tp_meta_from_file(eng, junk.c_str(), e, sizeof e) == nullptr && e[0], "garbage .torrent refused: '%s'", e);
    const fs::path empty = dir / "empty.torrent"; { std::ofstream f(empty, std::ios::binary); }
    e[0] = 0; CHECK(tp_meta_from_file(eng, empty.c_str(), e, sizeof e) == nullptr && e[0], "empty .torrent refused: '%s'", e);
    const fs::path big = dir / "big.torrent"; { std::ofstream f(big, std::ios::binary); std::string chunk(1 << 20, 'd'); for (int i = 0; i < 17; ++i) f << chunk; }
    e[0] = 0; CHECK(tp_meta_from_file(eng, big.c_str(), e, sizeof e) == nullptr && e[0], "oversized .torrent (17 MiB) refused: '%s'", e);
    e[0] = 0; CHECK(tp_meta_from_magnet(eng, "http://example.com", e, sizeof e) == nullptr && e[0], "non-magnet text refused: '%s'", e);
    e[0] = 0; CHECK(tp_meta_from_magnet(eng, "magnet:?dn=nohash", e, sizeof e) == nullptr && e[0], "magnet without hash refused: '%s'", e);
    CHECK(tp_stream_read(nullptr, 0, nullptr, 0, nullptr) == -1, "read on a null stream returns -1");
    CHECK(tp_meta_file_count(nullptr) == 0 && tp_meta_default_video(nullptr) == -1, "null metadata handled");
  }

  // ---- .torrent file path
  tp_meta* m = tp_meta_from_file(eng, tfile.c_str(), err, sizeof err);
  CHECK(m != nullptr, ".torrent loaded (%s)", err);
  if (!m) return 2;
  tp_meta_status ms; tp_meta_status_get(m, &ms);
  CHECK(ms.state == TP_META_READY, "metadata ready immediately for a .torrent file");
  CHECK(tp_meta_file_count(m) == 1, "file count = 1");
  char path[512]; int64_t fsize = 0; int isv = -1;
  CHECK(tp_meta_file_info(m, 0, path, sizeof path, &fsize, &isv) == 1 && fsize == pl.size && isv == 0, "file 0: '%s' %lld bytes, is_video=%d (a .bin is not a video)", path, (long long)fsize, isv);
  CHECK(tp_meta_default_video(m) == -1, "no playable video detected -> -1 (the app shows 'No playable video found')");
  CHECK(tp_meta_file_info(m, 5, path, sizeof path, &fsize, &isv) == 0, "out-of-range file index refused");
  for (auto* sd : {&s1, &s2}) tp_meta_add_peer(m, sd->endpoint().c_str());

  tp_stream_options opt{}; opt.cache_limit_bytes = 48LL << 20; opt.behind_seconds = 8; opt.ahead_seconds = 8; opt.web_seed_mode = 0;
  tp_stream* st = tp_stream_open(eng, m, 0, &opt, err, sizeof err);
  CHECK(st != nullptr, "stream opened (%s)", err);
  if (!st) return 2;
  CHECK(tp_stream_size(st) == pl.size, "stream size matches");
  tp_stream_set_bitrate(st, 4.0 * 1048576);

  // ---- verified reads: head, tail, forward, many random seeks (every byte compared with the deterministic payload)
  std::vector<char> buf(256 * 1024);
  int mism = 0, bad = 0; std::mt19937 rng(3);
  auto rd = [&](int64_t off, tp_cancel* c = nullptr) { int64_t n = tp_stream_read(st, off, buf.data(), static_cast<int64_t>(buf.size()), c); if (n > 0 && !pl.matches(off, buf.data(), n)) ++mism; return n; };
  CHECK(rd(pl.size - 65536) > 0, "tail read (container index) served");
  CHECK(rd(0) > 0, "head read served");
  int64_t pos = 0; int states_seen = 0; bool saw_ready = false;
  for (int i = 0; i < 300; ++i) { int64_t n = rd(pos); if (n <= 0) { ++bad; break; } pos += n; }
  tp_stream_status ss; tp_stream_status_get(st, &ss); states_seen |= 1 << ss.state; saw_ready |= ss.state == TP_STATE_READY;
  CHECK(bad == 0 && mism == 0, "300 sequential reads verified (mismatches %d)", mism);
  double worst = 0;
  for (int i = 0; i < 40; ++i) {
    const int64_t tgt = static_cast<int64_t>(rng() % static_cast<std::uint64_t>(pl.size - 1048576));
    tp_stream_seek_hint(st, tgt);
    auto t0 = std::chrono::steady_clock::now(); int64_t n = rd(tgt); double l = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    worst = std::max(worst, l); if (n <= 0) ++bad;
  }
  CHECK(bad == 0 && mism == 0, "40 random seeks served, every byte verified (worst %.2f s, mismatches %d)", worst, mism);

  // ---- cancellation: with every peer gone, a read of data not in the cache blocks; setting the token must release it quickly with -1
  tp_stream_status_get(st, &ss);
  s1.stop(); s2.stop();
  tp_cancel* cn = tp_cancel_new();
  int64_t far = -1;                                                 // an offset that is certainly NOT cached: the middle of an empty coverage bucket
  { std::vector<uint8_t> c0(64); tp_stream_coverage(st, 64, c0.data()); for (int i = 63; i >= 0; --i) if (c0[i] == 0) { far = pl.size / 64 * i + (pl.size / 128); break; } }
  CHECK(far >= 0, "found an uncached region for the cancellation test (offset %lld)", (long long)far);
  std::atomic<int64_t> result{12345}; std::atomic<double> took{0};
  std::thread reader([&] { auto t0 = std::chrono::steady_clock::now(); tp_stream_seek_hint(st, far); int64_t n = tp_stream_read(st, far, buf.data(), static_cast<int64_t>(buf.size()), cn); took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); result = n; });
  std::this_thread::sleep_for(std::chrono::seconds(3));
  tp_stream_status_get(st, &ss);
  CHECK(result.load() == 12345, "read of uncached data blocks while no peer is reachable (blocked readers %d, state '%s')", ss.blocked_readers, ss.state_text);
  CHECK(ss.state == TP_STATE_CONNECTING || ss.state == TP_STATE_NO_PEERS || ss.state == TP_STATE_BUFFERING || ss.state == TP_STATE_STALLED, "app-visible state while blocked is a named waiting state ('%s')", ss.state_text);
  tp_cancel_set(cn, 1); reader.join();
  CHECK(result.load() == -1 && took.load() < 4.5, "cancel token releases the blocked read with -1 (%.2f s total)", took.load());
  tp_cancel_free(cn);
  s1.start(); s2.start();
  { auto t0 = std::chrono::steady_clock::now(); int64_t n = rd(far); double l = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    CHECK(n > 0 && mism == 0, "after the peers return the same read succeeds and verifies (%.2f s)", l); }

  // ---- status, coverage, events
  tp_stream_status_get(st, &ss);
  CHECK(ss.cache_limit == (48LL << 20) && ss.cache_max_charged <= ss.cache_limit && ss.cap_violations == 0, "cache accounting visible through the C interface: charged max %.1f of %.0f MiB, violations %lld",
        ss.cache_max_charged / 1048576.0, ss.cache_limit / 1048576.0, (long long)ss.cap_violations);
  CHECK(ss.piece_size == 262144 && ss.num_pieces == 768 && ss.pieces_verified > 0 && ss.hash_failures == 0, "piece/integrity counters present (verified %d, failures %d)", ss.pieces_verified, ss.hash_failures);
  std::vector<uint8_t> cov(64); tp_stream_coverage(st, 64, cov.data()); int covered = 0; for (auto c : cov) covered += c > 0;
  CHECK(covered > 0 && covered < 64, "coverage map is partial for a bounded cache (%d of 64 buckets non-empty)", covered);
  char ev[8192]; int en = tp_stream_events(st, ev, sizeof ev);
  CHECK(en > 0 && std::strstr(ev, "recovery:") != nullptr, "event log available (%d bytes)", en);
  (void)states_seen; (void)saw_ready;

  tp_stream_close(st);

  // ---- magnet path: metadata fetched from the loopback seeders (x.pe), polled like the app does
  { lt::torrent_info ti(pl.torrent_bytes.data(), static_cast<int>(pl.torrent_bytes.size()));
    std::ostringstream hex; for (auto b : ti.info_hashes().get_best()) hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(static_cast<unsigned char>(b));
    std::string mag = "magnet:?xt=urn:btih:" + hex.str() + "&dn=payload&x.pe=" + s1.endpoint() + "&x.pe=" + s2.endpoint();
    tp_meta* mm = tp_meta_from_magnet(eng, mag.c_str(), err, sizeof err);
    CHECK(mm != nullptr, "magnet accepted (%s)", err);
    if (mm) {
      tp_meta_status q; auto t0 = std::chrono::steady_clock::now(); bool fetching_seen = false;
      for (;;) { tp_meta_status_get(mm, &q); fetching_seen |= q.state == TP_META_FETCHING; if (q.state != TP_META_FETCHING || std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() > 60) break; std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
      CHECK(q.state == TP_META_READY, "magnet metadata fetched via peers in %.1f s (state %d, error '%s')", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), q.state, q.error);
      CHECK(tp_meta_file_count(mm) == 1, "magnet metadata lists the file");
      char nm[200]; CHECK(tp_meta_name(mm, nm, sizeof nm) == 1 && std::string(nm) == "payload", "torrent name from metadata: '%s'", nm);
      tp_stream* s3 = tp_stream_open(eng, mm, 0, &opt, err, sizeof err);
      CHECK(s3 != nullptr, "stream opened from magnet metadata (%s)", err);
      if (s3) { std::vector<char> b2(65536); int64_t n = tp_stream_read(s3, 12345678, b2.data(), 65536, nullptr); CHECK(n > 0 && pl.matches(12345678, b2.data(), n), "magnet-opened stream serves verified bytes"); tp_stream_close(s3); }
      tp_meta_free(mm);
    }
    // a magnet with no reachable peers must end in an explicit state, not hang silently
    tp_meta* dead = tp_meta_from_magnet(eng, "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567", err, sizeof err);
    CHECK(dead != nullptr, "magnet with no peers accepted");
    if (dead) { std::this_thread::sleep_for(std::chrono::seconds(3)); tp_meta_status q; tp_meta_status_get(dead, &q);
      CHECK(q.state == TP_META_FETCHING && q.peers == 0, "no-peer magnet is still fetching with 0 peers after 3 s (the app shows 'No peers found' from this)");
      tp_meta_cancel(dead); tp_meta_status_get(dead, &q); CHECK(q.state == TP_META_FAILED && std::string(q.error) == "Cancelled", "cancel ends the fetch explicitly ('%s')", q.error); tp_meta_free(dead); }
  }

  tp_meta_free(m);
  tp_engine_destroy(eng);
  ITLOG("SUMMARY: %d failure(s)", failures);
  ITLOG(failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
