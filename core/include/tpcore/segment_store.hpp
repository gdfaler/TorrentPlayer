// tpcore/segment_store.hpp: one file per piece in a cache directory with a STRICT bound on charged disk space.
// Design (spec v0.2 r3, section 4.2):
//   * a piece is charged one FULL piece slot from its first byte (independent of APFS delayed allocation);
//   * every write reserves BEFORE touching the disk: victims are evicted until charged + need <= internal cap; if no victim
//     exists the write is REFUSED (NoSpace), never executed over the cap;
//   * accounting is reconciled against measured st_blocks (max(slot, measured)) incrementally;
//   * victims are never pinned pieces or pieces in the protected ahead window;
//   * recovery verifies complete-sized files by hash before trusting them, deletes partial/corrupt files, then trims to the cap.
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include "tpcore/pad_ranges.hpp"

namespace tp {

using Sha1 = std::array<std::uint8_t, 20>;
Sha1 sha1_of(const void* data, std::size_t len);

struct StoreParams {
  std::filesystem::path dir;
  std::int64_t piece_size = 262144;
  int num_pieces = 0;              // pieces in the TORRENT (piece indices are torrent-wide)
  std::int64_t total_size = 0;     // torrent size (last piece may be shorter)
  std::int64_t limit_bytes = 0;    // user-visible hard bound (0 = unlimited; tests only)
  std::int64_t margin_bytes = 0;   // internal cap = limit - margin
  std::vector<std::vector<PadRange>> pads;   // BEP 47 padding per piece (empty = the torrent has no padding files); padding is implicit zeros, never stored
};

// What must not be evicted. Ahead window [head, hi) is protected, pinned ranges are protected; [lo, head) is retained but evictable last.
struct Protection {
  int head = 0, lo = 0, hi = 0;
  int pin_a_first = -1, pin_a_last = -2;
  int pin_b_first = -1, pin_b_last = -2;
  bool pinned(int p) const { return (p >= pin_a_first && p <= pin_a_last) || (p >= pin_b_first && p <= pin_b_last); }
};

enum class WriteStatus { Ok, NoSpace, IoError };
// Result of assembling a piece for verification. Ok: all real bytes were read (padding ranges are zeros). Incomplete: real data is missing. Missing: no file.
enum class HashStatus { Ok, Incomplete, Missing };

struct StoreMetrics {
  std::int64_t limit = 0, internal_cap = 0, charged = 0, max_charged = 0;
  std::int64_t forced_evictions = 0, sweep_evictions = 0, refused_writes = 0, reserve_calls = 0;
  std::int64_t reconcile_corrections = 0, cap_violations = 0, writes = 0, rewrites_identical = 0, rewrites_different = 0;
  int present = 0, partial_files = 0;
  std::int64_t hash_incomplete = 0;   // verification attempts that found real data missing (distinct from a genuine SHA-1 mismatch)
};

struct RecoveryReport {
  int verified_present = 0, deleted_partial = 0, deleted_corrupt = 0, deleted_foreign = 0, trimmed = 0;
  std::int64_t charged_after = 0;
};

class SegmentStore {
 public:
  explicit SegmentStore(StoreParams p);
  SegmentStore(const SegmentStore&) = delete;
  SegmentStore& operator=(const SegmentStore&) = delete;

  // ---- recovery (call before any torrent activity)
  // verify(piece, sha1_of_file) returns true if the piece is a good copy. Only complete-sized files are hashed.
  using VerifyFn = std::function<bool(int piece, const Sha1&)>;
  RecoveryReport recover(const VerifyFn& verify, int head_hint);

  // ---- data path
  WriteStatus write(int piece, int offset, const void* buf, int len);
  int read(int piece, int offset, void* buf, int len) const;      // bytes read, or -1 if the piece file is absent/short
  bool compute_sha1(int piece, Sha1& out) const;                   // false if the real data is not all present (padding ranges count as zeros)
  // The full logical piece (padding ranges as zeros) for hashing. live=true additionally requires that every real byte range was WRITTEN in this
  // run (so a hole in real data can never be mistaken for padding) and counts an incomplete attempt in metrics().hash_incomplete.
  HashStatus logical_piece(int piece, bool live, std::vector<char>& bytes) const;
  const std::vector<PadRange>& pads_of(int piece) const;            // BEP 47 padding ranges of a piece (empty for ordinary pieces)
  void forget_written(int piece);                                   // after a genuine mismatch: the piece will be re-downloaded, so earlier write coverage no longer counts
  // Marks a piece verified. Returns false (and changes nothing) if its file no longer exists, e.g. it was evicted concurrently.
  bool mark_verified(int piece);
  bool has(int piece) const;
  int count_present() const;
  std::vector<int> present_pieces() const;

  // ---- eviction / maintenance
  void set_protection(const Protection& p);
  void evict(int piece);                                           // unconditional (caller decides); safe if absent
  int purge_partials();                                            // delete files of unverified pieces (only when libtorrent forgets partial state)
  int sweep_outside_window(std::chrono::nanoseconds touch_guard, int slack_after_hi);   // evict present pieces outside [lo, hi+slack), unpinned, untouched within the guard
  int reconcile_step(int n);                                       // incremental accounting correction against st_blocks; evicts if over the internal cap
  int trim_to_cap();
  void touch(int piece);

  // ---- waiting for data
  bool wait_present(int piece, std::chrono::milliseconds timeout, const std::atomic<bool>* cancel) const;

  // ---- observability (never takes the I/O lock)
  StoreMetrics metrics() const;
  std::int64_t charged() const { return charged_.load(); }
  std::int64_t internal_cap() const { return limit_ ? limit_ - margin_ : 0; }
  std::int64_t limit() const { return limit_; }
  std::uint64_t mutation_epoch() const { return epoch_.load(); }   // incremented on every file create/write/delete (for lock-free auditors)
  std::filesystem::path piece_path(int piece) const;
  std::int64_t piece_size_of(int piece) const;
  const StoreParams& params() const { return p_; }
  // Test hook: choose a victim under the current protection (exposed for unit tests).
  int pick_victim_for_test(int keep) const;
  // Fault injection hook for tests (called after each successful write with the piece index).
  std::function<void(int piece)> after_write_hook;

 private:
  int pick_victim_locked(int keep) const;                           // requires mu_
  void evict_locked_io(int piece);                                  // requires io_mu_
  void charge_locked(int piece);                                    // requires mu_
  bool file_size_acceptable(int piece, std::int64_t size) const;    // recovery: complete-sized, or (padded pieces) covering all real bytes
  StoreParams p_;
  std::int64_t limit_, margin_;
  mutable std::recursive_mutex io_mu_;                              // serialises file mutations
  mutable std::mutex mu_;                                           // guards bitmaps / charges / present set
  mutable std::condition_variable_any cv_;
  std::vector<char> present_flag_;
  std::set<int> present_set_;
  std::vector<std::int64_t> charge_;
  std::vector<std::int64_t> touch_ns_;
  std::map<int, ByteRanges> written_;                               // real byte ranges written so far, only for pieces that have padding (guarded by mu_)
  mutable std::atomic<std::int64_t> hash_incomplete_{0};
  Protection prot_;
  std::atomic<std::int64_t> charged_{0}, max_charged_{0};
  std::atomic<std::uint64_t> epoch_{0};
  std::atomic<std::int64_t> forced_evictions_{0}, sweep_evictions_{0}, refused_writes_{0}, reserve_calls_{0};
  std::atomic<std::int64_t> reconcile_corrections_{0}, cap_violations_{0}, writes_{0}, rw_same_{0}, rw_diff_{0};
  int reconcile_cursor_ = 0;
};

}  // namespace tp
