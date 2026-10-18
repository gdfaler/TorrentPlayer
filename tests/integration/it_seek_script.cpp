// Gate M1-G5: >= 100 scripted seeks (forward, backward, in-window, evicted, tail/head) through the real engine against loopback seeders,
// every byte verified against the payload; 0 stuck, 0 missed, 0 mismatches. Also exercises the single-peer reconnect/un-have path.
#include "loopback_fixture.hpp"
using namespace tpit;

int main(int argc, char** argv) {
  const int nseekers = argc > 1 ? std::atoi(argv[1]) : 2;               // 1 = single-seeder (hardest reconnect case)
  const int nseeks = argc > 2 ? std::atoi(argv[2]) : 120;
  Payload pl = Payload::create(data_dir(), 192LL << 20, 262144, 11);
  std::vector<std::unique_ptr<Seeder>> seeders;
  for (int i = 0; i < nseekers; ++i) seeders.push_back(std::make_unique<Seeder>(pl, 42101 + i));
  auto dir = fresh_dir("seek_script_" + std::to_string(nseekers));
  tp::Engine eng(engine_config(dir, 42001));
  tp::TorrentSource src; src.torrent_bytes = pl.torrent_bytes; src.strip_web_seeds = true; src.strip_trackers = true;
  for (auto& s : seeders) src.extra_peers.push_back(s->endpoint());
  tp::CacheConfig cc; cc.limit_bytes = 48LL << 20; cc.behind_seconds = 8; cc.ahead_seconds_auto = 8;     // small cache so most seeks hit evicted data
  std::string err;
  auto ss = eng.open_stream(src, 0, cc, &err);
  if (!ss) { ITLOG("open_stream failed: %s", err.c_str()); return 2; }
  const double bps = 4.0 * 1048576;                                       // 32 Mbit/s "video"
  ss->set_bitrate(bps);
  Player pl_(*ss, pl, bps);
  pl_.open_like_demuxer();
  ITLOG("started: %d seeders, cache %.0f MiB, window ahead %d behind %d pieces", nseekers, cc.limit_bytes / 1048576.0, 0, 0);
  std::atomic<bool> mon_stop{false};
  std::thread mon([&] { while (!mon_stop) { for (int i = 0; i < 50 && !mon_stop; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      auto st = ss->status(); ITLOG("  [monitor] state=%s peers=%d down=%.0f kB/s head=%d win=[%d,%d) wanted_hi=%d blocked=%d present=%d charged=%.1f/%.1f MiB resets=%d stale=%d file_err=%d", tp::to_string(st.state), st.peers, st.download_rate / 1000, st.head, st.lo, st.hi_full, st.hi_wanted, st.blocked_readers, st.store.present, st.store.charged / 1048576.0, st.store.internal_cap / 1048576.0, st.un_have_resets, st.stale_have_detections, st.file_errors);
      if (st.blocked_readers > 0 && getenv("TP_VERBOSE")) for (auto& e : ss->recent_events()) ITLOG("      event %s", e.c_str()); } });
  std::mt19937 rng(5);
  int stuck = 0;
  for (int i = 0; i < nseeks; ++i) {
    std::int64_t target; const char* kind;
    switch (i % 6) {
      case 0: target = static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(pl.size - 1048576)); kind = "random"; break;
      case 1: target = std::max<std::int64_t>(0, pl_.position() - (96LL << 20)); kind = "far-backward (evicted)"; break;
      case 2: target = std::min<std::int64_t>(pl.size - 1048576, pl_.position() + (64LL << 20)); kind = "far-forward"; break;
      case 3: target = std::max<std::int64_t>(0, pl_.position() - (2LL << 20)); kind = "near-backward (in window)"; break;
      case 4: target = pl.size - 1048576 - static_cast<std::int64_t>(rng() % (1 << 20)); kind = "near-tail"; break;
      default: target = static_cast<std::int64_t>(rng() % (4 << 20)); kind = "near-head"; break;
    }
    const double lat = pl_.seek(target, 60.0);
    if (lat < 0) { ++stuck; ITLOG("seek %d (%s) to %lld: STUCK/TIMEOUT", i, kind, (long long)target); }
    if (!pl_.play(1.0, 30.0)) { ++stuck; ITLOG("seek %d: playback after seek stuck", i); }
    if (i % 20 == 19) { auto st = ss->status(); ITLOG("seek %d: latency so far median %.3fs p90 %.3fs | resets %d stale-have %d | present %d charged %.1f/%.1f MiB | state=%s", i + 1, percentile(pl_.st.seek_latency, .5), percentile(pl_.st.seek_latency, .9), st.un_have_resets, st.stale_have_detections, st.store.present, st.store.charged / 1048576.0, st.store.internal_cap / 1048576.0, tp::to_string(st.state)); }
  }
  mon_stop = true; mon.join();
  auto st = ss->status();
  ITLOG("RESULT seeks=%d stuck_or_missed=%d mismatches=%d | latency median %.3fs p90 %.3fs max %.3fs | un-have resets=%d stale-have detections=%d | cap violations=%lld max_charged=%.2f MiB internal_cap=%.2f MiB limit=%.0f MiB | hash failures=%d (storage %d)",
        pl_.st.seeks, stuck, pl_.st.mismatches, percentile(pl_.st.seek_latency, .5), percentile(pl_.st.seek_latency, .9), percentile(pl_.st.seek_latency, 1.0), st.un_have_resets, st.stale_have_detections,
        (long long)st.store.cap_violations, st.store.max_charged / 1048576.0, st.store.internal_cap / 1048576.0, cc.limit_bytes / 1048576.0, st.hash_failures, st.hash_storage);
  const bool ok = stuck == 0 && pl_.st.mismatches == 0 && st.store.cap_violations == 0 && st.store.max_charged <= st.store.internal_cap && st.hash_storage == 0 && pl_.st.seeks >= nseeks;
  ITLOG(ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
