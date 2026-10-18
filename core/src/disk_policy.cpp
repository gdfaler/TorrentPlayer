#include "tpcore/disk_policy.hpp"
#include <algorithm>

namespace tp {

const char* to_string(DiskState s) {
  switch (s) { case DiskState::Ok: return "ok"; case DiskState::Warning: return "warning"; case DiskState::Paused: return "paused"; }
  return "?";
}

DiskPolicyConfig effective_disk_policy(const DiskPolicyConfig& c, std::int64_t cap) {
  DiskPolicyConfig e = c;
  e.pause_at = std::max(c.hard_floor, std::min(c.pause_at, cap / 20));
  e.warn_at = std::max(e.pause_at + c.hard_floor, std::min(c.warn_at, cap / 10));
  e.resume_at = std::max(e.warn_at, std::min(c.resume_at, cap / 10));
  return e;
}

DiskState DiskPolicy::update(std::int64_t free_bytes) {
  switch (state_) {
    case DiskState::Paused:
      if (free_bytes >= cfg_.resume_at) state_ = (free_bytes >= cfg_.warn_at) ? DiskState::Ok : DiskState::Warning;
      break;
    default:
      if (free_bytes < cfg_.pause_at) state_ = DiskState::Paused;
      else if (free_bytes < cfg_.warn_at) state_ = DiskState::Warning;
      else state_ = DiskState::Ok;
  }
  return state_;
}

DiskPolicy::Preflight DiskPolicy::preflight(std::int64_t required, std::int64_t free_bytes) const {
  const std::int64_t after = free_bytes - required;
  if (after < cfg_.pause_at) return Preflight::BlockWithChoice;
  if (after < cfg_.warn_at) return Preflight::Warn;
  return Preflight::Ok;
}

}  // namespace tp
