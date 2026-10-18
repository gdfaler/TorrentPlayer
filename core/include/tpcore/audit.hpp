// tpcore/audit.hpp: independent, NON-INTERFERING auditors for the strict cache bound (spec 4.2, gate M1-G2/G3).
// Neither auditor takes any lock the engine or the store needs.
//   * VolumeDeltaAuditor : samples statfs free space of the volume at ~1 ms; peak (baseline_free - min_free) is an O(1), scale-independent
//                          measure of ACTUAL allocation (includes allocator overhead). Noise from other processes is measured and reported.
//   * DirScanAuditor     : lock-free "seqlock" scan of the cache directory (st_blocks); a scan is accepted only if the store's mutation
//                          epoch did not change during it, so it is atomic without blocking writers. Acceptance rate is reported
//                          (at high write rates few scans are accepted; the volume auditor is then the primary instrument).
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <thread>
#include "tpcore/segment_store.hpp"

namespace tp {

// Sum of st_blocks*512 of regular files in dir (NOT atomic; for coarse checks and idle measurements).
std::int64_t measure_allocated_bytes(const std::filesystem::path& dir, int* files = nullptr);
std::int64_t volume_free_bytes(const std::filesystem::path& on_volume);

struct VolumeAudit {
  std::int64_t baseline_free = 0, min_free = 0, noise_floor = 0;
  std::int64_t samples = 0;
  std::int64_t peak_usage() const { return baseline_free - min_free; }                 // raw peak free-space drop
  std::int64_t peak_usage_noise_corrected() const { return std::max<std::int64_t>(0, peak_usage() - noise_floor); }
};

class VolumeDeltaAuditor {
 public:
  VolumeDeltaAuditor(std::filesystem::path on_volume, std::chrono::microseconds period = std::chrono::microseconds(1000));
  ~VolumeDeltaAuditor();
  void calibrate_noise(std::chrono::milliseconds idle = std::chrono::milliseconds(2000));   // measure free-space jitter with no cache activity
  void start();
  VolumeAudit stop();
 private:
  std::filesystem::path path_;
  std::chrono::microseconds period_;
  std::atomic<bool> run_{false};
  std::thread th_;
  VolumeAudit res_;
  std::atomic<std::int64_t> min_free_{INT64_MAX};
  std::atomic<std::int64_t> samples_{0};
};

struct DirAudit {
  std::int64_t max_accepted = 0;                  // peak of accepted (consistent) scans
  std::int64_t scans = 0, accepted = 0, rejected = 0;
  double acceptance() const { return scans ? static_cast<double>(accepted) / static_cast<double>(scans) : 0.0; }
};

class DirScanAuditor {
 public:
  DirScanAuditor(const SegmentStore& store, std::chrono::microseconds period = std::chrono::microseconds(2000));
  ~DirScanAuditor();
  void start();
  DirAudit stop();
 private:
  const SegmentStore& store_;
  std::chrono::microseconds period_;
  std::atomic<bool> run_{false};
  std::thread th_;
  std::atomic<std::int64_t> max_{0}, scans_{0}, acc_{0}, rej_{0};
};

}  // namespace tp
