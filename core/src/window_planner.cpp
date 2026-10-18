#include "tpcore/window_planner.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace tp {

WindowSizes compute_window_sizes(const CacheConfig& cfg, const StreamGeometry& g, double bps) {
  WindowSizes s;
  const std::int64_t P = g.piece_size;
  s.internal_cap = internal_cap(cfg, P);
  s.pinned_per_end = pinned_pieces_per_end(P, cfg.pin_bytes_per_end);
  s.slack_pieces = cfg.inflight_slack_pieces;
  if (bps <= 0) bps = 512.0 * 1024;  // unknown bitrate: conservative default of 0.5 MiB/s
  const std::int64_t pins = static_cast<std::int64_t>(2 * s.pinned_per_end) * P;
  const std::int64_t slack = static_cast<std::int64_t>(s.slack_pieces) * P;
  const std::int64_t budget = s.internal_cap - pins - slack;
  if (budget < 4 * P) {
    s.valid = false;
    s.note = "cache limit too small for this torrent's piece size (needs room for pinned head/tail, in-flight pieces and a playback window)";
    return s;
  }
  std::int64_t behind = static_cast<std::int64_t>(cfg.behind_seconds * bps);
  behind = std::min<std::int64_t>(behind, static_cast<std::int64_t>(cfg.max_behind_fraction * static_cast<double>(s.internal_cap)));
  std::int64_t ahead = cfg.ahead_override_bytes > 0
                           ? cfg.ahead_override_bytes
                           : static_cast<std::int64_t>(cfg.ahead_seconds_auto * bps);
  if (cfg.ahead_override_bytes <= 0) {
    ahead = std::max<std::int64_t>(ahead, static_cast<std::int64_t>(cfg.min_ahead_fraction * static_cast<double>(s.internal_cap)));
    ahead = std::min<std::int64_t>(ahead, static_cast<std::int64_t>(cfg.max_ahead_fraction * static_cast<double>(s.internal_cap)));
  }
  ahead = std::max<std::int64_t>(ahead, 2 * P);
  if (ahead + behind > budget) {
    s.clamped = true;
    behind = std::max<std::int64_t>(0, budget - ahead);
    if (ahead > budget) { ahead = budget; behind = 0; }
    s.note = "window clamped to fit the cache budget";
  }
  s.ahead_pieces = static_cast<int>(std::max<std::int64_t>(2, ahead / P));
  s.behind_pieces = static_cast<int>(behind / P);
  s.valid = true;
  return s;
}

WindowPlanner::WindowPlanner(CacheConfig cfg, StreamGeometry geo, double bps) : cfg_(cfg), geo_(geo) {
  sizes_ = compute_window_sizes(cfg_, geo_, bps);
}

void WindowPlanner::shrink_ahead(double f) { sizes_.ahead_pieces = std::max(2, static_cast<int>(sizes_.ahead_pieces * f)); sizes_.clamped = true; sizes_.note = "ahead window shrunk after a refused write"; }

void WindowPlanner::set_bitrate(double bps) { sizes_ = compute_window_sizes(cfg_, geo_, bps); }

bool WindowPlanner::is_pinned(int p) const {
  return (p >= pin_head_first() && p <= pin_head_last()) || (p >= pin_tail_first() && p <= pin_tail_last());
}

WindowPlan WindowPlanner::plan(int head_piece, const std::function<bool(int)>& has) {
  WindowPlan w;
  const int first = geo_.first_piece(), last = geo_.last_piece();
  w.head = std::max(first, std::min(last, head_piece));
  w.lo = std::max(first, w.head - sizes_.behind_pieces);
  w.hi_full = std::min(last + 1, w.head + sizes_.ahead_pieces);
  int contig = 0;
  while (w.head + contig < w.hi_full && has(w.head + contig)) ++contig;
  w.contiguous_ahead = contig;
  const int ahead_len = std::max(1, w.hi_full - w.head);
  if (contig < static_cast<int>(cfg_.refill_low_water * ahead_len)) refilling_ = true;
  else if (w.head + contig >= w.hi_full) refilling_ = false;
  w.refilling = refilling_;
  w.hi_wanted = refilling_ ? w.hi_full : w.head + contig;
  return w;
}

void PlayheadEstimator::on_read(std::int64_t pos) {
  const bool in_zone = pos < zone_ || pos >= file_size_ - zone_;
  if (std::llabs(pos - off_) <= follow_) off_ = clamp(pos);
  else if (!in_zone) off_ = clamp(pos);
  // far jump into the head/tail zone = container metadata read: ignored for playhead purposes
}

}  // namespace tp
