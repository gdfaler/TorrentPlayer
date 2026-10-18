// Startup-diagnostics helpers: what is the reader waiting for, and has the needed piece made progress? Includes the geometry of a real failure
// (a 3.18 GiB QuickTime MOV whose index sits in the last 25 KB of the file, 2 MiB pieces).
#include "tp_test.hpp"
#include "tpcore/config.hpp"
#include "tpcore/window_planner.hpp"
using namespace tp;

TP_TEST(wait_kind_classifies_head_tail_and_data) {
  TP_CHECK(classify_wait_piece(-1, 0, 0, 1626, 1626) == WaitKind::None);
  TP_CHECK(classify_wait_piece(0, 0, 0, 1626, 1626) == WaitKind::HeadIndex);
  TP_CHECK(classify_wait_piece(1626, 0, 0, 1626, 1626) == WaitKind::TailIndex);
  TP_CHECK(classify_wait_piece(500, 0, 0, 1626, 1626) == WaitKind::Data);
  TP_CHECK(classify_wait_piece(3, 0, 7, 1000, 1007) == WaitKind::HeadIndex);       // several pinned pieces per end (small pieces)
  TP_CHECK(classify_wait_piece(1003, 0, 7, 1000, 1007) == WaitKind::TailIndex);
  TP_CHECK(classify_wait_piece(8, 0, 7, 1000, 1007) == WaitKind::Data);
  TP_CHECK(classify_wait_piece(0, 0, 0, 0, 0) == WaitKind::HeadIndex);             // one-piece file: head wins
}

TP_TEST(blocks_in_piece_rounds_up) {
  TP_CHECK_EQ(blocks_in_piece(2 * MiB), 128);
  TP_CHECK_EQ(blocks_in_piece(16384), 1);
  TP_CHECK_EQ(blocks_in_piece(16385), 2);
  TP_CHECK_EQ(blocks_in_piece(725079), 45);                                        // a short last piece
  TP_CHECK_EQ(blocks_in_piece(0), 0);
}

TP_TEST(progress_watch_reports_idle_time_and_resets) {
  ProgressWatch w;
  TP_CHECK(w.idle_seconds(5.0) == 0.0);                                            // nothing watched yet
  w.update(10.0, 1626, 1);
  TP_CHECK(w.idle_seconds(10.0) == 0.0);
  w.update(20.0, 1626, 1);                                                         // no new block
  TP_CHECK(w.idle_seconds(30.0) == 20.0);
  w.update(31.0, 1626, 2);                                                         // a block arrived: the clock restarts
  TP_CHECK(w.idle_seconds(36.0) == 5.0);
  w.update(40.0, 1626, 2);
  w.update(50.0, 7, 0);                                                            // the target changed: the clock restarts for the new piece
  TP_CHECK(w.idle_seconds(51.0) == 1.0);
  w.update(60.0, 7, 0);
  TP_CHECK(w.idle_seconds(60.0) == 10.0);
}

TP_TEST(real_failure_geometry_moov_is_in_the_tail_pin_piece) {
  // spider-man ... prores.mov: file at torrent offset 0, 3,410,694,231 bytes, 2 MiB pieces; ftyp+wide+mdat then moov in the last 25,615 bytes.
  StreamGeometry g; g.piece_size = 2 * MiB; g.file_offset = 0; g.file_size = 3410694231LL;
  CacheConfig c; c.limit_bytes = 2 * GiB;
  WindowPlanner pl(c, g, 0);
  TP_CHECK_EQ(g.last_piece(), 1626);
  TP_CHECK_EQ(pl.pin_head_first(), 0);  TP_CHECK_EQ(pl.pin_head_last(), 0);
  TP_CHECK_EQ(pl.pin_tail_first(), 1626); TP_CHECK_EQ(pl.pin_tail_last(), 1626);
  const std::int64_t moov_off = 3410668616LL;                                       // end of the mdat atom = start of moov
  TP_CHECK_EQ(g.piece_of_file_offset(moov_off), 1626);
  TP_CHECK_EQ(g.piece_of_file_offset(g.file_size - 1), 1626);                       // the whole index lives in ONE piece
  TP_CHECK_EQ((moov_off - 1626LL * g.piece_size) / 16384, 42);                      // first block of that piece that holds the index
  TP_CHECK(classify_wait_piece(g.piece_of_file_offset(moov_off), pl.pin_head_first(), pl.pin_head_last(), pl.pin_tail_first(), pl.pin_tail_last()) == WaitKind::TailIndex);
  // the playhead estimator ignores the far jump to the index (it is metadata, not a seek)
  PlayheadEstimator est(g.file_size, c.pin_bytes_per_end);
  est.on_read(0); est.on_read(131072);
  est.on_read(moov_off);
  TP_CHECK(est.offset() < 1 * MiB);
}
TP_MAIN()
