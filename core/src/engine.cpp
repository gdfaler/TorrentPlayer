#include "tpcore/engine.hpp"
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/disk_buffer_holder.hpp>
#include <libtorrent/disk_interface.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/storage_defs.hpp>
#include <libtorrent/torrent_info.hpp>
#include <boost/asio/post.hpp>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tpcore/stream_session.hpp"

namespace fs = std::filesystem;
namespace tp {

static std::string hex_of(const lt::sha1_hash& h) {
  static const char* d = "0123456789abcdef";
  std::string r;
  for (int i = 0; i < 20; ++i) { auto u = static_cast<unsigned char>(h[i]); r += d[u >> 4]; r += d[u & 15]; }
  return r;
}

static int sidx(lt::storage_index_t i) { return static_cast<int>(static_cast<std::uint32_t>(i)); }

// =============================================================== custom disk
class SegmentDisk final : public lt::disk_interface, public lt::buffer_allocator_interface {
 public:
  SegmentDisk(lt::io_context& ioc, Engine& e) : ioc_(ioc), eng_(e) {}

  lt::storage_holder new_torrent(lt::storage_params const& p, std::shared_ptr<void> const&) override {
    std::lock_guard<std::mutex> g(mu_);
    const int idx = static_cast<int>(slots_.size());
    slots_.push_back(eng_.find_storage(std::string(p.path)));    // nullptr for unregistered (metadata-only) torrents
    return lt::storage_holder(lt::storage_index_t(idx), *this);
  }
  void remove_torrent(lt::storage_index_t i) override { std::lock_guard<std::mutex> g(mu_); if (sidx(i) < int(slots_.size())) slots_[sidx(i)].reset(); }

  void async_read(lt::storage_index_t st, lt::peer_request const& r, std::function<void(lt::disk_buffer_holder, lt::storage_error const&)> h, lt::disk_job_flags_t) override {
    auto ts = slot(st);
    char* buf = static_cast<char*>(std::malloc(static_cast<std::size_t>(r.length)));
    int n = ts ? ts->store->read(int(r.piece), r.start, buf, r.length) : -1;
    lt::storage_error se;
    if (n < 0) {                                   // evicted / absent piece asked for by a peer: harmless read error (spec F10)
      std::free(buf);
      se.ec = boost::system::errc::make_error_code(boost::system::errc::no_such_file_or_directory);
      se.operation = lt::operation_t::file_read;
      boost::asio::post(ioc_, [h, se] { h(lt::disk_buffer_holder(), se); });
      return;
    }
    auto holder = std::make_shared<lt::disk_buffer_holder>(*this, buf);
    boost::asio::post(ioc_, [h, se, holder]() mutable { h(std::move(*holder), se); });
  }

  bool async_write(lt::storage_index_t st, lt::peer_request const& r, char const* buf, std::shared_ptr<lt::disk_observer>,
                   std::function<void(lt::storage_error const&)> h, lt::disk_job_flags_t) override {
    auto ts = slot(st);
    lt::storage_error se;
    if (!ts) { se.ec = boost::system::errc::make_error_code(boost::system::errc::io_error); se.operation = lt::operation_t::file_write; }
    else {
      const int piece = int(r.piece);
      std::vector<char> tmp;
      const int c = ++ts->write_counter;
      if (ts->inject_arrival_every.load() && c % ts->inject_arrival_every.load() == 0) { tmp.assign(buf, buf + r.length); tmp[r.length / 2] ^= 0x5a; buf = tmp.data(); ++ts->injected_arrival; }
      ts->shadow->on_write(piece, r.start, buf, r.length);                   // what libtorrent handed us (after any injected arrival fault)
      WriteStatus ws = ts->store->write(piece, r.start, buf, r.length);
      if (ws == WriteStatus::NoSpace) { se.ec = boost::system::errc::make_error_code(boost::system::errc::no_space_on_device); se.operation = lt::operation_t::file_write; }
      else if (ws == WriteStatus::IoError) { se.ec = boost::system::errc::make_error_code(boost::system::errc::io_error); se.operation = lt::operation_t::file_write; }
      else {
        if (ts->inject_storage_every.load() && c % ts->inject_storage_every.load() == 0) {      // INJECT: storage flips a byte after the write
          int fd = ::open(ts->store->piece_path(piece).c_str(), O_RDWR);
          if (fd >= 0) { char b = 0; (void)::pread(fd, &b, 1, r.start); b ^= 0x33; (void)::pwrite(fd, &b, 1, r.start); ::close(fd); ++ts->injected_storage; }
        }
        const int pct = ts->inject_overalloc_pct.load();                                         // INJECT: allocation beyond EOF (models observed APFS behaviour)
        if (pct && (std::rand() % 100) < pct) {
          int fd = ::open(ts->store->piece_path(piece).c_str(), O_RDWR);
          if (fd >= 0) { struct stat s2; ::fstat(fd, &s2); long long beyond = (long long)s2.st_blocks * 512 - (long long)s2.st_size;
            if (beyond < 49152) { fstore_t f = {F_ALLOCATEALL, F_PEOFPOSMODE, 0, (off_t)(49152 - beyond), 0}; ::fcntl(fd, F_PREALLOCATE, &f); } ::close(fd); }
        }
      }
    }
    boost::asio::post(ioc_, [h, se] { h(se); });
    return false;
  }

  void async_hash(lt::storage_index_t st, lt::piece_index_t piece, lt::span<lt::sha256_hash>, lt::disk_job_flags_t,
                  std::function<void(lt::piece_index_t, lt::sha1_hash const&, lt::storage_error const&)> h) override {
    auto ts = slot(st);
    lt::sha1_hash out;                                // all-zero = "missing / mismatch" (NOT an I/O error)
    const int pc = int(piece);
    if (ts && pc < int(ts->expected.size())) {
      ++ts->hash_calls;
      const Sha1& exp = ts->expected[pc];
      if (ts->store->has(pc)) { std::memcpy(out.data(), exp.data(), 20); }          // verified earlier by us: trusted (used by recheck fallback)
      else {
        // Assemble the logical piece: real data from the cache, BEP 47 padding ranges as implicit zeros (they are never downloaded or stored).
        // Incomplete (real data missing) is NOT a hash failure: the all-zero result below tells libtorrent the piece is not available yet, and the store counts it.
        std::vector<char> bytes;
        if (ts->store->logical_piece(pc, /*live=*/true, bytes) == HashStatus::Ok) {
          const Sha1 got = sha1_of(bytes.data(), bytes.size());
          std::memcpy(out.data(), got.data(), 20);
          if (got == exp) {
            if (ts->store->mark_verified(pc)) { ts->shadow->on_pass(pc, bytes); if (ts->on_hash) ts->on_hash(pc, true); }
            else std::memset(out.data(), 0, 20);          // evicted concurrently: report "missing" (zero hash), not a failure
          }
          else { ts->shadow->on_failure(pc, bytes, exp); ts->store->forget_written(pc); if (ts->on_hash) ts->on_hash(pc, false); }   // genuine mismatch: libtorrent re-downloads the piece
        }
      }
    }
    boost::asio::post(ioc_, [h, piece, out] { h(piece, out, lt::storage_error()); });
  }
  void async_hash2(lt::storage_index_t, lt::piece_index_t piece, int, lt::disk_job_flags_t, std::function<void(lt::piece_index_t, lt::sha256_hash const&, lt::storage_error const&)> h) override {
    boost::asio::post(ioc_, [h, piece] { h(piece, lt::sha256_hash(), lt::storage_error()); });
  }
  void async_move_storage(lt::storage_index_t, std::string p, lt::move_flags_t, std::function<void(lt::status_t, std::string const&, lt::storage_error const&)> h) override {
    boost::asio::post(ioc_, [h, p] { h(lt::status_t{}, p, lt::storage_error()); });
  }
  void async_release_files(lt::storage_index_t, std::function<void()> h) override { if (h) boost::asio::post(ioc_, h); }
  void async_check_files(lt::storage_index_t, lt::add_torrent_params const*, lt::aux::vector<std::string, lt::file_index_t>, std::function<void(lt::status_t, lt::storage_error const&)> h) override {
    boost::asio::post(ioc_, [h] { h(lt::status_t{}, lt::storage_error()); });       // resume state (have_pieces) comes from the SegmentStore; no full check
  }
  void async_stop_torrent(lt::storage_index_t, std::function<void()> h) override { if (h) boost::asio::post(ioc_, h); }
  void async_rename_file(lt::storage_index_t, lt::file_index_t i, std::string n, std::function<void(std::string const&, lt::file_index_t, lt::storage_error const&)> h) override {
    boost::asio::post(ioc_, [h, i, n] { h(n, i, lt::storage_error()); });
  }
  void async_delete_files(lt::storage_index_t, lt::remove_flags_t, std::function<void(lt::storage_error const&)> h) override {
    boost::asio::post(ioc_, [h] { h(lt::storage_error()); });                          // the cache is deleted only by explicit application action
  }
  void async_set_file_priority(lt::storage_index_t, lt::aux::vector<lt::download_priority_t, lt::file_index_t> prio, std::function<void(lt::storage_error const&, lt::aux::vector<lt::download_priority_t, lt::file_index_t>)> h) override {
    boost::asio::post(ioc_, [h, prio] { h(lt::storage_error(), prio); });
  }
  void async_clear_piece(lt::storage_index_t st, lt::piece_index_t index, std::function<void(lt::piece_index_t)> h) override {
    if (auto ts = slot(st)) { ts->store->evict(int(index)); }                           // hash failure: drop the bad piece file
    boost::asio::post(ioc_, [h, index] { h(index); });
  }
  void update_stats_counters(lt::counters&) const override {}
  std::vector<lt::open_file_state> get_status(lt::storage_index_t) const override { return {}; }
  void abort(bool) override {}
  void submit_jobs() override {}
  void settings_updated() override {}
  void free_disk_buffer(char* b) override { std::free(b); }
  void free_multiple_buffers(lt::span<char*> bufs) override { for (auto b : bufs) std::free(b); }

 private:
  std::shared_ptr<TorrentStorage> slot(lt::storage_index_t st) { std::lock_guard<std::mutex> g(mu_); int i = sidx(st); return i < int(slots_.size()) ? slots_[i] : nullptr; }
  lt::io_context& ioc_;
  Engine& eng_;
  std::mutex mu_;
  std::vector<std::shared_ptr<TorrentStorage>> slots_;
};

// =============================================================== Engine
Engine::Engine(EngineConfig cfg) : cfg_(std::move(cfg)) {
  fs::create_directories(cfg_.cache_root);
  if (cfg_.evidence_dir.empty()) cfg_.evidence_dir = cfg_.cache_root / "evidence";
  lt::settings_pack sp;
  sp.set_str(lt::settings_pack::listen_interfaces, cfg_.listen_interfaces);
  sp.set_bool(lt::settings_pack::enable_dht, cfg_.enable_dht);
  sp.set_bool(lt::settings_pack::enable_lsd, cfg_.enable_lsd);
  sp.set_bool(lt::settings_pack::enable_upnp, cfg_.enable_upnp);
  sp.set_bool(lt::settings_pack::enable_natpmp, cfg_.enable_natpmp);
  sp.set_int(lt::settings_pack::min_reconnect_time, cfg_.min_reconnect_time_s);
  if (cfg_.download_rate_limit > 0) sp.set_int(lt::settings_pack::download_rate_limit, cfg_.download_rate_limit);
  if (cfg_.upload_rate_limit > 0) sp.set_int(lt::settings_pack::upload_rate_limit, cfg_.upload_rate_limit);
  if (cfg_.web_seed_max_request_bytes > 0) sp.set_int(lt::settings_pack::urlseed_max_request_bytes, cfg_.web_seed_max_request_bytes);
  if (cfg_.loopback_test_mode) {
    sp.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
    sp.set_int(lt::settings_pack::out_enc_policy, lt::settings_pack::pe_disabled);
    sp.set_int(lt::settings_pack::in_enc_policy, lt::settings_pack::pe_disabled);
  }
  {
    lt::alert_category_t mask = lt::alert_category::error | lt::alert_category::status | lt::alert_category::storage |
                                lt::alert_category::peer | lt::alert_category::connect | lt::alert_category::block_progress;
    // DIAGNOSTIC ONLY (default off): TP_NET_DIAG=<file> logs the peer-discovery / connection stages (tracker, DHT, LSD, connect, disconnect, metadata alerts).
    // It only adds alert categories and a log file; no session setting that affects behaviour is changed.
    if (const char* f = std::getenv("TP_NET_DIAG")) { if (*f) { net_diag_.open(f, std::ios::app); mask |= lt::alert_category::tracker | lt::alert_category::dht; } }
    sp.set_int(lt::settings_pack::alert_mask, mask);
  }
  lt::session_params sparams(sp);
  sparams.disk_io_constructor = [this](lt::io_context& ioc, lt::settings_interface const&, lt::counters&) {
    return std::unique_ptr<lt::disk_interface>(new SegmentDisk(ioc, *this));
  };
  ses_ = std::make_unique<lt::session>(std::move(sparams));
  alert_th_ = std::thread([this] { alert_loop(); });
}

Engine::~Engine() {
  stop_ = true;
  if (alert_th_.joinable()) alert_th_.join();
  ses_.reset();
}

void Engine::register_storage(const std::string& p, std::shared_ptr<TorrentStorage> s) { std::lock_guard<std::mutex> g(reg_mu_); storages_[p] = std::move(s); }
void Engine::unregister_storage(const std::string& p) { std::lock_guard<std::mutex> g(reg_mu_); storages_.erase(p); }
std::shared_ptr<TorrentStorage> Engine::find_storage(const std::string& p) { std::lock_guard<std::mutex> g(reg_mu_); auto it = storages_.find(p); return it == storages_.end() ? nullptr : it->second; }
void Engine::register_session(const std::string& h, StreamSession* s) { std::lock_guard<std::mutex> g(reg_mu_); sessions_[h] = s; }
void Engine::unregister_session(const std::string& h) { std::lock_guard<std::mutex> g(reg_mu_); sessions_.erase(h); }

void Engine::diag_alert(lt::alert* a) {
  static const char* keep[] = {"tracker_", "dht_", "peer_connect", "peer_disconnect", "incoming_connection", "peer_error", "lsd_peer", "listen_", "metadata_", "external_ip", "add_torrent", "torrent_error",
                               "session_error", "peer_blocked", "udp_error", "scrape_", "portmap", "torrent_added", "state_changed"};
  const std::string w = a->what();
  bool ok = false; for (auto* k : keep) if (w.rfind(k, 0) == 0) { ok = true; break; }
  if (!ok) return;
  const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - diag_t0_).count();
  char b[32]; std::snprintf(b, sizeof b, "[%8.2f] ", t);
  net_diag_ << b << w << ": " << a->message() << "\n"; net_diag_.flush();
}

void Engine::alert_loop() {
  while (!stop_) {
    std::vector<lt::alert*> v;
    ses_->wait_for_alert(std::chrono::milliseconds(50));
    ses_->pop_alerts(&v);
    for (auto* a : v) {
      if (net_diag_.is_open()) diag_alert(a);
      if (auto* m = lt::alert_cast<lt::metadata_received_alert>(a)) {          // metadata-only fetch support
        auto ti = m->handle.torrent_file();
        if (ti) {
          lt::create_torrent ct(*ti);
          std::vector<char> out; lt::bencode(std::back_inserter(out), ct.generate());
          { std::lock_guard<std::mutex> g(meta_mu_); meta_[hex_of(m->handle.info_hashes().get_best())] = out; }
          meta_cv_.notify_all();
        }
        continue;
      }
      lt::torrent_handle th;
      if (auto* ta = dynamic_cast<lt::torrent_alert*>(a)) th = ta->handle;
      if (!th.is_valid()) continue;
      StreamSession* s = nullptr;
      { std::lock_guard<std::mutex> g(reg_mu_); auto it = sessions_.find(hex_of(th.info_hashes().get_best())); if (it != sessions_.end()) s = it->second; }
      if (s) s->on_alert(a);
    }
  }
}

std::vector<char> Engine::fetch_metadata(const std::string& magnet, std::chrono::seconds timeout) {
  lt::error_code ec;
  lt::add_torrent_params atp = lt::parse_magnet_uri(magnet, ec);
  if (ec) return {};
  atp.save_path = (cfg_.cache_root / ".meta").string();
  atp.flags |= lt::torrent_flags::upload_mode;
  const std::string ih = hex_of(atp.info_hashes.get_best());
  auto h = ses_->add_torrent(atp);
  std::vector<char> out;
  {
    std::unique_lock<std::mutex> lk(meta_mu_);
    meta_cv_.wait_for(lk, timeout, [&] { return meta_.count(ih) > 0; });
    auto it = meta_.find(ih);
    if (it != meta_.end()) { out = it->second; meta_.erase(it); }
  }
  ses_->remove_torrent(h);
  return out;
}

std::shared_ptr<MetadataJob> Engine::start_metadata(const std::string& magnet, std::string* error) {
  lt::error_code ec;
  lt::add_torrent_params atp = lt::parse_magnet_uri(magnet, ec);
  if (ec) { if (error) *error = "not a valid magnet link: " + ec.message(); return nullptr; }
  atp.save_path = (cfg_.cache_root / ".meta").string();
  atp.flags |= lt::torrent_flags::upload_mode;
  auto job = std::make_shared<MetadataJob>();
  job->info_hash_hex = hex_of(atp.info_hashes.get_best());
  job->handle = ses_->add_torrent(atp);
  return job;
}

MetadataPoll Engine::poll_metadata(MetadataJob& job) {
  MetadataPoll r;
  r.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - job.started).count();
  { std::lock_guard<std::mutex> g(meta_mu_);
    auto it = meta_.find(job.info_hash_hex);
    if (it != meta_.end()) { r.bytes = it->second; r.ready = true; meta_.erase(it); } }
  if (!r.ready && job.handle.is_valid()) { try { r.peers = job.handle.status().num_peers; } catch (std::exception const&) {} }
  if (r.ready) cancel_metadata(job);
  return r;
}

void Engine::cancel_metadata(MetadataJob& job) {
  if (job.finished.exchange(true)) return;
  if (job.handle.is_valid()) { try { ses_->remove_torrent(job.handle); } catch (std::exception const&) {} }
}

std::shared_ptr<StreamSession> Engine::open_stream(const TorrentSource& src, int file_index, const CacheConfig& cache, std::string* error) {
  auto fail = [&](const std::string& m) -> std::shared_ptr<StreamSession> { if (error) *error = m; return nullptr; };
  std::shared_ptr<lt::torrent_info> ti;
  try {
    std::vector<char> bytes = src.torrent_bytes;
    if (bytes.empty() && !src.magnet_uri.empty()) bytes = fetch_metadata(src.magnet_uri, std::chrono::seconds(60));
    if (!bytes.empty()) ti = std::make_shared<lt::torrent_info>(bytes.data(), static_cast<int>(bytes.size()));
    else if (!src.torrent_file.empty()) ti = std::make_shared<lt::torrent_info>(src.torrent_file);
  } catch (std::exception const& e) { return fail(std::string("malformed torrent metadata: ") + e.what()); }
  if (!ti || !ti->is_valid()) return fail("no valid torrent metadata (timeout or malformed)");
  const auto& files = ti->files();
  if (file_index < 0) {                                                       // largest file
    std::int64_t best = -1;
    for (int i = 0; i < files.num_files(); ++i) if (files.file_size(lt::file_index_t(i)) > best) { best = files.file_size(lt::file_index_t(i)); file_index = i; }
  }
  if (file_index < 0 || file_index >= files.num_files()) return fail("file index out of range");
  try {
    std::shared_ptr<StreamSession> s(new StreamSession(*this, ti, file_index, cache, src));
    s->start(src);
    return s;
  } catch (std::exception const& e) { return fail(std::string("cannot start stream: ") + e.what()); }
}

}  // namespace tp

// Download & Play (tpcore/download.hpp). Included here because CMakeLists.txt is hash-locked and lists the tpcore sources explicitly.
#include "download_manager.inc"
