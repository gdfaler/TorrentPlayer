// DIAGNOSTIC (not a gate): why is per-round throughput in it_auditor_overhead bimodal? Runs the same transfer WITHOUT auditors and records, per round,
// throughput plus the fraction of 50 ms samples with zero connected peers, watchdog kicks and un-have resets. usage: it_g3_variance_probe volume=<mount> [rounds=14]
#include <map>
#include "loopback_fixture.hpp"
#include "tpcore/audit.hpp"
using namespace tpit;
int main(int argc, char** argv) {
  std::string vol = "/Volumes/TPTestVol"; int rounds = 14;
  for (int i = 1; i < argc; ++i) { std::string a = argv[i]; if (a.rfind("volume=", 0) == 0) vol = a.substr(7); if (a.rfind("rounds=", 0) == 0) rounds = std::atoi(a.c_str() + 7); }
  Payload pl = Payload::create(data_dir(), 256LL << 20, 262144, 55);
  for (int r = 0; r < rounds; ++r) {
    Seeder s1(pl, 42501), s2(pl, 42502);
    auto root = fs::path(vol) / ("probe_" + std::to_string(r)); fs::remove_all(root); fs::create_directories(root);
    tp::Engine eng(engine_config(root, 42003));
    tp::TorrentSource src; src.torrent_bytes = pl.torrent_bytes; src.strip_web_seeds = true; src.strip_trackers = true; src.extra_peers = {s1.endpoint(), s2.endpoint()};
    if (std::getenv("NO_REDUNDANT_CLOSE")) { lt::settings_pack sp; sp.set_bool(lt::settings_pack::close_redundant_connections, false); eng.session().apply_settings(sp); }   // EXPERIMENT only (never applied by the product)
    tp::CacheConfig cc; cc.limit_bytes = 128LL << 20;
    std::string err; auto ss = eng.open_stream(src, 0, cc, &err);
    ITLOG("round %d start: volume free %.2f GiB, disk policy state %s%s", r + 1, tp::volume_free_bytes(root) / 1073741824.0, tp::to_string(ss->status().disk_state), std::getenv("PROBE_RELAX_DISK") ? " (EXPERIMENT: thresholds lowered to the 1 GiB floor)" : "");
    if (std::getenv("PROBE_RELAX_DISK")) { tp::DiskPolicyConfig dc; dc.pause_at = dc.hard_floor; dc.warn_at = dc.hard_floor * 2; dc.resume_at = dc.hard_floor * 2; ss->set_disk_policy(dc); }
    std::atomic<bool> reported{false};
    std::thread diag([&] { for (int i = 0; i < 400 && !reported; ++i) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); auto st = ss->status(); if (st.blocked_readers > 0 && i > 300) { ITLOG("   READER BLOCKED >30 s: state=%s disk_state=%s peers=%d", tp::to_string(st.state), tp::to_string(st.disk_state), st.peers); for (auto& e : ss->recent_events()) ITLOG("      event %s", e.c_str()); reported = true; } } });
    std::vector<char> buf(256 * 1024); ss->read(pl.size - 65536, buf.data(), 65536, nullptr);
    std::atomic<bool> stop{false}; std::atomic<int> samples{0}, nopeer{0}, blocked{0};
    std::thread mon([&] { while (!stop) { auto st = ss->status(); ++samples; if (st.peers == 0) ++nopeer; if (st.blocked_readers > 0) ++blocked; std::this_thread::sleep_for(std::chrono::milliseconds(50)); } });
    const auto t1 = Clock::now(); std::int64_t off = 0; const std::int64_t want = 160LL << 20;
    while (off < want) { std::int64_t n = ss->read(off, buf.data(), static_cast<std::int64_t>(buf.size()), nullptr); if (n <= 0) break; off += n; }
    const double dt = secs(Clock::now() - t1); stop = true; mon.join();
    reported = true; diag.join();
    auto st = ss->status();
    ITLOG("round %2d: %5.1f MiB/s | samples %d, zero-peer %3.0f%%, reader-blocked %3.0f%% | watchdog kicks %d, watchdog resets %d, un-have resets %d", r + 1, (off / 1048576.0) / dt, samples.load(),
          100.0 * nopeer / std::max(1, samples.load()), 100.0 * blocked / std::max(1, samples.load()), st.connect_kicks, st.watchdog_resets, st.un_have_resets);
  }
}
