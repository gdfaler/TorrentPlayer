// Gate M1-G2: strict cache bound under realistic stress. The LIMIT is the user-visible bound on ACTUAL allocated disk space attributable to cache data.
// Measured three independent ways: (1) the store's internal charged-bytes counters, (2) a lock-free directory auditor (st_blocks, seqlock-style),
// (3) a dedicated-volume FREE-SPACE auditor (statfs on a volume that contains ONLY the cache; includes true allocator overhead and metadata).
// Phases: forward play -> seek storm (backward into evicted data, un-have resets) -> soak -> over-limit crash-style recovery (limit shrinks, audited BEFORE torrent activity) -> resume.
// usage: it_strict_cap limit_mib=64 payload_mib=256 secs=60 seeders=2 fault=none|overalloc3|overalloc10 volume=/Volumes/TPTestVol bps_mib=4 [recovery=1]
#include <map>
#include "loopback_fixture.hpp"
using namespace tpit;

static std::map<std::string, std::string> args(int argc, char** argv) { std::map<std::string, std::string> m; for (int i = 1; i < argc; ++i) { std::string a = argv[i]; auto e = a.find('='); if (e != std::string::npos) m[a.substr(0, e)] = a.substr(e + 1); } return m; }

int main(int argc, char** argv) {
  auto a = args(argc, argv);
  auto geti = [&](const char* k, long long d) { return a.count(k) ? std::atoll(a[k].c_str()) : d; };
  const std::int64_t limit = geti("limit_mib", 64) << 20, payload_sz = geti("payload_mib", 256) << 20;
  const int secs = static_cast<int>(geti("secs", 60)), nseed = static_cast<int>(geti("seeders", 2));
  const double bps = static_cast<double>(geti("bps_mib", 4)) * 1048576;
  const std::string fault = a.count("fault") ? a["fault"] : "none";
  const fs::path volume = a.count("volume") ? a["volume"] : "";
  if (volume.empty() || !fs::exists(volume)) { ITLOG("need volume=<dedicated APFS test volume> (tools/scripts/test_volume.sh attach)"); return 2; }
  const bool informational = a.count("informational") > 0;                // beyond the defined conditions: report, do not gate

  Payload pl = Payload::create(data_dir(), payload_sz, 262144, 21);       // payload lives on the MAIN volume; the cache lives on the dedicated volume
  std::vector<std::unique_ptr<Seeder>> seeders;
  for (int i = 0; i < nseed; ++i) seeders.push_back(std::make_unique<Seeder>(pl, 42101 + i));
  const fs::path cache_root = volume / "cache_strict";
  fs::remove_all(cache_root); fs::create_directories(cache_root);

  tp::CacheConfig cc; cc.limit_bytes = limit;
  if (a.count("fill")) { cc.max_ahead_fraction = 0.60; cc.max_behind_fraction = 0.35; cc.min_ahead_fraction = 0.60; cc.behind_seconds = 3600; cc.ahead_seconds_auto = 600; }   // windows sized to SATURATE the cap (stress the bound, not just the defaults)
  tp::VolumeDeltaAuditor vol(volume, std::chrono::microseconds(1000));
  vol.calibrate_noise(std::chrono::milliseconds(1500));
  ITLOG("%s", a.count("fill") ? "FILL-TO-CAP mode: window fractions raised so the cache saturates the internal cap" : "default window fractions");
  ITLOG("config: limit %.0f MiB (internal cap %.2f MiB), payload %.0f MiB, %d seeders, fault=%s, bitrate %.0f MiB/s, volume noise floor %lld B",
        limit / 1048576.0, tp::internal_cap(cc, 262144) / 1048576.0, payload_sz / 1048576.0, nseed, fault.c_str(), bps / 1048576, (long long)vol.stop().noise_floor);
  tp::VolumeDeltaAuditor vol2(volume, std::chrono::microseconds(1000));
  vol2.calibrate_noise(std::chrono::milliseconds(500));

  bool ok = true;
  std::int64_t worst_dir = 0, worst_vol = 0, margin_used = 0; double dir_acc = 1; int total_mismatch = 0, total_stuck = 0; long long total_viol = 0, total_storage = 0, refused = 0;
  std::int64_t sat_peak = 0;
  auto run_phase_session = [&](const tp::CacheConfig& cfg, bool start_paused, double play_secs, bool seekstorm, const char* label, bool* paused_ok, double pace_bps = 0) {
    tp::Engine eng(engine_config(cache_root, 42001));
    tp::TorrentSource src; src.torrent_bytes = pl.torrent_bytes; src.strip_web_seeds = true; src.strip_trackers = true; src.start_paused = start_paused;
    for (auto& s : seeders) src.extra_peers.push_back(s->endpoint());
    std::string err;
    auto ss = eng.open_stream(src, 0, cfg, &err);
    if (!ss) { ITLOG("open_stream failed: %s", err.c_str()); ok = false; return; }
    if (fault == "overalloc3") ss->storage().inject_overalloc_pct = 3;
    if (fault == "overalloc10") ss->storage().inject_overalloc_pct = 10;
    ss->set_bitrate(bps);
    auto rec = ss->recovery_report();
    ITLOG("[%s] recovery: verified=%d deleted_partial=%d deleted_corrupt=%d trimmed=%d charged_after=%.2f MiB (limit %.0f MiB, internal cap %.2f MiB)", label, rec.verified_present, rec.deleted_partial, rec.deleted_corrupt, rec.trimmed, rec.charged_after / 1048576.0, cfg.limit_bytes / 1048576.0, tp::internal_cap(cfg, 262144) / 1048576.0);
    if (start_paused) {
      const std::int64_t on_disk = tp::measure_allocated_bytes(ss->store().params().dir);
      ITLOG("[%s] BEFORE any torrent activity: allocated on disk %.2f MiB vs limit %.0f MiB => %s", label, on_disk / 1048576.0, cfg.limit_bytes / 1048576.0, on_disk <= cfg.limit_bytes ? "within limit" : "OVER LIMIT");
      if (on_disk > cfg.limit_bytes) { ok = false; if (paused_ok) *paused_ok = false; }
      ss->resume();
    }
    tp::DirScanAuditor dir(ss->store(), std::chrono::microseconds(1500));
    dir.start();
    Player player(*ss, pl, pace_bps > 0 ? pace_bps : bps);
    player.open_like_demuxer();
    std::mt19937 rng(77);
    const auto t_end = Clock::now() + std::chrono::duration<double>(play_secs);
    int n = 0;
    while (Clock::now() < t_end) {
      if (seekstorm) {
        const std::int64_t tgt = (n % 3 == 0) ? std::max<std::int64_t>(0, player.position() - (pl.size / 3)) : static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(pl.size - 1048576));
        const double l = player.seek(tgt, 90.0);
        if (l < 0) { ++total_stuck; ITLOG("[%s] seek %d STUCK", label, n); }
        if (!player.play(1.5, 60.0)) { ++total_stuck; ITLOG("[%s] playback stuck after seek %d", label, n); }
      } else if (!player.play(2.0, 60.0)) { ++total_stuck; ITLOG("[%s] playback stuck", label); break; }
      ++n;
    }
    auto st = ss->status();
    auto da = dir.stop();
    worst_dir = std::max(worst_dir, da.max_accepted); dir_acc = std::min(dir_acc, da.acceptance());
    total_mismatch += player.st.mismatches; total_viol += st.store.cap_violations; total_storage += st.hash_storage; refused += st.store.refused_writes;
    ITLOG("[%s] done: reads verified %.0f MiB, mismatches %d, stalls %d (max %.2fs), seeks %d (median %.3fs p90 %.3fs), resets %d, forced evictions %lld, sweeps %lld, reconcile corrections %lld, refused writes %lld, hash failures %d (storage %d)",
          label, player.st.bytes / 1048576.0, player.st.mismatches, player.st.stalls, player.st.max_stall, player.st.seeks, percentile(player.st.seek_latency, .5), percentile(player.st.seek_latency, .9), st.un_have_resets,
          (long long)st.store.forced_evictions, (long long)st.store.sweep_evictions, (long long)st.store.reconcile_corrections, (long long)st.store.refused_writes, st.hash_failures, st.hash_storage);
    ITLOG("[%s] bound: max charged %.2f MiB / internal cap %.2f MiB; dir auditor peak %.2f MiB (accepted %lld of %lld scans); violations %lld", label, st.store.max_charged / 1048576.0, st.store.internal_cap / 1048576.0, da.max_accepted / 1048576.0, (long long)da.accepted, (long long)da.scans, (long long)st.store.cap_violations);
    margin_used = std::max<std::int64_t>(margin_used, st.store.max_charged - st.store.internal_cap);
    sat_peak = std::max<std::int64_t>(sat_peak, st.store.max_charged);
    // GATE: accounting never exceeds the user-visible LIMIT; no write ever pushed accounting over the internal cap (write-path reservation); the independent directory auditor stays under the LIMIT.
    if (st.store.max_charged > cfg.limit_bytes || st.store.cap_violations > 0 || da.max_accepted > cfg.limit_bytes) ok = false;
  };

  vol2.start();                                                            // volume auditor covers every phase, including recovery
  run_phase_session(cc, false, secs * 0.25, false, "forward", nullptr, a.count("fill") ? 256.0 * 1048576 : 0);
  run_phase_session(cc, false, secs * 0.35, true, "seek-storm", nullptr);
  run_phase_session(cc, false, secs * 0.40, true, "soak", nullptr);
  // ---- over-limit recovery: the user lowers the limit to 60 % between runs; the cache directory still holds the old, larger content
  tp::CacheConfig smaller = cc; smaller.limit_bytes = static_cast<std::int64_t>(static_cast<double>(limit) * 0.60);
  bool paused_ok = true;
  run_phase_session(smaller, true, std::min(20.0, secs * 0.25), true, "recovery(limit x0.6)", &paused_ok);
  auto v = vol2.stop();
  worst_vol = v.peak_usage();
  const std::int64_t vol_peak_corrected = v.peak_usage_noise_corrected();
  ITLOG("VOLUME FREE-SPACE AUDITOR (dedicated volume, 1 ms): peak usage %.2f MiB (noise floor %.2f MiB, corrected %.2f MiB) vs limit %.0f MiB => %s",
        worst_vol / 1048576.0, v.noise_floor / 1048576.0, vol_peak_corrected / 1048576.0, limit / 1048576.0, vol_peak_corrected <= limit ? "within limit" : "OVER LIMIT");
  ITLOG("margin usage: accounting rose at most %.2f MiB above the internal cap (reconciliation of measured allocation); the user-visible limit has %.2f MiB of margin", margin_used / 1048576.0, (limit - tp::internal_cap(cc, 262144)) / 1048576.0);
  ITLOG("SUMMARY fault=%s limit=%.0f MiB: dir-auditor worst %.2f MiB (min acceptance %.0f%%), volume-auditor worst %.2f MiB, cap violations %lld, refused writes %lld, data mismatches %d, stuck %d, storage-corruption verdicts %lld",
        fault.c_str(), limit / 1048576.0, worst_dir / 1048576.0, dir_acc * 100, vol_peak_corrected / 1048576.0, total_viol, refused, total_mismatch, total_stuck, total_storage);
  const double saturation = static_cast<double>(sat_peak) / static_cast<double>(tp::internal_cap(cc, 262144));
  ITLOG("SATURATION: cache accounting peaked at %.1f %% of the internal cap (%.0f of %.0f MiB); volume peak %.1f %% of the limit", saturation * 100, sat_peak / 1048576.0, tp::internal_cap(cc, 262144) / 1048576.0, 100.0 * static_cast<double>(vol_peak_corrected) / static_cast<double>(limit));
  if (a.count("fill") && saturation < 0.90) { ITLOG("NOT SATURATED: this run did not stress the bound (needs >= 90 %% of the internal cap); it cannot count toward the gate"); ok = false; }
  if (vol_peak_corrected > limit) ok = false;
  if (total_mismatch || total_stuck || total_storage) ok = false;
  if (informational) { ITLOG("INFORMATIONAL RUN (beyond defined conditions): result %s", ok ? "within limit" : "limit exceeded"); return 0; }
  ITLOG(ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
