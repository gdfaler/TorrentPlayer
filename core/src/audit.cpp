#include "tpcore/audit.hpp"
#include <sys/mount.h>
#include <sys/stat.h>
#include <algorithm>
#include <thread>

namespace fs = std::filesystem;
namespace tp {

std::int64_t measure_allocated_bytes(const fs::path& dir, int* files) {
  std::int64_t t = 0; int n = 0; std::error_code ec;
  for (auto& e : fs::directory_iterator(dir, ec)) {
    struct stat st;
    if (::stat(e.path().c_str(), &st) == 0 && S_ISREG(st.st_mode)) { t += static_cast<std::int64_t>(st.st_blocks) * 512; ++n; }
  }
  if (files) *files = n;
  return t;
}

std::int64_t volume_free_bytes(const fs::path& p) {
  struct statfs s;
  if (::statfs(p.c_str(), &s) != 0) return -1;
  return static_cast<std::int64_t>(s.f_bavail) * static_cast<std::int64_t>(s.f_bsize);
}

VolumeDeltaAuditor::VolumeDeltaAuditor(fs::path p, std::chrono::microseconds period) : path_(std::move(p)), period_(period) {}
VolumeDeltaAuditor::~VolumeDeltaAuditor() { if (run_) stop(); }

void VolumeDeltaAuditor::calibrate_noise(std::chrono::milliseconds idle) {
  std::int64_t lo = INT64_MAX, hi = 0;
  const auto end = std::chrono::steady_clock::now() + idle;
  while (std::chrono::steady_clock::now() < end) {
    std::int64_t f = volume_free_bytes(path_);
    lo = std::min(lo, f); hi = std::max(hi, f);
    std::this_thread::sleep_for(period_);
  }
  res_.noise_floor = hi - lo;
}

void VolumeDeltaAuditor::start() {
  res_.baseline_free = volume_free_bytes(path_);
  min_free_ = res_.baseline_free; samples_ = 0; run_ = true;
  th_ = std::thread([this] {
    while (run_) {
      std::int64_t f = volume_free_bytes(path_);
      std::int64_t m = min_free_.load();
      while (f < m && !min_free_.compare_exchange_weak(m, f)) {}
      ++samples_;
      std::this_thread::sleep_for(period_);
    }
  });
}

VolumeAudit VolumeDeltaAuditor::stop() {
  run_ = false;
  if (th_.joinable()) th_.join();
  res_.min_free = min_free_.load(); res_.samples = samples_.load();
  return res_;
}

DirScanAuditor::DirScanAuditor(const SegmentStore& s, std::chrono::microseconds period) : store_(s), period_(period) {}
DirScanAuditor::~DirScanAuditor() { if (run_) stop(); }

void DirScanAuditor::start() {
  run_ = true;
  th_ = std::thread([this] {
    while (run_) {
      const std::uint64_t e1 = store_.mutation_epoch();
      const std::int64_t total = measure_allocated_bytes(store_.params().dir);
      const std::uint64_t e2 = store_.mutation_epoch();
      ++scans_;
      if (e1 == e2) { ++acc_; std::int64_t m = max_.load(); while (total > m && !max_.compare_exchange_weak(m, total)) {} }
      else ++rej_;
      std::this_thread::sleep_for(period_);
    }
  });
}

DirAudit DirScanAuditor::stop() {
  run_ = false;
  if (th_.joinable()) th_.join();
  DirAudit a; a.max_accepted = max_.load(); a.scans = scans_.load(); a.accepted = acc_.load(); a.rejected = rej_.load();
  return a;
}

}  // namespace tp
