#include "tpcore/diagnostics.hpp"
#include <algorithm>
#include <cstring>
#include <ctime>
#include <fstream>

namespace fs = std::filesystem;
namespace tp {

const char* to_string(Verdict v) {
  switch (v) {
    case Verdict::StorageCorruption: return "STORAGE CORRUPTION (bytes on disk differ from the bytes handed to the disk layer)";
    case Verdict::ArrivedCorrupt: return "ARRIVED CORRUPT (disk == received bytes, but the received bytes do not match the torrent hash)";
    case Verdict::Undetermined: return "UNDETERMINED";
  }
  return "?";
}

static std::string hex(const Sha1& h) {
  static const char* d = "0123456789abcdef";
  std::string r;
  for (auto c : h) { r += d[c >> 4]; r += d[c & 15]; }
  return r;
}

ShadowVerifier::ShadowVerifier(std::int64_t ps, int np, std::int64_t total, fs::path dir)
    : piece_size_(ps), num_pieces_(np), total_size_(total), dir_(std::move(dir)) {}

std::int64_t ShadowVerifier::size_of(int p) const {
  return p == num_pieces_ - 1 ? total_size_ - piece_size_ * (num_pieces_ - 1) : piece_size_;
}

void ShadowVerifier::on_write(int piece, int offset, const void* buf, int len) {
  std::lock_guard<std::mutex> g(mu_);
  auto& s = shadow_[piece];
  if (s.data.empty()) { s.data.assign(static_cast<std::size_t>(size_of(piece)), 0); s.got.assign(static_cast<std::size_t>((size_of(piece) + 16383) / 16384), 0); }
  std::memcpy(s.data.data() + offset, buf, static_cast<std::size_t>(len));
  for (int b = offset / 16384; b <= (offset + len - 1) / 16384; ++b) s.got[static_cast<std::size_t>(b)] = 1;
}

void ShadowVerifier::note_block_source(int piece, int block, const std::string& ep, bool web) {
  std::lock_guard<std::mutex> g(mu_);
  sources_[piece][block] = web ? ep + "(WEB SEED)" : ep;
}

std::string ShadowVerifier::attribution(int piece) const {
  std::lock_guard<std::mutex> g(mu_);
  auto it = sources_.find(piece);
  if (it == sources_.end()) return "(no source information recorded)";
  std::map<std::string, int> cnt;
  for (auto& kv : it->second) ++cnt[kv.second];
  std::string r;
  for (auto& kv : cnt) r += kv.first + " x" + std::to_string(kv.second) + "; ";
  return r;
}

void ShadowVerifier::forget(int piece) {
  std::lock_guard<std::mutex> g(mu_);
  shadow_.erase(piece); sources_.erase(piece);
}

Verdict ShadowVerifier::on_failure(int piece, const std::vector<char>& file_bytes, const Sha1& expected) {
  std::lock_guard<std::mutex> g(mu_);
  ++c_.failures;
  Verdict v = Verdict::Undetermined;
  auto it = shadow_.find(piece);
  if (it != shadow_.end()) {
    bool complete = true;
    for (char x : it->second.got) if (!x) { complete = false; break; }
    if (complete) {
      const bool same = file_bytes.size() == it->second.data.size() && std::memcmp(file_bytes.data(), it->second.data.data(), file_bytes.size()) == 0;
      const Sha1 sh = sha1_of(it->second.data.data(), it->second.data.size());
      if (!same) v = Verdict::StorageCorruption;
      else if (sh != expected) v = Verdict::ArrivedCorrupt;
    }
  }
  if (v == Verdict::StorageCorruption) ++c_.storage; else if (v == Verdict::ArrivedCorrupt) ++c_.arrived; else ++c_.undetermined;
  // evidence retention
  std::error_code ec; fs::create_directories(dir_, ec);
  char b[256]; std::snprintf(b, sizeof b, "%s/piece_%d_t%lld", dir_.c_str(), piece, static_cast<long long>(std::time(nullptr)));
  const std::vector<char>& received = it != shadow_.end() ? it->second.data : file_bytes;
  { std::ofstream f(std::string(b) + ".received.bin", std::ios::binary); f.write(received.data(), static_cast<std::streamsize>(received.size())); }
  { std::ofstream f(std::string(b) + ".disk.bin", std::ios::binary); f.write(file_bytes.data(), static_cast<std::streamsize>(file_bytes.size())); }
  { std::ofstream t(std::string(b) + ".txt");
    t << "piece " << piece << "\nverdict: " << to_string(v) << "\nexpected sha1: " << hex(expected) << "\n";
    for (std::size_t i = 0; i * 16384 < received.size(); ++i) { const std::size_t off = i * 16384, n = std::min<std::size_t>(16384, received.size() - off); t << "block " << i << " sha1(received)=" << hex(sha1_of(received.data() + off, n)) << "\n"; } }
  failed_[piece] = Failed{received, b, {}};
  last_ = std::string("piece ") + std::to_string(piece) + ": " + to_string(v);
  shadow_.erase(piece);
  return v;
}

void ShadowVerifier::attach_sources_to_failure(int piece) {
  std::lock_guard<std::mutex> g(mu_);
  auto f = failed_.find(piece); auto s = sources_.find(piece);
  if (f == failed_.end() || s == sources_.end()) return;
  int nb = s->second.empty() ? 0 : s->second.rbegin()->first + 1;
  f->second.sources.assign(static_cast<std::size_t>(nb), "?");
  for (auto& kv : s->second) f->second.sources[static_cast<std::size_t>(kv.first)] = kv.second;
}

void ShadowVerifier::on_pass(int piece, const std::vector<char>& good) {
  std::lock_guard<std::mutex> g(mu_);
  ++c_.verified_ok;
  shadow_.erase(piece);
  auto it = failed_.find(piece);
  if (it == failed_.end()) { sources_.erase(piece); return; }
  std::string line = "piece " + std::to_string(piece) + " later verified OK; blocks of the FAILED copy that differ from the good copy:";
  int nd = 0;
  for (std::size_t i = 0; i * 16384 < good.size() && i * 16384 < it->second.received.size(); ++i) {
    const std::size_t off = i * 16384, n = std::min<std::size_t>(16384, good.size() - off);
    if (std::memcmp(good.data() + off, it->second.received.data() + off, n) != 0) { line += " " + std::to_string(i); ++nd; }
  }
  if (!nd) line += " (none: the failed copy was identical)";
  std::ofstream t(it->second.base + ".diff.txt");
  t << line << "\n";
  for (std::size_t i = 0; i < it->second.sources.size(); ++i) t << "failed-copy block " << i << " delivered by " << it->second.sources[i] << "\n";
  last_ = line;
  failed_.erase(it);
  sources_.erase(piece);
}

std::string ShadowVerifier::last_report() const { std::lock_guard<std::mutex> g(mu_); return last_; }

}  // namespace tp
