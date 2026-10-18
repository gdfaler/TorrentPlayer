#include "tp_test.hpp"
#include "tpcore/config.hpp"
#include "tpcore/window_planner.hpp"
#include <set>
using namespace tp;

TP_TEST(pin_rule_is_per_end_rounded_up) {
  TP_CHECK_EQ(pinned_pieces_per_end(256 * KiB, 2 * MiB), 8);        // 2 MiB / 256 KiB
  TP_CHECK_EQ(pinned_pieces_per_end(2 * MiB, 2 * MiB), 1);
  TP_CHECK_EQ(pinned_pieces_per_end(4 * MiB, 2 * MiB), 1);          // at least one piece per end
  TP_CHECK_EQ(pinned_pieces_per_end(1 * MiB + 1, 2 * MiB), 2);      // rounded UP
  TP_CHECK_EQ(pinned_pieces_per_end(16 * KiB, 2 * MiB), 128);
  TP_CHECK_EQ(pinned_pieces_per_end(0, 2 * MiB), 1);
}

TP_TEST(internal_cap_is_limit_minus_margin) {
  CacheConfig c; c.limit_bytes = 2 * GiB;
  const std::int64_t margin = c.limit_bytes - internal_cap(c, 256 * KiB);
  TP_CHECK(margin >= static_cast<std::int64_t>(0.10 * 2 * GiB));
  TP_CHECK(margin <= static_cast<std::int64_t>(0.10 * 2 * GiB) + 1);
  c.limit_bytes = 9 * MiB;                                           // small limit: margin = max(2 pieces, 10 %)
  TP_CHECK(c.limit_bytes - internal_cap(c, 256 * KiB) >= 512 * KiB);
  c.limit_bytes = 10 * MiB;
  TP_CHECK(c.limit_bytes - internal_cap(c, 4 * MiB) >= 8 * MiB);      // 2 big pieces dominate
}

static StreamGeometry geo(std::int64_t piece, std::int64_t fsize) { StreamGeometry g; g.piece_size = piece; g.file_offset = 123456; g.file_size = fsize; return g; }

TP_TEST(window_sizes_fit_budget_and_prefer_ahead) {
  CacheConfig c; c.limit_bytes = 2 * GiB;
  auto s = compute_window_sizes(c, geo(256 * KiB, 4 * GiB), 2.0 * MiB);     // 16 Mbit/s
  TP_CHECK(s.valid);
  const std::int64_t used = (s.ahead_pieces + s.behind_pieces + 2 * s.pinned_per_end + s.slack_pieces) * 256 * KiB;
  TP_CHECK(used <= s.internal_cap);
  TP_CHECK(s.ahead_pieces >= 2);
  // 75 s at 2 MiB/s = 150 MiB ahead; behind 300 s = 600 MiB (< 40 % of the internal cap)
  TP_CHECK(s.ahead_pieces * 256 * KiB >= 140 * MiB);
  TP_CHECK(s.behind_pieces * 256 * KiB >= 590 * MiB);
}

TP_TEST(window_sizes_clamp_behind_first_then_ahead) {
  CacheConfig c; c.limit_bytes = 64 * MiB; c.max_ahead_fraction = 0.9; c.max_behind_fraction = 0.9;   // fractions relaxed so that only the budget clamps
  auto s = compute_window_sizes(c, geo(256 * KiB, 1 * GiB), 4.0 * MiB);     // 32 Mbit/s: wants 300 MiB ahead, 1.2 GiB behind
  TP_CHECK(s.valid);
  TP_CHECK(s.clamped);
  const std::int64_t used = (s.ahead_pieces + s.behind_pieces + 2 * s.pinned_per_end + s.slack_pieces) * 256 * KiB;
  TP_CHECK(used <= s.internal_cap);
  TP_CHECK(s.behind_pieces >= 0);
  TP_CHECK(s.behind_pieces < 300 * 4);   // behind shrank well below its 5-minute target
  TP_CHECK(s.ahead_pieces >= 2 * 4);     // ahead was preserved preferentially
}

TP_TEST(window_sizes_invalid_when_cache_too_small) {
  CacheConfig c; c.limit_bytes = 8 * MiB;
  auto s = compute_window_sizes(c, geo(4 * MiB, 1 * GiB), 1.0 * MiB);        // 4 MiB pieces cannot fit pins+slack in 8 MiB
  TP_CHECK(!s.valid);
  TP_CHECK(!s.note.empty());
}

TP_TEST(planner_hysteresis_refills_below_low_water_only) {
  CacheConfig c; c.limit_bytes = 256 * MiB;
  WindowPlanner pl(c, geo(256 * KiB, 2 * GiB), 1.0 * MiB);
  const int ahead = pl.sizes().ahead_pieces;
  const int head = pl.geometry().first_piece() + 100;
  std::set<int> have;
  auto has = [&](int p) { return have.count(p) > 0; };
  auto w = pl.plan(head, has);
  TP_CHECK(w.refilling);                                      // nothing buffered: refill the full window
  TP_CHECK_EQ(w.hi_wanted, w.hi_full);
  for (int p = head; p < head + ahead; ++p) have.insert(p);  // full buffer
  w = pl.plan(head, has);
  TP_CHECK(!w.refilling);                                     // buffer full: idle (wanted == what we have)
  TP_CHECK_EQ(w.hi_wanted, head + w.contiguous_ahead);
  for (int p = head; p < head + ahead / 2 + 1; ++p) have.erase(p);   // drops below 50 %? (contiguous from head is now 0)
  w = pl.plan(head, has);
  TP_CHECK(w.refilling);
  // moving the playhead forward by one piece with the buffer still > 50 % must NOT start refilling
  have.clear(); for (int p = head; p < head + ahead; ++p) have.insert(p);
  pl.plan(head, has);                                         // -> not refilling
  w = pl.plan(head + 1, has);
  TP_CHECK(!w.refilling);
}

TP_TEST(planner_pinned_ranges) {
  CacheConfig c; c.limit_bytes = 256 * MiB;
  StreamGeometry g = geo(256 * KiB, 100 * MiB);
  WindowPlanner pl(c, g, 1.0 * MiB);
  TP_CHECK_EQ(pl.sizes().pinned_per_end, 8);
  TP_CHECK(pl.is_pinned(g.first_piece()));
  TP_CHECK(pl.is_pinned(g.first_piece() + 7));
  TP_CHECK(!pl.is_pinned(g.first_piece() + 8));
  TP_CHECK(pl.is_pinned(g.last_piece()));
  TP_CHECK(pl.is_pinned(g.last_piece() - 7));
  TP_CHECK(!pl.is_pinned(g.last_piece() - 8));
}

TP_TEST(playhead_estimator_follows_rules) {
  PlayheadEstimator e(100 * MiB, 2 * MiB);
  e.on_read(0); e.on_read(1 * MiB); TP_CHECK_EQ(e.offset(), 1 * MiB);        // sequential
  e.on_read(100 * MiB - 100);                                                 // far jump into the tail zone: container metadata, ignored
  TP_CHECK_EQ(e.offset(), 1 * MiB);
  e.on_read(512 * KiB); TP_CHECK_EQ(e.offset(), 512 * KiB);                   // small jump back follows
  e.on_read(60 * MiB); TP_CHECK_EQ(e.offset(), 60 * MiB);                     // far jump into the body = seek
  e.hint(10 * MiB); TP_CHECK_EQ(e.offset(), 10 * MiB);                        // application hint moves immediately
  e.on_read(50); TP_CHECK_EQ(e.offset(), 10 * MiB);                           // head-zone jump ignored
  e.hint(-5); TP_CHECK_EQ(e.offset(), 0);
  e.hint(1000 * MiB); TP_CHECK_EQ(e.offset(), 100 * MiB - 1);
}

TP_MAIN()
