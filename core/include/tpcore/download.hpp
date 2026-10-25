// tpcore/download.hpp: "Download & Play" engine. Downloads the selected files of a torrent completely, hash-verified, into a user-chosen folder with libtorrent's
// ordinary file storage (the streaming Engine uses a bounded segment cache and is not touched). State is persisted so an interrupted download resumes after a restart.
// One DownloadManager per process; it owns its own lt::session (own listen port) and one entry per torrent info-hash (adding the same torrent twice returns the same entry).
#pragma once
#include <libtorrent/session.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tp {

enum class DlState { Checking, Downloading, WaitingForNetwork, WaitingForSources, PausedByUser, Resuming, Completed, Failed };
const char* dl_state_name(DlState s);

struct DownloadConfig {
  std::filesystem::path state_dir;                          // per-download folders: meta.torrent, state.bencode, resume.dat
  std::string listen_interfaces = "0.0.0.0:6882,[::]:6882"; // not the streaming Engine's port
  bool enable_dht = true, enable_lsd = true, enable_upnp = false, enable_natpmp = false;
  bool loopback_test_mode = false;                          // multiple connections per IP, no encryption, (tests only)
  int min_reconnect_time_s = 5;
  int upload_rate_limit = 0, download_rate_limit = 0;       // bytes/s, 0 = unlimited (tests use them to slow transfers down)
  bool auto_resume_on_network = true;                       // "Automatically resume downloads when the network returns"
  std::function<std::int64_t(const std::filesystem::path&)> free_space;   // test hook; default std::filesystem::space
  int resume_save_interval_s = 20;                          // periodic fastresume writes (also on pause, completion, network changes and shutdown)
};

struct DownloadRequest {
  std::vector<char> torrent_bytes;                          // complete .torrent (from a file, or the metadata fetched for a magnet link)
  std::filesystem::path save_path;                          // destination folder; the torrent's own folder structure is created inside it
  std::vector<int> files;                                   // selected file indices; empty = the whole torrent
  std::vector<std::string> extra_peers;                     // "ip:port" (tests / magnet x.pe); re-dialled while no source is connected
  std::vector<std::string> trackers;                        // announce URLs not inside the torrent (magnet tr=)
  std::vector<std::string> web_seed_urls;
  bool auto_play = true;
  bool start_paused = false;                                // starts as "paused by user"
  bool strip_trackers = false;                              // tests: no tracker/DHT discovery
  // internal, set by restore(): what the persisted state already knows
  bool restoring = false;
  std::vector<int> known_preexisting;                       // files that were on disk before this download first started (never deleted by this download)
};

struct DownloadStatus {
  std::string id;                                           // info-hash (hex)
  std::string name;                                         // torrent name
  DlState state = DlState::Checking;
  std::string error;                                        // set when state == Failed
  std::string save_path;                                    // destination folder
  std::vector<int> files;                                   // selected indices (all files if the whole torrent)
  std::string primary_file;                                 // absolute path of the first selected video-like file (or the first selected file)
  std::int64_t selected_bytes = 0;                          // total size of the selected files
  std::int64_t verified_bytes = 0;                          // bytes of selected files that belong to hash-verified pieces
  double progress = 0;                                      // verified_bytes / selected_bytes
  int download_rate = 0, upload_rate = 0;                   // bytes/s
  int peers = 0, web_seeds = 0;                             // connected sources
  std::int64_t eta_s = -1;                                  // -1: unknown (never invented)
  bool user_paused = false, network_available = true, auto_play = true, completed = false;
  double checking_progress = 0;                             // 0..1 while Checking
};

class DownloadManager {
 public:
  explicit DownloadManager(DownloadConfig cfg);
  ~DownloadManager();                                       // graceful: saves resume data, then closes the session
  DownloadManager(const DownloadManager&) = delete;
  DownloadManager& operator=(const DownloadManager&) = delete;

  // Adds (or returns the existing) download. Validates the destination (creatable, writable) and the free space for the selected files.
  std::string add(const DownloadRequest& req, std::string* error = nullptr);          // returns the id, or "" on error
  int restore();                                                                       // re-adds every persisted incomplete download; returns how many entries exist
  std::vector<DownloadStatus> list();
  bool status(const std::string& id, DownloadStatus* out);
  bool pause(const std::string& id);                                                   // explicit user pause: persisted, never overridden by network changes
  bool resume(const std::string& id);                                                  // explicit user resume (also retries a failed download)
  bool cancel(const std::string& id, bool delete_files);                               // removes the entry; files stay unless delete_files
  void set_network_available(bool online);                                             // fed by the OS path monitor (or tests); coalesced and bounded
  void set_auto_resume_on_network(bool on);
  void save_all();                                                                     // flush resume data now
  int network_restores_applied() const { return restores_applied_.load(); }            // diagnostics / tests
  int live_torrents();                                                                 // torrents in the libtorrent session (must equal the number of active entries)
  lt::session& session() { return *ses_; }

 private:
  struct Entry;
  void alert_loop();
  void handle_alert(lt::alert* a);
  void tick();
  std::shared_ptr<Entry> add_locked(const DownloadRequest& req, const std::string& id, bool restoring, std::string* error);
  void persist_state(Entry& e);
  void request_resume_data(Entry& e);
  void fill_status(Entry& e, DownloadStatus* out);
  std::filesystem::path dir_of(const std::string& id) const { return cfg_.state_dir / id; }
  bool verified_complete(Entry& e);
  void apply_network_restore();

  DownloadConfig cfg_;
  std::unique_ptr<lt::session> ses_;
  std::mutex mu_;
  std::map<std::string, std::shared_ptr<Entry>> entries_;
  std::atomic<bool> stop_{false};
  std::thread th_;
  std::atomic<bool> online_{true};
  std::atomic<int> restores_applied_{0};
  std::chrono::steady_clock::time_point last_restore_{}, last_save_ = std::chrono::steady_clock::now();
  bool restore_pending_ = false;
};

}  // namespace tp
