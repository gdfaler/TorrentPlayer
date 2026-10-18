// Regression test for the unexplained "stuck seek" seen once in the 2 GiB fill run (see docs/MILESTONE1_STATUS.md, "Stuck-seek investigation").
// Evidence points at the machine sleeping (lid closed) mid-run: every peer connection died and came back. This test models that as an OUTAGE:
// all seeders are destroyed (connections die, ports stop answering) and later restarted on the same ports, while a bounded cache is under
// eviction. After the peers return, reads/seeks must complete within a bound; cancelled and re-opened reads must leave no stale state;
// every byte is verified. It models sleep/wake only through its network effect (it cannot suspend the test process itself).
// usage: it_outage [seeders=2] [bound_s=30] [outage_s=20]
#include "loopback_fixture.hpp"
using namespace tpit;

int main(int argc, char** argv) {
  const int nseed = argc > 1 ? std::atoi(argv[1]) : 2;
  const double bound = argc > 2 ? std::atof(argv[2]) : 30.0;
  const double outage = argc > 3 ? std::atof(argv[3]) : 20.0;
  Payload pl = Payload::create(data_dir(), 192LL << 20, 262144, 11);
  std::vector<std::unique_ptr<Seeder>> seeders;
  for (int i = 0; i < nseed; ++i) seeders.push_back(std::make_unique<Seeder>(pl, 42101 + i));
  auto dir = fresh_dir("outage_" + std::to_string(nseed));
  tp::Engine eng(engine_config(dir, 42001));
  tp::TorrentSource src; src.torrent_bytes = pl.torrent_bytes; src.strip_web_seeds = true; src.strip_trackers = true;
  for (auto& s : seeders) src.extra_peers.push_back(s->endpoint());
  tp::CacheConfig cc; cc.limit_bytes = 48LL << 20; cc.behind_seconds = 8; cc.ahead_seconds_auto = 8;     // small cache: eviction is active throughout
  std::string err;
  auto ss = eng.open_stream(src, 0, cc, &err);
  if (!ss) { ITLOG("open_stream failed: %s", err.c_str()); return 2; }
  const double bps = 4.0 * 1048576;
  ss->set_bitrate(bps);
  Player p(*ss, pl, bps);
  p.open_like_demuxer();

  using Clock = std::chrono::steady_clock;
  auto secs_since = [](Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); };
  std::atomic<long long> back_ns{0};
  auto down = [&] { for (auto& s : seeders) s->stop(); back_ns = 0; };
  auto up = [&] { for (auto& s : seeders) s->start(); back_ns = Clock::now().time_since_epoch().count(); };
  auto since_back = [&] { const long long b = back_ns.load(); return b ? std::chrono::duration<double>(Clock::now().time_since_epoch() - std::chrono::nanoseconds(b)).count() : -1.0; };
  auto diag = [&](const char* why) {
    auto st = ss->status();
    ITLOG("  DIAG (%s): state=%s peers=%d blocked=%d head=%d win=[%d,%d) present=%d watchdog kicks=%lld resets=%lld un-have resets=%d charged=%.1f MiB", why, tp::to_string(st.state), st.peers, st.blocked_readers, st.head, st.lo, st.hi_full,
         ss->store().count_present(), (long long)st.connect_kicks, (long long)st.watchdog_resets, st.un_have_resets, st.store.charged / 1048576.0);
    for (auto& e : ss->recent_events()) ITLOG("      event %s", e.c_str());
  };
  bool ok = true; int failures = 0;
  auto check = [&](bool cond, const char* what, double v) { if (!cond) { ok = false; ++failures; ITLOG("  FAIL: %s (%.2f s, bound %.0f s)", what, v, bound); diag(what); } else ITLOG("  ok: %s (%.2f s after peers returned)", what, v); };

  for (int round = 0; round < 3; ++round) {
    ITLOG("=== round %d: outage %.0f s, %d seeders", round, outage, nseed);
    // ---- 1. outage during steady playback
    p.play(3.0, 60.0);
    down();
    std::thread t1([&] { std::this_thread::sleep_for(std::chrono::duration<double>(outage)); up(); });
    bool played = p.play(12.0, 120.0);                                   // blocks until data flows again after the outage
    t1.join();
    check(played && since_back() <= bound + 12.0 + outage * 0, "playback resumes after an outage (steady)", std::max(0.0, since_back() - 12.0));
    // ---- 2. seek into evicted data issued while all peers are down
    down();
    const std::int64_t far = std::max<std::int64_t>(0, p.position() - (100LL << 20)) % pl.size;
    std::thread t2([&] { std::this_thread::sleep_for(std::chrono::duration<double>(outage)); up(); });
    const auto t_seek = Clock::now();
    double l = p.seek(far, outage + bound + 30);
    t2.join();
    const double after = secs_since(t_seek) - outage;
    check(l >= 0 && after <= bound, "seek issued during an outage completes after peers return", after);
    p.play(2.0, 60.0);
    // ---- 3. cancelled / re-opened seeks during the outage (reader cancel -> new read at another offset), then recovery
    down();
    int cancelled = 0;
    for (int k = 0; k < 3; ++k) { const double r = p.seek((static_cast<std::int64_t>(k) * 61 + 7) % 150 * (1LL << 20), 3.0); if (r < 0) ++cancelled; }
    ITLOG("  %d/3 seeks were cancelled by the reader while peers were down (expected: all)", cancelled);
    up();
    const auto t_re = Clock::now();
    l = p.seek(static_cast<std::int64_t>(120LL << 20) + round * (3LL << 20), bound + 30);
    check(l >= 0 && secs_since(t_re) <= bound, "seek re-opened after cancellations completes after peers return", secs_since(t_re));
    p.play(2.0, 60.0);
    // ---- 4. repeated short flaps during a seek storm
    for (int k = 0; k < 4; ++k) {
      down(); std::this_thread::sleep_for(std::chrono::milliseconds(1500)); up();
      const auto t_f = Clock::now();
      l = p.seek((static_cast<std::int64_t>(k) * 47 + round * 13) % 160 * (1LL << 20), bound + 30);
      check(l >= 0 && secs_since(t_f) <= bound, "seek right after a short flap", secs_since(t_f));
    }
    p.play(2.0, 60.0);
  }
  auto st = ss->status();
  ITLOG("SUMMARY: seeders=%d, failures=%d, data mismatches=%d, seek timeouts=%d, cap violations=%lld, hash failures=%d (storage %d), max charged %.1f MiB of limit %.0f MiB",
        nseed, failures, p.st.mismatches, p.st.seek_timeouts, (long long)st.store.cap_violations, st.hash_failures, st.hash_storage, st.store.max_charged / 1048576.0, cc.limit_bytes / 1048576.0);
  if (p.st.mismatches || st.store.cap_violations || st.hash_storage || st.store.max_charged > cc.limit_bytes) ok = false;
  ITLOG(ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
