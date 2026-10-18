// tp_realswarm: ONE real-swarm streaming session through the production engine (gate M1-G8 / criterion 2 series).
// usage: tp_realswarm torrent=<file> cache=<dir on the dedicated test volume> volume=<mount> limit_mib=48 secs=240 bps_kib=1024 web=off|assist|always seeks=8
// Reads like a demuxer (tail/head first), sequential paced reads, scripted seeks (forward/backward/random). Libtorrent verifies every piece hash;
// the shadow classifier reports STORAGE vs ARRIVED verdicts with per-block source attribution (web seed vs peer). Auditors are lock-free / O(1).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include "tpcore/audit.hpp"
#include "tpcore/engine.hpp"
#include "tpcore/stream_session.hpp"

using Clock = std::chrono::steady_clock;
static double secs(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
static Clock::time_point T0 = Clock::now();
#define LOGF(...) do { std::printf("[%8.2f] ", secs(Clock::now() - T0)); std::printf(__VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while (0)

int main(int argc, char** argv) {
  std::map<std::string, std::string> a;
  for (int i = 1; i < argc; ++i) { std::string s = argv[i]; auto e = s.find('='); if (e != std::string::npos) a[s.substr(0, e)] = s.substr(e + 1); }
  auto geti = [&](const char* k, long long d) { return a.count(k) ? std::atoll(a[k].c_str()) : d; };
  if (!a.count("torrent") || !a.count("cache") || !a.count("volume")) { std::puts("need torrent= cache= volume="); return 2; }
  const std::int64_t limit = geti("limit_mib", 48) << 20;
  const double secs_total = static_cast<double>(geti("secs", 240)), bps = static_cast<double>(geti("bps_kib", 1024)) * 1024;
  const int nseeks = static_cast<int>(geti("seeks", 8));
  std::filesystem::create_directories(a["cache"]);
  tp::VolumeDeltaAuditor vol(a["volume"], std::chrono::microseconds(2000));
  vol.calibrate_noise(std::chrono::milliseconds(800));

  tp::EngineConfig ec; ec.cache_root = a["cache"]; ec.listen_interfaces = "0.0.0.0:6881,[::]:6881"; ec.min_reconnect_time_s = 1;
  tp::Engine eng(ec);
  tp::TorrentSource src; src.torrent_file = a["torrent"];
  tp::CacheConfig cc; cc.limit_bytes = limit; cc.ahead_seconds_auto = 20; cc.behind_seconds = 30;      // short windows so a 48 MiB cache is exercised hard on a 10-minute movie
  std::string err; auto ss = eng.open_stream(src, -1, cc, &err);
  if (!ss) { LOGF("open_stream failed: %s", err.c_str()); return 2; }
  const std::string web = a.count("web") ? a["web"] : "assist";
  ss->set_web_seed_mode(web == "off" ? tp::WebSeedMode::Off : web == "always" ? tp::WebSeedMode::Always : tp::WebSeedMode::Assist);
  ss->set_bitrate(bps);
  LOGF("streaming '%s' (%.1f MiB, %d pieces); limit %.0f MiB (internal cap %.2f MiB); web-seed mode=%s", ss->file_name().c_str(), ss->file_size() / 1048576.0, ss->num_pieces(), limit / 1048576.0, ss->store().internal_cap() / 1048576.0, web.c_str());
  vol.start();
  tp::DirScanAuditor dir(ss->store(), std::chrono::microseconds(3000));
  dir.start();

  std::vector<char> buf(256 * 1024);
  std::atomic<bool> cancel{false};
  auto timed_read = [&](std::int64_t off, double timeout_s, std::int64_t* got) {
    cancel = false; std::atomic<bool> done{false};
    std::thread killer([&] { auto dl = Clock::now() + std::chrono::duration<double>(timeout_s); while (!done && Clock::now() < dl) std::this_thread::sleep_for(std::chrono::milliseconds(50)); if (!done) cancel = true; });
    auto t = Clock::now(); std::int64_t n = ss->read(off, buf.data(), static_cast<std::int64_t>(buf.size()), &cancel); done = true; killer.join();
    *got = n; return secs(Clock::now() - t);
  };
  std::int64_t n = 0;
  const double t_tail = timed_read(ss->file_size() - 65536, 180, &n);
  const double t_head = timed_read(0, 180, &n);
  LOGF("STARTUP: first container reads done after %.2f s (tail %.2f s, head %.2f s)", secs(Clock::now() - T0), t_tail, t_head);
  const double startup = secs(Clock::now() - T0);
  std::int64_t pos = 65536; std::vector<double> seek_lat; int stuck = 0, seeks_done = 0, stalls = 0; double stall_total = 0;
  std::mt19937 rng(17);
  const auto t_end = Clock::now() + std::chrono::duration<double>(secs_total);
  double next_seek = secs_total / (nseeks + 1);
  const auto t_play0 = Clock::now();
  while (Clock::now() < t_end) {
    if (pos >= ss->file_size()) { pos = 65536; ss->seek_hint(pos); LOGF("end of file reached: replaying from the start (keeps long sessions running)"); }
    if (nseeks > seeks_done && secs(Clock::now() - t_play0) >= next_seek * (seeks_done + 1)) {
      const std::int64_t fsz = ss->file_size(); std::int64_t tgt;
      switch (seeks_done % 4) { case 0: tgt = static_cast<std::int64_t>(fsz * 0.70); break; case 1: tgt = static_cast<std::int64_t>(fsz * 0.05); break; case 2: tgt = std::max<std::int64_t>(0, pos - 20 * 1048576); break; default: tgt = static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(fsz - 1048576)); }
      ss->seek_hint(tgt); const double l = timed_read(tgt, 120, &n);
      if (n <= 0) { ++stuck; LOGF("seek %d to %.1f MiB: STUCK (120 s)", seeks_done + 1, tgt / 1048576.0); } else { seek_lat.push_back(l); pos = tgt + n; LOGF("seek %d to %.1f MiB: first byte after %.2f s", seeks_done + 1, tgt / 1048576.0, l); }
      ++seeks_done; continue;
    }
    const double took = timed_read(pos, 120, &n);
    if (n <= 0) { ++stuck; LOGF("read stuck at %lld", (long long)pos); break; }
    if (took > 0.5) { ++stalls; stall_total += took; }
    pos += n;
    const double budget = static_cast<double>(n) / bps; if (took < budget) std::this_thread::sleep_for(std::chrono::duration<double>(budget - took));
    static double last = 0; if (secs(Clock::now() - T0) - last > 15) { last = secs(Clock::now() - T0); auto st = ss->status(); LOGF("  state=%s peers=%d web_conn=%d down=%.0f kB/s present=%d charged=%.1f/%.1f MiB web=%.1f MiB peers=%.1f MiB resets=%d", tp::to_string(st.state), st.peers, st.web_connections, st.download_rate / 1000, st.store.present, st.store.charged / 1048576.0, st.store.internal_cap / 1048576.0, st.web_bytes / 1048576.0, st.peer_bytes / 1048576.0, st.un_have_resets); }
  }
  auto st = ss->status();
  auto da = dir.stop(); auto va = vol.stop();
  const std::int64_t vpeak = va.peak_usage_noise_corrected();
  LOGF("TRAFFIC: web seeds %.1f MiB, BitTorrent peers %.1f MiB", st.web_bytes / 1048576.0, st.peer_bytes / 1048576.0);
  LOGF("INTEGRITY: pieces verified %d, hash failures %d (ARRIVED-CORRUPT %d, STORAGE-CORRUPTION %d, UNDETERMINED %d), peer bans %d", st.pieces_verified, st.hash_failures, st.hash_arrived, st.hash_storage, st.hash_undetermined, st.peer_bans);
  for (auto& e : ss->recent_events()) if (e.find("HASH FAILED") != std::string::npos || e.find("PEER BAN") != std::string::npos) LOGF("  event %s", e.c_str());
  LOGF("BOUND: limit %.0f MiB | volume auditor peak %.2f MiB (noise %.2f) | dir auditor peak %.2f MiB (accepted %lld/%lld) | max charged %.2f MiB | write-path violations %lld | refused writes %lld",
       limit / 1048576.0, vpeak / 1048576.0, va.noise_floor / 1048576.0, da.max_accepted / 1048576.0, (long long)da.accepted, (long long)da.scans, st.store.max_charged / 1048576.0, (long long)st.store.cap_violations, (long long)st.store.refused_writes);
  LOGF("PLAYBACK: startup %.2f s, stalls %d (%.1f s total), seeks %d (median %.2f s, max %.2f s), stuck %d, un-have resets %d, watchdog kicks %d resets %d, states seen at end: %s",
       startup, stalls, stall_total, seeks_done, seek_lat.empty() ? -1.0 : [&] { auto v = seek_lat; std::sort(v.begin(), v.end()); return v[v.size() / 2]; }(), seek_lat.empty() ? -1.0 : *std::max_element(seek_lat.begin(), seek_lat.end()), stuck, st.un_have_resets, st.connect_kicks, st.watchdog_resets, tp::to_string(st.state));
  const bool ok = vpeak <= limit && da.max_accepted <= limit && st.store.max_charged <= limit && st.store.cap_violations == 0 && st.hash_storage == 0 && stuck == 0;
  LOGF(ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
