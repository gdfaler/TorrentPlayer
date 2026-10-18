// DIAGNOSTIC harness "CHURN-F" (NOT gate M1-G3, NOT a substitute for it). Implements docs/proposals/G3_CHURN_VARIABILITY_PROPOSAL.md revision 3 (approved as the basis for implementation).
// 2 x 2 factorial, randomised blocks of four (one transfer per cell, seeded random order), auditors OFF in every transfer:
//   X  churn policy : default (libtorrent behaviour) | suppressed (close_redundant_connections=false, HARNESS ENGINE ONLY; never the product)
//   G  sequencing   : gate-like (no gap) | spaced (exactly 5.0 s idle gap before the transfer's setup; the ONLY intended difference between the G levels)
// State handling is identical for all cells: a ring of 10 harness-owned slot directories (slot = transfer index mod 10) which are removed+recreated immediately before the
// timed region and RETAINED after the transfer (the original gate's steady-state behaviour, see proposal section 2).
// The transfer body is the body of it_auditor_overhead's one() copied unchanged (same payload, seeders, ports, cache size, read size) without the auditors; bookkeeping is new.
// This program never deletes a directory it did not create in this run; it does NOT delete the ring at the end (cleanup is a separate, evidence-preserving script).
// usage: diag_churn_f blocks=N seed=S out=<dir> volume=<mount> runid=<id>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <algorithm>
#include <map>
#include <random>
#include "loopback_fixture.hpp"
#include "tpcore/audit.hpp"
#include "churn_tree_stats.hpp"
using namespace tpit;

static std::map<std::string, std::string> args(int argc, char** argv) { std::map<std::string, std::string> m; for (int i = 1; i < argc; ++i) { std::string a = argv[i]; auto e = a.find('='); if (e != std::string::npos) m[a.substr(0, e)] = a.substr(e + 1); } return m; }
static double cpu_seconds() { rusage r; getrusage(RUSAGE_SELF, &r); return r.ru_utime.tv_sec + r.ru_utime.tv_usec / 1e6 + r.ru_stime.tv_sec + r.ru_stime.tv_usec / 1e6; }
static std::string sleep_stamp() { char b[128]; std::string s; for (const char* n : {"kern.sleeptime", "kern.waketime"}) { timeval tv{}; size_t l = sizeof tv; sysctlbyname(n, &tv, &l, nullptr, 0); std::snprintf(b, sizeof b, "%ld.%06d;", (long)tv.tv_sec, (int)tv.tv_usec); s += b; } return s; }
static bool on_ac_pmset() { FILE* f = popen("pmset -g batt | head -1", "r"); char b[256] = {0}; if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; pclose(f); } return std::string(b).find("AC Power") != std::string::npos; }
// in-process, ~1 ms: used at block starts so that the AC check does not add a gap before block-first transfers (proposal 4.2 item 5)
static bool on_ac_fast() { CFTypeRef info = IOPSCopyPowerSourcesInfo(); if (!info) return false; CFStringRef t = IOPSGetProvidingPowerSourceType(info); const bool ok = t && CFStringCompare(t, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo; CFRelease(info); return ok; }
static std::string jstr(const std::string& s) { std::string o = "\""; for (char c : s) { if (c == '"' || c == '\\') o += '\\'; o += c; } return o + "\""; }
using Mono = std::chrono::steady_clock;
static double since(Mono::time_point t0, Mono::time_point t) { return std::chrono::duration<double>(t - t0).count(); }
static ino_t inode_of(const fs::path& p) { struct stat st{}; return ::stat(p.c_str(), &st) == 0 ? st.st_ino : 0; }

// true if nothing else is bound to the port (TCP with SO_REUSEADDR as libtorrent uses it; UDP without reuse flags)
static bool port_free(int port) {
  bool ok = true;
  for (int type : {SOCK_STREAM, SOCK_DGRAM}) {
    int s = ::socket(AF_INET, type, 0); if (s < 0) return false;
    if (type == SOCK_STREAM) { int one = 1; setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one); }
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(static_cast<uint16_t>(port)); inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    if (::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) ok = false;
    ::close(s);
  }
  return ok;
}

struct Cell { int x; int g; };  // x: 0 default, 1 suppressed ; g: 0 gate-like, 1 spaced
static const char* xname(int x) { return x ? "suppressed" : "default"; }
static const char* gname(int g) { return g ? "spaced" : "gate-like"; }

struct Result {
  bool valid = true; std::string reason;
  double ttfb = 0, mibps = 0, dt = 0, cpu_s = 0, free_start_gib = 0, free_end_gib = 0;
  int samples = 0, zero_peer = 0, blocked = 0; long long peers_sum = 0; int kicks = 0, wd_resets = 0, unhave = 0; int connects = 0, disconnects = 0;
  int setting_close_redundant = -1;
  Mono::time_point construct_start_tp{};  // monotonic time at which construction of seeders/engine started
};

// body of it_auditor_overhead's one() (without the auditor arm), unchanged except for the bookkeeping marked NEW
static Result transfer(const Payload& pl, const fs::path& volume, const fs::path& root, int x_suppressed, const fs::path& out_dir, const std::string& tag) {
  Result R;
  R.free_start_gib = tp::volume_free_bytes(volume) / 1073741824.0;                                                        // NEW
  if (R.free_start_gib < 6.0) { R.valid = false; R.reason = "volume_free_below_6GiB"; return R; }                          // NEW (policy)
  const std::string stamp0 = sleep_stamp();                                                                                // NEW
  R.construct_start_tp = Mono::now();                                                                                      // NEW (construction of seeders/engine starts here)
  Seeder s1(pl, 42501), s2(pl, 42502);
  const fs::path netlog = out_dir / ("net_" + tag + ".log");
  setenv("TP_NET_DIAG", netlog.c_str(), 1);                                                                                // NEW (read at Engine construction; identical in every cell)
  tp::Engine eng(engine_config(root, 42003));
  { lt::settings_pack sp; sp.set_bool(lt::settings_pack::close_redundant_connections, x_suppressed ? false : true); eng.session().apply_settings(sp); }   // NEW: X factor; both levels make the same call (default level re-states libtorrent's default true)
  R.setting_close_redundant = eng.session().get_settings().get_bool(lt::settings_pack::close_redundant_connections) ? 1 : 0;  // NEW (read-back)
  tp::TorrentSource src; src.torrent_bytes = pl.torrent_bytes; src.strip_web_seeds = true; src.strip_trackers = true; src.extra_peers = {s1.endpoint(), s2.endpoint()};
  tp::CacheConfig cc; cc.limit_bytes = 128LL << 20;
  const double cpu0 = cpu_seconds();                                                                                        // NEW
  const auto t0 = Clock::now();
  std::string err; auto ss = eng.open_stream(src, 0, cc, &err);
  std::atomic<bool> cancel{false}, done{false}, hung{false};                                                                // NEW: 120 s hang rule
  std::thread wd([&] { while (!done) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); if (secs(Clock::now() - t0) > 120.0) { hung = true; char c[600]; std::snprintf(c, sizeof c, "sample %d 3 -file %s > /dev/null 2>&1", (int)getpid(), (out_dir / ("hang_sample_" + tag + ".txt")).c_str()); (void)system(c); cancel = true; return; } } });
  std::vector<char> buf(256 * 1024);
  ss->read(pl.size - 65536, buf.data(), 65536, &cancel);
  const double ttfb = secs(Clock::now() - t0);
  tp::StreamStatus st0 = ss->status();                                                                                      // NEW (counter baseline for deltas)
  std::atomic<bool> mon_stop{false}; std::atomic<int> samples{0}, zero{0}, blocked{0}; std::atomic<long long> psum{0};      // NEW: monitor thread, identical in every cell
  std::thread mon([&] { while (!mon_stop) { auto s = ss->status(); ++samples; psum += s.peers; if (s.peers == 0) ++zero; if (s.blocked_readers > 0) ++blocked; std::this_thread::sleep_for(std::chrono::milliseconds(50)); } });
  const auto t1 = Clock::now(); std::int64_t off = 0; const std::int64_t want = 160LL << 20;
  while (off < want && !cancel) { std::int64_t n = ss->read(off, buf.data(), static_cast<std::int64_t>(buf.size()), &cancel); if (n <= 0) break; off += n; }
  const double dt = secs(Clock::now() - t1);
  mon_stop = true; mon.join();
  tp::StreamStatus st1 = ss->status();                                                                                      // NEW
  done = true; wd.join();                                                                                                   // NEW
  R.ttfb = ttfb; R.dt = dt; R.mibps = (static_cast<double>(off) / 1048576.0) / dt;
  R.cpu_s = cpu_seconds() - cpu0; R.free_end_gib = tp::volume_free_bytes(volume) / 1073741824.0;
  R.samples = samples; R.zero_peer = zero; R.blocked = blocked; R.peers_sum = psum; R.kicks = st1.connect_kicks - st0.connect_kicks; R.wd_resets = st1.watchdog_resets - st0.watchdog_resets; R.unhave = st1.un_have_resets - st0.un_have_resets;
  if (hung) { R.valid = false; R.reason = "hang_over_120s"; }
  else if (!(R.mibps > 0) || !std::isfinite(R.mibps)) { R.valid = false; R.reason = "nonpositive_or_nonfinite_throughput"; }
  if (sleep_stamp() != stamp0) { R.valid = false; R.reason = R.reason.empty() ? "sleep_or_wake_during_transfer" : R.reason + "+sleep"; }
  ss.reset();                                                                                                               // stream torn down before the netlog is read
  if (fs::exists(netlog)) { std::ifstream f(netlog); std::string l; while (std::getline(f, l)) { if (l.find("] peer_connect:") != std::string::npos) ++R.connects; else if (l.find("] peer_disconnected:") != std::string::npos) ++R.disconnects; } }
  return R;                                                                                                                 // destructors: engine, seeders (gate order)
}

// recursive, two-method content accounting (fix of the 2026-10-17 smoke halt: the old non-recursive helper reported 0 files for the nested cache layout)
struct Meas { tpit::TreeStats a, b; bool agree() const { return a == b; } };
static Meas measure_tree(const fs::path& p) { return Meas{tpit::tree_stats_fs(p), tpit::tree_stats_posix(p.string())}; }
static bool contents_valid(const Meas& m) { return m.agree() && m.a.files >= 1 && m.a.files <= 480 && m.a.alloc_bytes > 0 && m.a.alloc_bytes <= (128LL << 20) && m.a.non_regular == 0; }
struct SlotUse { bool exists = false; int files = 0; long long bytes = 0; Meas found; ino_t inode_found = 0, inode_new = 0; double del_start = 0, del_end = 0; bool empty_after_create = false; int expected_prev = -1; };

int main(int argc, char** argv) {
  auto a = args(argc, argv);
  const int blocks = a.count("blocks") ? std::atoi(a["blocks"].c_str()) : 80;
  const std::uint64_t seed = a.count("seed") ? std::strtoull(a["seed"].c_str(), nullptr, 10) : 1;
  const fs::path out_dir = a.count("out") ? a["out"] : ""; const fs::path volume = a.count("volume") ? a["volume"] : ""; const std::string runid = a.count("runid") ? a["runid"] : "";
  if (out_dir.empty() || volume.empty() || runid.empty() || blocks < 1) { std::puts("usage: diag_churn_f blocks=N seed=S out=<dir> volume=<mount> runid=<id>"); return 2; }
  if (out_dir.string().rfind(volume.string(), 0) == 0) { std::puts("REFUSING: output directory must be outside the test volume"); return 2; }
  fs::create_directories(out_dir);
  const int N = blocks * 4; constexpr int RING = 10;
  // ---- seeded block orders (derived only from the seed) -------------------------------------------------------------------------------------------------------------------
  std::vector<std::array<int, 4>> order(blocks); std::mt19937_64 rng(seed);
  for (auto& o : order) { o = {0, 1, 2, 3}; std::shuffle(o.begin(), o.end(), rng); }   // cell id c: x = c / 2, g = c % 2
  { std::ofstream p(out_dir / "order_sequence.txt"); p << "seed " << seed << " blocks " << blocks << " (cell id c: x=c/2 [0 default,1 suppressed], g=c%2 [0 gate-like,1 spaced])\n"; for (auto& o : order) p << o[0] << o[1] << o[2] << o[3] << "\n"; }
  // ---- ring bookkeeping and manifest (written BEFORE any directory is created) --------------------------------------------------------------------------------------------
  const std::string prefix = "diagchurn_" + runid + "_slot";
  std::vector<fs::path> slot_path(RING); for (int j = 0; j < RING; ++j) slot_path[j] = volume / (prefix + std::to_string(j));
  for (auto& e : fs::directory_iterator(volume)) if (e.path().filename().string().rfind("diagchurn_" + runid + "_", 0) == 0) { std::printf("REFUSING: directory of this run id already exists: %s\n", e.path().c_str()); return 3; }
  const double free0 = tp::volume_free_bytes(volume) / 1073741824.0;
  if (free0 < 8.0) { std::printf("REFUSING: start gate: %.3f GiB free < 8 GiB\n", free0); return 4; }
  const bool ac_pm = on_ac_pmset(), ac_fast = on_ac_fast();
  if (!ac_pm || !ac_fast) { std::printf("REFUSING: not on AC power (pmset %d, IOPS %d)\n", (int)ac_pm, (int)ac_fast); return 5; }
  { std::ofstream m(out_dir / "manifest_intended.json");
    m << "{\"runid\":" << jstr(runid) << ",\"volume\":" << jstr(volume.string()) << ",\"ring\":" << RING << ",\"intended_slot_dirs\":[";
    for (int j = 0; j < RING; ++j) m << (j ? "," : "") << jstr(slot_path[j].string());
    m << "],\"blocks\":" << blocks << ",\"seed\":" << seed << ",\"free_start_gib\":" << free0 << "}\n"; }
  Payload pl = Payload::create(data_dir(), 256LL << 20, 262144, 55);                                                       // same payload as the gate (seed 55)
  std::ofstream out(out_dir / "transfers.jsonl", std::ios::app);
  const auto run_t0 = Mono::now();
  ITLOG("diag_churn_f blocks=%d (%d transfers) seed=%llu runid=%s out=%s free_start=%.3f GiB AC(pmset=%d,iops=%d)", blocks, N, (unsigned long long)seed, runid.c_str(), out_dir.c_str(), free0, (int)ac_pm, (int)ac_fast);
  std::vector<int> slot_last(RING, -1); std::vector<tpit::TreeStats> slot_ret(RING);
  int done_tr = 0, invalid_tr = 0, gap_dev = 0, supp_breach_run = 0; bool halted = false; std::string halt_reason;
  auto prev_end = run_t0;                                                                                                   // for k = 0 the spaced gap is measured from here (after payload creation)
  auto halt = [&](const std::string& why) { halted = true; halt_reason = why; ITLOG("HALT: %s", why.c_str()); };
  for (int b = 0; b < blocks && !halted; ++b) {
    // ---- block-start checks (in-process, ~1 ms; no pmset spawn between transfers) -------------------------------------------------------------------------------------------
    if (!on_ac_fast()) { halt("power_source_not_AC_before_block"); break; }
    if (tp::volume_free_bytes(volume) / 1073741824.0 < 6.0) { halt("volume_free_below_6GiB_before_block"); break; }
    { int n_other = 0; for (auto& e : fs::directory_iterator(volume)) { const std::string nm = e.path().filename().string(); if (nm.rfind("diagchurn_" + runid + "_", 0) == 0) { bool known = false; for (int j = 0; j < RING; ++j) if (e.path() == slot_path[j]) known = true; if (!known) ++n_other; } }
      if (n_other) { halt("hard_check_failed:unknown_directory_with_run_prefix"); break; } }
    for (int pos = 0; pos < 4 && !halted; ++pos) {
      const int k = b * 4 + pos; const int c = order[b][pos]; const Cell cell{c / 2, c % 2}; const int slot = k % RING;
      char tagb[64]; std::snprintf(tagb, sizeof tagb, "CF_%03d_%s_%s", k, xname(cell.x), gname(cell.g)); const std::string tag = tagb;
      // ---- sequencing intervention: the ONLY difference between the G levels ---------------------------------------------------------------------------------------------------
      double sleep_gap = 0;
      if (cell.g == 1) { std::this_thread::sleep_until(prev_end + std::chrono::milliseconds(5000)); sleep_gap = since(prev_end, Mono::now()); }
      // ---- protocol-level pre-checks (hard) ------------------------------------------------------------------------------------------------------------------------------------
      SlotUse su; su.expected_prev = slot_last[slot];
      const bool ports_ok = port_free(42501) && port_free(42502) && port_free(42003);
      if (!ports_ok) { halt("hard_check_failed:port_in_use_before_transfer"); break; }
      // ---- slot removal + recreation immediately before the timed region (gate behaviour) -------------------------------------------------------------------------------------
      const fs::path root = slot_path[slot];
      su.exists = fs::exists(root); su.inode_found = su.exists ? inode_of(root) : 0;
      if (su.exists) { su.found = measure_tree(root); su.files = static_cast<int>(su.found.a.files); su.bytes = su.found.a.alloc_bytes; }
      const bool expect_exists = su.expected_prev >= 0;
      if (su.exists != expect_exists) { halt(std::string("hard_check_failed:slot_state_mismatch exists=") + (su.exists ? "1" : "0") + " expected=" + (expect_exists ? "1" : "0")); break; }
      if (su.exists && !contents_valid(su.found)) { halt("hard_check_failed:retained_directory_contents_invalid_or_methods_disagree"); break; }
      if (su.exists && !(su.found.a == slot_ret[slot])) { halt("hard_check_failed:retained_directory_differs_from_post_transfer_measurement"); break; }   // populated-slot reuse: the directory must be exactly what the owning transfer left behind
      su.del_start = since(run_t0, Mono::now());
      if (su.exists) fs::remove_all(root);                                                                                  // harness-owned (name prefix + run id + recorded in manifest_intended.json)
      fs::create_directories(root);
      su.del_end = since(run_t0, Mono::now()); su.inode_new = inode_of(root);
      { Meas e = measure_tree(root); su.empty_after_create = e.agree() && e.a.files == 0 && e.a.dirs == 0 && e.a.non_regular == 0; }
      if (!su.empty_after_create) { halt("hard_check_failed:slot_not_empty_after_create"); break; }
      slot_last[slot] = k;
      const auto prev_end_k = prev_end;                                                                                     // completion of the previous transfer's destructors (run start for k = 0)
      Result r = transfer(pl, volume, root, cell.x, out_dir, tag);
      prev_end = Mono::now();                                                                                               // destructors of engine and seeders have completed
      const Meas ret = measure_tree(root);                                                                                  // recursive, two methods; quiescent (engine and seeders destroyed). Same step in every cell
      slot_ret[slot] = ret.a;
      if (!contents_valid(ret)) halt("hard_check_failed:retained_contents_invalid_or_methods_disagree_after_transfer");
      // ---- derived churn metrics and classifications --------------------------------------------------------------------------------------------------------------------------
      const int Rc = r.connects - 2; const double Z = r.samples ? static_cast<double>(r.zero_peer) / r.samples : 0.0; const double A = r.samples ? static_cast<double>(r.peers_sum) / r.samples : 0.0;
      const bool intact = (Rc <= 2 && r.wd_resets == 0 && Z < 0.10), present = (Rc >= 4);
      const char* manip = cell.x ? (intact ? "intact" : "breach") : (present ? "churn_present" : "churn_weak");
      if (r.valid && r.setting_close_redundant != (cell.x ? 0 : 1)) { halt("hard_check_failed:close_redundant_setting_readback_mismatch"); }
      const bool gap_ok = cell.g ? (std::fabs(sleep_gap - 5.0) <= 0.1) : true;
      // realised gap: previous transfer's completion -> start of this transfer's construction (includes sleep, port pre-check, slot deletion/creation); k = 0 is measured from the run start
      const double realised_gap = r.construct_start_tp != Mono::time_point{} ? since(prev_end_k, r.construct_start_tp) : -1.0;
      const bool gate_gap_ok = cell.g ? true : (realised_gap < 0.5);
      const bool deviation = cell.g ? !gap_ok : !gate_gap_ok; if (deviation) ++gap_dev;
      char b2[4200];
      std::snprintf(b2, sizeof b2,
        "{\"k\":%d,\"block\":%d,\"pos\":%d,\"x\":%s,\"g\":%s,\"tag\":\"%s\",\"slot\":%d,\"ring_fill\":%s,\"valid\":%s,\"reason\":%s,\"ttfb_s\":%.4f,\"mibps\":%.3f,\"read_s\":%.3f,\"cpu_s\":%.3f,\"free_start_gib\":%.3f,\"free_end_gib\":%.3f,"
        "\"samples\":%d,\"zero_peer_samples\":%d,\"blocked_samples\":%d,\"peers_sum\":%lld,\"Rc\":%d,\"Z\":%.4f,\"A\":%.3f,\"kicks\":%d,\"wd_resets\":%d,\"unhave_resets\":%d,\"connects\":%d,\"disconnects\":%d,\"manip\":\"%s\","
        "\"setting_close_redundant\":%d,\"slot_prev_use\":%d,\"slot_found\":%s,\"slot_found_files\":%d,\"slot_found_bytes\":%lld,\"slot_inode_found\":%llu,\"slot_inode_new\":%llu,\"del_start_s\":%.4f,\"del_end_s\":%.4f,\"empty_after_create\":%s,"
        "\"retained_files\":%lld,\"retained_bytes\":%lld,\"retained_apparent\":%lld,\"retained_dirs\":%lld,\"retained_non_regular\":%lld,\"retained_files_indep\":%lld,\"retained_bytes_indep\":%lld,\"retained_agree\":%s,\"slot_found_files_indep\":%lld,\"slot_found_bytes_indep\":%lld,\"slot_found_agree\":%s,\"sleep_gap_s\":%.4f,\"realised_gap_s\":%.4f,\"construct_start_s\":%.4f,\"gap_deviation\":%s,\"ports_free\":true,\"wall\":%ld}\n",
        k, b, pos, jstr(xname(cell.x)).c_str(), jstr(gname(cell.g)).c_str(), tag.c_str(), slot, k < RING ? "true" : "false", r.valid ? "true" : "false", jstr(r.reason).c_str(), r.ttfb, r.mibps, r.dt, r.cpu_s, r.free_start_gib, r.free_end_gib,
        r.samples, r.zero_peer, r.blocked, r.peers_sum, Rc, Z, A, r.kicks, r.wd_resets, r.unhave, r.connects, r.disconnects, manip,
        r.setting_close_redundant, su.expected_prev, su.exists ? "true" : "false", su.files, su.bytes, (unsigned long long)su.inode_found, (unsigned long long)su.inode_new, su.del_start, su.del_end, su.empty_after_create ? "true" : "false",
        ret.a.files, ret.a.alloc_bytes, ret.a.apparent_bytes, ret.a.dirs, ret.a.non_regular, ret.b.files, ret.b.alloc_bytes, ret.agree() ? "true" : "false", su.found.b.files, su.found.b.alloc_bytes, su.exists ? (su.found.agree() ? "true" : "false") : "true", sleep_gap, realised_gap, since(run_t0, r.construct_start_tp), deviation ? "true" : "false", (long)time(nullptr));
      out << b2; out.flush();
      ITLOG("k %3d blk %2d %-10s %-9s slot %d: %6.1f MiB/s Rc %2d Z %.2f A %.2f W %d %s gap(sleep %.2f, total %.2f)%s%s", k, b, xname(cell.x), gname(cell.g), slot, r.mibps, Rc, Z, A, r.wd_resets, manip, sleep_gap, realised_gap,
            r.valid ? "" : " INVALID: ", r.reason.c_str());
      ++done_tr; if (!r.valid) ++invalid_tr;
      if (r.reason == "volume_free_below_6GiB") { halt("volume_free_below_6GiB (cannot recover; run is incomplete)"); break; }
      if (cell.x == 1 && r.valid) { supp_breach_run = (std::string(manip) == "breach") ? supp_breach_run + 1 : 0; if (supp_breach_run >= 3) halt("manipulation_not_working: 3 consecutive suppressed breaches"); }   // counts valid suppressed transfers only
      if (done_tr >= 80 && invalid_tr * 5 > done_tr) halt("more_than_20_percent_invalid_transfers_after_20_blocks");
      if (done_tr >= 20 && gap_dev * 20 > done_tr) halt("gap_deviations_above_5_percent");
    }
  }
  // ---- final ring state (the ring is NOT deleted here) ---------------------------------------------------------------------------------------------------------------------
  { std::ofstream m(out_dir / "manifest_final.json");
    m << "{\"runid\":" << jstr(runid) << ",\"slots\":[";
    for (int j = 0; j < RING; ++j) { const bool ex = fs::exists(slot_path[j]); Meas fm; if (ex) fm = measure_tree(slot_path[j]);
      m << (j ? "," : "") << "{\"slot\":" << j << ",\"path\":" << jstr(slot_path[j].string()) << ",\"exists\":" << (ex ? "true" : "false") << ",\"last_transfer\":" << slot_last[j] << ",\"inode\":" << (unsigned long long)(ex ? inode_of(slot_path[j]) : 0)
        << ",\"files\":" << fm.a.files << ",\"dirs\":" << fm.a.dirs << ",\"non_regular\":" << fm.a.non_regular << ",\"allocated_bytes\":" << fm.a.alloc_bytes << ",\"apparent_bytes\":" << fm.a.apparent_bytes
        << ",\"files_indep\":" << fm.b.files << ",\"allocated_bytes_indep\":" << fm.b.alloc_bytes << ",\"methods_agree\":" << (ex ? (fm.agree() ? "true" : "false") : "true") << "}"; }
    m << "],\"free_end_gib\":" << tp::volume_free_bytes(volume) / 1073741824.0 << "}\n"; }
  // ---- status of every registered safeguard (so that a rule that could not be evaluated is stated, never silently omitted) ---------------------------------------------------
  { std::ofstream m(out_dir / "safeguards_status.json");
    const bool inv_eval = done_tr >= 80, gap_eval = done_tr >= 20;
    m << "{\"transfers_run\":" << done_tr << ",\"rules\":["
      << "{\"rule\":\"hard_checks_ring_state_ports_readback\",\"evaluable\":true,\"note\":\"checked before/after every transfer in every run\"},"
      << "{\"rule\":\"free_space_below_6GiB_halt\",\"evaluable\":true},"
      << "{\"rule\":\"AC_power_each_block\",\"evaluable\":true},"
      << "{\"rule\":\"three_consecutive_suppressed_breaches_halt\",\"evaluable\":true,\"final_run_length\":" << supp_breach_run << "},"
      << "{\"rule\":\"more_than_20_percent_invalid_after_20_blocks\",\"evaluable\":" << (inv_eval ? "true" : "false") << ",\"min_transfers\":80,\"invalid\":" << invalid_tr << ",\"note\":\"NOT EVALUABLE with fewer than 80 transfers (smoke); remains ACTIVE in the registered run\"},"
      << "{\"rule\":\"gap_deviations_above_5_percent_halt\",\"evaluable\":" << (gap_eval ? "true" : "false") << ",\"min_transfers\":20,\"gap_deviations\":" << gap_dev << ",\"note\":\"NOT EVALUABLE with fewer than 20 transfers (the 3-block smoke has 12); remains ACTIVE in the registered run; deviations are still recorded per transfer and reported\"}"
      << "],\"halted\":" << (halted ? "true" : "false") << "}\n"; }
  if (done_tr < 80) ITLOG("NOTE: %d transfers < 80: the invalid-rate halt (min 80 transfers) was not evaluable; %s", done_tr, done_tr < 20 ? "the gap-deviation halt (min 20 transfers) was not evaluable either (both stay active in a registered run)" : "gap-deviation halt evaluable");
  ITLOG("FINISHED: %d of %d transfers run, %d invalid, %d gap deviations%s%s", done_tr, N, invalid_tr, gap_dev, halted ? "; HALTED: " : "", halt_reason.c_str());
  std::ofstream(out_dir / "finished.txt") << "transfers_run " << done_tr << " of " << N << " invalid " << invalid_tr << " gap_deviations " << gap_dev << (halted ? " HALTED " + halt_reason : std::string(" complete")) << "\n";
  return halted ? 6 : 0;
}
