// tpcore/window_planner.hpp: pure window sizing / hysteresis / playhead logic (no I/O, fully unit-testable).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "tpcore/config.hpp"

namespace tp {

// Where the selected media file sits inside the torrent's piece space.
struct StreamGeometry {
  std::int64_t piece_size = 262144;
  std::int64_t file_offset = 0;   // torrent offset of the file's first byte
  std::int64_t file_size = 0;
  int first_piece() const { return static_cast<int>(file_offset / piece_size); }
  int last_piece() const { return static_cast<int>((file_offset + file_size - 1) / piece_size); }
  int piece_of_file_offset(std::int64_t off) const { return static_cast<int>((file_offset + off) / piece_size); }
};

struct WindowSizes {
  int ahead_pieces = 0;
  int behind_pieces = 0;
  int pinned_per_end = 0;
  int slack_pieces = 0;
  std::int64_t internal_cap = 0;
  bool valid = false;
  bool clamped = false;
  std::string note;               // human-readable reason when clamped or invalid
};

// Compute window sizes from the budget. Order of preference when the budget is too small: keep ahead, shrink behind, then shrink ahead.
// Budget = internal_cap - pinned (2 ends) - in-flight slack. bytes_per_second <= 0 means "unknown" (a conservative default is used).
WindowSizes compute_window_sizes(const CacheConfig& cfg, const StreamGeometry& g, double bytes_per_second);

struct WindowPlan {
  int head = 0;          // playhead piece
  int lo = 0;            // oldest retained piece behind the playhead
  int hi_full = 0;       // exclusive end of the full ahead window
  int hi_wanted = 0;     // exclusive end of what should be requested now (after hysteresis)
  bool refilling = true;
  int contiguous_ahead = 0;
};

class WindowPlanner {
 public:
  WindowPlanner(CacheConfig cfg, StreamGeometry geo, double bytes_per_second = 0);
  void set_bitrate(double bytes_per_second);
  const WindowSizes& sizes() const { return sizes_; }
  // `has(p)` = piece p is verified and present. Updates the hysteresis state.
  WindowPlan plan(int head_piece, const std::function<bool(int)>& has);
  // Pinned piece ranges (inclusive), per end.
  int pin_head_first() const { return geo_.first_piece(); }
  int pin_head_last() const { return geo_.first_piece() + sizes_.pinned_per_end - 1; }
  int pin_tail_first() const { return geo_.last_piece() - sizes_.pinned_per_end + 1; }
  int pin_tail_last() const { return geo_.last_piece(); }
  bool is_pinned(int piece) const;
  const StreamGeometry& geometry() const { return geo_; }
  // The cache refused a write (budget miscalculation): shrink the ahead window to `factor` of its size (never below 2 pieces).
  void shrink_ahead(double factor);
  const CacheConfig& config() const { return cfg_; }
 private:
  CacheConfig cfg_;
  StreamGeometry geo_;
  WindowSizes sizes_;
  bool refilling_ = true;
};

// Playhead estimate from the demuxer's read pattern. Rules (verified in the research harness):
//  - sequential reads / small jumps (<= follow_bytes) move the playhead;
//  - far jumps into the head/tail "container metadata" zones are ignored;
//  - far jumps into the body are treated as a real seek;
//  - an application seek hint moves the playhead immediately.
class PlayheadEstimator {
 public:
  PlayheadEstimator(std::int64_t file_size, std::int64_t zone_bytes, std::int64_t follow_bytes = 8 * MiB)
      : file_size_(file_size), zone_(zone_bytes), follow_(follow_bytes) {}
  void on_read(std::int64_t file_offset);
  void hint(std::int64_t file_offset) { off_ = clamp(file_offset); }
  std::int64_t offset() const { return off_; }
 private:
  std::int64_t clamp(std::int64_t v) const { return v < 0 ? 0 : (v >= file_size_ ? file_size_ - 1 : v); }
  std::int64_t file_size_, zone_, follow_, off_ = 0;
};

// ---- startup diagnostics helpers (pure, unit-tested)
// What kind of data is the reader waiting for? Container metadata lives in the pinned head/tail pieces (an MP4/MOV without "faststart" keeps its index at the END).
enum class WaitKind { None = 0, HeadIndex = 1, TailIndex = 2, Data = 3 };
inline WaitKind classify_wait_piece(int piece, int head_first, int head_last, int tail_first, int tail_last) {
  if (piece < 0) return WaitKind::None;
  if (piece >= head_first && piece <= head_last) return WaitKind::HeadIndex;
  if (piece >= tail_first && piece <= tail_last) return WaitKind::TailIndex;
  return WaitKind::Data;
}
// Number of 16 KiB blocks in a piece of `piece_bytes` bytes (the last piece of a torrent is shorter).
inline int blocks_in_piece(std::int64_t piece_bytes, int block_size = 16384) {
  return piece_bytes <= 0 ? 0 : static_cast<int>((piece_bytes + block_size - 1) / block_size);
}
// Tracks how long the piece the reader needs has made no progress. `progress` is any measure that only grows while data for the target arrives
// (finished blocks). Changing the target restarts the clock. It states a fact (seconds without a new block); it never decides that data is unavailable.
class ProgressWatch {
 public:
  void update(double now, int target, std::int64_t progress) {
    if (!init_ || target != target_) { init_ = true; target_ = target; last_ = progress; since_ = now; }
    else if (progress > last_) { last_ = progress; since_ = now; }
  }
  double idle_seconds(double now) const { return init_ ? now - since_ : 0.0; }
 private:
  bool init_ = false;
  int target_ = -1;
  std::int64_t last_ = 0;
  double since_ = 0;
};

}  // namespace tp
