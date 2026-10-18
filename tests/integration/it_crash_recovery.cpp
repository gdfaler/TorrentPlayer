// Gate M1-G6: >= 20 kill -9 cycles while the engine is downloading, seeking and evicting. After every kill the parent re-opens the cache
// (paused) and checks: recovery trims to the cap BEFORE any torrent activity, allocated bytes <= limit, every piece the store claims is
// present is byte-identical to the payload, nothing but piece files remain. A final run plays through with every byte verified.
// usage: it_crash_recovery [cycles=20] [limit_mib=48]
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <map>
#include "loopback_fixture.hpp"
using namespace tpit;
extern char** environ;

static std::map<std::string, std::string> args(int argc, char** argv) { std::map<std::string, std::string> m; for (int i = 1; i < argc; ++i) { std::string a = argv[i]; auto e = a.find('='); if (e != std::string::npos) m[a.substr(0, e)] = a.substr(e + 1); } return m; }

static int child_main(const Payload& pl, const fs::path& cache_root, std::int64_t limit, int port, int s0) {
  tp::Engine eng(engine_config(cache_root, port));
  tp::TorrentSource src; src.torrent_bytes = pl.torrent_bytes; src.strip_web_seeds = true; src.strip_trackers = true;
  src.extra_peers = {"127.0.0.1:" + std::to_string(s0), "127.0.0.1:" + std::to_string(s0 + 1)};
  tp::CacheConfig cc; cc.limit_bytes = limit;
  std::string err; auto ss = eng.open_stream(src, 0, cc, &err);
  if (!ss) return 3;
  ss->set_bitrate(8.0 * 1048576);
  Player p(*ss, pl, 8.0 * 1048576);
  p.open_like_demuxer();
  std::mt19937 rng(static_cast<unsigned>(::getpid()));
  for (;;) { p.seek(static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(pl.size - 1048576)), 30.0); p.play(0.7, 20.0); }   // runs until killed
}

int main(int argc, char** argv) {
  auto a = args(argc, argv);
  const int cycles = a.count("cycles") ? std::atoi(a["cycles"].c_str()) : 20;
  const std::int64_t limit = (a.count("limit_mib") ? std::atoll(a["limit_mib"].c_str()) : 48) << 20;
  Payload pl = Payload::create(data_dir(), 384LL << 20, 262144, 44);
  const fs::path cache_root = fs::path(TP_BUILD_DIR) / "it_run" / "crash_cache";
  if (a.count("child")) return child_main(pl, cache_root, limit, 42301, 42401);
  fs::remove_all(cache_root); fs::create_directories(cache_root);
  Seeder s1(pl, 42401), s2(pl, 42402);
  char exe[4096]; std::uint32_t sz = sizeof exe; _NSGetExecutablePath(exe, &sz);
  std::mt19937 rng(123);
  bool ok = true; int total_verified_after = 0, total_trimmed = 0, total_corrupt_deleted = 0, total_partial_deleted = 0, bad_pieces = 0;
  std::string cache_dir;
  for (int c = 0; c < cycles; ++c) {
    pid_t pid; std::string lim = "limit_mib=" + std::to_string(limit >> 20); const char* av[] = {exe, "child=1", lim.c_str(), nullptr};
    if (posix_spawn(&pid, exe, nullptr, nullptr, const_cast<char* const*>(av), environ) != 0) { ITLOG("spawn failed"); return 2; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1500 + rng() % 3500));
    ::kill(pid, SIGKILL); int status = 0; ::waitpid(pid, &status, 0);
    // ---- parent: audit the crashed cache, then run recovery with the SAME limit and check everything BEFORE any torrent activity
    tp::Engine eng(engine_config(cache_root, 42302));
    tp::TorrentSource src; src.torrent_bytes = pl.torrent_bytes; src.strip_web_seeds = true; src.strip_trackers = true; src.start_paused = true;
    src.extra_peers = {s1.endpoint(), s2.endpoint()};
    tp::CacheConfig cc; cc.limit_bytes = limit;
    std::string err; auto ss = eng.open_stream(src, 0, cc, &err);
    if (!ss) { ITLOG("cycle %d: open failed: %s", c, err.c_str()); return 2; }
    cache_dir = ss->store().params().dir.string();
    auto rec = ss->recovery_report();
    const std::int64_t alloc = tp::measure_allocated_bytes(ss->store().params().dir);
    int foreign = 0, files = 0; for (auto& e : fs::directory_iterator(ss->store().params().dir)) { ++files; auto n = e.path().filename().string(); if (n.rfind("piece_", 0) != 0) ++foreign; }
    // every piece the store says is present must be byte-identical to the payload
    int bad = 0; std::vector<char> b(262144), want(262144);
    for (int p : ss->store().present_pieces()) { const std::int64_t n = ss->store().piece_size_of(p); if (ss->store().read(p, 0, b.data(), static_cast<int>(n)) < 0) { ++bad; continue; } pl.fill(static_cast<std::int64_t>(p) * 262144, n, want.data()); if (std::memcmp(b.data(), want.data(), static_cast<std::size_t>(n)) != 0) ++bad; }
    bad_pieces += bad; total_trimmed += rec.trimmed; total_corrupt_deleted += rec.deleted_corrupt; total_partial_deleted += rec.deleted_partial; total_verified_after += rec.verified_present;
    const bool cyc_ok = alloc <= limit && ss->store().charged() <= ss->store().internal_cap() && bad == 0 && foreign == 0;
    ITLOG("cycle %2d: killed; recovery verified=%d deleted_partial=%d deleted_corrupt=%d trimmed=%d | allocated %.2f MiB (limit %.0f) | present pieces byte-identical to payload: %s (%d bad) | foreign files %d => %s",
          c + 1, rec.verified_present, rec.deleted_partial, rec.deleted_corrupt, rec.trimmed, alloc / 1048576.0, limit / 1048576.0, bad == 0 ? "yes" : "NO", bad, foreign, cyc_ok ? "ok" : "FAIL");
    ok &= cyc_ok;
  }
  // ---- final run: after 20 crashes the cache must still serve correct bytes end to end
  { tp::Engine eng(engine_config(cache_root, 42303));
    tp::TorrentSource src; src.torrent_bytes = pl.torrent_bytes; src.strip_web_seeds = true; src.strip_trackers = true; src.extra_peers = {s1.endpoint(), s2.endpoint()};
    tp::CacheConfig cc; cc.limit_bytes = limit; std::string err; auto ss = eng.open_stream(src, 0, cc, &err);
    ss->set_bitrate(8.0 * 1048576); Player p(*ss, pl, 8.0 * 1048576); p.open_like_demuxer();
    const bool played = p.play(15.0, 60.0);
    auto st = ss->status();
    ITLOG("final run after %d crashes: played ok=%d, verified reads %.0f MiB, mismatches %d, hash failures %d (storage %d), violations %lld", cycles, played, p.st.bytes / 1048576.0, p.st.mismatches, st.hash_failures, st.hash_storage, (long long)st.store.cap_violations);
    ok &= played && p.st.mismatches == 0 && st.hash_storage == 0 && st.store.cap_violations == 0; }
  ITLOG("RESULT crash cycles=%d: total pieces verified after recovery %d, partial files deleted %d, corrupt files deleted %d, trimmed %d, bad (non-identical) present pieces %d", cycles, total_verified_after, total_partial_deleted, total_corrupt_deleted, total_trimmed, bad_pieces);
  ITLOG(ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
