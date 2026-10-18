#include "tpcore/segment_store.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

namespace fs = std::filesystem;
namespace tp {

static std::int64_t now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

Sha1 sha1_of(const void* data, std::size_t len) {
  Sha1 out{};
  CC_SHA1(data, static_cast<CC_LONG>(len), out.data());
  return out;
}

SegmentStore::SegmentStore(StoreParams p)
    : p_(std::move(p)), limit_(p_.limit_bytes), margin_(p_.margin_bytes) {
  fs::create_directories(p_.dir);
  present_flag_.assign(p_.num_pieces, 0);
  charge_.assign(p_.num_pieces, 0);
  touch_ns_.assign(p_.num_pieces, -1'000'000'000'000LL);
}

fs::path SegmentStore::piece_path(int piece) const { return p_.dir / ("piece_" + std::to_string(piece) + ".seg"); }

std::int64_t SegmentStore::piece_size_of(int piece) const {
  if (piece == p_.num_pieces - 1) return p_.total_size - p_.piece_size * (p_.num_pieces - 1);
  return p_.piece_size;
}

void SegmentStore::charge_locked(int p) {
  if (charge_[p] == 0) {
    charge_[p] = piece_size_of(p);
    const std::int64_t t = charged_ += charge_[p];
    std::int64_t m = max_charged_.load();
    while (t > m && !max_charged_.compare_exchange_weak(m, t)) {}
    if (limit_ && t > internal_cap()) ++cap_violations_;
  }
}

// ---------------------------------------------------------------- victim policy
int SegmentStore::pick_victim_locked(int keep) const {
  // 1) outside the window: farthest from the playhead first; 2) behind the playhead (inside [lo, head)); never pinned, never [head, hi).
  const Protection& pr = prot_;
  int best_low = -1, best_high = -1;
  for (auto it = present_set_.begin(); it != present_set_.end() && *it < pr.lo; ++it)
    if (*it != keep && !pr.pinned(*it)) { best_low = *it; break; }
  for (auto it = present_set_.rbegin(); it != present_set_.rend() && *it >= pr.hi; ++it)
    if (*it != keep && !pr.pinned(*it)) { best_high = *it; break; }
  if (best_low >= 0 || best_high >= 0) {
    if (best_low < 0) return best_high;
    if (best_high < 0) return best_low;
    return (pr.head - best_low >= best_high - pr.head) ? best_low : best_high;
  }
  for (auto it = present_set_.lower_bound(pr.lo); it != present_set_.end() && *it < pr.head; ++it)
    if (*it != keep && !pr.pinned(*it)) return *it;     // oldest retained behind-piece first
  return -1;
}

int SegmentStore::pick_victim_for_test(int keep) const {
  std::lock_guard<std::mutex> g(mu_);
  return pick_victim_locked(keep);
}

void SegmentStore::set_protection(const Protection& p) {
  std::lock_guard<std::mutex> g(mu_);
  prot_ = p;
}

// ---------------------------------------------------------------- eviction
void SegmentStore::evict_locked_io(int piece) {
  std::error_code ec;
  fs::remove(piece_path(piece), ec);
  std::lock_guard<std::mutex> g(mu_);
  if (present_flag_[piece]) { present_flag_[piece] = 0; present_set_.erase(piece); }
  if (charge_[piece]) { charged_ -= charge_[piece]; charge_[piece] = 0; }
  ++epoch_;
  cv_.notify_all();
}

void SegmentStore::evict(int piece) {
  if (piece < 0 || piece >= p_.num_pieces) return;
  std::lock_guard<std::recursive_mutex> io(io_mu_);
  evict_locked_io(piece);
}

int SegmentStore::purge_partials() {
  std::lock_guard<std::recursive_mutex> io(io_mu_);
  std::vector<int> v;
  {
    std::lock_guard<std::mutex> g(mu_);
    for (int p = 0; p < p_.num_pieces; ++p) if (charge_[p] > 0 && !present_flag_[p]) v.push_back(p);
  }
  for (int p : v) evict_locked_io(p);
  return static_cast<int>(v.size());
}

int SegmentStore::sweep_outside_window(std::chrono::nanoseconds guard, int slack_after_hi) {
  std::lock_guard<std::recursive_mutex> io(io_mu_);
  std::vector<int> v;
  {
    std::lock_guard<std::mutex> g(mu_);
    const std::int64_t t = now_ns();
    for (int p : present_set_) {
      if ((p >= prot_.lo && p < prot_.hi + slack_after_hi) || prot_.pinned(p)) continue;
      if (t - touch_ns_[p] < guard.count()) continue;
      v.push_back(p);
    }
  }
  for (int p : v) { evict_locked_io(p); ++sweep_evictions_; }
  return static_cast<int>(v.size());
}

int SegmentStore::trim_to_cap() {
  if (!limit_) return 0;
  std::lock_guard<std::recursive_mutex> io(io_mu_);
  int n = 0;
  for (int guard = 0; guard < 1'000'000 && charged_.load() > internal_cap(); ++guard) {
    int v;
    { std::lock_guard<std::mutex> g(mu_); v = pick_victim_locked(-1); }
    if (v < 0) break;
    evict_locked_io(v); ++forced_evictions_; ++n;
  }
  return n;
}

void SegmentStore::touch(int piece) {
  if (piece < 0 || piece >= p_.num_pieces) return;
  std::lock_guard<std::mutex> g(mu_);
  touch_ns_[piece] = now_ns();
}

// ---------------------------------------------------------------- data path
WriteStatus SegmentStore::write(int piece, int offset, const void* buf, int len) {
  if (piece < 0 || piece >= p_.num_pieces || offset < 0 || len < 0 || offset + len > piece_size_of(piece)) return WriteStatus::IoError;
  std::lock_guard<std::recursive_mutex> io(io_mu_);
  ++reserve_calls_;
  if (limit_) {
    for (int guard = 0;; ++guard) {
      std::int64_t need;
      int v = -1;
      {
        std::lock_guard<std::mutex> g(mu_);
        need = charge_[piece] == 0 ? piece_size_of(piece) : 0;
        if (charged_.load() + need <= internal_cap()) break;
        v = pick_victim_locked(piece);
      }
      if (v < 0 || guard > 100000) { ++refused_writes_; return WriteStatus::NoSpace; }     // never write over the cap
      evict_locked_io(v); ++forced_evictions_;
    }
  }
  const std::string path = piece_path(piece).string();
  int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
  if (fd < 0) return WriteStatus::IoError;
  // rewrite detection (duplicate block deliveries): identical vs different data
  struct stat st;
  if (::fstat(fd, &st) == 0 && st.st_size >= offset + len && len > 0) {
    std::vector<char> old(len);
    if (::pread(fd, old.data(), len, offset) == len) {
      bool zero = true;
      for (char c : old) if (c) { zero = false; break; }
      if (!zero) { if (std::memcmp(old.data(), buf, len) == 0) ++rw_same_; else ++rw_diff_; }
    }
  }
  const char* b = static_cast<const char*>(buf);
  int done = 0;
  while (done < len) {
    ssize_t n = ::pwrite(fd, b + done, len - done, offset + done);
    if (n < 0) { if (errno == EINTR) continue; ::close(fd); return WriteStatus::IoError; }
    done += static_cast<int>(n);
  }
  ::close(fd);
  {
    std::lock_guard<std::mutex> g(mu_);
    charge_locked(piece);
    touch_ns_[piece] = now_ns();
  }
  ++epoch_;
  ++writes_;
  if (after_write_hook) after_write_hook(piece);
  return WriteStatus::Ok;
}

int SegmentStore::read(int piece, int offset, void* buf, int len) const {
  if (piece < 0 || piece >= p_.num_pieces) return -1;
  int fd = ::open(piece_path(piece).c_str(), O_RDONLY);
  if (fd < 0) return -1;
  ssize_t n = ::pread(fd, buf, len, offset);
  ::close(fd);
  return n == len ? len : -1;
}

bool SegmentStore::compute_sha1(int piece, Sha1& out) const {
  const std::int64_t sz = piece_size_of(piece);
  std::vector<char> b(static_cast<std::size_t>(sz));
  int fd = ::open(piece_path(piece).c_str(), O_RDONLY);
  if (fd < 0) return false;
  ssize_t n = ::pread(fd, b.data(), b.size(), 0);
  ::close(fd);
  if (n != static_cast<ssize_t>(b.size())) return false;
  out = sha1_of(b.data(), b.size());
  return true;
}

bool SegmentStore::mark_verified(int piece) {
  if (piece < 0 || piece >= p_.num_pieces) return false;
  std::lock_guard<std::recursive_mutex> io(io_mu_);               // serialise with evict/write so "present" always implies "file exists and is charged"
  {
    std::lock_guard<std::mutex> g(mu_);
    if (charge_[piece] == 0) return false;                         // evicted (or never written): nothing to verify
    if (!present_flag_[piece]) { present_flag_[piece] = 1; present_set_.insert(piece); }
    touch_ns_[piece] = now_ns();
  }
  cv_.notify_all();
  return true;
}

bool SegmentStore::has(int piece) const {
  if (piece < 0 || piece >= p_.num_pieces) return false;
  std::lock_guard<std::mutex> g(mu_);
  return present_flag_[piece] != 0;
}

int SegmentStore::count_present() const { std::lock_guard<std::mutex> g(mu_); return static_cast<int>(present_set_.size()); }

std::vector<int> SegmentStore::present_pieces() const { std::lock_guard<std::mutex> g(mu_); return {present_set_.begin(), present_set_.end()}; }

bool SegmentStore::wait_present(int piece, std::chrono::milliseconds timeout, const std::atomic<bool>* cancel) const {
  std::unique_lock<std::mutex> lk(mu_);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!present_flag_[piece]) {
    if (cancel && cancel->load()) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    cv_.wait_for(lk, std::chrono::milliseconds(20));
  }
  return true;
}

// ---------------------------------------------------------------- reconciliation
int SegmentStore::reconcile_step(int n) {
  if (!limit_ || p_.num_pieces == 0) return 0;
  std::lock_guard<std::recursive_mutex> io(io_mu_);
  int fixed = 0;
  for (int i = 0; i < n; ++i) {
    int p = reconcile_cursor_;
    reconcile_cursor_ = (reconcile_cursor_ + 1) % p_.num_pieces;
    { std::lock_guard<std::mutex> g(mu_); if (charge_[p] == 0) continue; }
    struct stat st;
    if (::stat(piece_path(p).c_str(), &st) != 0) continue;
    const std::int64_t blk = static_cast<std::int64_t>(st.st_blocks) * 512;
    const std::int64_t want = std::max<std::int64_t>(piece_size_of(p), blk);
    std::lock_guard<std::mutex> g(mu_);
    if (want != charge_[p]) {
      const std::int64_t t = (charged_ += want - charge_[p]);
      charge_[p] = want;
      std::int64_t m = max_charged_.load();
      while (t > m && !max_charged_.compare_exchange_weak(m, t)) {}
      ++fixed; ++reconcile_corrections_;
    }
  }
  if (charged_.load() > internal_cap()) trim_to_cap();
  return fixed;
}

// ---------------------------------------------------------------- recovery
RecoveryReport SegmentStore::recover(const VerifyFn& verify, int head_hint) {
  std::lock_guard<std::recursive_mutex> io(io_mu_);
  RecoveryReport r;
  std::error_code ec;
  std::vector<std::pair<int, fs::path>> files;
  for (auto& e : fs::directory_iterator(p_.dir, ec)) {
    int pn = -1; char tail[8] = {0};
    const std::string name = e.path().filename().string();
    if (std::sscanf(name.c_str(), "piece_%d.%7s", &pn, tail) == 2 && std::strcmp(tail, "seg") == 0 && pn >= 0 && pn < p_.num_pieces) files.emplace_back(pn, e.path());
    else { fs::remove(e.path(), ec); ++r.deleted_foreign; }
  }
  // 1) incomplete files are never trusted
  std::vector<std::pair<int, fs::path>> complete;
  for (auto& [pn, path] : files) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) continue;
    if (st.st_size != piece_size_of(pn)) { fs::remove(path, ec); ++r.deleted_partial; continue; }
    complete.emplace_back(pn, path);
  }
  // 2) TRIM FIRST (before hashing and before charging anything): keep the pieces closest to the head hint that fit under the internal cap
  if (limit_) {
    std::sort(complete.begin(), complete.end(), [&](auto& x, auto& y) { return std::abs(x.first - head_hint) < std::abs(y.first - head_hint); });
    std::int64_t budget = internal_cap(), used = 0;
    std::size_t keep_n = 0;
    for (; keep_n < complete.size(); ++keep_n) { std::int64_t c = piece_size_of(complete[keep_n].first); if (used + c > budget) break; used += c; }
    for (std::size_t i = keep_n; i < complete.size(); ++i) { fs::remove(complete[i].second, ec); ++r.trimmed; }
    complete.resize(keep_n);
  }
  // 3) verify what is left: complete-sized files can still be corrupt (holes after a crash)
  for (auto& [pn, path] : complete) {
    Sha1 h;
    if (!compute_sha1(pn, h) || !verify(pn, h)) { fs::remove(path, ec); ++r.deleted_corrupt; continue; }
    std::lock_guard<std::mutex> g(mu_);
    present_flag_[pn] = 1; present_set_.insert(pn); charge_locked(pn); ++r.verified_present;
  }
  { std::lock_guard<std::mutex> g(mu_);
    prot_.head = prot_.lo = prot_.hi = head_hint; }
  r.charged_after = charged_.load();
  ++epoch_;
  return r;
}

StoreMetrics SegmentStore::metrics() const {
  StoreMetrics m;
  m.limit = limit_; m.internal_cap = internal_cap(); m.charged = charged_.load(); m.max_charged = max_charged_.load();
  m.forced_evictions = forced_evictions_.load(); m.sweep_evictions = sweep_evictions_.load(); m.refused_writes = refused_writes_.load();
  m.reserve_calls = reserve_calls_.load(); m.reconcile_corrections = reconcile_corrections_.load(); m.cap_violations = cap_violations_.load();
  m.writes = writes_.load(); m.rewrites_identical = rw_same_.load(); m.rewrites_different = rw_diff_.load();
  std::lock_guard<std::mutex> g(mu_);
  m.present = static_cast<int>(present_set_.size());
  int partial = 0;
  for (int p = 0; p < p_.num_pieces; ++p) if (charge_[p] > 0 && !present_flag_[p]) ++partial;
  m.partial_files = partial;
  return m;
}

}  // namespace tp
