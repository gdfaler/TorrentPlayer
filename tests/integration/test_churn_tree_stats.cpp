// Regression test for churn_tree_stats.hpp (fix for the CHURN-F smoke halt): nested layout identical to the real cache store, agreement of both methods, empty/missing/symlink cases,
// and a documented demonstration that the old non-recursive helper reports 0 files for that layout.
#include <fstream>
#include <iostream>
#include <unistd.h>
#include "churn_tree_stats.hpp"
#include "tpcore/audit.hpp"
namespace fs = std::filesystem;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::cerr << "FAIL line " << __LINE__ << ": " #c "\n"; ++fails; } } while (0)
int main() {
  const fs::path root = fs::temp_directory_path() / ("churn_tree_stats_test_" + std::to_string(::getpid())); fs::remove_all(root); fs::create_directories(root / "slot" / "57c308abc");
  for (int i = 0; i < 7; ++i) { std::ofstream f(root / "slot" / "57c308abc" / ("piece_" + std::to_string(i) + ".seg"), std::ios::binary); std::string blk(100000 + i, 'x'); f << blk; }
  fs::create_directories(root / "slot" / "57c308abc" / "deeper" / "deepest"); { std::ofstream f(root / "slot" / "57c308abc" / "deeper" / "deepest" / "x.bin", std::ios::binary); f << std::string(5000, 'y'); }
  const fs::path slot = root / "slot";
  auto a = tpit::tree_stats_fs(slot), b = tpit::tree_stats_posix(slot.string());
  CHECK(a.ok && b.ok); CHECK(a.files == 8); CHECK(a.dirs == 3); CHECK(a.max_depth == 4); CHECK(a == b);
  long long apparent = 0; for (int i = 0; i < 7; ++i) apparent += 100000 + i; apparent += 5000; CHECK(a.apparent_bytes == apparent); CHECK(a.alloc_bytes >= apparent);
  // root cause regression: the old helper is non-recursive and reports 0 files for this layout
  int old_files = -1; const auto old_bytes = tp::measure_allocated_bytes(slot, &old_files); CHECK(old_files == 0); CHECK(old_bytes == 0);
  // empty directory
  fs::create_directories(root / "empty"); auto e1 = tpit::tree_stats_fs(root / "empty"), e2 = tpit::tree_stats_posix((root / "empty").string()); CHECK(e1.ok && e2.ok && e1.files == 0 && e1.dirs == 0 && e1 == e2);
  // missing directory fails closed (ok = false), never a silent zero
  CHECK(!tpit::tree_stats_fs(root / "nope").ok); CHECK(!tpit::tree_stats_posix((root / "nope").string()).ok);
  // symlinks are not followed and are counted as non-regular
  fs::create_symlink(slot, root / "empty" / "link"); auto s1 = tpit::tree_stats_fs(root / "empty"), s2 = tpit::tree_stats_posix((root / "empty").string()); CHECK(s1.non_regular == 1 && s1.files == 0 && s1 == s2);
  // a populated directory differs from an empty one: a zero-vs-populated confusion cannot pass
  CHECK(!(a == e1));
  fs::remove_all(root);
  std::cout << (fails ? "FAILED" : "PASSED") << " (" << fails << " failures)\n"; return fails ? 1 : 0;
}
