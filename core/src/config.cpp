#include "tpcore/config.hpp"
#include <algorithm>
#include <cmath>

namespace tp {

int pinned_pieces_per_end(std::int64_t piece_size, std::int64_t pin_bytes_per_end) {
  if (piece_size <= 0) return 1;
  std::int64_t n = (pin_bytes_per_end + piece_size - 1) / piece_size;
  return static_cast<int>(std::max<std::int64_t>(1, n));
}

std::int64_t internal_cap(const CacheConfig& c, std::int64_t piece_size) {
  std::int64_t margin = std::max<std::int64_t>(static_cast<std::int64_t>(c.margin_min_pieces) * piece_size,
                                               static_cast<std::int64_t>(std::ceil(static_cast<double>(c.limit_bytes) * c.margin_fraction)));
  return std::max<std::int64_t>(0, c.limit_bytes - margin);
}

}  // namespace tp
