// Loopback test fixture: deterministic payload (any byte range reproducible in O(1) for end-to-end verification), real libtorrent seeders
// on 127.0.0.1 using the default disk, and a paced "player" that reads through StreamSession exactly like mpv's stream callback will.
#pragma once
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <thread>
#include <vector>
#include "tpcore/audit.hpp"
#include "tpcore/config.hpp"
#include "tpcore/engine.hpp"
#include "tpcore/stream_session.hpp"

namespace fs = std::filesystem;
namespace lt = libtorrent;

namespace tpit {

using Clock = std::chrono::steady_clock;
inline double secs(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
inline double now_s() { static auto t0 = Clock::now(); return secs(Clock::now() - t0); }
#define ITLOG(...) do { std::printf("[%8.3f] ", tpit::now_s()); std::printf(__VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while (0)

inline std::uint64_t splitmix(std::uint64_t x) { x += 0x9e3779b97f4a7c15ULL; x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL; x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL; return x ^ (x >> 31); }

struct Payload {
  fs::path root, data_path;
  std::int64_t size = 0;
  int piece_size = 262144;
  std::uint64_t seed = 1;
  std::vector<char> torrent_bytes;
  std::string name = "payload";
  void fill(std::int64_t off, std::int64_t len, char* out) const {
    for (std::int64_t i = 0; i < len;) {
      const std::int64_t pos = off + i, word = pos / 8; const int sh = static_cast<int>(pos % 8);
      const std::uint64_t w = splitmix(seed * 0x1000193ULL + static_cast<std::uint64_t>(word));
      const int n = static_cast<int>(std::min<std::int64_t>(8 - sh, len - i));
      std::memcpy(out + i, reinterpret_cast<const char*>(&w) + sh, static_cast<std::size_t>(n)); i += n;
    }
  }
  bool matches(std::int64_t off, const char* buf, std::int64_t len) const { std::vector<char> v(static_cast<std::size_t>(len)); fill(off, len, v.data()); return std::memcmp(v.data(), buf, static_cast<std::size_t>(len)) == 0; }

  static Payload create(const fs::path& base, std::int64_t size, int piece_size, std::uint64_t seed) {
    Payload p; p.size = size; p.piece_size = piece_size; p.seed = seed;
    p.root = base / ("payload_" + std::to_string(size) + "_" + std::to_string(piece_size) + "_" + std::to_string(seed));
    fs::create_directories(p.root / "payload");
    p.data_path = p.root / "payload" / "data.bin";
    const fs::path tfile = p.root / "payload.torrent";
    if (fs::exists(p.data_path) && static_cast<std::int64_t>(fs::file_size(p.data_path)) == size && fs::exists(tfile)) {
      std::ifstream f(tfile, std::ios::binary); p.torrent_bytes.assign(std::istreambuf_iterator<char>(f), {}); return p;
    }
    ITLOG("creating payload %.0f MiB (piece %d KiB) ...", size / 1048576.0, piece_size / 1024);
    { std::ofstream f(p.data_path, std::ios::binary | std::ios::trunc); std::vector<char> buf(8 << 20);
      for (std::int64_t off = 0; off < size; off += static_cast<std::int64_t>(buf.size())) { const std::int64_t n = std::min<std::int64_t>(buf.size(), size - off); p.fill(off, n, buf.data()); f.write(buf.data(), n); } }
    lt::file_storage fsr; fsr.add_file("payload/data.bin", size);
    lt::create_torrent ct(fsr, piece_size, lt::create_flags_t{} | lt::create_torrent::v1_only);
    lt::set_piece_hashes(ct, p.root.string());
    lt::bencode(std::back_inserter(p.torrent_bytes), ct.generate());
    std::ofstream(tfile, std::ios::binary).write(p.torrent_bytes.data(), static_cast<std::streamsize>(p.torrent_bytes.size()));
    ITLOG("payload ready");
    return p;
  }
};

struct Seeder {
  std::unique_ptr<lt::session> ses;
  lt::torrent_handle h;
  int port;
  const Payload* pay = nullptr; int upload_limit_ = 0;
  Seeder(const Payload& p, int port_, int upload_limit = 0) : port(port_) { pay = &p; upload_limit_ = upload_limit; start(); }
  // Outage model (used by it_outage): stop() destroys the session so every connection dies and the port stops answering; start() brings it back on the same port.
  void stop() { h = lt::torrent_handle(); ses.reset(); }
  void start() {
    const Payload& p = *pay; const int upload_limit = upload_limit_;
    lt::settings_pack sp;
    sp.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:" + std::to_string(port));
    sp.set_bool(lt::settings_pack::enable_dht, false); sp.set_bool(lt::settings_pack::enable_lsd, false);
    sp.set_bool(lt::settings_pack::enable_upnp, false); sp.set_bool(lt::settings_pack::enable_natpmp, false);
    sp.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
    sp.set_int(lt::settings_pack::out_enc_policy, lt::settings_pack::pe_disabled); sp.set_int(lt::settings_pack::in_enc_policy, lt::settings_pack::pe_disabled);
    sp.set_int(lt::settings_pack::alert_mask, 0);
    if (upload_limit > 0) sp.set_int(lt::settings_pack::upload_rate_limit, upload_limit);
    ses = std::make_unique<lt::session>(lt::session_params(sp));
    lt::add_torrent_params atp;
    atp.ti = std::make_shared<lt::torrent_info>(p.torrent_bytes.data(), static_cast<int>(p.torrent_bytes.size()));
    atp.save_path = p.root.string();
    atp.flags |= lt::torrent_flags::seed_mode;
    h = ses->add_torrent(atp);
    for (int i = 0; i < 500 && h.status().state != lt::torrent_status::seeding; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::string endpoint() const { return "127.0.0.1:" + std::to_string(port); }
};

inline tp::EngineConfig engine_config(const fs::path& cache_root, int port) {
  tp::EngineConfig c;
  c.cache_root = cache_root; c.listen_interfaces = "127.0.0.1:" + std::to_string(port);
  c.enable_dht = false; c.enable_lsd = false; c.enable_upnp = false; c.enable_natpmp = false; c.loopback_test_mode = true; c.min_reconnect_time_s = 1;
  return c;
}

struct PlayerStats {
  std::int64_t bytes = 0; int mismatches = 0, seeks = 0, seek_timeouts = 0, stalls = 0;
  double max_stall = 0, total_stall = 0;
  std::vector<double> seek_latency;
};

// Reads like a demuxer: container-index reads at the tail and head first, then sequential paced reads. Verifies EVERY byte against the payload.
class Player {
 public:
  Player(tp::StreamSession& s, const Payload& p, double bytes_per_second) : s_(s), p_(p), bps_(bytes_per_second) {}
  void open_like_demuxer() { read_one(p_.size - 65536); read_one(0); pos_ = 65536; }
  // Plays for `secs` of wall time (reads paced at bps). Returns false if a single read took longer than `max_read_s` (stuck).
  bool play(double secs, double max_read_s = 60.0) {
    const auto end = Clock::now() + std::chrono::duration<double>(secs);
    while (Clock::now() < end && pos_ < p_.size) {
      const auto t0 = Clock::now();
      std::int64_t n = read_one(pos_);
      if (n < 0) return false;
      const double took = secs_(Clock::now() - t0);
      if (took > max_read_s) return false;
      if (took > 0.25) { ++st.stalls; st.total_stall += took; st.max_stall = std::max(st.max_stall, took); }
      pos_ += n;
      const double budget = static_cast<double>(n) / bps_;                  // pace to the bitrate
      if (took < budget) std::this_thread::sleep_for(std::chrono::duration<double>(budget - took));
    }
    return true;
  }
  // Seek: hint + read at the target; returns the latency until the first verified chunk, or -1 on timeout.
  double seek(std::int64_t off, double timeout_s = 60.0) {
    ++st.seeks; pos_ = std::max<std::int64_t>(0, std::min(off, p_.size - 1));
    s_.seek_hint(pos_);
    const auto t0 = Clock::now();
    std::atomic<bool> cancel{false};
    std::thread killer([&] { const auto dl = Clock::now() + std::chrono::duration<double>(timeout_s); while (!cancel && Clock::now() < dl) std::this_thread::sleep_for(std::chrono::milliseconds(20)); cancel = true; });
    std::int64_t n = read_one(pos_, &cancel);
    const bool timed_out = cancel.load() && n < 0;
    cancel = true; killer.join();
    if (n < 0) { ++st.seek_timeouts; return timed_out ? -1 : -1; }
    pos_ += n;
    const double l = secs_(Clock::now() - t0); st.seek_latency.push_back(l); return l;
  }
  std::int64_t position() const { return pos_; }
  PlayerStats st;
 private:
  static double secs_(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
  std::int64_t read_one(std::int64_t off, const std::atomic<bool>* cancel = nullptr) {
    std::vector<char> buf(256 * 1024);
    std::int64_t n = s_.read(off, buf.data(), static_cast<std::int64_t>(buf.size()), cancel);
    if (n > 0) { st.bytes += n; if (!p_.matches(off, buf.data(), n)) { ++st.mismatches; ITLOG("DATA MISMATCH at offset %lld len %lld", (long long)off, (long long)n); } }
    return n;
  }
  tp::StreamSession& s_;
  const Payload& p_;
  double bps_;
  std::int64_t pos_ = 0;
};

inline double percentile(std::vector<double> v, double q) { if (v.empty()) return -1; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))]; }

inline fs::path data_dir() { fs::path p = fs::path(TP_BUILD_DIR) / "it_data"; fs::create_directories(p); return p; }
inline fs::path fresh_dir(const std::string& name) { fs::path p = fs::path(TP_BUILD_DIR) / "it_run" / name; fs::remove_all(p); fs::create_directories(p); return p; }

}  // namespace tpit
