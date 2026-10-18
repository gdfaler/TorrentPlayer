// tpcore/config.hpp: user-visible and provisional-internal cache configuration (spec v0.2 r3, section 4.1)
#pragma once
#include <cstdint>

namespace tp {

constexpr std::int64_t KiB = 1024, MiB = 1024 * KiB, GiB = 1024 * MiB;

struct CacheConfig {
  // ---- user-visible
  std::int64_t limit_bytes = 2 * GiB;            // HARD bound on actual allocated disk space attributable to cache data (spec 4.2)
  double behind_seconds = 300.0;                 // provisional 5 minutes, configurable 0..3600
  double ahead_seconds_auto = 75.0;              // automatic ahead buffer target (60..90 s)
  std::int64_t ahead_override_bytes = 0;         // 0 = automatic
  // ---- provisional internal defaults (user-approved as provisional; adjustable from measurements, spec 0.1)
  double margin_fraction = 0.10;                 // internal cap = limit - max(margin_min_pieces * piece, margin_fraction * limit)
  int margin_min_pieces = 2;
  std::int64_t pin_bytes_per_end = 2 * MiB;      // head and tail budgeted SEPARATELY (4 MiB combined), rounded up to whole pieces
  double touch_guard_seconds = 30.0;
  double refill_low_water = 0.50;                // refill when contiguous buffer ahead < 50% of target
  int inflight_slack_pieces = 4;                 // window sizing slack for in-flight partial pieces
  double max_behind_fraction = 0.40;             // behind window never exceeds 40% of the internal cap
  double max_ahead_fraction = 0.35;              // automatic ahead never exceeds 35% of the internal cap
  double min_ahead_fraction = 0.10;
};

// Number of pinned pieces at EACH end: ceil(pin_bytes_per_end / piece_size), at least 1.
int pinned_pieces_per_end(std::int64_t piece_size, std::int64_t pin_bytes_per_end);
// Internal cap = limit - max(margin_min_pieces * piece_size, ceil(margin_fraction * limit)).
std::int64_t internal_cap(const CacheConfig& c, std::int64_t piece_size);

}  // namespace tp
