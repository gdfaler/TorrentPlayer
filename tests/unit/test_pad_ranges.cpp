// BEP 47 padding handling at the verification boundary: padding ranges are implicit zeros (never downloaded or stored), real data must be
// present and correct, and ordinary torrents are unaffected. Includes the structure of the reported incident (a 2 MiB piece whose first
// 725,079 bytes are the end of a video file and whose remaining 1,372,073 bytes are a padding file).
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <fstream>
#include <random>
#include "tp_test.hpp"
#include "tpcore/config.hpp"
#include "tpcore/pad_ranges.hpp"
#include "tpcore/segment_store.hpp"
#include "tpcore/window_planner.hpp"
using namespace tp;
namespace fs = std::filesystem;

static fs::path tmpdir(const char* name) { fs::path p = fs::temp_directory_path() / ("tp_ut_pad_" + std::string(name) + "_" + std::to_string(::getpid())); fs::remove_all(p); return p; }
static std::vector<char> rnd(std::size_t n, unsigned seed) { std::vector<char> v(n); std::mt19937 r(seed); for (auto& c : v) c = static_cast<char>(r()); return v; }
static std::int64_t file_size_of(const fs::path& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 ? static_cast<std::int64_t>(st.st_size) : -1; }
static bool eq(const std::vector<PadRange>& a, const std::vector<PadRange>& b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) if (a[i].start != b[i].start || a[i].len != b[i].len) return false;
  return true;
}

// The logical piece a correct client would hash: real bytes with zeros in the padding ranges.
static std::vector<char> logical(std::int64_t plen, const std::vector<PadRange>& pads, const std::vector<char>& real_stream) {
  std::vector<char> v(static_cast<std::size_t>(plen), 0);
  std::size_t k = 0;
  for (const auto& r : real_ranges(plen, pads)) for (std::int64_t i = 0; i < r.len; ++i) v[static_cast<std::size_t>(r.start + i)] = real_stream[k++];
  return v;
}
static std::int64_t real_total(std::int64_t plen, const std::vector<PadRange>& pads) { std::int64_t t = 0; for (const auto& r : real_ranges(plen, pads)) t += r.len; return t; }

// Write the real data of one piece in 16 KiB blocks the way libtorrent hands them over: block boundaries follow the piece, but nothing is
// written for blocks that are entirely padding, and a block that straddles real data and padding carries only its real bytes.
static void write_real(SegmentStore& s, int piece, std::int64_t plen, const std::vector<PadRange>& pads, const std::vector<char>& logical_bytes, int skip_block = -1) {
  const int blocks = blocks_in_piece(plen);
  for (int b = 0; b < blocks; ++b) {
    if (b == skip_block) continue;
    const std::int64_t bs = static_cast<std::int64_t>(b) * 16384, be = std::min<std::int64_t>(bs + 16384, plen);
    for (const auto& r : real_ranges(plen, pads)) {
      const std::int64_t a = std::max(bs, r.start), e = std::min(be, r.start + r.len);
      if (e > a) TP_CHECK(s.write(piece, static_cast<int>(a), logical_bytes.data() + a, static_cast<int>(e - a)) == WriteStatus::Ok);
    }
  }
}

// ---------------------------------------------------------------- pure geometry
TP_TEST(build_piece_pads_splits_clips_and_merges) {
  // pad file spanning pieces 1..3 (piece size 100): torrent bytes 150..330
  auto v = build_piece_pads({{150, 180}}, 100, 400, 4);
  TP_CHECK_EQ(v.size(), 4u);
  TP_CHECK(v[0].empty());
  TP_CHECK(eq(v[1], {{50, 50}})); TP_CHECK(eq(v[2], {{0, 100}})); TP_CHECK(eq(v[3], {{0, 30}}));
  // two pad files in the same piece, adjacent ones merge, ones past the end of the torrent are clipped
  auto w = build_piece_pads({{10, 20}, {30, 10}, {70, 50}}, 100, 100, 1);
  TP_CHECK(eq(w[0], {{10, 30}, {70, 30}}));
  TP_CHECK(build_piece_pads({}, 100, 400, 4).empty());                               // ordinary torrent: no per-piece data at all
}

TP_TEST(real_ranges_and_block_helpers) {
  std::vector<PadRange> pads{{10, 20}, {50, 10}};
  TP_CHECK(eq(real_ranges(100, pads), {{0, 10}, {30, 20}, {60, 40}}));
  TP_CHECK(eq(real_ranges(100, {}), {{0, 100}}));
  TP_CHECK(real_ranges(100, {{0, 100}}).empty());
  TP_CHECK_EQ(real_end(100, pads), 100);
  TP_CHECK_EQ(real_end(100, {{40, 60}}), 40);
  TP_CHECK_EQ(real_end(100, {{0, 100}}), 0);
}

TP_TEST(incident_piece_geometry_has_45_real_blocks_and_83_padding_blocks) {
  const std::int64_t plen = 2 * MiB;
  std::vector<PadRange> pads{{725079, plen - 725079}};
  TP_CHECK_EQ(blocks_in_piece(plen), 128);
  TP_CHECK_EQ(real_blocks_in_piece(plen, pads), 45);                                 // blocks 0..44 hold real bytes
  int padb = 0; for (int b = 0; b < 128; ++b) padb += block_is_pad(b, plen, pads) ? 1 : 0;
  TP_CHECK_EQ(padb, 83);
  TP_CHECK(!block_is_pad(44, plen, pads));                                           // block 44 straddles the end of the video and the padding
  TP_CHECK(block_is_pad(45, plen, pads));
  TP_CHECK_EQ(real_blocks_in_piece(plen, {}), 128);                                  // ordinary piece: every block is real
}

TP_TEST(byte_ranges_track_coverage) {
  ByteRanges r;
  TP_CHECK(!r.covers(0, 1)); TP_CHECK(r.covers(5, 0));
  r.add(0, 10); r.add(20, 10);
  TP_CHECK(r.covers(0, 10)); TP_CHECK(r.covers(22, 5)); TP_CHECK(!r.covers(5, 20)); TP_CHECK(!r.covers(9, 12));
  r.add(10, 10);                                                                       // fills the gap: merges into one interval
  TP_CHECK(r.covers(0, 30)); TP_CHECK(!r.covers(0, 31));
  r.add(5, 3);                                                                         // contained: no change
  TP_CHECK(r.covers(0, 30));
}

// ---------------------------------------------------------------- store: verification boundary
static StoreParams pad_params(const fs::path& d, std::int64_t ps, std::int64_t total, int np, const std::vector<std::pair<std::int64_t, std::int64_t>>& pad_files) {
  StoreParams p; p.dir = d; p.piece_size = ps; p.num_pieces = np; p.total_size = total; p.pads = build_piece_pads(pad_files, ps, total, np); return p;
}

TP_TEST(regression_incident_structure_verifies_with_implicit_padding_and_nothing_extra_is_stored) {
  // piece 0 is ordinary video data; piece 1 = the last 725,079 bytes of the video + a 1,372,073-byte padding file (as in the reported torrent)
  const std::int64_t PS = 2 * MiB, real1 = 725079;
  auto d = tmpdir("incident");
  SegmentStore s(pad_params(d, PS, 2 * PS, 2, {{PS + real1, PS - real1}}));
  const auto& pads = s.pads_of(1);
  TP_CHECK(eq(pads, {{real1, PS - real1}}));
  TP_CHECK(s.pads_of(0).empty());
  const auto stream = rnd(static_cast<std::size_t>(real1), 7);
  const auto want = logical(PS, pads, stream);
  const Sha1 expected = sha1_of(want.data(), want.size());                           // what the torrent's piece hash would be for zero padding
  // all real blocks but one (block 20) arrive: verification must stay INCOMPLETE and must not be counted as a mismatch
  write_real(s, 1, PS, pads, want, /*skip_block=*/20);
  std::vector<char> b;
  TP_CHECK(s.logical_piece(1, true, b) == HashStatus::Incomplete);
  Sha1 h; TP_CHECK(s.compute_sha1(1, h));                                             // non-live (recovery-style) assembly reads holes as zeros...
  TP_CHECK(!(h == expected));                                                         // ...so it can never be mistaken for the correct piece
  TP_CHECK_EQ(s.metrics().hash_incomplete, 1);
  // the missing block arrives: now all real data and the implicit padding produce the expected SHA-1
  { const std::int64_t a = 20LL * 16384; TP_CHECK(s.write(1, static_cast<int>(a), want.data() + a, 16384) == WriteStatus::Ok); }
  TP_CHECK(s.logical_piece(1, true, b) == HashStatus::Ok);
  TP_CHECK(sha1_of(b.data(), b.size()) == expected);
  TP_CHECK_EQ(static_cast<std::int64_t>(b.size()), PS);
  for (std::int64_t i = real1; i < PS; ++i) if (b[static_cast<std::size_t>(i)] != 0) { TP_CHECK(false); break; }   // the padding part of the logical piece is zeros
  TP_CHECK(s.mark_verified(1)); TP_CHECK(s.has(1));
  TP_CHECK_EQ(file_size_of(s.piece_path(1)), real1);                                  // NOTHING artificial was written: the file ends at the last real byte
  // reads of real data still work; a read that reaches into the padding is a harmless "absent" (never served as data)
  std::vector<char> r(1000);
  TP_CHECK_EQ(s.read(1, 700000, r.data(), 1000), 1000);
  TP_CHECK(std::memcmp(r.data(), want.data() + 700000, 1000) == 0);
  TP_CHECK_EQ(s.read(1, static_cast<int>(real1) - 10, r.data(), 100), -1);
}

TP_TEST(wrong_real_data_is_a_genuine_hash_mismatch_not_incomplete) {
  const std::int64_t PS = 2 * MiB, real1 = 725079;
  auto d = tmpdir("wrong");
  SegmentStore s(pad_params(d, PS, 2 * PS, 2, {{PS + real1, PS - real1}}));
  const auto& pads = s.pads_of(1);
  const auto want = logical(PS, pads, rnd(static_cast<std::size_t>(real1), 11));
  const Sha1 expected = sha1_of(want.data(), want.size());
  auto bad = want; bad[300000] ^= 0x5a;                                                // one corrupt real byte
  write_real(s, 1, PS, pads, bad);
  std::vector<char> b;
  TP_CHECK(s.logical_piece(1, true, b) == HashStatus::Ok);                             // all real data is present...
  TP_CHECK(!(sha1_of(b.data(), b.size()) == expected));                                // ...and the hash genuinely differs
  TP_CHECK_EQ(s.metrics().hash_incomplete, 0);                                         // not counted as an incomplete attempt
  TP_CHECK(!s.has(1));
  // after a genuine mismatch the write record is forgotten: the piece must be delivered again in full before it can be hashed
  s.forget_written(1);
  TP_CHECK(s.logical_piece(1, true, b) == HashStatus::Incomplete);
  write_real(s, 1, PS, pads, want);                                                    // the correct data arrives (overwriting the corrupt bytes)
  TP_CHECK(s.logical_piece(1, true, b) == HashStatus::Ok);
  TP_CHECK(sha1_of(b.data(), b.size()) == expected);
}

TP_TEST(missing_real_data_is_never_taken_for_padding) {
  const std::int64_t PS = 2 * MiB, real1 = 725079;
  auto d = tmpdir("hole");
  SegmentStore s(pad_params(d, PS, 2 * PS, 2, {{PS + real1, PS - real1}}));
  const auto& pads = s.pads_of(1);
  const auto want = logical(PS, pads, rnd(static_cast<std::size_t>(real1), 13));
  // the LAST real block never arrives, the file is short exactly like a padded piece would be: still incomplete
  write_real(s, 1, PS, pads, want, /*skip_block=*/44);
  std::vector<char> b;
  TP_CHECK(s.logical_piece(1, true, b) == HashStatus::Incomplete);
  // only the padding-adjacent tail of block 44 is missing (real bytes 720,896..725,078)
  TP_CHECK(s.write(1, 720896, want.data() + 720896, 3000) == WriteStatus::Ok);         // 3,000 of 4,183 real bytes
  TP_CHECK(s.logical_piece(1, true, b) == HashStatus::Incomplete);
  TP_CHECK(s.write(1, 723896, want.data() + 723896, 1183) == WriteStatus::Ok);
  TP_CHECK(s.logical_piece(1, true, b) == HashStatus::Ok);
  // a different piece with no writes at all: no file => Missing (not Incomplete, not Ok)
  TP_CHECK(s.logical_piece(0, true, b) == HashStatus::Missing);
  // an eviction forgets what was written: the piece must be rewritten, never inferred from leftovers
  s.evict(1);
  TP_CHECK(s.logical_piece(1, true, b) == HashStatus::Missing);
  TP_CHECK(s.write(1, 0, want.data(), 16384) == WriteStatus::Ok);
  TP_CHECK(s.logical_piece(1, true, b) == HashStatus::Incomplete);
}

TP_TEST(padding_layouts_start_middle_multiple_and_partial_last_piece) {
  const std::int64_t PS = 64 * KiB;
  struct Case { const char* name; std::int64_t total; std::vector<std::pair<std::int64_t, std::int64_t>> pad_files; int np; };
  const std::vector<Case> cases = {
    {"padding at the start of a piece", 2 * PS, {{PS, 20000}}, 2},                      // real data follows the padding
    {"real, padding, real", 2 * PS, {{PS + 10000, 30000}}, 2},                          // hole in the middle of a piece
    {"several ranges in one piece", 2 * PS, {{PS + 5000, 7000}, {PS + 30000, 9000}, {PS + 60000, 5536}}, 2},
    {"partial final piece with padding at its end", PS + 40000, {{PS + 25000, 15000}}, 2},   // last piece is 40,000 bytes, 15,000 of them padding
    {"padding across a whole piece boundary", 3 * PS, {{PS - 4000, 2 * PS + 4000 - 100}}, 3},
  };
  unsigned seed = 100;
  for (const auto& c : cases) {
    auto d = tmpdir("layout");
    SegmentStore s(pad_params(d, PS, c.total, c.np, c.pad_files));
    for (int p = 0; p < c.np; ++p) {
      const std::int64_t plen = s.piece_size_of(p);
      const auto& pads = s.pads_of(p);
      if (real_total(plen, pads) == 0) continue;                                         // a piece made only of padding has nothing to verify
      const auto want = logical(plen, pads, rnd(static_cast<std::size_t>(real_total(plen, pads)), ++seed));
      const Sha1 expected = sha1_of(want.data(), want.size());
      std::vector<char> b;
      const int skip = static_cast<int>((real_end(plen, pads) - 1) / 16384);             // the last block that holds real bytes
      write_real(s, p, plen, pads, want, skip);
      const bool nothing_written = real_ranges(plen, pads).front().start >= static_cast<std::int64_t>(skip) * 16384;   // all real data lives in the skipped block
      const HashStatus st = s.logical_piece(p, true, b);
      TP_CHECK(st == (nothing_written ? HashStatus::Missing : HashStatus::Incomplete));     // a missing block is never Ok and never taken for padding
      { const std::int64_t bs = static_cast<std::int64_t>(skip) * 16384, be = std::min<std::int64_t>(bs + 16384, plen);   // now deliver it (its real bytes only)
        for (const auto& r : real_ranges(plen, pads)) { const std::int64_t x = std::max(bs, r.start), y = std::min(be, r.start + r.len);
          if (y > x) TP_CHECK(s.write(p, static_cast<int>(x), want.data() + x, static_cast<int>(y - x)) == WriteStatus::Ok); } }
      TP_CHECK(s.logical_piece(p, true, b) == HashStatus::Ok);
      TP_CHECK(sha1_of(b.data(), b.size()) == expected);
      TP_CHECK(s.mark_verified(p));
      TP_CHECK(file_size_of(s.piece_path(p)) <= plen);
    }
  }
}

TP_TEST(ordinary_pieces_are_unaffected_by_the_padding_support) {
  const std::int64_t PS = 64 * KiB;
  auto d = tmpdir("ordinary");
  StoreParams p; p.dir = d; p.piece_size = PS; p.num_pieces = 3; p.total_size = 2 * PS + 1000;     // short last piece, NO padding files
  SegmentStore s(p);
  TP_CHECK(s.pads_of(0).empty() && s.pads_of(2).empty());
  for (int pc = 0; pc < 3; ++pc) {
    const std::int64_t plen = s.piece_size_of(pc);
    const auto data = rnd(static_cast<std::size_t>(plen), 50u + pc);
    const Sha1 expected = sha1_of(data.data(), data.size());
    std::vector<char> b;
    TP_CHECK(s.logical_piece(pc, true, b) == HashStatus::Missing);                       // nothing written yet
    for (std::int64_t off = 0; off < plen; off += 16384) {
      const int len = static_cast<int>(std::min<std::int64_t>(16384, plen - off));
      if (off + len < plen) TP_CHECK(s.logical_piece(pc, true, b) != HashStatus::Ok);    // never "Ok" before the whole piece is there
      TP_CHECK(s.write(pc, static_cast<int>(off), data.data() + off, len) == WriteStatus::Ok);
    }
    TP_CHECK(s.logical_piece(pc, true, b) == HashStatus::Ok);
    TP_CHECK(sha1_of(b.data(), b.size()) == expected);
    Sha1 h; TP_CHECK(s.compute_sha1(pc, h)); TP_CHECK(h == expected);
    TP_CHECK(s.mark_verified(pc));
    TP_CHECK_EQ(file_size_of(s.piece_path(pc)), plen);                                   // full-size file, exactly as before
  }
  // a short file is incomplete for an ordinary piece (the old rule is unchanged)
  { auto d2 = tmpdir("ordinary_short"); StoreParams q = p; q.dir = d2; SegmentStore t(q); const auto data = rnd(1000, 9);
    TP_CHECK(t.write(0, 0, data.data(), 1000) == WriteStatus::Ok); std::vector<char> b; TP_CHECK(t.logical_piece(0, true, b) == HashStatus::Incomplete); Sha1 h; TP_CHECK(!t.compute_sha1(0, h)); }
}

TP_TEST(recovery_keeps_verified_padded_pieces_and_rejects_short_or_corrupt_ones) {
  const std::int64_t PS = 2 * MiB, real1 = 725079;
  auto d = tmpdir("recover");
  const auto params = pad_params(d, PS, 2 * PS, 2, {{PS + real1, PS - real1}});
  std::vector<char> want; Sha1 expected;
  {
    SegmentStore s(params);
    const auto& pads = s.pads_of(1);
    want = logical(PS, pads, rnd(static_cast<std::size_t>(real1), 21));
    expected = sha1_of(want.data(), want.size());
    write_real(s, 1, PS, pads, want);
    std::vector<char> b; TP_CHECK(s.logical_piece(1, true, b) == HashStatus::Ok);
    TP_CHECK(s.mark_verified(1));
  }
  auto verify = [&](int p, const Sha1& h) { return p == 1 && h == expected; };
  { SegmentStore s2(params); auto r = s2.recover(verify, 0); TP_CHECK_EQ(r.verified_present, 1); TP_CHECK_EQ(r.deleted_partial, 0); TP_CHECK_EQ(r.deleted_corrupt, 0); TP_CHECK(s2.has(1)); }   // the short padded file survives a restart
  { fs::resize_file(params.dir / "piece_1.seg", real1 - 5);                                                                                    // truncated inside real data
    SegmentStore s3(params); auto r = s3.recover(verify, 0); TP_CHECK_EQ(r.deleted_partial, 1); TP_CHECK(!s3.has(1)); }
  { std::ofstream f(params.dir / "piece_1.seg", std::ios::binary | std::ios::trunc); auto bad = want; bad[1000] ^= 1; f.write(bad.data(), real1);   // right size, wrong content
    f.close(); SegmentStore s4(params); auto r = s4.recover(verify, 0); TP_CHECK_EQ(r.deleted_corrupt, 1); TP_CHECK(!s4.has(1)); }
}
TP_MAIN()
