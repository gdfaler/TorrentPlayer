// tpcore/disk_policy.hpp: free-disk policy (spec 4.1 / 7a). Defaults chosen by the user: warn 10 GiB, pause 5 GiB, resume 10 GiB.
#pragma once
#include <cstdint>
#include <string>
#include "tpcore/config.hpp"

namespace tp {

struct DiskPolicyConfig {
  std::int64_t warn_at = 10 * GiB;
  std::int64_t pause_at = 5 * GiB;
  std::int64_t resume_at = 10 * GiB;
  std::int64_t hard_floor = 1 * GiB;     // never lower than this, whatever the user configures
};

enum class DiskState { Ok, Warning, Paused };
const char* to_string(DiskState s);

// Clamp thresholds on small volumes (never above 5 % / 10 % of capacity) and never below the hard floor.
DiskPolicyConfig effective_disk_policy(const DiskPolicyConfig& cfg, std::int64_t volume_capacity);

class DiskPolicy {
 public:
  explicit DiskPolicy(DiskPolicyConfig cfg) : cfg_(cfg) {}
  DiskState update(std::int64_t free_bytes);          // hysteresis state machine
  DiskState state() const { return state_; }
  // Full-download preflight: decision before starting.
  enum class Preflight { Ok, Warn, BlockWithChoice };
  Preflight preflight(std::int64_t required_bytes, std::int64_t free_bytes) const;
  const DiskPolicyConfig& config() const { return cfg_; }
 private:
  DiskPolicyConfig cfg_;
  DiskState state_ = DiskState::Ok;
};

}  // namespace tp
