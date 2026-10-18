#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <random>
#include <thread>
#include <fstream>
#include "tp_test.hpp"
#include "tpcore/config.hpp"
#include "tpcore/segment_store.hpp"
using namespace tp;
namespace fs = std::filesystem;

static const std::int64_t PS = 64 * KiB;
static fs::path tmpdir(const char* name) { fs::path p = fs::temp_directory_path() / ("tp_ut_" + std::string(name) + "_" + std::to_string(::getpid())); fs::remove_all(p); return p; }
static StoreParams params(const fs::path& d, int np, std::int64_t limit, std::int64_t margin) { StoreParams p; p.dir = d; p.piece_size = PS; p.num_pieces = np; p.total_size = PS * np; p.limit_bytes = limit; p.margin_bytes = margin; return p; }
static std::vector<char> piece_bytes(int p) { std::vector<char> v(PS); std::mt19937 r(static_cast<unsigned>(p) * 7919u + 1); for (auto& c : v) c = static_cast<char>(r()); return v; }
static bool put_piece(SegmentStore& s, int p, bool verify = true) {
  auto b = piece_bytes(p);
  for (int off = 0; off < PS; off += 16384) { if (s.write(p, off, b.data() + off, 16384) != WriteStatus::Ok) return false; }
  if (verify) s.mark_verified(p);
  return true;
}
static Protection prot(int head, int lo, int hi) { Protection p; p.head = head; p.lo = lo; p.hi = hi; return p; }

TP_TEST(reservation_never_exceeds_internal_cap_while_filling) {
  auto d = tmpdir("res");
  SegmentStore s(params(d, 100, 40 * PS, 4 * PS));              // cap = 36 pieces
  s.set_protection(prot(50, 45, 60));
  for (int p = 0; p < 100; ++p) {
    TP_CHECK(put_piece(s, p));
    TP_CHECK(s.charged() <= s.internal_cap());
  }
  auto m = s.metrics();
  TP_CHECK(m.max_charged <= s.internal_cap());
  TP_CHECK_EQ(m.cap_violations, 0);
  for (int p = 50; p < 60; ++p) TP_CHECK(s.has(p));              // protected ahead window survived the pressure
  TP_CHECK(m.forced_evictions > 0);
  TP_CHECK_EQ(m.present, static_cast<int>(s.present_pieces().size()));
  fs::remove_all(d);
}

TP_TEST(write_is_refused_not_executed_when_nothing_is_evictable) {
  auto d = tmpdir("refuse");
  SegmentStore s(params(d, 100, 24 * PS, 4 * PS));              // cap = 20 pieces
  s.set_protection(prot(10, 10, 30));                             // protected window = exactly 20 pieces
  for (int p = 10; p < 30; ++p) TP_CHECK(put_piece(s, p));
  TP_CHECK_EQ(s.charged(), 20 * PS);
  auto b = piece_bytes(70);
  TP_CHECK(s.write(70, 0, b.data(), 16384) == WriteStatus::NoSpace);   // would exceed the cap and nothing may be evicted
  TP_CHECK(!fs::exists(s.piece_path(70)));                              // and nothing was written to disk
  TP_CHECK_EQ(s.charged(), 20 * PS);
  TP_CHECK_EQ(s.metrics().refused_writes, 1);
  for (int p = 10; p < 30; ++p) TP_CHECK(s.has(p));
  fs::remove_all(d);
}

TP_TEST(victim_policy_order_and_exclusions) {
  auto d = tmpdir("victim");
  SegmentStore s(params(d, 200, 0, 0));
  Protection p = prot(100, 90, 110);                              // ahead [100,110) protected, behind [90,100)
  p.pin_a_first = 0; p.pin_a_last = 1; p.pin_b_first = 198; p.pin_b_last = 199;
  s.set_protection(p);
  for (int q : {0, 1, 5, 20, 95, 99, 100, 105, 109, 150, 190, 198, 199}) put_piece(s, q);
  // outside-window pieces first, farthest from the playhead first: 5(95 away), 190(90 away), 20(80), 150(50), then behind (oldest first): 95, 99
  std::vector<int> got;
  for (int i = 0; i < 6; ++i) { int v = s.pick_victim_for_test(-1); got.push_back(v); s.evict(v); }
  TP_CHECK(got == (std::vector<int>{5, 190, 20, 150, 95, 99}));
  TP_CHECK_EQ(s.pick_victim_for_test(-1), -1);                    // only pinned (0,1,198,199) and ahead-window pieces remain: never victims
  TP_CHECK(s.has(0) && s.has(1) && s.has(198) && s.has(199) && s.has(100) && s.has(105) && s.has(109));
  put_piece(s, 50);
  TP_CHECK_EQ(s.pick_victim_for_test(50), -1);                    // `keep` is honoured
  fs::remove_all(d);
}

TP_TEST(partial_piece_is_charged_a_full_slot_from_the_first_byte) {
  auto d = tmpdir("slot");
  SegmentStore s(params(d, 10, 0, 0));
  auto b = piece_bytes(3);
  TP_CHECK(s.write(3, 16384, b.data(), 16384) == WriteStatus::Ok);
  TP_CHECK_EQ(s.charged(), PS);
  TP_CHECK(!s.has(3));
  TP_CHECK(s.write(3, 0, b.data(), 16384) == WriteStatus::Ok);
  TP_CHECK_EQ(s.charged(), PS);                                   // second block of the same piece costs nothing extra
  TP_CHECK_EQ(s.metrics().partial_files, 1);
  fs::remove_all(d);
}

TP_TEST(purge_partials_keeps_verified_pieces) {
  auto d = tmpdir("purge");
  SegmentStore s(params(d, 10, 0, 0));
  put_piece(s, 1); put_piece(s, 2, false); auto b = piece_bytes(4); s.write(4, 0, b.data(), 16384);
  TP_CHECK_EQ(s.purge_partials(), 2);                              // pieces 2 (unverified) and 4 (partial)
  TP_CHECK(s.has(1)); TP_CHECK(!fs::exists(s.piece_path(2))); TP_CHECK(!fs::exists(s.piece_path(4)));
  TP_CHECK_EQ(s.charged(), PS);
  fs::remove_all(d);
}

static void make_crashed_cache(const fs::path& d) {
  fs::create_directories(d);
  SegmentStore w(params(d, 100, 0, 0));
  for (int p : {10, 20, 30, 40, 50, 60, 70, 80}) put_piece(w, p);          // 8 good complete pieces
  auto b = piece_bytes(5); w.write(5, 0, b.data(), 16384);                   // partial file (crash mid-download)
  int fd = ::open(w.piece_path(15).c_str(), O_CREAT | O_RDWR, 0644); ::pwrite(fd, b.data(), 16384, PS - 16384); ::close(fd);   // complete-SIZED but holey/corrupt
  std::ofstream(d / "notes.txt") << "foreign";                               // foreign file in the cache dir
}
static bool verify_fn(int p, const Sha1& h) { auto b = piece_bytes(p); return h == sha1_of(b.data(), b.size()); }

TP_TEST(recovery_trims_first_then_verifies_and_never_charges_above_the_cap) {
  auto d = tmpdir("recover");
  make_crashed_cache(d);
  SegmentStore s(params(d, 100, 6 * PS, 1 * PS));                          // new limit: cap = 5 pieces (the limit shrank since the last run)
  auto rep = s.recover(verify_fn, 60);
  TP_CHECK_EQ(rep.deleted_partial, 1);
  TP_CHECK_EQ(rep.deleted_foreign, 1);
  TP_CHECK_EQ(rep.verified_present, 5);
  TP_CHECK_EQ(rep.trimmed, 4);                                              // 3 good pieces farthest from the hint + the far corrupt file: all removed BEFORE hashing
  TP_CHECK(s.charged() <= s.internal_cap());
  TP_CHECK_EQ(s.metrics().max_charged, 5 * PS);                             // accounting never exceeded the cap, not even transiently
  TP_CHECK_EQ(s.metrics().cap_violations, 0);
  for (int p : {40, 50, 60, 70, 80}) TP_CHECK(s.has(p));                   // kept the pieces closest to the head hint
  TP_CHECK(!fs::exists(d / "notes.txt"));
  fs::remove_all(d);
}

TP_TEST(recovery_detects_corrupt_complete_sized_files_when_the_cache_is_roomy) {
  auto d = tmpdir("recover2");
  make_crashed_cache(d);
  SegmentStore s(params(d, 100, 90 * PS, 1 * PS));
  auto rep = s.recover(verify_fn, 60);
  TP_CHECK_EQ(rep.verified_present, 8);
  TP_CHECK_EQ(rep.deleted_corrupt, 1);                                      // piece 15: right size, wrong content => deleted, never trusted
  TP_CHECK_EQ(rep.deleted_partial, 1);
  TP_CHECK_EQ(rep.trimmed, 0);
  TP_CHECK(!fs::exists(s.piece_path(15)));
  fs::remove_all(d);
}

TP_TEST(reconcile_charges_measured_allocation_and_evicts_when_over_cap) {
  auto d = tmpdir("recon");
  SegmentStore s(params(d, 40, 30 * PS, 2 * PS));                          // cap = 28 pieces
  s.set_protection(prot(35, 30, 40));
  for (int p = 0; p < 28; ++p) put_piece(s, p);
  TP_CHECK_EQ(s.charged(), 28 * PS);
  // model APFS allocation beyond the logical size: preallocate beyond EOF for every file
  for (int p = 0; p < 28; ++p) {
    int fd = ::open(s.piece_path(p).c_str(), O_RDWR); fstore_t f = {F_ALLOCATEALL, F_PEOFPOSMODE, 0, 49152, 0}; ::fcntl(fd, F_PREALLOCATE, &f); ::close(fd);
  }
  struct stat st; ::stat(s.piece_path(0).c_str(), &st);
  TP_CHECK(static_cast<std::int64_t>(st.st_blocks) * 512 > PS);            // the filesystem really allocated more than the logical size
  int fixed = s.reconcile_step(200);
  TP_CHECK(fixed > 0);
  TP_CHECK(s.metrics().reconcile_corrections > 0);
  TP_CHECK(s.charged() <= s.internal_cap());                               // corrected accounting pushed the store over the cap => it evicted
  TP_CHECK(s.metrics().forced_evictions > 0);
  fs::remove_all(d);
}

TP_TEST(sweep_honours_touch_guard) {
  auto d = tmpdir("sweep");
  SegmentStore s(params(d, 60, 0, 0));
  s.set_protection(prot(30, 25, 40));
  for (int p : {5, 10, 28, 35, 50}) put_piece(s, p);
  TP_CHECK_EQ(s.sweep_outside_window(std::chrono::seconds(30), 2), 0);      // everything was just touched: guard protects all
  TP_CHECK_EQ(s.sweep_outside_window(std::chrono::nanoseconds(0), 2), 3);   // 5, 10, 50 are outside [25, 42)
  TP_CHECK(s.has(28) && s.has(35) && !s.has(5) && !s.has(10) && !s.has(50));
  fs::remove_all(d);
}

TP_TEST(epoch_changes_on_every_mutation_for_lock_free_auditors) {
  auto d = tmpdir("epoch");
  SegmentStore s(params(d, 10, 0, 0));
  auto e0 = s.mutation_epoch();
  put_piece(s, 1); auto e1 = s.mutation_epoch(); TP_CHECK(e1 > e0);
  s.evict(1); auto e2 = s.mutation_epoch(); TP_CHECK(e2 > e1);
  s.evict(1);                                                               // evicting an absent piece must be safe
  TP_CHECK(s.read(1, 0, nullptr, 0) == -1);
  fs::remove_all(d);
}

TP_TEST(concurrent_writers_and_evictors_keep_the_invariant) {
  auto d = tmpdir("conc");
  SegmentStore s(params(d, 200, 60 * PS, 6 * PS));                          // cap 54
  s.set_protection(prot(100, 90, 110));
  std::atomic<bool> stop{false};
  std::atomic<std::int64_t> worst{0};
  std::thread mon([&] { while (!stop) { std::int64_t c = s.charged(); std::int64_t w = worst.load(); while (c > w && !worst.compare_exchange_weak(w, c)) {} } });
  auto writer = [&](int seed) { std::mt19937 r(seed); for (int i = 0; i < 400; ++i) { int p = static_cast<int>(r() % 200); auto b = piece_bytes(p); for (int off = 0; off < PS; off += 16384) s.write(p, off, b.data() + off, 16384); if (r() % 2) s.mark_verified(p); } };
  auto evictor = [&] { std::mt19937 r(99); for (int i = 0; i < 600; ++i) { s.evict(static_cast<int>(r() % 200)); s.sweep_outside_window(std::chrono::nanoseconds(0), 2); s.reconcile_step(32); } };
  std::thread w1(writer, 1), w2(writer, 2), e1(evictor);
  w1.join(); w2.join(); e1.join(); stop = true; mon.join();
  TP_CHECK(worst.load() <= s.internal_cap());
  TP_CHECK_EQ(s.metrics().cap_violations, 0);
  // accounting equals the files really on disk (slots): every charged piece has a file
  int files = 0; for (auto& e : fs::directory_iterator(d)) { (void)e; ++files; }
  TP_CHECK_EQ(static_cast<std::int64_t>(files) * PS, s.charged());
  fs::remove_all(d);
}

TP_MAIN()
