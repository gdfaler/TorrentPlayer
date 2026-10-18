// tpcore/diagnostics.hpp: storage-integrity forensics (spec section 9).
//   ShadowVerifier keeps, per in-flight piece, the exact bytes handed to the disk layer so that a failed piece hash can be classified as
//   STORAGE CORRUPTION (disk != received) vs ARRIVED CORRUPT (disk == received, but wrong hash) vs UNDETERMINED, and retains evidence.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "tpcore/segment_store.hpp"

namespace tp {

enum class Verdict { StorageCorruption, ArrivedCorrupt, Undetermined };
const char* to_string(Verdict v);

struct DiagCounters {
  std::atomic<int> failures{0}, storage{0}, arrived{0}, undetermined{0}, verified_ok{0};
};

class ShadowVerifier {
 public:
  ShadowVerifier(std::int64_t piece_size, int num_pieces, std::int64_t total_size, std::filesystem::path evidence_dir);
  void on_write(int piece, int offset, const void* buf, int len);                          // every block handed to the disk layer
  // A piece failed its hash. `file_bytes` = what is on disk. Saves evidence and returns the verdict.
  Verdict on_failure(int piece, const std::vector<char>& file_bytes, const Sha1& expected);
  // A piece passed its hash. If an earlier failed copy exists, writes a block-level diff with the failed copy's per-block sources.
  void on_pass(int piece, const std::vector<char>& good_bytes);
  // Source attribution (from libtorrent block_finished_alert): block -> "ip:port" and whether it is a web seed.
  void note_block_source(int piece, int block, const std::string& endpoint, bool web_seed);
  void attach_sources_to_failure(int piece);                                                // call when the failure alert arrives
  std::string attribution(int piece) const;
  void forget(int piece);                                                                   // piece verified or cleared
  const DiagCounters& counters() const { return c_; }
  std::string last_report() const;
 private:
  struct Shadow { std::vector<char> data; std::vector<char> got; };
  struct Failed { std::vector<char> received; std::string base; std::vector<std::string> sources; };
  std::int64_t piece_size_;
  int num_pieces_;
  std::int64_t total_size_;
  std::filesystem::path dir_;
  mutable std::mutex mu_;
  std::map<int, Shadow> shadow_;
  std::map<int, Failed> failed_;
  std::map<int, std::map<int, std::string>> sources_;
  DiagCounters c_;
  std::string last_;
  std::int64_t size_of(int piece) const;
};

}  // namespace tp
