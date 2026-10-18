// Regression test for the "stuck seek" investigation: models what a laptop sleep does to the process. The WHOLE child process (client engine,
// loopback seeders, reader threads, wall-clock deadlines) is frozen with SIGSTOP for longer than the reader's 90 s deadline and thawed with SIGCONT.
// Expectations: (a) a seek that spans a freeze may exceed its deadline (a test artefact of wall-clock deadlines, NOT a defect);
// (b) every seek that starts after the thaw must complete within the bound, i.e. the engine is not wedged after "wake"; (c) every byte verified.
// usage: it_freeze [cycles=3] [freeze_s=100] [bound_s=30]
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <fstream>
#include <iomanip>
#include <sstream>
#include "loopback_fixture.hpp"
using namespace tpit;
extern char** environ;

static double wall() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }

static int child_main(const std::string& log_path) {
  Payload pl = Payload::create(data_dir(), 192LL << 20, 262144, 11);
  Seeder s1(pl, 42501), s2(pl, 42502);
  auto dir = fresh_dir("freeze_child");
  tp::Engine eng(engine_config(dir, 42401));
  tp::TorrentSource src; src.torrent_bytes = pl.torrent_bytes; src.strip_web_seeds = true; src.strip_trackers = true;
  src.extra_peers = {s1.endpoint(), s2.endpoint()};
  tp::CacheConfig cc; cc.limit_bytes = 48LL << 20; cc.behind_seconds = 8; cc.ahead_seconds_auto = 8;
  std::string err; auto ss = eng.open_stream(src, 0, cc, &err);
  if (!ss) return 3;
  const double bps = 4.0 * 1048576; ss->set_bitrate(bps);
  Player p(*ss, pl, bps); p.open_like_demuxer();
  std::ofstream log(log_path, std::ios::app); log << std::fixed << std::setprecision(3);
  std::mt19937 rng(99);
  for (int n = 0;; ++n) {
    const double t0 = wall();
    const double l = p.seek(static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(pl.size - 1048576)), 90.0);
    const bool pl_ok = l >= 0 && p.play(1.5, 60.0);
    log << n << " " << t0 << " " << wall() << " " << (l >= 0 ? 1 : 0) << " " << (pl_ok ? 1 : 0) << " " << p.st.mismatches << "\n"; log.flush();
  }
}

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "child") return child_main(argv[2]);
  const int cycles = argc > 1 ? std::atoi(argv[1]) : 3;
  const double freeze_s = argc > 2 ? std::atof(argv[2]) : 100.0;
  const double bound = argc > 3 ? std::atof(argv[3]) : 30.0;
  const std::string log_path = (fs::path(TP_BUILD_DIR) / "it_run" / "freeze_log.txt").string();
  fs::create_directories(fs::path(log_path).parent_path()); fs::remove(log_path);
  char exe[4096]; std::uint32_t sz = sizeof exe; _NSGetExecutablePath(exe, &sz);
  pid_t pid; const char* av[] = {exe, "child", log_path.c_str(), nullptr};
  if (posix_spawn(&pid, exe, nullptr, nullptr, const_cast<char* const*>(av), environ) != 0) { ITLOG("spawn failed"); return 2; }
  std::mt19937 rng(7);
  std::vector<std::pair<double, double>> frozen;
  std::this_thread::sleep_for(std::chrono::seconds(30));
  for (int c = 0; c < cycles; ++c) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2000 + rng() % 9000));          // land the freeze at a random point of a seek/play cycle
    const double a = wall(); ::kill(pid, SIGSTOP);
    std::this_thread::sleep_for(std::chrono::duration<double>(freeze_s));
    ::kill(pid, SIGCONT); const double b = wall(); frozen.emplace_back(a, b);
    ITLOG("cycle %d: process frozen %.0f s (wall %.0f..%.0f), thawed", c + 1, b - a, a, b);
    std::this_thread::sleep_for(std::chrono::seconds(40));
  }
  ::kill(pid, SIGKILL); int status = 0; ::waitpid(pid, &status, 0);
  std::ifstream in(log_path); std::string line; int n_total = 0, spanning = 0, spanning_failed = 0, after_ok = 0, after_bad = 0, mism = 0; double worst_after = 0;
  while (std::getline(in, line)) {
    std::istringstream ss(line); int n, ok1, ok2, mm; double t0, t1; ss >> n >> t0 >> t1 >> ok1 >> ok2 >> mm; ++n_total; mism = std::max(mism, mm);
    bool span = false; for (auto& f : frozen) if (t0 < f.second && t1 > f.first) span = true;
    if (span) { ++spanning; if (!(ok1 && ok2)) ++spanning_failed; continue; }
    const double dur = t1 - t0; worst_after = std::max(worst_after, dur);
    if (ok1 && ok2 && dur <= bound + 5) ++after_ok; else { ++after_bad; ITLOG("  NOT-SPANNING seek %d failed or slow: ok=%d/%d dur=%.1f s", n, ok1, ok2, dur); }
  }
  ITLOG("SUMMARY: %d seek records; %d spanned a freeze (%d of them missed their deadline: expected artefact); %d non-spanning seeks ok, %d bad; worst non-spanning %.1f s; data mismatches %d",
        n_total, spanning, spanning_failed, after_ok, after_bad, worst_after, mism);
  const bool ok = after_bad == 0 && mism == 0 && after_ok > 0 && spanning > 0;
  ITLOG(ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
