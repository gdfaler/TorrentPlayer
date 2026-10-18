// tpcore/engine.hpp: libtorrent session + custom segment disk + alert dispatch. One Engine per process.
#pragma once
#include <libtorrent/session.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "tpcore/config.hpp"
#include "tpcore/diagnostics.hpp"
#include "tpcore/segment_store.hpp"
#include "tpcore/web_seed_policy.hpp"

namespace tp {

class StreamSession;

struct EngineConfig {
  std::filesystem::path cache_root;                 // per-torrent cache directories live here
  std::string listen_interfaces = "0.0.0.0:6881,[::]:6881";
  bool enable_dht = true, enable_lsd = true, enable_upnp = false, enable_natpmp = false;
  bool loopback_test_mode = false;                  // allow multiple connections per IP, disable encryption (tests only)
  int min_reconnect_time_s = 1;                     // verified necessary (default 60 s stalled playback)
  int download_rate_limit = 0;                      // bytes/s, 0 = unlimited
  int upload_rate_limit = 0;
  std::filesystem::path evidence_dir;               // hash-failure evidence (default cache_root/evidence)
};

struct TorrentSource {
  std::string torrent_file;                         // path to a .torrent file, or
  std::vector<char> torrent_bytes;                  // raw .torrent contents, or
  std::string magnet_uri;                           // magnet link (metadata fetched first)
  std::vector<std::string> extra_peers;             // "ip:port" initial peers (tests / known peers / magnet x.pe)
  std::vector<std::string> trackers;                // announce URLs that are not inside the torrent bytes (magnet tr=)
  std::vector<std::string> web_seed_urls;           // web seeds that are not inside the torrent bytes (magnet ws=)
  bool strip_web_seeds = false;                     // remove the torrent's web seeds regardless of policy (tests)
  bool strip_trackers = false;                      // peers only from extra_peers / DHT (tests)
  bool start_paused = false;                        // add the torrent paused (recovery is audited before any torrent activity); call StreamSession::resume()
};

// Disk-layer hooks registered per torrent (internal, exposed for tests).
struct TorrentStorage {
  std::shared_ptr<SegmentStore> store;
  std::shared_ptr<ShadowVerifier> shadow;
  std::vector<Sha1> expected;                       // per-piece SHA-1 (v1 hashes)
  std::atomic<int> hash_calls{0};
  std::function<void(int piece, bool ok)> on_hash;  // optional test hook
  // fault injection (tests only)
  std::atomic<int> inject_arrival_every{0}, inject_storage_every{0}, inject_overalloc_pct{0};
  std::atomic<int> write_counter{0}, injected_arrival{0}, injected_storage{0};
};

// Magnet metadata fetch that the UI can observe (peers found, elapsed) and cancel. Additive; fetch_metadata() above keeps working.
struct MetadataJob {
  lt::torrent_handle handle;
  std::string info_hash_hex;
  std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
  std::atomic<bool> finished{false};
};
struct MetadataPoll { bool ready = false; int peers = 0; double elapsed_s = 0; std::vector<char> bytes; };

class Engine {
 public:
  explicit Engine(EngineConfig cfg);
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Fetch metadata for a magnet link (blocks up to timeout). Returns raw .torrent bytes, or empty on timeout.
  std::vector<char> fetch_metadata(const std::string& magnet_uri, std::chrono::seconds timeout);

  std::shared_ptr<MetadataJob> start_metadata(const std::string& magnet_uri, std::string* error = nullptr);
  MetadataPoll poll_metadata(MetadataJob& job);                 // non-blocking; bytes filled once when ready
  void cancel_metadata(MetadataJob& job);                       // idempotent

  // Open a torrent for streaming one file (file_index < 0 selects the largest file). Blocks only for local setup.
  std::shared_ptr<StreamSession> open_stream(const TorrentSource& src, int file_index, const CacheConfig& cache, std::string* error = nullptr);

  lt::session& session() { return *ses_; }
  const EngineConfig& config() const { return cfg_; }
  // internal: used by StreamSession / disk layer
  void register_storage(const std::string& save_path, std::shared_ptr<TorrentStorage> s);
  void unregister_storage(const std::string& save_path);
  std::shared_ptr<TorrentStorage> find_storage(const std::string& save_path);
  void register_session(const std::string& info_hash_hex, StreamSession* s);
  void unregister_session(const std::string& info_hash_hex);

 private:
  void alert_loop();
  void diag_alert(lt::alert* a);                    // TP_NET_DIAG only
  std::ofstream net_diag_;
  std::chrono::steady_clock::time_point diag_t0_ = std::chrono::steady_clock::now();
  EngineConfig cfg_;
  std::unique_ptr<lt::session> ses_;
  std::mutex reg_mu_;
  std::map<std::string, std::shared_ptr<TorrentStorage>> storages_;
  std::map<std::string, StreamSession*> sessions_;
  std::atomic<bool> stop_{false};
  std::thread alert_th_;
  // metadata fetch support
  std::mutex meta_mu_;
  std::condition_variable meta_cv_;
  std::map<std::string, std::vector<char>> meta_;
};

}  // namespace tp
