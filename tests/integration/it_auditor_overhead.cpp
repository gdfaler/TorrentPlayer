// Gate M1-G3: the independent auditors must not materially interfere with libtorrent / the engine. Alternates sessions WITHOUT and WITH both auditors
// (lock-free directory scan + 1 ms volume free-space sampler) and compares time-to-first-byte and sequential throughput. Pass: with-auditor results within
// 10 % (+ small absolute allowance) of without. Controls first: this is exactly the check that caught a perturbing "heavy auditor" in the research harness.
// usage: it_auditor_overhead volume=/Volumes/TPTestVol [rounds=5]
#include <map>
#include "loopback_fixture.hpp"
using namespace tpit;
static std::map<std::string, std::string> args(int argc, char** argv) { std::map<std::string, std::string> m; for (int i = 1; i < argc; ++i) { std::string a = argv[i]; auto e = a.find('='); if (e != std::string::npos) m[a.substr(0, e)] = a.substr(e + 1); } return m; }

struct Run { double ttfb, mibps; };
static Run one(const Payload& pl, const fs::path& volume, bool with_auditors, int round) {
  Seeder s1(pl, 42501), s2(pl, 42502);
  auto root = volume / ("overhead_" + std::to_string(round) + (with_auditors ? "_aud" : "_noaud")); fs::remove_all(root); fs::create_directories(root);
  tp::Engine eng(engine_config(root, 42003));
  tp::TorrentSource src; src.torrent_bytes = pl.torrent_bytes; src.strip_web_seeds = true; src.strip_trackers = true; src.extra_peers = {s1.endpoint(), s2.endpoint()};
  tp::CacheConfig cc; cc.limit_bytes = 128LL << 20;
  const auto t0 = Clock::now();
  std::string err; auto ss = eng.open_stream(src, 0, cc, &err);
  std::unique_ptr<tp::VolumeDeltaAuditor> va; std::unique_ptr<tp::DirScanAuditor> da;
  if (with_auditors) { va = std::make_unique<tp::VolumeDeltaAuditor>(volume, std::chrono::microseconds(1000)); va->start(); da = std::make_unique<tp::DirScanAuditor>(ss->store(), std::chrono::microseconds(1500)); da->start(); }
  std::vector<char> buf(256 * 1024);
  ss->read(pl.size - 65536, buf.data(), 65536, nullptr);
  const double ttfb = secs(Clock::now() - t0);
  const auto t1 = Clock::now(); std::int64_t off = 0; const std::int64_t want = 160LL << 20;
  while (off < want) { std::int64_t n = ss->read(off, buf.data(), static_cast<std::int64_t>(buf.size()), nullptr); if (n <= 0) break; off += n; }
  const double dt = secs(Clock::now() - t1);
  if (va) va->stop(); if (da) da->stop();
  return {ttfb, (static_cast<double>(off) / 1048576.0) / dt};
}

int main(int argc, char** argv) {
  auto a = args(argc, argv);
  const fs::path volume = a.count("volume") ? a["volume"] : ""; if (volume.empty()) return 2;
  const int rounds = a.count("rounds") ? std::atoi(a["rounds"].c_str()) : 5;
  Payload pl = Payload::create(data_dir(), 256LL << 20, 262144, 55);
  std::vector<double> t_no, t_yes, m_no, m_yes;
  for (int r = 0; r < rounds; ++r) {
    auto n = one(pl, volume, false, r), y = one(pl, volume, true, r);
    t_no.push_back(n.ttfb); t_yes.push_back(y.ttfb); m_no.push_back(n.mibps); m_yes.push_back(y.mibps);
    ITLOG("round %d: without auditors ttfb %.3fs, %.0f MiB/s | with auditors ttfb %.3fs, %.0f MiB/s", r + 1, n.ttfb, n.mibps, y.ttfb, y.mibps);
  }
  auto mean = [](const std::vector<double>& v) { double s = 0; for (double x : v) s += x; return s / static_cast<double>(v.size()); };
  const double tn = mean(t_no), ty = mean(t_yes), mn = mean(m_no), my = mean(m_yes);
  ITLOG("RESULT means: ttfb without %.3fs with %.3fs | throughput without %.0f MiB/s with %.0f MiB/s (%.1f %% of baseline)", tn, ty, mn, my, 100.0 * my / mn);
  const bool ok = ty <= tn * 1.10 + 0.30 && my >= mn * 0.90;
  ITLOG(ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
