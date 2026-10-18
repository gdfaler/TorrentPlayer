// tpcore/pad_ranges.hpp: BEP 47 padding-file geometry (pure, header-only, unit-tested).
// Bytes that belong to padding files are all zero by definition. libtorrent never requests them, so they are never written to the cache;
// the verification layer treats them as implicit zeros when it hashes a piece. Offsets here are IN-PIECE byte offsets.
#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace tp {

struct PadRange { std::int64_t start = 0, len = 0; };

// Sort and merge overlapping or adjacent ranges.
inline void normalize_pad_ranges(std::vector<PadRange>& v) {
  std::sort(v.begin(), v.end(), [](const PadRange& a, const PadRange& b) { return a.start < b.start; });
  std::vector<PadRange> out;
  for (const auto& r : v) {
    if (r.len <= 0) continue;
    if (!out.empty() && r.start <= out.back().start + out.back().len) out.back().len = std::max(out.back().len, r.start + r.len - out.back().start);
    else out.push_back(r);
  }
  v.swap(out);
}

// Per-piece padding ranges for a whole torrent. `pad_files` = (torrent byte offset, length) of every padding file.
// The result is EMPTY when there are no padding files (ordinary torrents carry no per-piece data at all).
inline std::vector<std::vector<PadRange>> build_piece_pads(const std::vector<std::pair<std::int64_t, std::int64_t>>& pad_files,
                                                           std::int64_t piece_size, std::int64_t total_size, int num_pieces) {
  std::vector<std::vector<PadRange>> out;
  if (pad_files.empty() || piece_size <= 0 || num_pieces <= 0) return out;
  out.resize(static_cast<std::size_t>(num_pieces));
  for (const auto& pf : pad_files) {
    const std::int64_t a = std::max<std::int64_t>(0, pf.first), b = std::min<std::int64_t>(total_size, pf.first + pf.second);
    for (std::int64_t pos = a; pos < b;) {
      const std::int64_t piece = pos / piece_size;
      if (piece >= num_pieces) break;
      const std::int64_t end = std::min<std::int64_t>((piece + 1) * piece_size, b);
      out[static_cast<std::size_t>(piece)].push_back({pos - piece * piece_size, end - pos});
      pos = end;
    }
  }
  for (auto& v : out) normalize_pad_ranges(v);
  return out;
}

// The complement: the byte ranges of a piece that hold REAL data (must be downloaded, stored and hashed). `pads` must be normalized.
inline std::vector<PadRange> real_ranges(std::int64_t piece_len, const std::vector<PadRange>& pads) {
  std::vector<PadRange> out;
  std::int64_t pos = 0;
  for (const auto& p : pads) {
    const std::int64_t s = std::max<std::int64_t>(0, p.start), e = std::min(piece_len, p.start + p.len);
    if (e <= s) continue;
    if (s > pos) out.push_back({pos, s - pos});
    pos = std::max(pos, e);
  }
  if (pos < piece_len) out.push_back({pos, piece_len - pos});
  return out;
}

// End (exclusive, in-piece) of the last real byte; 0 if the piece has no real data.
inline std::int64_t real_end(std::int64_t piece_len, const std::vector<PadRange>& pads) {
  const auto r = real_ranges(piece_len, pads);
  return r.empty() ? 0 : r.back().start + r.back().len;
}

// Is the whole 16 KiB block inside padding (nothing to download)?
inline bool block_is_pad(int block, std::int64_t piece_len, const std::vector<PadRange>& pads, int block_size = 16384) {
  const std::int64_t s = static_cast<std::int64_t>(block) * block_size, e = std::min<std::int64_t>(s + block_size, piece_len);
  if (e <= s) return false;
  for (const auto& p : pads) if (p.start <= s && p.start + p.len >= e) return true;
  return false;
}

// Blocks that need real data (at least one non-padding byte).
inline int real_blocks_in_piece(std::int64_t piece_len, const std::vector<PadRange>& pads, int block_size = 16384) {
  if (piece_len <= 0) return 0;
  const int all = static_cast<int>((piece_len + block_size - 1) / block_size);
  int n = 0;
  for (int b = 0; b < all; ++b) if (!block_is_pad(b, piece_len, pads, block_size)) ++n;
  return n;
}

// Set of byte intervals that were actually written to a piece (merged). Used to tell "real data still missing" from "data present but wrong".
class ByteRanges {
 public:
  void add(std::int64_t start, std::int64_t len) {
    if (len <= 0) return;
    std::int64_t s = start, e = start + len;
    std::vector<std::pair<std::int64_t, std::int64_t>> out;
    bool placed = false;
    for (const auto& r : v_) {
      if (r.second < s) out.push_back(r);
      else if (r.first > e) { if (!placed) { out.emplace_back(s, e); placed = true; } out.push_back(r); }
      else { s = std::min(s, r.first); e = std::max(e, r.second); }
    }
    if (!placed) out.emplace_back(s, e);
    v_.swap(out);
  }
  bool covers(std::int64_t start, std::int64_t len) const {
    if (len <= 0) return true;
    for (const auto& r : v_) if (r.first <= start && r.second >= start + len) return true;
    return false;
  }
  bool empty() const { return v_.empty(); }
 private:
  std::vector<std::pair<std::int64_t, std::int64_t>> v_;
};

}  // namespace tp
