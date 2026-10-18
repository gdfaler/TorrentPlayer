// Functional tests (not gates): magnet metadata resolution over loopback (ut_metadata) and session-level free-disk policy (pause/resume with hysteresis).
#include "loopback_fixture.hpp"
using namespace tpit;

static std::string hex20(const lt::sha1_hash& h) { static const char* d = "0123456789abcdef"; std::string r; for (int i = 0; i < 20; ++i) { auto u = static_cast<unsigned char>(h[i]); r += d[u >> 4]; r += d[u & 15]; } return r; }

int main() {
  bool ok = true;
  Payload pl = Payload::create(data_dir(), 128LL << 20, 262144, 66);
  Seeder seeder(pl, 43101);
  lt::torrent_info ti(pl.torrent_bytes.data(), static_cast<int>(pl.torrent_bytes.size()));
  {  // ---- magnet link: metadata is fetched from the swarm, then the file streams and verifies
    auto dir = fresh_dir("functional_magnet");
    tp::Engine eng(engine_config(dir, 43001));
    tp::TorrentSource src; src.magnet_uri = "magnet:?xt=urn:btih:" + hex20(ti.info_hashes().v1) + "&x.pe=" + seeder.endpoint(); src.strip_web_seeds = true; src.strip_trackers = true;
    src.extra_peers = {seeder.endpoint()};
    const auto t0 = Clock::now();
    std::string err; auto ss = eng.open_stream(src, -1, tp::CacheConfig{}, &err);
    const double t_meta = secs(Clock::now() - t0);
    bool magnet_ok = ss != nullptr;
    if (!ss) ITLOG("magnet open failed: %s", err.c_str());
    else {
      ss->set_bitrate(4.0 * 1048576);
      Player p(*ss, pl, 4.0 * 1048576); p.open_like_demuxer(); p.play(3.0, 30.0);
      magnet_ok = p.st.mismatches == 0 && p.st.bytes > 1000000;
      ITLOG("magnet: metadata + stream open took %.2f s; verified %.1f MiB, mismatches %d", t_meta, p.st.bytes / 1048576.0, p.st.mismatches);
    }
    ITLOG("magnet metadata resolution: %s", magnet_ok ? "PASS" : "FAIL"); ok &= magnet_ok;
  }
  {  // ---- disk policy at session level: below the pause threshold downloads stop; they resume only at the resume threshold
    auto dir = fresh_dir("functional_disk");
    tp::Engine eng(engine_config(dir, 43002));
    tp::TorrentSource src; src.torrent_bytes = pl.torrent_bytes; src.strip_web_seeds = true; src.strip_trackers = true; src.extra_peers = {seeder.endpoint()};
    std::string err; auto ss = eng.open_stream(src, 0, tp::CacheConfig{}, &err);
    if (!ss) return 2;
    std::atomic<std::int64_t> free_bytes{50LL << 30};
    ss->set_free_space_probe([&] { return free_bytes.load(); });
    ss->set_bitrate(4.0 * 1048576);
    Player p(*ss, pl, 4.0 * 1048576); p.open_like_demuxer(); p.play(2.0, 30.0);
    auto st0 = ss->status();
    const int present_before = st0.store.present;
    free_bytes = 4LL << 30;                                                  // below the 5 GiB pause threshold
    std::this_thread::sleep_for(std::chrono::seconds(5));
    auto st1 = ss->status();
    ITLOG("free=4 GiB: disk_state=%s, state=%s", tp::to_string(st1.disk_state), tp::to_string(st1.state));
    const int present_paused_a = st1.store.present;
    p.seek(96LL << 20, 3.0);                                                 // new region: nothing can arrive while paused
    std::this_thread::sleep_for(std::chrono::seconds(4));
    const int present_paused_b = ss->status().store.present;
    free_bytes = 7LL << 30;                                                  // between pause and resume thresholds: must STAY paused (hysteresis)
    std::this_thread::sleep_for(std::chrono::seconds(5));
    auto st2 = ss->status();
    ITLOG("free=7 GiB (between 5 and 10): disk_state=%s, state=%s", tp::to_string(st2.disk_state), tp::to_string(st2.state));
    free_bytes = 11LL << 30;                                                 // above the resume threshold: resume
    const double l = p.seek(96LL << 20, 40.0);
    auto st3 = ss->status();
    ITLOG("free=11 GiB: disk_state=%s; seek to the new region completed in %.2f s", tp::to_string(st3.disk_state), l);
    const bool d_ok = st1.disk_state == tp::DiskState::Paused && st1.state == tp::StreamState::DiskPaused && present_paused_b <= present_paused_a + 2 && st2.disk_state == tp::DiskState::Paused && st3.disk_state == tp::DiskState::Ok && l >= 0 && p.st.mismatches == 0;
    ITLOG("disk policy at session level (present before %d, paused %d -> %d): %s", present_before, present_paused_a, present_paused_b, d_ok ? "PASS" : "FAIL"); ok &= d_ok;
  }
  ITLOG(ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
