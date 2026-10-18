// DIAGNOSTIC harness (NOT gate M1-G3, NOT a substitute for it): paired auditor-on/auditor-off transfers for the G3 variance diagnosis.
// docs/proposals/G3_DIAGNOSTIC_PROPOSAL.md (revision 2) defines the protocols; this file implements:
//   --mode A    protocol A  : paired comparison, auditors on/off, MINIMAL instrumentation (wall timers, process CPU time, free space), no monitor thread, no network log
//   --mode C2   protocol C2 : same pairing, auditors on/off, FULL churn instrumentation in BOTH arms (50 ms monitor thread + TP_NET_DIAG) for observational stratification
//   --mode OVH  instrumentation-overhead pairs: auditors OFF in both arms, instrumentation on vs off (bounds the cost of the C2 instrumentation)
// The transfer function is the body of it_auditor_overhead's one() copied unchanged (same payload, seeders, ports, cache size, read size, auditor intervals); only the
// bookkeeping around it is new. The original gate file is untouched. usage:
//   diag_g3_paired mode=A|C2|OVH pairs=N seed=S out=<dir> volume=<mount>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <algorithm>
#include <map>
#include <random>
#include "loopback_fixture.hpp"
#include "tpcore/audit.hpp"
using namespace tpit;

static std::map<std::string, std::string> args(int argc, char** argv) { std::map<std::string, std::string> m; for (int i = 1; i < argc; ++i) { std::string a = argv[i]; auto e = a.find('='); if (e != std::string::npos) m[a.substr(0, e)] = a.substr(e + 1); } return m; }
static double cpu_seconds() { rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6; }
static std::string sleep_stamp() { char b[128]; std::string s; for (const char* n : {"kern.sleeptime", "kern.waketime"}) { timeval tv{}; size_t l = sizeof tv; sysctlbyname(n, &tv, &l, nullptr, 0); std::snprintf(b, sizeof b, "%ld.%06d;", (long)tv.tv_sec, (int)tv.tv_usec); s += b; } return s; }
static bool on_ac() { FILE* f = popen("pmset -g batt | head -1", "r"); char b[256] = {0}; if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; pclose(f); } return std::string(b).find("AC Power") != std::string::npos; }
static std::string jstr(const std::string& s) { std::string o = "\""; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; } return o + "\""; }

struct Result {
  bool valid = true; std::string reason;
  double ttfb = 0, mibps = 0, dt = 0, cpu_s = 0, free_start_gib = 0, free_end_gib = 0;
  // auditor stats
  long long dir_scans = 0, dir_accepted = 0; long long vol_peak = 0;
  // churn instrumentation (C2 / OVH instrumented arm only)
  bool instrumented = false; int samples = 0, zero_peer = 0, blocked = 0; int kicks = 0, wd_resets = 0, unhave = 0; int connects = 0, disconnects = 0;
};

// body of it_auditor_overhead's one(), unchanged except for the bookkeeping marked NEW
static Result transfer(const Payload& pl, const fs::path& volume, bool with_auditors, bool instrument, const fs::path& tag_dir, const std::string& tag, std::string* hang_sample_path) {
  Result R; R.instrumented = instrument;
  R.free_start_gib = tp::volume_free_bytes(volume) / 1073741824.0;                                                        // NEW
  if (R.free_start_gib < 6.0) { R.valid = false; R.reason = "volume_free_below_6GiB"; return R; }                          // NEW (policy)
  const std::string stamp0 = sleep_stamp();                                                                                // NEW
  Seeder s1(pl, 42501), s2(pl, 42502);
  auto root = volume / ("diag_" + tag); fs::remove_all(root); fs::create_directories(root);
  const fs::path netlog = tag_dir / ("net_" + tag + ".log");
  if (instrument) setenv("TP_NET_DIAG", netlog.c_str(), 1); else unsetenv("TP_NET_DIAG");                                  // NEW (instrumentation switch, read at Engine construction)
  tp::Engine eng(engine_config(root, 42003));
  tp::TorrentSource src; src.torrent_bytes = pl.torrent_bytes; src.strip_web_seeds = true; src.strip_trackers = true; src.extra_peers = {s1.endpoint(), s2.endpoint()};
  tp::CacheConfig cc; cc.limit_bytes = 128LL << 20;
  const double cpu0 = cpu_seconds();                                                                                        // NEW
  const auto t0 = Clock::now();
  std::string err; auto ss = eng.open_stream(src, 0, cc, &err);
  std::unique_ptr<tp::VolumeDeltaAuditor> va; std::unique_ptr<tp::DirScanAuditor> da;
  if (with_auditors) { va = std::make_unique<tp::VolumeDeltaAuditor>(volume, std::chrono::microseconds(1000)); va->start(); da = std::make_unique<tp::DirScanAuditor>(ss->store(), std::chrono::microseconds(1500)); da->start(); }
  std::atomic<bool> cancel{false}, done{false}, hung{false};                                                                // NEW: 120 s hang rule
  std::thread wd([&] { while (!done) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); if (secs(Clock::now() - t0) > 120.0) { hung = true; if (hang_sample_path) { char c[512]; std::snprintf(c, sizeof c, "sample %d 3 -file %s > /dev/null 2>&1", (int)getpid(), hang_sample_path->c_str()); (void)system(c); } cancel = true; return; } } });
  std::vector<char> buf(256 * 1024);
  ss->read(pl.size - 65536, buf.data(), 65536, &cancel);
  const double ttfb = secs(Clock::now() - t0);
  tp::StreamStatus st0 = ss->status();                                                                                      // NEW (counter baseline for deltas)
  std::atomic<bool> mon_stop{false}; std::atomic<int> samples{0}, zero{0}, blocked{0};                                      // NEW: instrumentation monitor (C2 / OVH instrumented arm only)
  std::thread mon;
  if (instrument) mon = std::thread([&] { while (!mon_stop) { auto s = ss->status(); ++samples; if (s.peers == 0) ++zero; if (s.blocked_readers > 0) ++blocked; std::this_thread::sleep_for(std::chrono::milliseconds(50)); } });
  const auto t1 = Clock::now(); std::int64_t off = 0; const std::int64_t want = 160LL << 20;
  while (off < want && !cancel) { std::int64_t n = ss->read(off, buf.data(), static_cast<std::int64_t>(buf.size()), &cancel); if (n <= 0) break; off += n; }
  const double dt = secs(Clock::now() - t1);
  if (instrument) { mon_stop = true; mon.join(); }
  tp::StreamStatus st1 = ss->status();                                                                                      // NEW
  if (va) { auto r = va->stop(); R.vol_peak = static_cast<long long>(r.peak_usage()); } if (da) { auto r = da->stop(); R.dir_scans = static_cast<long long>(r.scans); R.dir_accepted = static_cast<long long>(r.accepted); }
  done = true; wd.join();                                                                                                   // NEW
  R.ttfb = ttfb; R.dt = dt; R.mibps = (static_cast<double>(off) / 1048576.0) / dt;
  R.cpu_s = cpu_seconds() - cpu0; R.free_end_gib = tp::volume_free_bytes(volume) / 1073741824.0;
  R.samples = samples; R.zero_peer = zero; R.blocked = blocked; R.kicks = st1.connect_kicks - st0.connect_kicks; R.wd_resets = st1.watchdog_resets - st0.watchdog_resets; R.unhave = st1.un_have_resets - st0.un_have_resets;
  if (hung) { R.valid = false; R.reason = "hang_over_120s"; }
  else if (!(R.mibps > 0) || !std::isfinite(R.mibps)) { R.valid = false; R.reason = "nonpositive_or_nonfinite_throughput"; }
  if (sleep_stamp() != stamp0) { R.valid = false; R.reason = R.reason.empty() ? "sleep_or_wake_during_transfer" : R.reason + "+sleep"; }
  ss.reset();                                                                                                               // engine torn down before the netlog is read
  if (instrument && fs::exists(netlog)) { std::ifstream f(netlog); std::string l; while (std::getline(f, l)) { if (l.find("] peer_connect:") != std::string::npos) ++R.connects; else if (l.find("] peer_disconnected:") != std::string::npos) ++R.disconnects; } }
  fs::remove_all(root);                                                                                                     // NEW: harness-owned scratch only (diag_<tag>), keeps the volume free
  return R;
}

static void emit(std::ofstream& out, int pair, int slot, const std::string& cond, const Result& r, const std::string& mode, const std::string& tag) {
  char b[900];
  std::snprintf(b, sizeof b, "{\"mode\":\"%s\",\"pair\":%d,\"slot\":%d,\"cond\":\"%s\",\"tag\":\"%s\",\"valid\":%s,\"reason\":%s,\"ttfb_s\":%.4f,\"mibps\":%.3f,\"read_s\":%.3f,\"cpu_s\":%.3f,\"free_start_gib\":%.3f,\"free_end_gib\":%.3f,"
                "\"dir_scans\":%lld,\"dir_accepted\":%lld,\"vol_peak\":%lld,\"instrumented\":%s,\"samples\":%d,\"zero_peer_samples\":%d,\"blocked_samples\":%d,\"kicks\":%d,\"wd_resets\":%d,\"unhave_resets\":%d,\"connects\":%d,\"disconnects\":%d,\"wall\":%ld}\n",
                mode.c_str(), pair, slot, cond.c_str(), tag.c_str(), r.valid ? "true" : "false", jstr(r.reason).c_str(), r.ttfb, r.mibps, r.dt, r.cpu_s, r.free_start_gib, r.free_end_gib, r.dir_scans, r.dir_accepted, r.vol_peak,
                r.instrumented ? "true" : "false", r.samples, r.zero_peer, r.blocked, r.kicks, r.wd_resets, r.unhave, r.connects, r.disconnects, (long)time(nullptr));
  out << b; out.flush();
}

int main(int argc, char** argv) {
  auto a = args(argc, argv);
  const std::string mode = a.count("mode") ? a["mode"] : "A"; const int pairs = a.count("pairs") ? std::atoi(a["pairs"].c_str()) : 120;
  const std::uint64_t seed = a.count("seed") ? std::strtoull(a["seed"].c_str(), nullptr, 10) : 1; const fs::path out_dir = a.count("out") ? a["out"] : "";
  const fs::path volume = a.count("volume") ? a["volume"] : ""; if (out_dir.empty() || volume.empty() || (mode != "A" && mode != "C2" && mode != "OVH")) { std::puts("usage: diag_g3_paired mode=A|C2|OVH pairs=N seed=S out=<dir> volume=<mount>"); return 2; }
  fs::create_directories(out_dir);
  // balanced pre-registered order: exactly pairs/2 of (off,on) and pairs/2 of (on,off); the sequence is derived only from the seed
  std::vector<int> order; for (int i = 0; i < pairs; ++i) order.push_back(i % 2);
  std::mt19937_64 rng(seed); std::shuffle(order.begin(), order.end(), rng);
  { std::ofstream p(out_dir / "order_sequence.txt"); p << "mode " << mode << " seed " << seed << " pairs " << pairs << "\n"; for (int v : order) p << v; p << "\n(0 = the OFF condition runs first in the pair, 1 = the ON condition runs first)\n"; }
  Payload pl = Payload::create(data_dir(), 256LL << 20, 262144, 55);                                                       // same payload as the gate (seed 55)
  std::ofstream out(out_dir / "transfers.jsonl", std::ios::app);
  ITLOG("diag_g3_paired mode=%s pairs=%d seed=%llu out=%s", mode.c_str(), pairs, (unsigned long long)seed, out_dir.c_str());
  int invalid_pairs = 0, done_pairs = 0; bool halted = false; std::string halt_reason;
  for (int k = 0; k < pairs && !halted; ++k) {
    if (!on_ac()) { halted = true; halt_reason = "power_source_not_AC_before_pair"; break; }
    Result res[2]; std::string conds[2];
    for (int slot = 0; slot < 2; ++slot) {
      const bool on_first = order[k] == 1;
      const bool is_on = (slot == 0) == on_first;                                                                         // slot 0 runs the ON condition iff on_first
      conds[slot] = is_on ? "on" : "off";
      const bool aud = (mode == "OVH") ? false : is_on;
      const bool instr = (mode == "A") ? false : (mode == "C2" ? true : is_on);
      char tag[64]; std::snprintf(tag, sizeof tag, "%s_%03d_%s", mode.c_str(), k, conds[slot].c_str());
      std::string hang_path = (out_dir / (std::string("hang_sample_") + tag + ".txt")).string();
      res[slot] = transfer(pl, volume, aud, instr, out_dir, tag, &hang_path);
      emit(out, k, slot, conds[slot], res[slot], mode, tag);
      ITLOG("pair %3d slot %d %-3s: %6.1f MiB/s ttfb %.2f s cpu %.1f s%s%s", k + 1, slot, conds[slot].c_str(), res[slot].mibps, res[slot].ttfb, res[slot].cpu_s, res[slot].valid ? "" : " INVALID: ", res[slot].reason.c_str());
      if (res[slot].reason == "volume_free_below_6GiB") { halted = true; halt_reason = "volume_free_below_6GiB (cannot recover; run is incomplete)"; break; }
      if (slot == 0) std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    ++done_pairs; if (!(res[0].valid && res[1].valid)) ++invalid_pairs;
    if (done_pairs >= 20 && invalid_pairs * 5 > done_pairs) { halted = true; halt_reason = "more_than_20_percent_invalid_pairs"; }
    if (!halted) std::this_thread::sleep_for(std::chrono::seconds(5));
  }
  ITLOG("FINISHED: %d of %d pairs run, %d invalid%s%s", done_pairs, pairs, invalid_pairs, halted ? "; HALTED: " : "", halt_reason.c_str());
  std::ofstream(out_dir / "finished.txt") << "pairs_run " << done_pairs << " of " << pairs << " invalid " << invalid_pairs << (halted ? " HALTED " + halt_reason : std::string(" complete")) << "\n";
  return 0;
}
