// DIAGNOSTIC protocol C1 (NOT gate M1-G3; NOT an end-to-end G3 equivalent; cannot resolve G3): paired auditor-on/auditor-off measurement of the PRODUCTION
// SegmentStore path under a synthetic piece stream, with no peers, no libtorrent and no network. docs/proposals/G3_DIAGNOSTIC_PROPOSAL.md (C1 pre-registration).
//   workload (per transfer): store with the gate's geometry (256 MiB torrent of 256 KiB pieces, 128 MiB user limit, default 10% margin => internal cap as in the
//   production StreamSession); the main thread "downloads" the first 160 MiB sequentially in 16 KiB block writes (the production write path incl. pre-write reservation
//   and eviction), marks each piece verified and reads it back once (the reader); a ticker thread does what the scheduler does every 100 ms (set_protection, sweep,
//   reconcile_step). Auditors (when ON) are the gate's: VolumeDeltaAuditor at 1 ms and DirScanAuditor at 1.5 ms. usage:
//   diag_c1_store pairs=N seed=S out=<dir> volume=<mount>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <algorithm>
#include <map>
#include <random>
#include "loopback_fixture.hpp"
#include "tpcore/audit.hpp"
#include "tpcore/config.hpp"
#include "tpcore/segment_store.hpp"
using namespace tpit;

static std::map<std::string, std::string> args(int argc, char** argv) { std::map<std::string, std::string> m; for (int i = 1; i < argc; ++i) { std::string a = argv[i]; auto e = a.find('='); if (e != std::string::npos) m[a.substr(0, e)] = a.substr(e + 1); } return m; }
static double cpu_seconds() { rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6; }
static std::string sleep_stamp() { char b[128]; std::string s; for (const char* n : {"kern.sleeptime", "kern.waketime"}) { timeval tv{}; size_t l = sizeof tv; sysctlbyname(n, &tv, &l, nullptr, 0); std::snprintf(b, sizeof b, "%ld.%06d;", (long)tv.tv_sec, (int)tv.tv_usec); s += b; } return s; }
static bool on_ac() { FILE* f = popen("pmset -g batt | head -1", "r"); char b[256] = {0}; if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; pclose(f); } return std::string(b).find("AC Power") != std::string::npos; }
static std::string jstr(const std::string& s) { std::string o = "\""; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; } return o + "\""; }
static double quantile(std::vector<double>& v, double q) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))]; }

struct Result {
  bool valid = true; std::string reason;
  double mibps = 0, dt = 0, cpu_s = 0, ttfb = 0, free_start_gib = 0, free_end_gib = 0;
  double w_p50 = 0, w_p99 = 0, w_max = 0, r_p50 = 0, r_p99 = 0;        // microseconds
  long long dir_scans = 0, dir_accepted = 0, vol_peak = 0; long long cap_violations = 0, refused = 0, forced_ev = 0, sweep_ev = 0, writes = 0; double max_charged_mib = 0, limit_mib = 0;
};

static Result transfer(const fs::path& volume, bool with_auditors, const std::string& tag, std::string* hang_sample_path) {
  Result R;
  R.free_start_gib = tp::volume_free_bytes(volume) / 1073741824.0;
  if (R.free_start_gib < 6.0) { R.valid = false; R.reason = "volume_free_below_6GiB"; return R; }
  const std::string stamp0 = sleep_stamp();
  const std::int64_t PS = 262144, TOTAL = 256LL << 20, LIMIT = 128LL << 20; const int NP = static_cast<int>(TOTAL / PS), WANT = static_cast<int>((160LL << 20) / PS);
  tp::CacheConfig cc; cc.limit_bytes = LIMIT;
  auto root = volume / ("diagc1_" + tag); fs::remove_all(root); fs::create_directories(root);
  tp::StoreParams sp; sp.dir = root; sp.piece_size = PS; sp.num_pieces = NP; sp.total_size = TOTAL; sp.limit_bytes = LIMIT; sp.margin_bytes = LIMIT - tp::internal_cap(cc, PS);
  tp::SegmentStore store(sp);
  std::vector<char> blk(16384), rbuf(static_cast<size_t>(PS));
  for (size_t i = 0; i < blk.size(); ++i) blk[i] = static_cast<char>(i * 31 + 7);
  const int ahead = 24, behind = 40;                                                // window in pieces (about 6 MiB ahead, 10 MiB behind): evicts from piece ~ (cap/PS) on
  std::atomic<int> head{0}; std::atomic<bool> stop_tick{false}, cancel{false}, done{false};
  const double cpu0 = cpu_seconds(); const auto t0 = Clock::now();
  std::unique_ptr<tp::VolumeDeltaAuditor> va; std::unique_ptr<tp::DirScanAuditor> da;
  if (with_auditors) { va = std::make_unique<tp::VolumeDeltaAuditor>(volume, std::chrono::microseconds(1000)); va->start(); da = std::make_unique<tp::DirScanAuditor>(store, std::chrono::microseconds(1500)); da->start(); }
  std::thread ticker([&] {                                                           // what StreamSession::tick does for the store, every 100 ms
    while (!stop_tick) { const int h = head.load(); tp::Protection pr; pr.head = h; pr.lo = std::max(0, h - behind); pr.hi = std::min(NP, h + ahead); pr.pin_a_first = 0; pr.pin_a_last = 7; pr.pin_b_first = NP - 8; pr.pin_b_last = NP - 1;
      store.set_protection(pr); store.sweep_outside_window(std::chrono::nanoseconds(0), 2); store.reconcile_step(256); std::this_thread::sleep_for(std::chrono::milliseconds(100)); } });
  std::thread wd([&] { while (!done) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); if (secs(Clock::now() - t0) > 120.0) { if (hang_sample_path) { char c[512]; std::snprintf(c, sizeof c, "sample %d 3 -file %s > /dev/null 2>&1", (int)getpid(), hang_sample_path->c_str()); (void)system(c); } cancel = true; return; } } });
  std::vector<double> wl, rl; wl.reserve(static_cast<size_t>(WANT) * 16); rl.reserve(static_cast<size_t>(WANT));
  const auto t1 = Clock::now(); int p = 0; bool aborted = false; double first = -1;
  for (; p < WANT && !cancel; ++p) {
    head = p;
    for (int off = 0; off < PS; off += static_cast<int>(blk.size())) {
      const auto a = Clock::now(); auto ws = store.write(p, off, blk.data(), static_cast<int>(blk.size())); wl.push_back(std::chrono::duration<double, std::micro>(Clock::now() - a).count());
      if (ws != tp::WriteStatus::Ok) { R.valid = false; R.reason = "write_refused_or_io_error"; aborted = true; break; }
    }
    if (aborted) break;
    store.mark_verified(p); if (first < 0) first = secs(Clock::now() - t1);
    const auto b = Clock::now(); store.touch(p); store.read(p, 0, rbuf.data(), static_cast<int>(PS)); rl.push_back(std::chrono::duration<double, std::micro>(Clock::now() - b).count());
  }
  const double dt = secs(Clock::now() - t1);
  stop_tick = true; ticker.join();
  if (va) { auto r = va->stop(); R.vol_peak = static_cast<long long>(r.peak_usage()); } if (da) { auto r = da->stop(); R.dir_scans = static_cast<long long>(r.scans); R.dir_accepted = static_cast<long long>(r.accepted); }
  done = true; wd.join();
  auto m = store.metrics();
  R.dt = dt; R.mibps = (static_cast<double>(p) * static_cast<double>(PS) / 1048576.0) / dt; R.ttfb = first < 0 ? 0 : first; R.cpu_s = cpu_seconds() - cpu0; R.free_end_gib = tp::volume_free_bytes(volume) / 1073741824.0;
  R.w_p50 = quantile(wl, 0.50); R.w_p99 = quantile(wl, 0.99); R.w_max = wl.empty() ? 0 : wl.back(); R.r_p50 = quantile(rl, 0.50); R.r_p99 = quantile(rl, 0.99);
  R.cap_violations = m.cap_violations; R.refused = m.refused_writes; R.forced_ev = m.forced_evictions; R.sweep_ev = m.sweep_evictions; R.writes = m.writes; R.max_charged_mib = m.max_charged / 1048576.0; R.limit_mib = m.limit / 1048576.0;
  if (cancel) { R.valid = false; R.reason = "hang_over_120s"; }
  else if (R.valid && (!(R.mibps > 0) || !std::isfinite(R.mibps))) { R.valid = false; R.reason = "nonpositive_or_nonfinite_throughput"; }
  if (sleep_stamp() != stamp0) { R.valid = false; R.reason = R.reason.empty() ? "sleep_or_wake_during_transfer" : R.reason + "+sleep"; }
  fs::remove_all(root);
  return R;
}

int main(int argc, char** argv) {
  auto a = args(argc, argv);
  const int pairs = a.count("pairs") ? std::atoi(a["pairs"].c_str()) : 60; const std::uint64_t seed = a.count("seed") ? std::strtoull(a["seed"].c_str(), nullptr, 10) : 1;
  const fs::path out_dir = a.count("out") ? a["out"] : ""; const fs::path volume = a.count("volume") ? a["volume"] : "";
  if (out_dir.empty() || volume.empty()) { std::puts("usage: diag_c1_store pairs=N seed=S out=<dir> volume=<mount>"); return 2; }
  fs::create_directories(out_dir);
  std::vector<int> order; for (int i = 0; i < pairs; ++i) order.push_back(i % 2);
  std::mt19937_64 rng(seed); std::shuffle(order.begin(), order.end(), rng);
  { std::ofstream p(out_dir / "order_sequence.txt"); p << "mode C1 seed " << seed << " pairs " << pairs << "\n"; for (int v : order) p << v; p << "\n(0 = OFF first, 1 = ON first)\n"; }
  std::ofstream out(out_dir / "transfers.jsonl", std::ios::app);
  ITLOG("diag_c1_store pairs=%d seed=%llu out=%s", pairs, (unsigned long long)seed, out_dir.c_str());
  int invalid_pairs = 0, done_pairs = 0; bool halted = false; std::string halt_reason;
  for (int k = 0; k < pairs && !halted; ++k) {
    if (!on_ac()) { halted = true; halt_reason = "power_source_not_AC_before_pair"; break; }
    Result res[2];
    for (int slot = 0; slot < 2; ++slot) {
      const bool is_on = (slot == 0) == (order[k] == 1); const std::string cond = is_on ? "on" : "off";
      char tag[64]; std::snprintf(tag, sizeof tag, "C1_%03d_%s", k, cond.c_str());
      std::string hang = (out_dir / (std::string("hang_sample_") + tag + ".txt")).string();
      res[slot] = transfer(volume, is_on, tag, &hang); const Result& r = res[slot];
      char b[1100];
      std::snprintf(b, sizeof b, "{\"mode\":\"C1\",\"pair\":%d,\"slot\":%d,\"cond\":\"%s\",\"tag\":\"%s\",\"valid\":%s,\"reason\":%s,\"ttfb_s\":%.4f,\"mibps\":%.3f,\"read_s\":%.4f,\"cpu_s\":%.3f,\"free_start_gib\":%.3f,\"free_end_gib\":%.3f,"
                    "\"dir_scans\":%lld,\"dir_accepted\":%lld,\"vol_peak\":%lld,\"instrumented\":false,\"samples\":0,\"zero_peer_samples\":0,\"blocked_samples\":0,\"kicks\":0,\"wd_resets\":0,\"unhave_resets\":0,\"connects\":0,\"disconnects\":0,"
                    "\"write_p50_us\":%.1f,\"write_p99_us\":%.1f,\"write_max_us\":%.1f,\"read_p50_us\":%.1f,\"read_p99_us\":%.1f,\"cap_violations\":%lld,\"refused_writes\":%lld,\"forced_evictions\":%lld,\"sweep_evictions\":%lld,\"writes\":%lld,\"max_charged_mib\":%.2f,\"limit_mib\":%.0f,\"wall\":%ld}\n",
                    k, slot, cond.c_str(), tag, r.valid ? "true" : "false", jstr(r.reason).c_str(), r.ttfb, r.mibps, r.dt, r.cpu_s, r.free_start_gib, r.free_end_gib, r.dir_scans, r.dir_accepted, r.vol_peak, r.w_p50, r.w_p99, r.w_max, r.r_p50, r.r_p99,
                    r.cap_violations, r.refused, r.forced_ev, r.sweep_ev, r.writes, r.max_charged_mib, r.limit_mib, (long)time(nullptr));
      out << b; out.flush();
      ITLOG("pair %3d slot %d %-3s: %7.1f MiB/s write p99 %.0f us cpu %.2f s evict f/s %lld/%lld%s%s", k + 1, slot, cond.c_str(), r.mibps, r.w_p99, r.cpu_s, r.forced_ev, r.sweep_ev, r.valid ? "" : " INVALID: ", r.reason.c_str());
      if (r.reason == "volume_free_below_6GiB") { halted = true; halt_reason = "volume_free_below_6GiB (cannot recover; run is incomplete)"; break; }
      if (slot == 0) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    ++done_pairs; if (!(res[0].valid && res[1].valid)) ++invalid_pairs;
    if (done_pairs >= 20 && invalid_pairs * 5 > done_pairs) { halted = true; halt_reason = "more_than_20_percent_invalid_pairs"; }
    if (!halted) std::this_thread::sleep_for(std::chrono::seconds(3));
  }
  ITLOG("FINISHED: %d of %d pairs run, %d invalid%s%s", done_pairs, pairs, invalid_pairs, halted ? "; HALTED: " : "", halt_reason.c_str());
  std::ofstream(out_dir / "finished.txt") << "pairs_run " << done_pairs << " of " << pairs << " invalid " << invalid_pairs << (halted ? " HALTED " + halt_reason : std::string(" complete")) << "\n";
  return 0;
}
