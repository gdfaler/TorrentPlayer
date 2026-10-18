#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <random>
#include <thread>
#include <sstream>
#include "tp_test.hpp"
#include "tpcore/config.hpp"
#include "tpcore/audit.hpp"
#include "tpcore/diagnostics.hpp"
using namespace tp;
namespace fs = std::filesystem;

static const std::int64_t PS = 64 * KiB;
static fs::path tmp(const char* n) { fs::path p = fs::temp_directory_path() / ("tp_ut_" + std::string(n) + "_" + std::to_string(::getpid())); fs::remove_all(p); fs::create_directories(p); return p; }
static std::vector<char> rnd(int seed, std::size_t n) { std::vector<char> v(n); std::mt19937 r(seed); for (auto& c : v) c = static_cast<char>(r()); return v; }
static void feed(ShadowVerifier& v, int piece, const std::vector<char>& b) { for (int off = 0; off < PS; off += 16384) v.on_write(piece, off, b.data() + off, 16384); }
static std::string slurp(const fs::path& p) { std::ifstream f(p); std::stringstream s; s << f.rdbuf(); return s.str(); }

TP_TEST(classifier_storage_corruption) {
  auto d = tmp("diag1");
  ShadowVerifier v(PS, 10, PS * 10, d);
  auto good = rnd(1, PS); feed(v, 3, good);
  auto disk = good; disk[5000] ^= 0x33;                                  // storage flipped a byte after the write
  auto exp = sha1_of(good.data(), good.size());
  TP_CHECK(v.on_failure(3, disk, exp) == Verdict::StorageCorruption);
  TP_CHECK_EQ(v.counters().storage.load(), 1);
  fs::remove_all(d);
}

TP_TEST(classifier_arrived_corruption_and_evidence) {
  auto d = tmp("diag2");
  ShadowVerifier v(PS, 10, PS * 10, d);
  auto good = rnd(2, PS), bad = good; bad[40000] ^= 0x5a;                // the data was already wrong when handed to us
  feed(v, 4, bad);
  v.note_block_source(4, 2, "10.0.0.5:6881", false);                      // block 2 (offset 32768..49151) contains byte 40000
  v.note_block_source(4, 0, "10.0.0.9:6881", false);
  v.note_block_source(4, 1, "93.184.216.34:443", true);
  v.note_block_source(4, 3, "10.0.0.9:6881", false);
  auto exp = sha1_of(good.data(), good.size());
  TP_CHECK(v.on_failure(4, bad, exp) == Verdict::ArrivedCorrupt);          // disk == received, hash wrong
  v.attach_sources_to_failure(4);
  TP_CHECK(v.attribution(4).find("(WEB SEED)") != std::string::npos);
  // evidence retained: received bytes, disk bytes, text with expected hash + per-block hashes
  int bins = 0, txts = 0; for (auto& e : fs::directory_iterator(d)) { auto n = e.path().string(); if (n.find(".received.bin") != std::string::npos) ++bins; if (n.size() > 4 && n.substr(n.size() - 4) == ".txt") ++txts; }
  TP_CHECK(bins == 1 && txts == 1);
  // the piece is later re-downloaded correctly: the diff names the corrupt block and who delivered the FAILED copy of it
  v.on_pass(4, good);
  std::string diff;
  for (auto& e : fs::directory_iterator(d)) if (e.path().string().find(".diff.txt") != std::string::npos) diff = slurp(e.path());
  TP_CHECK(diff.find("differ from the good copy: 2") != std::string::npos);
  TP_CHECK(diff.find("failed-copy block 2 delivered by 10.0.0.5:6881") != std::string::npos);
  fs::remove_all(d);
}

TP_TEST(classifier_undetermined_when_shadow_incomplete) {
  auto d = tmp("diag3");
  ShadowVerifier v(PS, 10, PS * 10, d);
  auto b = rnd(3, PS);
  v.on_write(6, 0, b.data(), 16384);                                       // only one block was seen in this session
  TP_CHECK(v.on_failure(6, b, sha1_of(b.data(), 10)) == Verdict::Undetermined);
  fs::remove_all(d);
}

TP_TEST(classifier_no_false_positive_on_clean_pieces) {
  auto d = tmp("diag4");
  ShadowVerifier v(PS, 10, PS * 10, d);
  for (int p = 0; p < 10; ++p) { auto b = rnd(100 + p, PS); feed(v, p, b); v.on_pass(p, b); }
  TP_CHECK_EQ(v.counters().failures.load(), 0);
  TP_CHECK_EQ(v.counters().verified_ok.load(), 10);
  fs::remove_all(d);
}

TP_TEST(dir_auditor_accepts_scans_on_an_idle_store_and_sees_the_peak) {
  auto d = tmp("aud1");
  StoreParams sp; sp.dir = d / "c"; sp.piece_size = PS; sp.num_pieces = 20; sp.total_size = PS * 20;
  SegmentStore s(sp);
  DirScanAuditor a(s, std::chrono::microseconds(500));
  a.start();
  for (int p = 0; p < 10; ++p) { auto b = rnd(p, PS); for (int off = 0; off < PS; off += 16384) s.write(p, off, b.data() + off, 16384); s.mark_verified(p); }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  auto r = a.stop();
  TP_CHECK(r.accepted > 0);
  TP_CHECK(r.max_accepted >= 10 * PS);                                       // idle phase: a consistent scan saw all ten pieces
  TP_CHECK(r.max_accepted <= 10 * PS + 64 * KiB);
  fs::remove_all(d);
}

TP_TEST(volume_auditor_detects_real_allocation) {
  auto d = tmp("aud2");
  VolumeDeltaAuditor v(d, std::chrono::microseconds(1000));
  v.calibrate_noise(std::chrono::milliseconds(500));
  v.start();
  { std::ofstream f(d / "big.bin", std::ios::binary); auto b = rnd(7, 1 * MiB); for (int i = 0; i < 64; ++i) f.write(b.data(), static_cast<std::streamsize>(b.size())); f.flush(); }
  int fd = ::open((d / "big.bin").c_str(), O_RDONLY); ::fsync(fd); ::close(fd);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  auto r = v.stop();
  TP_CHECK(r.samples > 50);
  TP_CHECK(r.peak_usage() >= 60 * MiB);                                      // a 64 MiB write is visible as a free-space drop (>= 60 MiB despite noise)
  TP_CHECK(r.peak_usage() <= 64 * MiB + 64 * MiB);                           // and not wildly more (other processes may add noise)
  fs::remove_all(d);
}

TP_MAIN()
