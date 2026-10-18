// DIAGNOSTIC protocol C1b (NOT gate M1-G3; NOT an end-to-end G3 equivalent; cannot resolve G3): paired auditor-on/auditor-off measurement of the production
// SegmentStore NEAR THE CACHE CAP (pre-write reservation and forced eviction), synthetic sequential workload, no peers/libtorrent/network.
// Implements docs/proposals/G3_C1B_PLAN.md revision 1.1 with the REVISION 2 change approved on 2026-10-17 (docs/proposals/G3_C1B_PLAN_REV2_PROPOSAL.md,
// code review: docs/proposals/G3_C1B_REV2_CODE_REVIEW.md): the WRITER sets the protection window before every piece; the ticker only sweeps and reconciles.
// Rev 1.1 text follows:
//   geometry: 256 MiB torrent / 256 KiB pieces, 128 MiB limit, default 10% margin => internal cap 120,795,955 B = 460 pieces; write the first 900 pieces sequentially in
//   16 KiB blocks; protection (head = p, lo = max(0, p-480), hi = min(1024, p+24), pins 0-7 and 1016-1023) set by the writer BEFORE piece p's first write (rev 2); ticker every 100 ms: sweep_outside_window(0 ns, 2), reconcile_step(256).
//   Expected per transfer: 440 forced evictions, 0 sweep evictions, victims 8..447, final present set {0..7} U {448..899}, max charged about 115 MiB.
//   HARD correctness checks (any failure = FAILED DIAGNOSTIC CHECK: halt at once, preserve the store directory and evidence, no retry/replacement):
//   cap violations, max/final charged vs limit, refused writes, reservation accounting, allocated bytes vs limit, read-back integrity, absent-piece reads, pinned head pieces,
//   and the exact predicted final piece set. VALIDITY rule (not correctness): the workload must reach the cap regime (forced >= 300, max charged >= 0.95 x cap, sweeps <= 20% of forced).
// usage: diag_c1b_store pairs=N seed=S out=<dir> volume=<mount>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <random>
#include <sstream>
#include "loopback_fixture.hpp"
#include "tpcore/audit.hpp"
#include "tpcore/config.hpp"
#include "tpcore/segment_store.hpp"
using namespace tpit;

static std::map<std::string, std::string> args(int argc, char** argv) { std::map<std::string, std::string> m; for (int i = 1; i < argc; ++i) { std::string a = argv[i]; auto e = a.find('='); if (e != std::string::npos) m[a.substr(0, e)] = a.substr(e + 1); } return m; }
static double cpu_seconds() { rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6; }
static std::string sleep_stamp() { char b[128]; std::string s; for (const char* n : {"kern.sleeptime", "kern.waketime"}) { timeval tv{}; size_t l = sizeof tv; sysctlbyname(n, &tv, &l, nullptr, 0); std::snprintf(b, sizeof b, "%ld.%06d;", (long)tv.tv_sec, (int)tv.tv_usec); s += b; } return s; }
static bool on_ac() { FILE* f = popen("pmset -g batt | head -1", "r"); char b[256] = {0}; if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; pclose(f); } return std::string(b).find("AC Power") != std::string::npos; }
static std::string jstr(const std::string& s) { std::string o = "\""; for (char c : s) { if (c == '"' || c == '\\' ) o += '\\'; if (c == '\n') { o += "\\n"; continue; } o += c; } return o + "\""; }
static double quantile(std::vector<double> v, double q) { if (v.empty()) return 0; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))]; }

constexpr std::int64_t PS = 262144, TOTAL = 256LL << 20, LIMIT = 128LL << 20; constexpr int NP = static_cast<int>(TOTAL / PS), WANT = 900, BLK = 16384, BEHIND = 480, AHEAD = 24;

struct Result {
  bool valid = true; std::string reason;
  bool hard_ok = true; std::string hard_failures;                       // correctness
  bool cap_reached = false;                                              // validity (workload regime)
  double mibps = 0, evict_mibps = 0, dt = 0, cpu_s = 0, ttfb = 0, free_start_gib = 0, free_end_gib = 0;
  double fw_p50 = 0, fw_p99 = 0, fw_max = 0, ow_p50 = 0, ow_p99 = 0, r_p50 = 0, r_p99 = 0;
  long long dir_scans = 0, dir_accepted = 0, vol_peak = 0;
  long long cap_violations = 0, refused = 0, forced_ev = 0, sweep_ev = 0, writes = 0, reserve_calls = 0, recon = 0; double max_charged_mib = 0, charged_end_mib = 0, allocated_mib = 0, limit_mib = 0, internal_cap_mib = 0;
  int present_end = 0, set_diff = 0, integrity_bad = 0, absent_bad = 0;
  std::string set_diff_detail;
};

static void stamp(std::vector<char>& blk, int piece, int off) { std::int64_t a = piece, b = off; std::memcpy(blk.data(), &a, 8); std::memcpy(blk.data() + 8, &b, 8); }

// returns the filled Result; on hard failure the store directory is left in place and `preserve` is set
static Result transfer(const fs::path& volume, bool with_auditors, const std::string& tag, const fs::path& out_dir, bool* preserve) {
  Result R; *preserve = false;
  R.free_start_gib = tp::volume_free_bytes(volume) / 1073741824.0;
  if (R.free_start_gib < 6.0) { R.valid = false; R.reason = "volume_free_below_6GiB"; return R; }
  const std::string stamp0 = sleep_stamp();
  tp::CacheConfig cc; cc.limit_bytes = LIMIT;
  auto root = volume / ("diagc1b2_" + out_dir.filename().string() + "_" + tag);                                      // unique per run: never touches the preserved rev 1.1 failure directory diagc1b_calib_off
  if (fs::exists(root)) { R.valid = false; R.reason = "store_directory_already_exists_refusing_to_reuse"; return R; }   // never delete or reuse existing evidence
  fs::create_directories(root);
  tp::StoreParams sp; sp.dir = root; sp.piece_size = PS; sp.num_pieces = NP; sp.total_size = TOTAL; sp.limit_bytes = LIMIT; sp.margin_bytes = LIMIT - tp::internal_cap(cc, PS);
  auto store = std::make_unique<tp::SegmentStore>(sp);
  const std::int64_t cap = tp::internal_cap(cc, PS); R.internal_cap_mib = cap / 1048576.0; R.limit_mib = LIMIT / 1048576.0;
  std::vector<char> base(BLK), blk(BLK), rbuf(static_cast<size_t>(PS));
  for (size_t i = 0; i < base.size(); ++i) base[i] = static_cast<char>(i * 31 + 7);
  std::atomic<int> head{0}; std::atomic<bool> stop_tick{false}, cancel{false}, done{false};
  const double cpu0 = cpu_seconds(); const auto t0 = Clock::now();
  std::unique_ptr<tp::VolumeDeltaAuditor> va; std::unique_ptr<tp::DirScanAuditor> da;
  if (with_auditors) { va = std::make_unique<tp::VolumeDeltaAuditor>(volume, std::chrono::microseconds(1000)); va->start(); da = std::make_unique<tp::DirScanAuditor>(*store, std::chrono::microseconds(1500)); da->start(); }
  // REV 2: protection is set by the writer before each piece (the window tracks the write front, as the production window tracks the download front); identical in both arms.
  auto set_window = [&](int piece) { tp::Protection pr; pr.head = piece; pr.lo = std::max(0, piece - BEHIND); pr.hi = std::min(NP, piece + AHEAD); pr.pin_a_first = 0; pr.pin_a_last = 7; pr.pin_b_first = NP - 8; pr.pin_b_last = NP - 1; store->set_protection(pr); };
  set_window(0);                                                                                                       // before the ticker exists: nothing is ever swept under the default (empty) protection
  std::thread ticker([&] {                                                                                             // the ticker keeps only the store maintenance the scheduler also performs
    while (!stop_tick) { store->sweep_outside_window(std::chrono::nanoseconds(0), 2); store->reconcile_step(256); std::this_thread::sleep_for(std::chrono::milliseconds(100)); } });
  std::string hang_path = (out_dir / ("hang_sample_" + tag + ".txt")).string();
  std::thread wd([&] { while (!done) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); if (secs(Clock::now() - t0) > 120.0) { char c[512]; std::snprintf(c, sizeof c, "sample %d 3 -file %s > /dev/null 2>&1", (int)getpid(), hang_path.c_str()); (void)system(c); cancel = true; return; } } });
  std::vector<double> fw, ow, rl; fw.reserve(WANT); ow.reserve(static_cast<size_t>(WANT) * 15); rl.reserve(WANT);
  const auto t1 = Clock::now(); int p = 0; bool aborted = false; double first = -1; Clock::time_point t_ev{}; bool t_ev_set = false;
  for (; p < WANT && !cancel; ++p) {
    head = p; set_window(p);                                                                                           // REV 2: window updated BEFORE the first write of piece p
    if (p == 460 && !t_ev_set) { t_ev = Clock::now(); t_ev_set = true; }
    for (int off = 0, k = 0; off < PS; off += BLK, ++k) {
      blk = base; stamp(blk, p, off);
      const auto a = Clock::now(); auto ws = store->write(p, off, blk.data(), BLK); const double us = std::chrono::duration<double, std::micro>(Clock::now() - a).count();
      (k == 0 ? fw : ow).push_back(us);
      if (ws != tp::WriteStatus::Ok) { R.valid = true; R.hard_ok = false; R.hard_failures += "write_status_not_ok(piece " + std::to_string(p) + ");"; aborted = true; break; }
    }
    if (aborted) break;
    store->mark_verified(p); if (first < 0) first = secs(Clock::now() - t1);
    const auto b = Clock::now(); store->touch(p); store->read(p, 0, rbuf.data(), static_cast<int>(PS)); rl.push_back(std::chrono::duration<double, std::micro>(Clock::now() - b).count());
  }
  const auto t_end = Clock::now(); const double dt = secs(t_end - t1);
  stop_tick = true; ticker.join();
  if (va) { auto r = va->stop(); R.vol_peak = static_cast<long long>(r.peak_usage()); } if (da) { auto r = da->stop(); R.dir_scans = static_cast<long long>(r.scans); R.dir_accepted = static_cast<long long>(r.accepted); }
  done = true; wd.join();
  const auto m = store->metrics();
  R.dt = dt; R.mibps = (static_cast<double>(p) * static_cast<double>(PS) / 1048576.0) / dt; R.ttfb = first < 0 ? 0 : first; R.cpu_s = cpu_seconds() - cpu0;
  R.evict_mibps = t_ev_set && p > 460 ? (static_cast<double>(p - 460) * static_cast<double>(PS) / 1048576.0) / secs(t_end - t_ev) : 0;
  R.fw_p50 = quantile(fw, .5); R.fw_p99 = quantile(fw, .99); R.fw_max = fw.empty() ? 0 : *std::max_element(fw.begin(), fw.end()); R.ow_p50 = quantile(ow, .5); R.ow_p99 = quantile(ow, .99); R.r_p50 = quantile(rl, .5); R.r_p99 = quantile(rl, .99);
  R.cap_violations = m.cap_violations; R.refused = m.refused_writes; R.forced_ev = m.forced_evictions; R.sweep_ev = m.sweep_evictions; R.writes = m.writes; R.reserve_calls = m.reserve_calls; R.recon = m.reconcile_corrections;
  R.max_charged_mib = m.max_charged / 1048576.0; R.charged_end_mib = m.charged / 1048576.0;
  // ---------------- validity and hard checks (outside the timed window; auditors already stopped)
  if (cancel) { R.valid = false; R.reason = "hang_over_120s"; }
  else if (!(R.mibps > 0) || !std::isfinite(R.mibps)) { R.valid = false; R.reason = "nonpositive_or_nonfinite_throughput"; }
  if (sleep_stamp() != stamp0) { R.valid = false; R.reason = R.reason.empty() ? "sleep_or_wake_during_transfer" : R.reason + "+sleep"; }
  auto hard = [&](bool bad, const std::string& what) { if (bad) { R.hard_ok = false; R.hard_failures += what + ";"; } };
  if (!cancel && !aborted) {
    hard(m.cap_violations != 0, "cap_violations=" + std::to_string(m.cap_violations));
    hard(m.max_charged > LIMIT, "max_charged_over_limit"); hard(m.charged > LIMIT, "charged_end_over_limit"); hard(m.refused_writes != 0, "refused_writes=" + std::to_string(m.refused_writes));
    auto present = store->present_pieces(); R.present_end = static_cast<int>(present.size());
    hard(m.charged < static_cast<std::int64_t>(present.size()) * PS, "charged_below_present_x_piece_size"); hard(present.size() > 462, "present_pieces_above_462");
    hard(m.reserve_calls != m.writes, "reserve_calls_ne_writes(" + std::to_string(m.reserve_calls) + "/" + std::to_string(m.writes) + ")");
    R.allocated_mib = tp::measure_allocated_bytes(root) / 1048576.0; hard(R.allocated_mib > R.limit_mib, "allocated_bytes_over_limit");
    // predicted final set {0..7} U {448..899}
    std::vector<int> predicted; for (int i = 0; i < 8; ++i) predicted.push_back(i); for (int i = 448; i < WANT; ++i) predicted.push_back(i);
    std::vector<int> diff; std::set_symmetric_difference(present.begin(), present.end(), predicted.begin(), predicted.end(), std::back_inserter(diff));
    R.set_diff = static_cast<int>(diff.size()); { std::ostringstream o; for (size_t i = 0; i < diff.size() && i < 40; ++i) o << diff[i] << ' '; R.set_diff_detail = o.str(); }
    hard(R.set_diff != 0, "final_piece_set_differs_from_prediction(" + std::to_string(R.set_diff) + " pieces: " + R.set_diff_detail + ")");
    for (int i = 0; i < 8; ++i) hard(!store->has(i), "pinned_head_piece_missing(" + std::to_string(i) + ")");
    // integrity: every present piece content-correct; every written-but-absent piece unreadable
    for (int q = 0; q < WANT; ++q) {
      const bool has = store->has(q);
      if (has) {
        bool bad = false;
        for (int off = 0; off < PS && !bad; off += BLK) {
          int n = store->read(q, off, rbuf.data(), BLK);
          if (n != BLK) { bad = true; break; }
          blk = base; stamp(blk, q, off); if (std::memcmp(blk.data(), rbuf.data(), BLK) != 0) bad = true;
        }
        if (bad) { ++R.integrity_bad; if (R.integrity_bad <= 5) R.hard_failures += "readback_mismatch(piece " + std::to_string(q) + ");"; R.hard_ok = false; }
      } else if (store->read(q, 0, rbuf.data(), BLK) != -1) { ++R.absent_bad; if (R.absent_bad <= 5) R.hard_failures += "absent_piece_still_readable(" + std::to_string(q) + ");"; R.hard_ok = false; }
    }
  }
  R.free_end_gib = tp::volume_free_bytes(volume) / 1073741824.0;
  R.cap_reached = R.forced_ev >= 300 && R.max_charged_mib >= 0.95 * R.internal_cap_mib && R.sweep_ev * 5 <= R.forced_ev;
  if (R.valid && R.hard_ok && !R.cap_reached) { R.valid = false; R.reason = R.forced_ev < 300 ? "forced_evictions_not_observed" : "cap_not_reached"; }
  if (!R.hard_ok) *preserve = true;                                                    // the failing store directory is LEFT IN PLACE on the test volume (diagc1b_<tag>) as evidence
  store.reset();
  if (R.hard_ok) fs::remove_all(root);                                                  // only this transfer's own, uniquely named directory is ever removed
  return R;
}

static void emit(std::ofstream& out, const std::string& file_tag, int pair, int slot, const std::string& cond, const std::string& tag, const Result& r) {
  char b[2200];
  std::snprintf(b, sizeof b, "{\"mode\":\"C1B\",\"pair\":%d,\"slot\":%d,\"cond\":\"%s\",\"tag\":\"%s\",\"valid\":%s,\"reason\":%s,\"hard_ok\":%s,\"hard_failures\":%s,\"cap_reached\":%s,\"ttfb_s\":%.4f,\"mibps\":%.3f,\"evict_phase_mibps\":%.3f,\"read_s\":%.4f,\"cpu_s\":%.3f,"
                "\"free_start_gib\":%.3f,\"free_end_gib\":%.3f,\"dir_scans\":%lld,\"dir_accepted\":%lld,\"vol_peak\":%lld,\"instrumented\":false,\"samples\":0,\"zero_peer_samples\":0,\"blocked_samples\":0,\"kicks\":0,\"wd_resets\":0,\"unhave_resets\":0,\"connects\":0,\"disconnects\":0,"
                "\"first_write_p50_us\":%.1f,\"first_write_p99_us\":%.1f,\"first_write_max_us\":%.1f,\"other_write_p50_us\":%.1f,\"other_write_p99_us\":%.1f,\"read_p50_us\":%.1f,\"read_p99_us\":%.1f,"
                "\"cap_violations\":%lld,\"refused_writes\":%lld,\"forced_evictions\":%lld,\"sweep_evictions\":%lld,\"writes\":%lld,\"reserve_calls\":%lld,\"reconcile_corrections\":%lld,\"max_charged_mib\":%.2f,\"charged_end_mib\":%.2f,"
                "\"allocated_mib\":%.2f,\"limit_mib\":%.1f,\"internal_cap_mib\":%.2f,\"present_end\":%d,\"set_diff\":%d,\"integrity_bad\":%d,\"absent_bad\":%d,\"wall\":%ld}\n",
                pair, slot, cond.c_str(), tag.c_str(), r.valid ? "true" : "false", jstr(r.reason).c_str(), r.hard_ok ? "true" : "false", jstr(r.hard_failures).c_str(), r.cap_reached ? "true" : "false", r.ttfb, r.mibps, r.evict_mibps, r.dt, r.cpu_s,
                r.free_start_gib, r.free_end_gib, r.dir_scans, r.dir_accepted, r.vol_peak, r.fw_p50, r.fw_p99, r.fw_max, r.ow_p50, r.ow_p99, r.r_p50, r.r_p99,
                r.cap_violations, r.refused, r.forced_ev, r.sweep_ev, r.writes, r.reserve_calls, r.recon, r.max_charged_mib, r.charged_end_mib, r.allocated_mib, r.limit_mib, r.internal_cap_mib, r.present_end, r.set_diff, r.integrity_bad, r.absent_bad, (long)time(nullptr));
  out << b; out.flush(); (void)file_tag;
}

int main(int argc, char** argv) {
  auto a = args(argc, argv);
  const int pairs = a.count("pairs") ? std::atoi(a["pairs"].c_str()) : 96; const std::uint64_t seed = a.count("seed") ? std::strtoull(a["seed"].c_str(), nullptr, 10) : 20261021;
  const fs::path out_dir = a.count("out") ? a["out"] : ""; const fs::path volume = a.count("volume") ? a["volume"] : "";
  if (out_dir.empty() || volume.empty()) { std::puts("usage: diag_c1b_store pairs=N seed=S out=<dir> volume=<mount>"); return 2; }
  fs::create_directories(out_dir);
  std::vector<int> order; for (int i = 0; i < pairs; ++i) order.push_back(i % 2);
  std::mt19937_64 rng(seed); std::shuffle(order.begin(), order.end(), rng);
  { std::ofstream p(out_dir / "order_sequence.txt"); p << "mode C1B seed " << seed << " pairs " << pairs << "\n"; for (int v : order) p << v; p << "\n(0 = OFF first, 1 = ON first)\n"; }
  std::ofstream out(out_dir / "transfers.jsonl", std::ios::app), calib(out_dir / "calibration.jsonl", std::ios::app);
  ITLOG("diag_c1b_store pairs=%d seed=%llu out=%s (internal cap %.2f MiB = %d pieces)", pairs, (unsigned long long)seed, out_dir.c_str(), tp::internal_cap([] { tp::CacheConfig c; c.limit_bytes = LIMIT; return c; }(), PS) / 1048576.0, 460);
  auto finish = [&](const std::string& s) { std::ofstream(out_dir / "finished.txt") << s << "\n"; ITLOG("%s", s.c_str()); };
  auto fail_halt = [&](const std::string& where, const Result& r) {
    std::ofstream(out_dir / ("FAILURE_" + where + ".json")) << "{\"where\":" << jstr(where) << ",\"hard_failures\":" << jstr(r.hard_failures) << ",\"forced\":" << r.forced_ev << ",\"sweep\":" << r.sweep_ev << ",\"max_charged_mib\":" << r.max_charged_mib << ",\"present_end\":" << r.present_end << ",\"set_diff\":" << r.set_diff << "}\n";
    std::string cmd = "ls -la " + (volume / ("diagc1b2_" + out_dir.filename().string() + "_" + where)).string() + " > " + (out_dir / ("failure_listing_" + where + ".txt")).string() + " 2>&1"; (void)system(cmd.c_str());
  };
  // ---- calibration (outside the paired dataset): one OFF and one ON transfer; must reach the cap regime and pass every hard check, otherwise STOP (no tuning)
  for (int c = 0; c < 2; ++c) {
    const bool on = c == 1; const std::string cond = on ? "on" : "off"; const std::string tag = std::string("calib_") + cond; bool preserve = false;
    if (!on_ac()) { finish("HALTED power_source_not_AC_before_calibration"); return 0; }
    Result r = transfer(volume, on, tag, out_dir, &preserve); emit(calib, "calib", -1, c, cond, tag, r);
    ITLOG("calibration %-3s: %.1f MiB/s (eviction phase %.1f) forced %lld sweep %lld max charged %.1f MiB present %d set_diff %d hard_ok %d cap_reached %d valid %d %s", cond.c_str(), r.mibps, r.evict_mibps, r.forced_ev, r.sweep_ev, r.max_charged_mib, r.present_end, r.set_diff, r.hard_ok, r.cap_reached, r.valid, r.hard_failures.c_str());
    if (!r.hard_ok) { fail_halt(tag, r); finish("HALTED CALIBRATION_HARD_CHECK_FAILED " + tag + ": " + r.hard_failures); return 0; }
    if (!r.valid || !r.cap_reached) { fail_halt(tag, r); finish("HALTED CALIBRATION_FAILED " + tag + " (workload did not reach the registered forced-eviction regime or transfer invalid: " + r.reason + ")"); return 0; }
    std::this_thread::sleep_for(std::chrono::seconds(2));
  }
  int invalid_pairs = 0, done_pairs = 0, consec_cap_fail = 0; bool halted = false; std::string halt_reason;
  for (int k = 0; k < pairs && !halted; ++k) {
    if (!on_ac()) { halted = true; halt_reason = "power_source_not_AC_before_pair"; break; }
    Result res[2]; bool pair_cap_fail = false;
    for (int slot = 0; slot < 2; ++slot) {
      const bool is_on = (slot == 0) == (order[k] == 1); const std::string cond = is_on ? "on" : "off"; char tag[64]; std::snprintf(tag, sizeof tag, "C1B_%03d_%s", k, cond.c_str());
      bool preserve = false; res[slot] = transfer(volume, is_on, tag, out_dir, &preserve); const Result& r = res[slot];
      emit(out, "pairs", k, slot, cond, tag, r);
      ITLOG("pair %3d slot %d %-3s: %7.1f MiB/s (evict phase %.1f) forced %lld sweep %lld max charged %.1f MiB first-write p99 %.0f us cpu %.2f s%s%s%s", k + 1, slot, cond.c_str(), r.mibps, r.evict_mibps, r.forced_ev, r.sweep_ev, r.max_charged_mib, r.fw_p99, r.cpu_s,
            r.valid ? "" : " INVALID: ", r.reason.c_str(), r.hard_ok ? "" : " HARD FAILURE");
      if (!r.hard_ok) { fail_halt(tag, r); halted = true; halt_reason = std::string("HARD_CORRECTNESS_FAILURE ") + tag + ": " + r.hard_failures; break; }
      if (r.reason == "volume_free_below_6GiB") { halted = true; halt_reason = "volume_free_below_6GiB (cannot recover; run is incomplete)"; break; }
      if (r.reason == "cap_not_reached" || r.reason == "forced_evictions_not_observed") pair_cap_fail = true;
      if (slot == 0) std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (halted && halt_reason.rfind("HARD_CORRECTNESS", 0) == 0) break;
    ++done_pairs; if (!(res[0].valid && res[1].valid)) ++invalid_pairs;
    consec_cap_fail = pair_cap_fail ? consec_cap_fail + 1 : 0;
    if (consec_cap_fail >= 3) { halted = true; halt_reason = "workload_failed_to_reach_cap_in_3_consecutive_pairs"; }
    if (done_pairs >= 20 && invalid_pairs * 5 > done_pairs) { halted = true; halt_reason = "more_than_20_percent_invalid_pairs"; }
    if (!halted) std::this_thread::sleep_for(std::chrono::seconds(3));
  }
  char b[400]; std::snprintf(b, sizeof b, "pairs_run %d of %d invalid %d %s%s", done_pairs, pairs, invalid_pairs, halted ? "HALTED " : "complete", halt_reason.c_str());
  finish(b);
  return 0;
}
