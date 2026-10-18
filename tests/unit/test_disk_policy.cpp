#include "tp_test.hpp"
#include "tpcore/disk_policy.hpp"
using namespace tp;

TP_TEST(hysteresis_defaults_10_5_10) {
  DiskPolicy p{DiskPolicyConfig{}};
  struct Step { double gib; DiskState want; } seq[] = {{50, DiskState::Ok}, {10.0, DiskState::Ok}, {9.9, DiskState::Warning}, {7, DiskState::Warning},
    {5.0, DiskState::Warning}, {4.99, DiskState::Paused}, {6, DiskState::Paused}, {9.9, DiskState::Paused}, {10.0, DiskState::Ok},
    {9.0, DiskState::Warning}, {4, DiskState::Paused}, {1.2, DiskState::Paused}, {30, DiskState::Ok}};
  for (auto& s : seq) TP_CHECK(p.update(static_cast<std::int64_t>(s.gib * GiB)) == s.want);
}

TP_TEST(no_flapping_between_pause_and_resume) {
  DiskPolicy p{DiskPolicyConfig{}};
  p.update(4 * GiB);
  for (double g = 4.0; g < 10.0; g += 0.25) TP_CHECK(p.update(static_cast<std::int64_t>(g * GiB)) == DiskState::Paused);
}

TP_TEST(preflight_for_full_download) {
  DiskPolicy p{DiskPolicyConfig{}};
  TP_CHECK(p.preflight(20 * GiB, 100 * GiB) == DiskPolicy::Preflight::Ok);
  TP_CHECK(p.preflight(92 * GiB, 100 * GiB) == DiskPolicy::Preflight::Warn);
  TP_CHECK(p.preflight(97 * GiB, 100 * GiB) == DiskPolicy::Preflight::BlockWithChoice);
  TP_CHECK(p.preflight(95 * GiB, 100 * GiB) == DiskPolicy::Preflight::Warn);            // leaves exactly 5 GiB: not below the pause threshold
  TP_CHECK(p.preflight(95 * GiB + 1, 100 * GiB) == DiskPolicy::Preflight::BlockWithChoice);
}

TP_TEST(small_volume_clamping_and_hard_floor) {
  auto small = effective_disk_policy(DiskPolicyConfig{}, 64 * GiB);
  TP_CHECK_EQ(small.pause_at, 64 * GiB / 20); TP_CHECK_EQ(small.warn_at, 64 * GiB / 10); TP_CHECK_EQ(small.resume_at, 64 * GiB / 10);   // 5 % / 10 % of capacity
  auto tiny = effective_disk_policy(DiskPolicyConfig{}, 8 * GiB);
  TP_CHECK_EQ(tiny.pause_at, 1 * GiB);                                                   // hard floor
  auto big = effective_disk_policy(DiskPolicyConfig{}, 460 * GiB);
  TP_CHECK_EQ(big.pause_at, 5 * GiB); TP_CHECK_EQ(big.warn_at, 10 * GiB); TP_CHECK_EQ(big.resume_at, 10 * GiB);
  DiskPolicyConfig low; low.pause_at = 100 * MiB;                                        // a user value below the floor is raised to it
  TP_CHECK(effective_disk_policy(low, 1000 * GiB).pause_at >= 1 * GiB);
}

TP_MAIN()
