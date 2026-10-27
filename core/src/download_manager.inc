// core/src/download_manager.inc: implementation of tp::DownloadManager (tpcore/download.hpp). It is #included at the end of engine.cpp because the project's
// CMakeLists.txt is hash-locked and lists the tpcore sources explicitly; it can become its own translation unit as soon as that list may be edited.
// Not meant to be compiled on its own.

#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bdecode.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/torrent_flags.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <libtorrent/peer_info.hpp>
#include <set>
#include "tpcore/download.hpp"

namespace tp {

const char* dl_state_name(DlState s) {
  switch (s) {
    case DlState::Checking: return "Checking files";
    case DlState::Downloading: return "Downloading";
    case DlState::WaitingForNetwork: return "Waiting for network";
    case DlState::WaitingForSources: return "Waiting for sources";
    case DlState::PausedByUser: return "Paused";
    case DlState::Resuming: return "Resuming";
    case DlState::Completed: return "Completed";
    case DlState::Failed: return "Failed";
  }
  return "?";
}

struct DownloadManager::Entry {
  std::string id, name, error;
  lt::torrent_handle h;                                      // invalid once completed or removed
  std::shared_ptr<lt::torrent_info> ti;
  fs::path save_path;
  std::vector<int> files;
  std::vector<std::string> extra_peers;
  std::vector<int> preexisting;                              // torrent files that existed before this download first started: never deleted by it
  std::vector<std::string> fingerprint;                      // size:mtime of each selected file when the download completed (restart fast path)
  bool auto_play = true, user_paused = false, net_paused = false, completed = false, failed = false;
  std::int64_t selected_bytes = 0, last_verified = 0;
  std::chrono::steady_clock::time_point resuming_since{}, no_source_since{}, last_redial{}, last_resume_save{}, last_diag{};
  int redial_backoff_s = 10;
  std::atomic<int> resume_outstanding{0};
};

namespace {
std::string hex20(const lt::sha1_hash& h) {
  static const char* d = "0123456789abcdef";
  std::string r;
  for (int i = 0; i < 20; ++i) { auto u = static_cast<unsigned char>(h[i]); r += d[u >> 4]; r += d[u & 15]; }
  return r;
}
bool write_atomic(const fs::path& p, const char* data, std::size_t n) {
  const fs::path tmp = p.string() + ".tmp";
  { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); if (!f) return false; f.write(data, static_cast<std::streamsize>(n)); f.flush(); if (!f) return false; }
  std::error_code ec; fs::rename(tmp, p, ec);
  return !ec;
}
bool read_all(const fs::path& p, std::vector<char>* out) {
  std::ifstream f(p, std::ios::binary); if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), {});
  return true;
}
std::string file_fingerprint(const fs::path& p) {            // "size:mtime" or "" when the file is missing
  std::error_code ec; const auto sz = fs::file_size(p, ec); if (ec) return "";
  const auto mt = fs::last_write_time(p, ec); if (ec) return "";
  return std::to_string(static_cast<unsigned long long>(sz)) + ":" + std::to_string(static_cast<long long>(mt.time_since_epoch().count()));
}
bool is_video_name(const std::string& n) {
  static const char* exts[] = {".mkv", ".mp4", ".m4v", ".avi", ".webm", ".mov", ".ts", ".m2ts", ".mts", ".wmv", ".flv", ".mpg", ".mpeg", ".ogv", ".vob"};
  std::string l; for (char c : n) l += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (auto* e : exts) if (l.size() >= std::strlen(e) && l.compare(l.size() - std::strlen(e), std::strlen(e), e) == 0) return true;
  return false;
}
bool parse_ep(const std::string& s, lt::tcp::endpoint* out) {
  const auto c = s.rfind(':'); if (c == std::string::npos) return false;
  lt::error_code ec; auto a = lt::make_address(s.substr(0, c), ec); if (ec) return false;
  const int port = std::atoi(s.c_str() + c + 1); if (port <= 0 || port > 65535) return false;
  *out = lt::tcp::endpoint(a, static_cast<std::uint16_t>(port));
  return true;
}
}  // namespace

DownloadManager::DownloadManager(DownloadConfig cfg) : cfg_(std::move(cfg)) {
  fs::create_directories(cfg_.state_dir);
  lt::settings_pack sp;
  sp.set_str(lt::settings_pack::listen_interfaces, cfg_.listen_interfaces);
  sp.set_bool(lt::settings_pack::enable_dht, cfg_.enable_dht);
  sp.set_bool(lt::settings_pack::enable_lsd, cfg_.enable_lsd);
  sp.set_bool(lt::settings_pack::enable_upnp, cfg_.enable_upnp);
  sp.set_bool(lt::settings_pack::enable_natpmp, cfg_.enable_natpmp);
  sp.set_int(lt::settings_pack::min_reconnect_time, cfg_.min_reconnect_time_s);
  if (cfg_.download_rate_limit > 0) sp.set_int(lt::settings_pack::download_rate_limit, cfg_.download_rate_limit);
  if (cfg_.upload_rate_limit > 0) sp.set_int(lt::settings_pack::upload_rate_limit, cfg_.upload_rate_limit);
  if (cfg_.loopback_test_mode) {
    sp.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
    sp.set_int(lt::settings_pack::out_enc_policy, lt::settings_pack::pe_disabled);
    sp.set_int(lt::settings_pack::in_enc_policy, lt::settings_pack::pe_disabled);
  }
  lt::alert_category_t mask = lt::alert_category::error | lt::alert_category::status | lt::alert_category::storage;
  // DIAGNOSTICS ONLY (default off): TP_DL_DIAG=<file> records tracker, DHT, peer and connection alerts. It adds alert categories and a log file; no behaviour changes.
  if (const char* f = std::getenv("TP_DL_DIAG")) { if (*f) { diag_.open(f, std::ios::app); mask |= lt::alert_category::tracker | lt::alert_category::dht | lt::alert_category::peer | lt::alert_category::connect; } }
  sp.set_int(lt::settings_pack::alert_mask, mask);
  ses_ = std::make_unique<lt::session>(lt::session_params(sp));
  th_ = std::thread([this] { alert_loop(); });
}

DownloadManager::~DownloadManager() {
  // graceful shutdown: ask every live torrent for its resume data, give the alert thread a bounded time to write it, then stop
  {
    std::lock_guard<std::mutex> g(mu_);
    for (auto& kv : entries_) { Entry& e = *kv.second; if (e.h.is_valid() && !e.completed) { persist_state(e); request_resume_data(e); } }
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (std::chrono::steady_clock::now() < deadline) {
    bool pending = false;
    { std::lock_guard<std::mutex> g(mu_); for (auto& kv : entries_) if (kv.second->resume_outstanding.load() > 0) pending = true; }
    if (!pending) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  stop_ = true;
  if (th_.joinable()) th_.join();
  ses_.reset();
}

void DownloadManager::persist_state(Entry& e) {
  lt::entry d(lt::entry::dictionary_t);
  d["version"] = 1;
  d["name"] = e.name;
  d["save_path"] = e.save_path.string();
  lt::entry::list_type fl; for (int f : e.files) fl.emplace_back(f);
  d["files"] = lt::entry(std::move(fl));
  d["user_paused"] = e.user_paused ? 1 : 0;
  d["auto_play"] = e.auto_play ? 1 : 0;
  d["completed"] = e.completed ? 1 : 0;
  { lt::entry::list_type pre; for (int f : e.preexisting) pre.emplace_back(f); d["preexisting"] = lt::entry(std::move(pre)); }
  { lt::entry::list_type fpl; for (auto& f : e.fingerprint) fpl.emplace_back(f); d["fingerprint"] = lt::entry(std::move(fpl)); }
  lt::entry::list_type pl; for (auto& p : e.extra_peers) pl.emplace_back(p);
  d["extra_peers"] = lt::entry(std::move(pl));
  std::vector<char> buf; lt::bencode(std::back_inserter(buf), d);
  write_atomic(dir_of(e.id) / "state.bencode", buf.data(), buf.size());
}

void DownloadManager::request_resume_data(Entry& e) {
  if (!e.h.is_valid()) return;
  ++e.resume_outstanding;
  e.last_resume_save = std::chrono::steady_clock::now();
  e.h.save_resume_data();
}

std::shared_ptr<DownloadManager::Entry> DownloadManager::add_locked(const DownloadRequest& req, const std::string& id_in, bool restoring, std::string* error) {
  auto fail = [&](const std::string& m) { if (error) *error = m; return std::shared_ptr<Entry>(); };
  lt::error_code ec;
  std::shared_ptr<lt::torrent_info> ti;
  try { ti = std::make_shared<lt::torrent_info>(req.torrent_bytes.data(), static_cast<int>(req.torrent_bytes.size())); }
  catch (std::exception const& x) { return fail(std::string("This is not a valid torrent: ") + x.what()); }
  const std::string id = hex20(ti->info_hashes().get_best());
  if (!id_in.empty() && id_in != id) return fail("The stored torrent does not match its identity.");
  auto it = entries_.find(id);
  if (it != entries_.end()) return it->second;                       // never a second session for the same torrent
  const auto& fs_ = ti->files();
  auto e = std::make_shared<Entry>();
  e->id = id; e->name = ti->name(); e->ti = ti; e->save_path = req.save_path; e->extra_peers = req.extra_peers; e->auto_play = req.auto_play;
  e->user_paused = req.start_paused;
  if (req.files.empty()) { for (int i = 0; i < fs_.num_files(); ++i) e->files.push_back(i); }
  else {
    for (int f : req.files) { if (f < 0 || f >= fs_.num_files()) return fail("The chosen file is not part of this torrent."); e->files.push_back(f); }
    std::sort(e->files.begin(), e->files.end()); e->files.erase(std::unique(e->files.begin(), e->files.end()), e->files.end());
  }
  for (int f : e->files) e->selected_bytes += fs_.file_size(lt::file_index_t(f));
  // Two downloads must never share a file: they would overwrite each other's data (a neighbouring file of the torrent is written too where it shares a piece with a
  // selected file) and one could delete the other's file. A download is refused if any file of its torrent is a selected file of another download or the other way round.
  {
    auto norm = [&](const fs::path& base, const lt::file_storage& st, int f) { return (base / st.file_path(lt::file_index_t(f))).lexically_normal(); };
    std::set<fs::path> mine_sel, mine_all;
    for (int f : e->files) mine_sel.insert(norm(req.save_path, fs_, f));
    for (int f = 0; f < fs_.num_files(); ++f) mine_all.insert(norm(req.save_path, fs_, f));
    for (auto& kv : entries_) {
      if (!kv.second->ti) continue;
      const auto& ofs = kv.second->ti->files();
      for (int f : kv.second->files) { const auto other = norm(kv.second->save_path, ofs, f); if (mine_all.count(other)) return fail("The file " + other.string() + " is already used by another download."); }
      for (int f = 0; f < ofs.num_files(); ++f) { const auto other = norm(kv.second->save_path, ofs, f); if (mine_sel.count(other)) return fail("The file " + other.string() + " is already used by another download."); }
    }
  }
  // destination: must exist (or be creatable) and be writable; enough free space for the selected files (not re-checked when restoring partial data)
  std::error_code fec;
  fs::create_directories(req.save_path, fec);
  if (fec || !fs::is_directory(req.save_path)) return fail("Cannot create the destination folder: " + req.save_path.string());
  {
    const fs::path probe = req.save_path / (".tp_write_test_" + id.substr(0, 8));
    { std::ofstream t(probe, std::ios::binary | std::ios::trunc); if (!t) return fail("TorrentPlayer cannot write to the destination folder: " + req.save_path.string()); t << "x"; }
    fs::remove(probe, fec);
  }
  if (!restoring) {
    std::int64_t avail = -1;
    if (cfg_.free_space) avail = cfg_.free_space(req.save_path);
    else { std::error_code sec; auto sp = fs::space(req.save_path, sec); if (!sec) avail = static_cast<std::int64_t>(sp.available); }
    const std::int64_t need = e->selected_bytes + 2 * static_cast<std::int64_t>(ti->piece_length());   // selected data plus the pieces shared with neighbouring files
    if (avail >= 0 && avail < need)
      return fail("Not enough free space on the destination: " + std::to_string(need / (1024 * 1024)) + " MB needed, " + std::to_string(avail / (1024 * 1024)) + " MB available.");
  }
  if (req.restoring) e->preexisting = req.known_preexisting;
  else for (int i = 0; i < fs_.num_files(); ++i) { std::error_code xe; if (fs::exists(req.save_path / fs_.file_path(lt::file_index_t(i)), xe)) e->preexisting.push_back(i); }
  fs::create_directories(dir_of(id), fec);
  if (fec) return fail("Cannot create the download state folder.");
  if (!fs::exists(dir_of(id) / "meta.torrent")) write_atomic(dir_of(id) / "meta.torrent", req.torrent_bytes.data(), req.torrent_bytes.size());
  if (req.strip_trackers) ti->clear_trackers();

  lt::add_torrent_params atp;
  {
    // Resume data is only trusted when no selected file was touched after it was written (an unclean exit, or edits by the user). Otherwise it is discarded and libtorrent
    // rechecks the files on disk piece by piece (a recheck requested after adding would be ignored while the resume data is being applied).
    std::vector<char> rb;
    const fs::path rp = dir_of(id) / "resume.dat";
    bool trust = read_all(rp, &rb) && !rb.empty();
    if (trust) {
      std::error_code mec; const auto rt = fs::last_write_time(rp, mec);
      for (int f : e->files) {
        std::error_code fe; const auto ft = fs::last_write_time(req.save_path / fs_.file_path(lt::file_index_t(f)), fe);
        if (!mec && !fe && ft > rt + std::chrono::seconds(2)) { trust = false; break; }
      }
    }
    if (trust) { lt::error_code rec; auto p = lt::read_resume_data(rb, rec); if (!rec) atp = std::move(p); }       // invalid resume data is ignored too
  }
  atp.ti = ti;
  atp.save_path = req.save_path.string();
  atp.file_priorities.assign(static_cast<std::size_t>(fs_.num_files()), lt::dont_download);
  for (int f : e->files) atp.file_priorities[static_cast<std::size_t>(f)] = lt::default_priority;
  atp.flags &= ~lt::torrent_flags::auto_managed;                      // no hidden queueing: state is ours
  if (e->user_paused) atp.flags |= lt::torrent_flags::paused; else atp.flags &= ~lt::torrent_flags::paused;
  for (auto const& t : req.trackers) atp.trackers.push_back(t);
  for (auto const& w : req.web_seed_urls) atp.url_seeds.push_back(w);
  for (auto const& p : req.extra_peers) { lt::tcp::endpoint ep; if (parse_ep(p, &ep)) atp.peers.push_back(ep); }
  e->h = ses_->add_torrent(std::move(atp), ec);
  if (ec || !e->h.is_valid()) return fail("Could not start the download: " + ec.message());
  e->resuming_since = std::chrono::steady_clock::now();
  entries_[id] = e;
  persist_state(*e);
  return e;
}

std::string DownloadManager::add(const DownloadRequest& req, std::string* error) {
  std::lock_guard<std::mutex> g(mu_);
  auto e = add_locked(req, "", false, error);
  return e ? e->id : std::string();
}

int DownloadManager::restore() {
  std::vector<fs::path> dirs;
  std::error_code ec;
  for (auto& de : fs::directory_iterator(cfg_.state_dir, ec)) if (de.is_directory() && fs::exists(de.path() / "state.bencode") && fs::exists(de.path() / "meta.torrent")) dirs.push_back(de.path());
  std::lock_guard<std::mutex> g(mu_);
  for (auto& d : dirs) {
    std::vector<char> sb, tb;
    if (!read_all(d / "state.bencode", &sb) || !read_all(d / "meta.torrent", &tb)) continue;
    lt::error_code bec;
    lt::bdecode_node n = lt::bdecode(sb, bec);
    if (bec || n.type() != lt::bdecode_node::dict_t) continue;
    DownloadRequest r; r.torrent_bytes = tb;
    r.save_path = std::string(n.dict_find_string_value("save_path"));
    r.auto_play = n.dict_find_int_value("auto_play", 1) != 0;
    r.start_paused = n.dict_find_int_value("user_paused", 0) != 0; r.restoring = true;
    if (auto l = n.dict_find_list("preexisting")) for (int i = 0; i < l.list_size(); ++i) r.known_preexisting.push_back(static_cast<int>(l.list_int_value_at(i)));
    std::vector<std::string> stored_fp; if (auto l = n.dict_find_list("fingerprint")) for (int i = 0; i < l.list_size(); ++i) stored_fp.push_back(std::string(l.list_string_value_at(i)));
    if (auto l = n.dict_find_list("files")) for (int i = 0; i < l.list_size(); ++i) r.files.push_back(static_cast<int>(l.list_int_value_at(i)));
    if (auto l = n.dict_find_list("extra_peers")) for (int i = 0; i < l.list_size(); ++i) r.extra_peers.push_back(std::string(l.list_string_value_at(i)));
    const bool was_completed = n.dict_find_int_value("completed", 0) != 0;
    std::string err;
    if (was_completed) {
      // a completed download stays "Completed" only while its files are still there; otherwise it is downloaded (rechecked) again
      lt::torrent_info ti(tb.data(), static_cast<int>(tb.size()));
      // Trusted without re-hashing only if every selected file still has exactly the size and modification time recorded when it was verified. Anything else
      // (a different size or timestamp, a missing file, no record) is treated as unverified: the torrent is added again and libtorrent re-checks the data against the
      // piece hashes before it can complete (unchanged data is accepted without any transfer; damaged pieces are fetched again).
      bool all = !r.files.empty() && stored_fp.size() == r.files.size();
      for (std::size_t k = 0; all && k < r.files.size(); ++k) {
        const int f = r.files[k]; const fs::path p = r.save_path / ti.files().file_path(lt::file_index_t(f));
        const std::string fp = file_fingerprint(p);
        if (fp.empty() || fp != stored_fp[k] || fp.substr(0, fp.find(':')) != std::to_string(static_cast<long long>(ti.files().file_size(lt::file_index_t(f))))) all = false;
      }
      if (all) {
        auto e = std::make_shared<Entry>();
        e->id = hex20(ti.info_hashes().get_best()); e->name = ti.name(); e->save_path = r.save_path; e->files = r.files; e->auto_play = r.auto_play; e->completed = true;
        e->preexisting = r.known_preexisting; e->fingerprint = stored_fp; e->extra_peers = r.extra_peers;
        e->ti = std::make_shared<lt::torrent_info>(tb.data(), static_cast<int>(tb.size()));
        for (int f : e->files) e->selected_bytes += ti.files().file_size(lt::file_index_t(f));
        entries_[e->id] = e;
        continue;
      }
      std::error_code rem; fs::remove(d / "resume.dat", rem);
    }
    add_locked(r, d.filename().string(), true, &err);
  }
  return static_cast<int>(entries_.size());
}

bool DownloadManager::verified_complete(Entry& e) {
  if (!e.h.is_valid() || !e.ti) return false;
  const auto& fs_ = e.ti->files(); const int plen = e.ti->piece_length();
  for (int f : e.files) {
    const std::int64_t size = fs_.file_size(lt::file_index_t(f)); if (size == 0) continue;
    const std::int64_t off = fs_.file_offset(lt::file_index_t(f));
    for (std::int64_t p = off / plen; p <= (off + size - 1) / plen; ++p) if (!e.h.have_piece(lt::piece_index_t(static_cast<int>(p)))) return false;
  }
  return true;
}

void DownloadManager::fill_status(Entry& e, DownloadStatus* o) {
  *o = DownloadStatus{};                                                 // counters below are accumulated: a reused out-parameter must not carry the previous call over
  o->id = e.id; o->name = e.name; o->save_path = e.save_path.string(); o->files = e.files; o->auto_play = e.auto_play;
  o->selected_bytes = e.selected_bytes; o->user_paused = e.user_paused; o->network_available = online_.load(); o->error = e.error;
  if (e.ti) {
    std::string first; for (int f : e.files) { auto p = (e.save_path / e.ti->files().file_path(lt::file_index_t(f))).string(); if (first.empty()) first = p; if (is_video_name(p)) { first = p; break; } }
    o->primary_file = first;
  }
  if (e.completed) { o->state = DlState::Completed; o->completed = true; o->verified_bytes = e.selected_bytes; o->progress = 1.0; return; }
  if (e.failed) o->state = DlState::Failed;
  if (!e.h.is_valid()) { if (!e.failed) o->state = e.user_paused ? DlState::PausedByUser : DlState::Failed; return; }
  const lt::torrent_status s = e.h.status();
  std::int64_t verified = 0;
  { auto fp = e.h.file_progress(lt::torrent_handle::piece_granularity); for (int f : e.files) if (f < static_cast<int>(fp.size())) verified += fp[static_cast<std::size_t>(f)]; }
  o->verified_bytes = std::min(verified, e.selected_bytes);
  o->progress = e.selected_bytes > 0 ? static_cast<double>(o->verified_bytes) / static_cast<double>(e.selected_bytes) : 0.0;
  o->download_rate = s.download_payload_rate; o->upload_rate = s.upload_payload_rate;
  std::vector<lt::peer_info> pi; e.h.get_peer_info(pi);
  for (auto const& p : pi) {
    if (p.flags & lt::peer_info::connecting) { ++o->connecting; continue; }               // a connection attempt is not a source
    if (p.connection_type == lt::peer_info::web_seed) { ++o->web_seeds; continue; }       // web seeds never do a BitTorrent handshake: libtorrent keeps the "handshake" flag on them until the server names itself
    if (p.flags & lt::peer_info::handshake) { ++o->connecting; continue; }
    ++o->peers;
  }
  for (auto const& t : e.h.trackers()) {
    ++o->trackers_total;
    bool ok = false, bad = false;
    for (auto const& ep : t.endpoints) for (auto const& ih : ep.info_hashes) { if (ih.fails > 0) bad = true; else if (ih.start_sent && !ih.updating) ok = true; }
    if (bad) ++o->trackers_failing; else if (ok) ++o->trackers_ok;
  }
  o->checking_progress = s.progress;
  if (e.failed) return;
  const bool checking = s.state == lt::torrent_status::checking_files || s.state == lt::torrent_status::checking_resume_data;
  const bool connected = (o->peers + o->web_seeds) > 0 || o->download_rate > 0;       // bytes arriving prove a source even if its connection is not listed as established
  if (e.user_paused) o->state = DlState::PausedByUser;
  else if (checking) o->state = DlState::Checking;
  else if (!online_.load() || e.net_paused) o->state = DlState::WaitingForNetwork;
  else if (e.resuming_since.time_since_epoch().count() != 0 && std::chrono::steady_clock::now() - e.resuming_since < std::chrono::seconds(25) &&
           !connected && o->download_rate == 0 && o->verified_bytes <= e.last_verified) o->state = DlState::Resuming;      // not claimed until a source is really connected or bytes move
  else if (!connected) o->state = DlState::WaitingForSources;
  else o->state = DlState::Downloading;
  if (o->state == DlState::Downloading && o->download_rate > 4096) {
    const std::int64_t remaining = e.selected_bytes - o->verified_bytes;
    if (remaining > 0) o->eta_s = remaining / o->download_rate;
  }
}

std::vector<DownloadStatus> DownloadManager::list() {
  std::lock_guard<std::mutex> g(mu_);
  std::vector<DownloadStatus> v;
  for (auto& kv : entries_) { DownloadStatus s; fill_status(*kv.second, &s); v.push_back(std::move(s)); }
  return v;
}
bool DownloadManager::status(const std::string& id, DownloadStatus* out) {
  std::lock_guard<std::mutex> g(mu_);
  auto it = entries_.find(id); if (it == entries_.end()) return false;
  fill_status(*it->second, out); return true;
}
int DownloadManager::live_torrents() { return static_cast<int>(ses_->get_torrents().size()); }

bool DownloadManager::pause(const std::string& id) {
  std::lock_guard<std::mutex> g(mu_);
  auto it = entries_.find(id); if (it == entries_.end() || it->second->completed) return false;
  Entry& e = *it->second;
  e.user_paused = true; e.net_paused = false;
  if (e.h.is_valid()) { e.h.pause(); request_resume_data(e); }
  persist_state(e);
  return true;
}
bool DownloadManager::resume(const std::string& id) {
  std::lock_guard<std::mutex> g(mu_);
  auto it = entries_.find(id); if (it == entries_.end() || it->second->completed) return false;
  Entry& e = *it->second;
  e.user_paused = false; e.net_paused = false; e.failed = false; e.error.clear();
  e.resuming_since = std::chrono::steady_clock::now(); e.redial_backoff_s = 10;
  if (e.h.is_valid()) { e.h.clear_error(); e.h.resume(); }
  persist_state(e);
  return true;
}
bool DownloadManager::cancel(const std::string& id, bool delete_files) {
  std::lock_guard<std::mutex> g(mu_);
  auto it = entries_.find(id); if (it == entries_.end()) return false;
  std::shared_ptr<Entry> e = it->second;
  entries_.erase(it);
  if (e->h.is_valid()) ses_->remove_torrent(e->h);                    // never libtorrent's delete_files: it removes every file of the torrent, including ones that were the user's
  if (delete_files && e->ti) {
    // Only files this download created are deleted: not files that were already in the folder when it started, and not files another download uses.
    std::set<fs::path> claimed;
    for (auto& kv : entries_) if (kv.second->ti) for (int f : kv.second->files) claimed.insert((kv.second->save_path / kv.second->ti->files().file_path(lt::file_index_t(f))).lexically_normal());
    const auto& fs_ = e->ti->files();
    for (int f = 0; f < fs_.num_files(); ++f) {
      if (std::find(e->preexisting.begin(), e->preexisting.end(), f) != e->preexisting.end()) continue;
      const fs::path p = (e->save_path / fs_.file_path(lt::file_index_t(f))).lexically_normal();
      if (claimed.count(p)) continue;
      std::error_code ec; fs::remove(p, ec);
      for (fs::path dir = p.parent_path(); dir != e->save_path.lexically_normal() && dir.has_parent_path() && dir != dir.parent_path(); dir = dir.parent_path()) {   // empty folders this download created; never the destination itself
        std::error_code de; if (!fs::is_empty(dir, de) || de) break; fs::remove(dir, de); if (de) break;
      }
    }
  }
  std::error_code ec; fs::remove_all(dir_of(id), ec);                // only our own bookkeeping folder
  return true;
}

void DownloadManager::set_auto_resume_on_network(bool on) {
  std::lock_guard<std::mutex> g(mu_);
  cfg_.auto_resume_on_network = on;
  if (on && online_.load()) restore_pending_ = true;
}
void DownloadManager::set_network_available(bool online) {
  const bool prev = online_.exchange(online);
  if (prev == online) return;
  std::lock_guard<std::mutex> g(mu_);
  if (!online) {
    if (!cfg_.auto_resume_on_network)                                   // the user opted out of automatic recovery: hold transfers until they press Resume
      for (auto& kv : entries_) { Entry& e = *kv.second; if (e.h.is_valid() && !e.user_paused && !e.completed) { e.h.pause(); e.net_paused = true; } }
    for (auto& kv : entries_) if (kv.second->h.is_valid() && !kv.second->completed) request_resume_data(*kv.second);       // progress survives even if the app is killed while offline
  } else restore_pending_ = true;                                       // applied from tick(), at most once per 2 s, so rapid flaps collapse into one action
}

void DownloadManager::apply_network_restore() {                         // mu_ held
  restore_pending_ = false;
  if (!online_.load()) return;
  last_restore_ = std::chrono::steady_clock::now();
  ++restores_applied_;
  ses_->reopen_network_sockets();
  for (auto& kv : entries_) {
    Entry& e = *kv.second;
    if (!e.h.is_valid() || e.completed || e.user_paused) continue;     // a user pause is never overridden
    if (e.net_paused) { if (!cfg_.auto_resume_on_network) continue; e.net_paused = false; e.h.resume(); }
    if (e.h.status().num_peers == 0) e.h.clear_peers();                // nothing is connected: forget peers that failed while the network was down, so they are dialled again
    e.h.force_reannounce(); e.h.force_dht_announce();
    for (auto const& p : e.extra_peers) { lt::tcp::endpoint ep; if (parse_ep(p, &ep)) e.h.connect_peer(ep); }
    e.resuming_since = last_restore_; e.last_verified = std::max<std::int64_t>(e.last_verified, 0); e.redial_backoff_s = 10; e.no_source_since = {};
  }
}

void DownloadManager::save_all() {
  std::lock_guard<std::mutex> g(mu_);
  for (auto& kv : entries_) if (kv.second->h.is_valid() && !kv.second->completed) { persist_state(*kv.second); request_resume_data(*kv.second); }
}

void DownloadManager::handle_alert(lt::alert* a) {
  if (diag_.is_open() && (a->category() & (lt::alert_category::tracker | lt::alert_category::dht | lt::alert_category::peer | lt::alert_category::connect | lt::alert_category::error))) {
    char t[32]; std::snprintf(t, sizeof t, "[%8.2f] ", std::chrono::duration<double>(std::chrono::steady_clock::now() - diag_t0_).count());
    diag_ << t << a->what() << ": " << a->message() << "\n"; diag_.flush();
  }
  if (auto* r = lt::alert_cast<lt::save_resume_data_alert>(a)) {
    const std::string id = hex20(r->handle.info_hashes().get_best());
    auto buf = lt::write_resume_data_buf(r->params);
    std::lock_guard<std::mutex> g(mu_);
    auto it = entries_.find(id);
    if (it != entries_.end()) { write_atomic(dir_of(id) / "resume.dat", buf.data(), buf.size()); if (it->second->resume_outstanding > 0) --it->second->resume_outstanding; }
    return;
  }
  if (auto* r = lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
    const std::string id = hex20(r->handle.info_hashes().get_best());
    std::lock_guard<std::mutex> g(mu_);
    auto it = entries_.find(id); if (it != entries_.end() && it->second->resume_outstanding > 0) --it->second->resume_outstanding;
    return;
  }
  auto fail_entry = [&](lt::torrent_handle h, const std::string& why) {
    const std::string id = hex20(h.info_hashes().get_best());
    std::lock_guard<std::mutex> g(mu_);
    auto it = entries_.find(id); if (it == entries_.end() || it->second->completed) return;
    it->second->failed = true; it->second->error = why;
  };
  if (auto* r = lt::alert_cast<lt::file_error_alert>(a)) {
    std::string why = r->error.message();
    if (r->error == boost::system::errc::no_space_on_device) why = "The destination disk is full.";
    else if (r->error == boost::system::errc::permission_denied) why = "TorrentPlayer is not allowed to write to the destination folder.";
    fail_entry(r->handle, why + " (" + std::string(r->filename()) + ")");
    return;
  }
  if (auto* r = lt::alert_cast<lt::torrent_error_alert>(a)) { fail_entry(r->handle, r->error.message()); return; }
}

void DownloadManager::tick() {
  std::lock_guard<std::mutex> g(mu_);
  const auto now = std::chrono::steady_clock::now();
  if (restore_pending_ && online_.load() && now - last_restore_ >= std::chrono::seconds(2)) apply_network_restore();
  std::vector<std::string> done;
  for (auto& kv : entries_) {
    Entry& e = *kv.second;
    if (!e.h.is_valid() || e.completed) continue;
    const lt::torrent_status s = e.h.status();
    // completion: libtorrent says every wanted piece is done AND each piece of each selected file is individually verified
    if (!e.failed && !e.user_paused && s.is_finished && verified_complete(e)) { done.push_back(e.id); continue; }
    { auto fp = e.h.file_progress(lt::torrent_handle::piece_granularity); std::int64_t v = 0; for (int f : e.files) if (f < static_cast<int>(fp.size())) v += fp[static_cast<std::size_t>(f)]; if (v > e.last_verified) { e.last_verified = v; e.resuming_since = {}; } }
    if (now - e.last_resume_save >= std::chrono::seconds(cfg_.resume_save_interval_s) && !e.user_paused) { e.h.save_resume_data(lt::torrent_handle::only_if_modified); ++e.resume_outstanding; e.last_resume_save = now; }
    if (diag_.is_open() && now - e.last_diag >= std::chrono::seconds(30)) {              // TP_DL_DIAG only: where the missing pieces are (counts, no addresses)
      e.last_diag = now;
      std::vector<lt::peer_info> pi; e.h.get_peer_info(pi); std::vector<int> av; e.h.piece_availability(av);
      int np = static_cast<int>(av.size()), have = 0, missing_unavail = 0, missing_rare = 0, connecting = 0, shaking = 0, up = 0, seeds = 0, choked_us = 0, interesting = 0;
      for (int i = 0; i < np; ++i) { if (e.h.have_piece(lt::piece_index_t(i))) { ++have; continue; } if (av[static_cast<std::size_t>(i)] == 0) ++missing_unavail; else if (av[static_cast<std::size_t>(i)] <= 2) ++missing_rare; }
      for (auto const& p : pi) { if (p.flags & lt::peer_info::connecting) { ++connecting; continue; } if (p.flags & lt::peer_info::handshake) { ++shaking; continue; } ++up;
                                 if (p.flags & lt::peer_info::seed) ++seeds; if (p.flags & lt::peer_info::remote_choked) ++choked_us; if (p.flags & lt::peer_info::interesting) ++interesting; }
      char t[32]; std::snprintf(t, sizeof t, "[%8.2f] ", std::chrono::duration<double>(now - diag_t0_).count());
      diag_ << t << "swarm: pieces have=" << have << " missing=" << (np - have) << " (offered by no connected peer=" << missing_unavail << ", by 1-2 peers=" << missing_rare << ")"
            << " | peers connecting=" << connecting << " handshaking=" << shaking << " established=" << up << " seeds=" << seeds << " we-are-interested-in=" << interesting << " choking-us=" << choked_us
            << " known=" << s.list_peers << " | tracker-reported seeds=" << s.num_complete << " leechers=" << s.num_incomplete << " | requested/downloading pieces=" << e.h.get_download_queue().size() << " | rate=" << s.download_payload_rate << "\n";
      for (auto const& tr : e.h.trackers()) for (auto const& ep : tr.endpoints) for (auto const& ih : ep.info_hashes)
        diag_ << t << "  tracker " << tr.url << ": fails=" << static_cast<int>(ih.fails) << (ih.updating ? " updating" : "") << (ih.message.empty() ? "" : " message=" + ih.message) << (ih.last_error ? " error=" + ih.last_error.message() : "") << "\n";
      diag_.flush();
    }
    // sources: while online, not paused and nothing is connected, re-announce and re-dial known peers with a growing, bounded pause
    if (!e.user_paused && !e.net_paused && !e.failed && online_.load() && s.num_peers == 0 && s.state != lt::torrent_status::checking_files) {
      if (e.no_source_since.time_since_epoch().count() == 0) { e.no_source_since = now; e.last_redial = now; }
      else if (now - e.last_redial >= std::chrono::seconds(e.redial_backoff_s)) {
        e.last_redial = now; e.redial_backoff_s = std::min(120, e.redial_backoff_s * 2);
        e.h.clear_peers();                                              // peers that failed too often are otherwise never tried again
        e.h.force_reannounce();
        for (auto const& p : e.extra_peers) { lt::tcp::endpoint ep; if (parse_ep(p, &ep)) e.h.connect_peer(ep); }
      }
    } else if (s.num_peers > 0) { e.no_source_since = {}; e.redial_backoff_s = 10; }
  }
  for (auto const& id : done) {
    Entry& e = *entries_[id];
    e.completed = true; e.failed = false; e.error.clear();
    e.fingerprint.clear(); for (int f : e.files) e.fingerprint.push_back(file_fingerprint(e.save_path / e.ti->files().file_path(lt::file_index_t(f))));
    persist_state(e);
    ses_->remove_torrent(e.h);                                          // stop transferring; the verified files stay where the user put them
    e.h = lt::torrent_handle();
    std::error_code ec; fs::remove(dir_of(id) / "resume.dat", ec);
  }
}

void DownloadManager::alert_loop() {
  while (!stop_) {
    ses_->wait_for_alert(std::chrono::milliseconds(300));
    std::vector<lt::alert*> v; ses_->pop_alerts(&v);
    for (auto* a : v) handle_alert(a);
    tick();
  }
}

}  // namespace tp
