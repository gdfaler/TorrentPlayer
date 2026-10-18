// tpcore/stream_session.hpp: scheduler + blocking reader for ONE streamed file (spec sections 4, 6, 8).
#pragma once
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "tpcore/disk_policy.hpp"
#include "tpcore/engine.hpp"
#include "tpcore/window_planner.hpp"

namespace tp {

enum class StreamState { Connecting, NoPeers, Preparing, Buffering, Ready, Stalled, DiskPaused, Error };
const char* to_string(StreamState s);

struct StreamStatus {
  StreamState state = StreamState::Connecting;
  std::string detail;
  int peers = 0, seeds = 0, web_connections = 0;
  double download_rate = 0, upload_rate = 0;
  std::int64_t web_bytes = 0, peer_bytes = 0;
  int head = 0, lo = 0, hi_full = 0, hi_wanted = 0, contiguous_ahead = 0;
  bool refilling = true;
  int blocked_readers = 0;
  StoreMetrics store;
  DiskState disk_state = DiskState::Ok;
  int connect_kicks = 0, watchdog_resets = 0;
  int un_have_resets = 0, stale_have_detections = 0, file_errors = 0, enospc_events = 0, peer_bans = 0;
  int hash_failures = 0, hash_storage = 0, hash_arrived = 0, hash_undetermined = 0, pieces_verified = 0;
  bool web_seed_active = false;
  double seconds_without_peers = 0;
};

class StreamSession {
 public:
  ~StreamSession();
  StreamSession(const StreamSession&) = delete;
  StreamSession& operator=(const StreamSession&) = delete;

  std::int64_t file_size() const { return geo_.file_size; }
  const std::string& file_name() const { return file_name_; }
  int num_pieces() const { return num_pieces_; }
  std::int64_t piece_size() const { return geo_.piece_size; }
  int piece_of_file_offset(std::int64_t off) const { return static_cast<int>((geo_.file_offset + off) / geo_.piece_size); }
  bool has_file_offset(std::int64_t off) { return store_->has(piece_of_file_offset(off)); }

  // Blocking read of the streamed file. Returns bytes copied (> 0), 0 at EOF, -1 if cancelled. Never copies across a piece boundary.
  std::int64_t read(std::int64_t file_offset, void* buf, std::int64_t len, const std::atomic<bool>* cancel);
  void seek_hint(std::int64_t file_offset);                 // the application knows where the user is going
  void set_bitrate(double bytes_per_second);                // refines window sizing
  void set_free_space_probe(std::function<std::int64_t()> probe);   // tests inject free space; default: statfs of the cache directory
  void set_disk_policy(const DiskPolicyConfig& c);
  void set_web_seed_mode(WebSeedMode m);

  StreamStatus status() const;
  std::vector<std::string> recent_events() const;          // bounded log: bans, ENOSPC, resets, state changes
  SegmentStore& store() { return *store_; }
  ShadowVerifier& shadow() { return *shadow_; }
  TorrentStorage& storage() { return *storage_; }
  const RecoveryReport& recovery_report() const { return recovery_; }
  void resume() { paused_by_user_start_ = false; h_.resume(); }   // after TorrentSource::start_paused
  void request_un_have_reset() { reset_requested_ = true; }  // tests / diagnostics
  lt::torrent_handle handle() const { return h_; }

 private:
  friend class Engine;
  StreamSession(Engine& e, std::shared_ptr<lt::torrent_info> ti, int file_index, CacheConfig cfg, const TorrentSource& src);
  void start(const TorrentSource& src);
  void tick();
  void scheduler_loop();
  void apply_priorities(const WindowPlan& plan, int demand_piece);
  void do_reset_have();
  void on_alert(lt::alert* a);                             // called from the engine alert thread
  void update_known_peers();
  void update_traffic_split();
  std::vector<std::int64_t> window_deadlines_;
  Engine& eng_;
  std::shared_ptr<lt::torrent_info> ti_;
  CacheConfig cfg_;
  StreamGeometry geo_;
  std::string file_name_, save_path_, ihex_;
  int file_index_ = 0, num_pieces_ = 0;
  std::shared_ptr<TorrentStorage> storage_;
  std::shared_ptr<SegmentStore> store_;
  std::shared_ptr<ShadowVerifier> shadow_;
  RecoveryReport recovery_;
  WindowPlanner planner_;
  PlayheadEstimator est_;
  mutable std::mutex est_mu_;
  lt::torrent_handle h_;
  std::thread th_;
  std::atomic<bool> stop_{false};
  // reader / scheduler shared state
  std::atomic<int> blocked_{0}, demand_piece_{-1};
  std::atomic<bool> reset_requested_{false}, resetting_{false};
  std::atomic<int> resets_{0}, stale_{0}, file_errors_{0}, enospc_{0}, bans_{0};
  mutable std::mutex status_mu_;
  StreamStatus status_;
  std::vector<lt::tcp::endpoint> known_peers_;
  std::mutex known_mu_;
  std::function<std::int64_t()> free_probe_;
  std::mutex cfg_mu_;
  DiskPolicy disk_policy_;
  WebSeedPolicy web_policy_;
  WebSeedPolicyConfig web_cfg_;
  bool strip_web_always_ = false;
  std::chrono::steady_clock::time_point t0_, last_reset_, last_peer_seen_;
  std::vector<std::string> web_urls_, trackers_;
  std::atomic<std::int64_t> web_bytes_{0}, peer_bytes_{0};
  std::atomic<int> web_conns_{0};
  std::int64_t last_web_total_ = 0, last_peer_total_ = 0;
  bool paused_by_disk_ = false;
  std::atomic<bool> paused_by_user_start_{false};
  double nopeer_since_ = -1, last_kick_ = -100;
  std::atomic<int> kicks_{0}, wd_resets_{0};
  void connectivity_watchdog(const lt::torrent_status& s, bool need_data, double t);
  std::vector<std::string> web_ips_;
  std::mutex ips_mu_;
  std::int64_t prio_sig_ = -1;
  mutable std::mutex ev_mu_;
  std::vector<std::string> events_;
  void log_event(const std::string& s);
  StreamState compute_state(const lt::torrent_status& st, int demand_blocked, double since_peer);
  std::chrono::steady_clock::time_point last_data_;
  std::chrono::steady_clock::time_point last_prio_;
};

}  // namespace tp
