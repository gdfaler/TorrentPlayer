// Recursive content accounting for the CHURN-F ring directories (regression fix for the smoke-run halt of 2026-10-17: tp::measure_allocated_bytes() is NON-recursive and the
// cache store lives in <slot>/<torrent-hash>/piece_N.seg, so it reported 0 files for a populated slot). Two independent implementations (std::filesystem iteration vs POSIX
// opendir/readdir/lstat) are provided so that the harness can require them to agree; neither follows symlinks.
#pragma once
#include <dirent.h>
#include <sys/stat.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace tpit {
struct TreeStats { long long files = 0, dirs = 0, non_regular = 0, alloc_bytes = 0, apparent_bytes = 0; int max_depth = 0; bool ok = true; };
inline bool operator==(const TreeStats& a, const TreeStats& b) { return a.ok && b.ok && a.files == b.files && a.dirs == b.dirs && a.non_regular == b.non_regular && a.alloc_bytes == b.alloc_bytes && a.apparent_bytes == b.apparent_bytes; }

// method 1: std::filesystem::recursive_directory_iterator + lstat (st_blocks * 512 = allocated bytes). Counts sub-directories (excluding the root), regular files, and anything else.
inline TreeStats tree_stats_fs(const std::filesystem::path& root) {
  TreeStats t; std::error_code ec;
  if (!std::filesystem::is_directory(root, ec) || ec) { t.ok = false; return t; }
  std::filesystem::recursive_directory_iterator it(root, std::filesystem::directory_options::none, ec), end;
  if (ec) { t.ok = false; return t; }
  for (; it != end; it.increment(ec)) {
    if (ec) { t.ok = false; return t; }
    struct stat st{};
    if (::lstat(it->path().c_str(), &st) != 0) { t.ok = false; return t; }
    t.max_depth = std::max(t.max_depth, it.depth() + 1);
    if (S_ISREG(st.st_mode)) { ++t.files; t.alloc_bytes += static_cast<long long>(st.st_blocks) * 512; t.apparent_bytes += st.st_size; }
    else if (S_ISDIR(st.st_mode)) ++t.dirs;
    else ++t.non_regular;
  }
  return t;
}

// method 2: explicit stack over opendir/readdir/lstat (no std::filesystem).
inline TreeStats tree_stats_posix(const std::string& root) {
  TreeStats t; std::vector<std::pair<std::string, int>> stack{{root, 0}};
  while (!stack.empty()) {
    auto [dir, depth] = stack.back(); stack.pop_back();
    DIR* d = ::opendir(dir.c_str()); if (!d) { t.ok = false; return t; }
    while (dirent* e = ::readdir(d)) {
      const std::string n = e->d_name; if (n == "." || n == "..") continue;
      const std::string p = dir + "/" + n; struct stat st{};
      if (::lstat(p.c_str(), &st) != 0) { ::closedir(d); t.ok = false; return t; }
      t.max_depth = std::max(t.max_depth, depth + 1);
      if (S_ISREG(st.st_mode)) { ++t.files; t.alloc_bytes += static_cast<long long>(st.st_blocks) * 512; t.apparent_bytes += st.st_size; }
      else if (S_ISDIR(st.st_mode)) { ++t.dirs; stack.push_back({p, depth + 1}); }
      else ++t.non_regular;
    }
    ::closedir(d);
  }
  return t;
}
}  // namespace tpit
