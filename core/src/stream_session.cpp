#include "tpcore/stream_session.hpp"
#include <libtorrent/alert_types.hpp>
#include <libtorrent/peer_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <algorithm>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include "tpcore/audit.hpp"

namespace fs = std::filesystem;
namespace tp {

using Clock = std::chrono::steady_clock;
static double secs(Clock::duration d) { return std::chrono::duration<double>(d).count(); }
static void resolve_web_urls(std::vector<std::string> urls, std::shared_ptr<WebEpMap> out);      // web-seed stall remedy, defined below

static std::string hex_of(const lt::sha1_hash& h) {
  static const char* d = "0123456789abcdef";
  std::string r;
  for (int i = 0; i < 20; ++i) { auto u = static_cast<unsigned char>(h[i]); r += d[u >> 4]; r += d[u & 15]; }
  return r;
}

const char* to_string(StreamState s) {
  switch (s) {
    case StreamState::Connecting: return "Connecting to peers";
    case StreamState::NoPeers: return "No peers found";
    case StreamState::Preparing: return "Preparing (reading container index)";
    case StreamState::Buffering: return "Buffering";
    case StreamState::Ready: return "Ready";
    case StreamState::Stalled: return "Stalled (peers connected but no data)";
    case StreamState::DiskPaused: return "Paused: disk space low";
    case StreamState::Error: return "Error";
  }
  return "?";
}

StreamSession::StreamSession(Engine& e, std::shared_ptr<lt::torrent_info> ti, int file_index, CacheConfig cfg, const TorrentSource& src)
    : eng_(e), ti_(std::move(ti)), cfg_(cfg), file_index_(file_index),
      planner_(cfg, StreamGeometry{}, 0), est_(1, 1), disk_policy_(DiskPolicyConfig{}), web_policy_(WebSeedPolicyConfig{}) {
  const auto& files = ti_->files();
  geo_.piece_size = ti_->piece_length();
  geo_.file_offset = files.file_offset(lt::file_index_t(file_index_));
  geo_.file_size = files.file_size(lt::file_index_t(file_index_));
  file_name_ = std::string(files.file_name(lt::file_index_t(file_index_)));
  num_pieces_ = ti_->num_pieces();
  ihex_ = hex_of(ti_->info_hashes().get_best());
  save_path_ = (eng_.config().cache_root / ihex_).string();
  StoreParams sp;
  sp.dir = save_path_; sp.piece_size = geo_.piece_size; sp.num_pieces = num_pieces_; sp.total_size = ti_->total_size();
  {                                                                           // BEP 47: padding files are implicit zeros (libtorrent never requests them)
    std::vector<std::pair<std::int64_t, std::int64_t>> pad_files;
    for (int i = 0; i < files.num_files(); ++i)
      if (files.pad_file_at(lt::file_index_t(i))) pad_files.emplace_back(files.file_offset(lt::file_index_t(i)), files.file_size(lt::file_index_t(i)));
    sp.pads = build_piece_pads(pad_files, geo_.piece_size, ti_->total_size(), num_pieces_);
  }
  sp.limit_bytes = cfg_.limit_bytes;
  sp.margin_bytes = cfg_.limit_bytes ? cfg_.limit_bytes - internal_cap(cfg_, geo_.piece_size) : 0;
  store_ = std::make_shared<SegmentStore>(sp);
  shadow_ = std::make_shared<ShadowVerifier>(geo_.piece_size, num_pieces_, sp.total_size, eng_.config().evidence_dir / ihex_);
  storage_ = std::make_shared<TorrentStorage>();
  storage_->store = store_; storage_->shadow = shadow_;
  storage_->expected.resize(static_cast<std::size_t>(num_pieces_));
  for (int i = 0; i < num_pieces_; ++i) { auto h = ti_->hash_for_piece(lt::piece_index_t(i)); std::memcpy(storage_->expected[static_cast<std::size_t>(i)].data(), h.data(), 20); }
  planner_ = WindowPlanner(cfg_, geo_, 0);
  est_ = PlayheadEstimator(geo_.file_size, cfg_.pin_bytes_per_end);
  for (auto const& u : ti_->web_seeds()) web_urls_.push_back(u.url);
  for (auto const& u : src.web_seed_urls) if (std::find(web_urls_.begin(), web_urls_.end(), u) == web_urls_.end()) web_urls_.push_back(u);
  trackers_ = src.trackers;
  free_probe_ = nullptr;
  t0_ = last_reset_ = last_peer_seen_ = last_prio_ = last_data_ = Clock::now();
}

StreamSession::~StreamSession() {
  stop_ = true;
  if (th_.joinable()) th_.join();
  eng_.unregister_session(ihex_);
  if (h_.is_valid()) {
    eng_.session().remove_torrent(h_);
    for (int i = 0; i < 250 && h_.is_valid(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  eng_.unregister_storage(save_path_);
}

void StreamSession::log_event(const std::string& s) {
  std::lock_guard<std::mutex> g(ev_mu_);
  char b[64]; std::snprintf(b, sizeof b, "[%7.2f] ", secs(Clock::now() - t0_));
  events_.push_back(b + s);
  if (events_.size() > 200) events_.erase(events_.begin(), events_.begin() + 50);
}
std::vector<std::string> StreamSession::recent_events() const { std::lock_guard<std::mutex> g(ev_mu_); return events_; }

void StreamSession::set_bitrate(double bps) { std::lock_guard<std::mutex> g(cfg_mu_); planner_.set_bitrate(bps); }
void StreamSession::set_free_space_probe(std::function<std::int64_t()> p) { std::lock_guard<std::mutex> g(cfg_mu_); free_probe_ = std::move(p); }
void StreamSession::set_disk_policy(const DiskPolicyConfig& c) { std::lock_guard<std::mutex> g(cfg_mu_); disk_policy_ = DiskPolicy(c); }
void StreamSession::set_web_seed_mode(WebSeedMode m) { std::lock_guard<std::mutex> g(cfg_mu_); web_cfg_.mode = m; web_policy_ = WebSeedPolicy(web_cfg_); }
void StreamSession::seek_hint(std::int64_t off) { std::lock_guard<std::mutex> g(est_mu_); est_.hint(off); }

void StreamSession::start(const TorrentSource& src) {
  // ---- recovery BEFORE any torrent activity: verify, delete partial/corrupt, trim to the cap (spec 4.2 item 6)
  recovery_ = store_->recover([this](int p, const Sha1& h) { return p < num_pieces_ && h == storage_->expected[static_cast<std::size_t>(p)]; }, 0);
  log_event("recovery: verified=" + std::to_string(recovery_.verified_present) + " deleted_partial=" + std::to_string(recovery_.deleted_partial) +
            " deleted_corrupt=" + std::to_string(recovery_.deleted_corrupt) + " trimmed=" + std::to_string(recovery_.trimmed));
  web_cfg_.mode = src.strip_web_seeds ? WebSeedMode::Off : WebSeedMode::Assist;
  web_policy_ = WebSeedPolicy(web_cfg_);
  strip_web_always_ = src.strip_web_seeds;
  eng_.register_storage(save_path_, storage_);
  eng_.register_session(ihex_, this);

  lt::add_torrent_params atp;
  atp.ti = ti_;
  atp.save_path = save_path_;
  atp.flags &= ~lt::torrent_flags::auto_managed;
  if (src.start_paused) { atp.flags |= lt::torrent_flags::paused; paused_by_user_start_ = true; } else atp.flags &= ~lt::torrent_flags::paused;
  atp.file_priorities.assign(static_cast<std::size_t>(ti_->num_files()), lt::dont_download);
  atp.file_priorities[static_cast<std::size_t>(file_index_)] = lt::default_priority;
  lt::typed_bitfield<lt::piece_index_t> have(num_pieces_, false);
  for (int p : store_->present_pieces()) have.set_bit(lt::piece_index_t(p));
  atp.have_pieces = have;
  std::vector<lt::download_priority_t> prio(static_cast<std::size_t>(num_pieces_), lt::download_priority_t{0});
  atp.piece_priorities = prio;
  for (auto const& ep : src.extra_peers) {
    auto c = ep.rfind(':');
    lt::error_code ec;
    auto addr = lt::make_address(ep.substr(0, c), ec);
    if (!ec) { atp.peers.emplace_back(addr, static_cast<unsigned short>(std::stoi(ep.substr(c + 1))));
               std::lock_guard<std::mutex> g(known_mu_); known_peers_.push_back(atp.peers.back()); }      // initial peers are always "known" (kept for re-adds)
  }
  atp.trackers = trackers_;
  h_ = eng_.session().add_torrent(atp);
  for (auto const& u : web_urls_) h_.remove_url_seed(u);           // the policy decides when web seeds are used
  if (src.strip_trackers) h_.replace_trackers({});
  if (!web_urls_.empty() && !strip_web_always_) {                  // one background pass over at most 16 URLs; detached on purpose: getaddrinfo cannot be cancelled and must not delay shutdown
    web_ep_ = std::make_shared<WebEpMap>();
    std::thread([urls = std::vector<std::string>(web_urls_.begin(), web_urls_.begin() + std::min<std::size_t>(web_urls_.size(), 16)), m = web_ep_] { resolve_web_urls(urls, m); }).detach();
  }
  th_ = std::thread([this] { scheduler_loop(); });
}

// ============================================================== reader
std::int64_t StreamSession::read(std::int64_t off, void* buf, std::int64_t len, const std::atomic<bool>* cancel) {
  if (off < 0) return -1;
  if (off >= geo_.file_size) return 0;
  while (true) {
    const std::int64_t toff = geo_.file_offset + off;
    const int piece = static_cast<int>(toff / geo_.piece_size);
    { std::lock_guard<std::mutex> g(est_mu_); est_.on_read(off); }
    store_->touch(piece);
    if (!store_->has(piece)) {
      ++blocked_;
      blocked_offset_ = off;
      demand_piece_ = piece;
      if (blocked_logged_.fetch_add(1) < 30) log_event("read blocked: file offset " + std::to_string(off) + " needs piece " + std::to_string(piece) + " (len " + std::to_string(len) + ")");
      bool ok = false;
      while (!stop_.load()) {
        if (store_->wait_present(piece, std::chrono::milliseconds(100), cancel)) { ok = true; break; }
        if (cancel && cancel->load()) break;
      }
      --blocked_;
      blocked_offset_ = -1;
      if (!ok) return -1;
      demand_piece_ = -1;
    }
    const std::int64_t piece_end = (static_cast<std::int64_t>(piece) + 1) * geo_.piece_size;
    std::int64_t n = std::min<std::int64_t>({len, piece_end - toff, geo_.file_size - off});
    const int r = store_->read(piece, static_cast<int>(toff - static_cast<std::int64_t>(piece) * geo_.piece_size), buf, static_cast<int>(n));
    if (r < 0) continue;                                            // evicted between the check and the read: wait again
    return n;
  }
}

// ============================================================== alerts (engine alert thread)
void StreamSession::on_alert(lt::alert* a) {
  if (auto* b = lt::alert_cast<lt::block_finished_alert>(a)) {
    bool web;
    { std::lock_guard<std::mutex> g(ips_mu_); web = std::find(web_ips_.begin(), web_ips_.end(), b->endpoint.address().to_string()) != web_ips_.end(); }
    shadow_->note_block_source(int(b->piece_index), int(b->block_index), b->endpoint.address().to_string() + ":" + std::to_string(b->endpoint.port()), web);
  } else if (auto* hf = lt::alert_cast<lt::hash_failed_alert>(a)) {
    shadow_->attach_sources_to_failure(int(hf->piece_index));
    log_event("HASH FAILED piece " + std::to_string(int(hf->piece_index)) + ": " + shadow_->attribution(int(hf->piece_index)) + " | " + shadow_->last_report());
  } else if (auto* fe = lt::alert_cast<lt::file_error_alert>(a)) {
    ++file_errors_;
    if (fe->error == boost::system::errc::make_error_code(boost::system::errc::no_space_on_device)) { ++enospc_; log_event("ENOSPC from the store (write refused to protect the cache bound)"); }
  } else if (auto* pb = lt::alert_cast<lt::peer_ban_alert>(a)) {
    ++bans_; log_event("PEER BAN: " + pb->message());
  } else if (auto* te = lt::alert_cast<lt::torrent_error_alert>(a)) {
    log_event("TORRENT ERROR: " + te->message());
  } else if (auto* us = lt::alert_cast<lt::url_seed_alert>(a)) {                  // a web seed failed or is not usable
    const int n = ++web_alerts_;
    { std::lock_guard<std::mutex> g(diag_mu_); web_last_error_ = us->message(); }
    if (n <= 20) log_event("WEB SEED ALERT: " + us->message());
  } else if (auto* pe = lt::alert_cast<lt::peer_error_alert>(a)) {
    if (is_web_endpoint(pe->endpoint)) {
      const int n = ++web_alerts_;
      { std::lock_guard<std::mutex> g(diag_mu_); web_last_error_ = pe->message(); }
      if (n <= 20) log_event("WEB SEED PEER ERROR: " + pe->message());
    }
  } else if (auto* pd = lt::alert_cast<lt::peer_disconnected_alert>(a)) {
    if (is_web_endpoint(pd->endpoint)) {
      const int n = ++web_disconnects_;
      { std::lock_guard<std::mutex> g(diag_mu_); web_last_error_ = pd->message(); }
      if (n <= 20) log_event("WEB SEED DISCONNECT: " + pd->message());
    }
  } else if (lt::alert_cast<lt::request_dropped_alert>(a)) {
    ++request_drops_;
  } else if (lt::alert_cast<lt::block_timeout_alert>(a)) {
    ++block_timeouts_;
  }
}

bool StreamSession::is_web_endpoint(const lt::tcp::endpoint& ep) {
  std::lock_guard<std::mutex> g(ips_mu_);
  return std::find(web_ips_.begin(), web_ips_.end(), ep.address().to_string()) != web_ips_.end();
}

// ============================================================== web-seed stall remedy
// Observed live (Internet Archive torrent) and reproduced offline (tests/integration/it_webseed_starvation.cpp): libtorrent hands the blocks of the most urgent piece to
// whichever web-seed connection is ready first. A seed that accepts the connection but never answers keeps those blocks until its inactivity timeout (about 20 s) and, after
// reconnecting, is given them again, while the one working seed spends its bandwidth on other pieces. The remedy is narrow: only when the READER is blocked, only a web-seed
// connection that (a) owns requested blocks of the piece the reader needs, (b) has received nothing for kWebStallSeconds and (c) has requests queued, and only while another
// source that can serve the waited piece IS delivering data (a sole slow source, or a source that is merely busy with other pieces, never counts). That seed is removed (libtorrent returns its blocks to the picker) and re-added after a penalty that doubles
// per repeat (30 s .. 240 s). Slow but flowing seeds, peers and healthy startup are untouched; no global timeout changes.
// "Can serve the piece" is read from the peer's own advertised piece set (peer_info::pieces). libtorrent builds that set exactly when a web connection opens, but a seed it learned
// through a per-file redirect may later advertise boundary pieces that also span files it does not have. So for a piece that spans several files a web seed only counts when its
// advertised set also contains a piece lying entirely inside EACH file the piece spans (those are advertised only for files the seed really knows); a file too small to hold a
// whole piece can only be vouched for by an endpoint that maps to exactly one configured URL. BitTorrent peers always count. If that cannot be established the stalled seed is kept:
// it may be the only source that holds every file slice of the piece (observed: a slow first response from the plain seed plus a redirected seed that knows only the MP4).
static constexpr double kWebStallSeconds = 8.0;

// Resolves every URL's host once. A failed lookup simply leaves that URL unmapped (it is then never released: the remedy only acts on connections it can attribute to exactly one URL).
static void resolve_web_urls(std::vector<std::string> urls, std::shared_ptr<WebEpMap> out) {
  std::map<std::string, std::vector<std::string>> m;
  for (auto const& u : urls) {
    const auto se = u.find("://"); if (se == std::string::npos) continue;
    const std::string scheme = u.substr(0, se);
    const auto hs = se + 3;
    std::string host, port = scheme == "https" ? "443" : "80";
    std::string::size_type after;
    if (hs < u.size() && u[hs] == '[') { const auto be = u.find(']', hs); if (be == std::string::npos) continue; host = u.substr(hs + 1, be - hs - 1); after = be + 1; }   // IPv6 literal
    else { const auto he = u.find_first_of(":/", hs); host = u.substr(hs, he == std::string::npos ? std::string::npos : he - hs); after = he == std::string::npos ? u.size() : he; }
    if (after < u.size() && u[after] == ':') { const auto pe = u.find('/', after); port = u.substr(after + 1, pe == std::string::npos ? std::string::npos : pe - after - 1); }
    addrinfo hints{}; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (host.empty() || ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) continue;
    for (addrinfo* r = res; r; r = r->ai_next) {
      char b[NI_MAXHOST];
      if (::getnameinfo(r->ai_addr, r->ai_addrlen, b, sizeof b, nullptr, 0, NI_NUMERICHOST) != 0) continue;
      lt::error_code ec; auto a = lt::make_address(std::string(b).substr(0, std::string(b).find('%')), ec);
      if (ec) continue;
      auto& v = m[a.to_string() + ":" + port];
      if (std::find(v.begin(), v.end(), u) == v.end()) v.push_back(u);
    }
    ::freeaddrinfo(res);
  }
  std::lock_guard<std::mutex> g(out->mu);
  out->m = std::move(m);
}

void StreamSession::release_stalled_web_seeds(double) {
  const auto now = Clock::now();
  for (auto& kv : web_stall_) {                                       // penalty over: put the seed back (libtorrent's own retry handling takes over from there)
    if (kv.second.off && now >= kv.second.until) {
      kv.second.off = false;
      if (web_enabled_ && !strip_web_always_) { h_.add_url_seed(kv.first); log_event("web seed back after its penalty (strike " + std::to_string(kv.second.strikes) + ")"); }
    }
  }
  if (!web_enabled_ || strip_web_always_ || blocked_.load() <= 0) return;
  const int need = wait_cache_.piece;
  if (need < 0) return;
  std::vector<lt::peer_info> pi; h_.get_peer_info(pi);
  std::vector<lt::tcp::endpoint> stalled;
  bool other_source_delivering = false;
  std::vector<lt::file_index_t> spanned;                                    // files with real data in the waited piece (BEP 47 padding excluded)
  try {
    const auto& fs = ti_->files();
    for (auto const& sl : ti_->map_block(lt::piece_index_t(need), 0, ti_->piece_size(lt::piece_index_t(need)))) if (!fs.pad_file_at(sl.file_index)) spanned.push_back(sl.file_index);
  } catch (std::exception const&) { return; }
  auto single_configured_url = [&](lt::tcp::endpoint const& ep) {            // the endpoint is exactly one configured web-seed URL, not a redirect target and not shared
    if (!web_ep_) return false;
    std::lock_guard<std::mutex> g(web_ep_->mu);
    auto it = web_ep_->m.find(ep.address().to_string() + ":" + std::to_string(ep.port()));
    return it != web_ep_->m.end() && it->second.size() == 1;
  };
  auto web_seed_has_all_files = [&](lt::peer_info const& p) {                // evidence that a web seed knows every file of the waited piece (see the note above)
    const auto& fs = ti_->files(); const std::int64_t ps = ti_->piece_length();
    for (auto const f : spanned) {
      const std::int64_t off = fs.file_offset(f), end = off + fs.file_size(f);
      const std::int64_t first = (off + ps - 1) / ps, last = end / ps - 1;      // pieces lying entirely inside the file
      if (first <= last) { if (first >= static_cast<std::int64_t>(p.pieces.size()) || !p.pieces.get_bit(lt::piece_index_t(static_cast<int>(first)))) return false; }
      else if (!single_configured_url(p.ip)) return false;
    }
    return true;
  };
  for (auto const& p : pi) {
    // a source counts as an alternative only if it delivers right now AND advertises the waited piece; a source busy with other pieces (or one that is merely connected) does not
    if (p.payload_down_speed > 0 && static_cast<int>(p.pieces.size()) > need && p.pieces.get_bit(lt::piece_index_t(need)) &&
        (spanned.size() <= 1 || p.connection_type != lt::peer_info::web_seed || web_seed_has_all_files(p))) other_source_delivering = true;
    if (p.connection_type != lt::peer_info::web_seed || p.download_queue_length <= 0) continue;
    if (p.payload_down_speed > 0 || lt::total_milliseconds(p.last_active) < static_cast<int>(kWebStallSeconds * 1000)) continue;
    stalled.push_back(p.ip);
  }
  if (stalled.empty() || !other_source_delivering) return;
  std::vector<lt::partial_piece_info> q;
  try { q = h_.get_download_queue(); } catch (std::exception const&) { return; }
  std::vector<lt::tcp::endpoint> owners;                              // who holds requested blocks of the waited piece
  for (auto const& pp : q) {
    if (static_cast<int>(pp.piece_index) != need || !pp.blocks) continue;
    for (int b = 0; b < pp.blocks_in_piece; ++b) if (pp.blocks[b].state == lt::block_info::requested && pp.blocks[b].num_peers > 0) owners.push_back(pp.blocks[b].peer());
  }
  for (auto const& ep : stalled) {
    if (std::find(owners.begin(), owners.end(), ep) == owners.end()) continue;
    std::string url;
    if (web_ep_) {
      std::lock_guard<std::mutex> g(web_ep_->mu);
      auto it = web_ep_->m.find(ep.address().to_string() + ":" + std::to_string(ep.port()));
      if (it != web_ep_->m.end() && it->second.size() == 1) url = it->second.front();      // two URLs on one server cannot be told apart: leave those to libtorrent
    }
    if (url.empty() || web_seed_off(url)) continue;                   // not one of the resolved seeds (for example a redirect target): leave it to libtorrent
    auto& st = web_stall_[url];
    if (st.strikes > 0 && now - st.last_release > std::chrono::minutes(10)) st.strikes = 0;     // it behaved for a long time: start the penalties over
    ++st.strikes; st.off = true; st.last_release = now;
    const int penalty_s = std::min(240, 30 << std::min(st.strikes - 1, 3));
    st.until = now + std::chrono::seconds(penalty_s);
    h_.remove_url_seed(url);
    const auto idx = std::find(web_urls_.begin(), web_urls_.end(), url) - web_urls_.begin();
    log_event("released stalled web seed #" + std::to_string(idx) + ": it holds requested blocks of piece " + std::to_string(need) + " that the reader waits for and delivered nothing for " +
              std::to_string(static_cast<int>(kWebStallSeconds)) + " s while another source delivers; off for " + std::to_string(penalty_s) + " s (strike " + std::to_string(st.strikes) + ")");
  }
}

// ============================================================== scheduler
void StreamSession::scheduler_loop() {
  int n = 0;
  while (!stop_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    try { tick(); } catch (std::exception const& e) { log_event(std::string("tick exception: ") + e.what()); }
    ++n;
  }
}

void StreamSession::update_known_peers() {
  std::vector<lt::peer_info> pi;
  h_.get_peer_info(pi);
  std::sort(pi.begin(), pi.end(), [](auto& a, auto& b) { return a.total_download > b.total_download; });
  std::lock_guard<std::mutex> g(known_mu_);
  for (std::size_t i = 0; i < pi.size() && i < 150; ++i) {
    if (pi[i].connection_type == lt::peer_info::web_seed) continue;
    if (std::find(known_peers_.begin(), known_peers_.end(), pi[i].ip) == known_peers_.end()) known_peers_.push_back(pi[i].ip);
  }
  if (known_peers_.size() > 400) known_peers_.erase(known_peers_.begin(), known_peers_.begin() + 100);
}

void StreamSession::update_traffic_split() {
  std::vector<lt::peer_info> pi;
  h_.get_peer_info(pi);
  std::int64_t w = 0, p = 0; int nweb = 0;
  std::vector<std::string> ips;
  for (auto& x : pi) {
    if (x.connection_type == lt::peer_info::web_seed) { w += x.total_download; ++nweb; ips.push_back(x.ip.address().to_string()); }
    else p += x.total_download;
  }
  if (w >= last_web_total_) web_bytes_ += w - last_web_total_;
  if (p >= last_peer_total_) peer_bytes_ += p - last_peer_total_;
  last_web_total_ = w; last_peer_total_ = p; web_conns_ = nweb;
  std::lock_guard<std::mutex> g(ips_mu_);
  for (auto& ip : ips) if (std::find(web_ips_.begin(), web_ips_.end(), ip) == web_ips_.end()) web_ips_.push_back(ip);
}

// ~1 Hz: what is the reader waiting for, how far has that piece got, and who could supply it. Read-only with respect to scheduling (no priority or deadline is changed here).
void StreamSession::update_wait_info(double t) {
  WaitInfo w;
  int ha, hb, ta, tb;
  { std::lock_guard<std::mutex> g(cfg_mu_); ha = planner_.pin_head_first(); hb = planner_.pin_head_last(); ta = planner_.pin_tail_first(); tb = planner_.pin_tail_last(); }
  std::vector<lt::partial_piece_info> q;
  try { q = h_.get_download_queue(); } catch (std::exception const&) {}
  // Progress counts REAL data blocks only: padding blocks (BEP 47) are never downloaded; libtorrent marks them finished, which must not look like progress.
  auto blocks_of = [&](int p, int& total, int& done, int& writing, int& requested, int& pad, bool& inq) {
    const std::int64_t plen = ti_->piece_size(lt::piece_index_t(p));
    const int all = blocks_in_piece(plen);
    const auto& pads = store_->pads_of(p);
    pad = 0;
    for (int b = 0; b < all; ++b) if (block_is_pad(b, plen, pads)) ++pad;
    total = all - pad;
    done = writing = requested = 0; inq = false;
    if (store_->has(p)) { done = total; return; }
    for (auto const& pp : q) {
      if (static_cast<int>(pp.piece_index) != p) continue;
      inq = true;
      if (pp.blocks) {
        for (int b = 0; b < pp.blocks_in_piece && b < all; ++b) {
          if (block_is_pad(b, plen, pads)) continue;
          const auto st = pp.blocks[b].state;
          if (st == lt::block_info::finished) ++done; else if (st == lt::block_info::writing) ++writing; else if (st == lt::block_info::requested) ++requested;
        }
      } else { done = pp.finished; writing = pp.writing; requested = pp.requested; }
      break;
    }
  };
  auto pin = [&](int a, int b, int& np, int& present, int& bt, int& bd) {
    np = std::max(0, b - a + 1); present = bt = bd = 0;
    for (int p = a; p <= b; ++p) { int t_, d_, w_, r_, pd_; bool iq; blocks_of(p, t_, d_, w_, r_, pd_, iq); bt += t_; bd += d_; present += store_->has(p) ? 1 : 0; }
  };
  pin(ha, hb, w.head_pin_pieces, w.head_pin_present, w.head_pin_blocks_total, w.head_pin_blocks_done);
  pin(ta, tb, w.tail_pin_pieces, w.tail_pin_present, w.tail_pin_blocks_total, w.tail_pin_blocks_done);

  const bool blocked = blocked_.load() > 0;
  const int dm = demand_piece_.load();
  int need = -1;
  if (blocked && dm >= 0) need = dm;
  else {
    for (int p = ha; p <= hb && need < 0; ++p) if (!store_->has(p)) need = p;
    for (int p = ta; p <= tb && need < 0; ++p) if (!store_->has(p)) need = p;
  }
  w.offset = blocked ? blocked_offset_.load() : -1;
  w.piece = need;
  w.kind = classify_wait_piece(need, ha, hb, ta, tb);
  if (need >= 0) {
    blocks_of(need, w.blocks_total, w.blocks_done, w.blocks_writing, w.blocks_requested, w.blocks_pad, w.in_queue);
    try { w.priority = static_cast<int>(static_cast<std::uint8_t>(h_.piece_priority(lt::piece_index_t(need)))); } catch (...) { w.priority = -1; }
    auto it = deadlines_applied_.find(need);
    w.deadline_ms = it == deadlines_applied_.end() ? -1 : it->second;
  }
  std::vector<lt::peer_info> pi;
  try { h_.get_peer_info(pi); } catch (std::exception const&) {}
  for (auto const& x : pi) {
    if (x.connection_type == lt::peer_info::web_seed) { if (x.download_queue_length > 0) ++w.web_requesting; continue; }
    ++w.peers_total;
    if (need >= 0 && static_cast<int>(x.pieces.size()) > need && x.pieces.get_bit(lt::piece_index_t(need))) ++w.peers_with_piece;
    if (x.download_queue_length > 0) ++w.peers_requesting;
  }
  watch_.update(t, need, w.blocks_done);
  w.no_progress_s = (blocked && need >= 0) ? watch_.idle_seconds(t) : 0.0;
  if (w.no_progress_s < 5.0) idle_logged_ = false;
  else if (w.no_progress_s >= 20.0 && !idle_logged_) {
    idle_logged_ = true;
    log_event("no new block for piece " + std::to_string(need) + " for " + std::to_string(static_cast<int>(w.no_progress_s)) + " s: blocks " + std::to_string(w.blocks_done) + "/" +
              std::to_string(w.blocks_total) + " (writing " + std::to_string(w.blocks_writing) + ", requested " + std::to_string(w.blocks_requested) + "), in queue " + (w.in_queue ? "yes" : "no") +
              ", peers having it " + std::to_string(w.peers_with_piece) + "/" + std::to_string(w.peers_total) + ", peers with open requests " + std::to_string(w.peers_requesting) +
              ", web seeds with open requests " + std::to_string(w.web_requesting) + ", priority " + std::to_string(w.priority) + ", deadline " + std::to_string(w.deadline_ms) + " ms");
  }
  wait_cache_ = w;
}

void StreamSession::apply_priorities(const WindowPlan& plan, int dm) {
  std::vector<lt::download_priority_t> prio(static_cast<std::size_t>(num_pieces_), lt::download_priority_t{0});
  for (int p = plan.head; p < plan.hi_wanted; ++p) prio[static_cast<std::size_t>(p)] = lt::download_priority_t{7};
  const int pa = planner_.pin_head_first(), pb = planner_.pin_head_last(), pc = planner_.pin_tail_first(), pd = planner_.pin_tail_last();
  for (int p = pa; p <= pb; ++p) if (!store_->has(p)) prio[static_cast<std::size_t>(p)] = lt::download_priority_t{7};
  for (int p = pc; p <= pd; ++p) if (!store_->has(p)) prio[static_cast<std::size_t>(p)] = lt::download_priority_t{7};
  if (dm >= 0 && dm < num_pieces_ && !store_->has(dm)) prio[static_cast<std::size_t>(dm)] = lt::download_priority_t{7};
  h_.prioritize_pieces(prio);
  deadlines_applied_.clear();                                        // diagnostics only: what we asked libtorrent for
  auto set_dl = [&](int p, int ms) { h_.set_piece_deadline(lt::piece_index_t(p), ms); deadlines_applied_[p] = ms; };
  int k = 0;
  for (int p = plan.head; p < plan.hi_wanted; ++p) if (!store_->has(p)) set_dl(p, 200 + (k++) * 120);
  int j = 0;
  for (int p = pa; p <= pb; ++p) if (!store_->has(p)) set_dl(p, 10 + (j++) * 20);
  for (int p = pc; p <= pd; ++p) if (!store_->has(p)) set_dl(p, 10 + (j++) * 20);
  if (dm >= 0 && dm < num_pieces_ && !store_->has(dm)) set_dl(dm, 0);
}

// Peer-starvation watchdog (verified remedy order, research phase): re-dial known peers first; if there is still nobody after a few
// seconds, re-add the torrent with the known-peers list (clear_peers / pause-resume / re-announce did NOT recover a stuck peer list).
void StreamSession::connectivity_watchdog(const lt::torrent_status& s, bool need_data, double t) {
  if (!need_data || s.num_peers > 0) { nopeer_since_ = -1; return; }
  if (nopeer_since_ < 0) nopeer_since_ = t;
  const double starved = t - nopeer_since_;
  const bool just_reset = secs(Clock::now() - last_reset_) < 5.0;                  // right after a re-add: dial immediately and often
  if (starved >= (just_reset ? 0.1 : 2.0) && t - last_kick_ >= (just_reset ? 0.4 : 1.0)) {
    std::vector<lt::tcp::endpoint> peers;
    { std::lock_guard<std::mutex> g(known_mu_); peers = known_peers_; }
    int n = 0;
    for (auto const& ep : peers) { if (n++ >= 60) break; try { h_.connect_peer(ep); } catch (std::exception const&) {} }
    last_kick_ = t; ++kicks_;
    if (kicks_.load() <= 5 || kicks_.load() % 20 == 0) log_event("watchdog: no peers for " + std::to_string(starved) + " s while data is needed; re-dialled " + std::to_string(std::min<int>(n, 60)) + " known peers");
  }
  if (starved >= 6.0 && !just_reset && secs(Clock::now() - last_reset_) >= 10.0) {
    ++wd_resets_;
    log_event("watchdog: still no peers after 6 s; re-adding the torrent with the known-peers list");
    reset_requested_ = true;
    nopeer_since_ = t;
  }
}

void StreamSession::do_reset_have() {
  // un-have = remove + re-add with have_pieces from the store, current window priorities and the known-peers list (spec F3).
  resetting_ = true;
  log_event("un-have reset: remove + re-add (have_pieces from store, known peers handed back)");
  std::vector<lt::tcp::endpoint> peers;
  { std::lock_guard<std::mutex> g(known_mu_); peers = known_peers_; }
  eng_.session().remove_torrent(h_);
  for (int i = 0; i < 250 && h_.is_valid(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  store_->purge_partials();                                           // libtorrent has forgotten its partial state
  lt::add_torrent_params atp;
  atp.ti = ti_;
  atp.save_path = save_path_;
  atp.flags &= ~lt::torrent_flags::auto_managed;
  atp.flags &= ~lt::torrent_flags::paused;
  atp.file_priorities.assign(static_cast<std::size_t>(ti_->num_files()), lt::dont_download);
  atp.file_priorities[static_cast<std::size_t>(file_index_)] = lt::default_priority;
  lt::typed_bitfield<lt::piece_index_t> have(num_pieces_, false);
  for (int p : store_->present_pieces()) have.set_bit(lt::piece_index_t(p));
  atp.have_pieces = have;
  int head;
  { std::lock_guard<std::mutex> g(est_mu_); head = geo_.piece_of_file_offset(est_.offset()); }
  WindowPlan plan = planner_.plan(head, [&](int p) { return store_->has(p); });
  std::vector<lt::download_priority_t> prio(static_cast<std::size_t>(num_pieces_), lt::download_priority_t{0});
  for (int p = plan.head; p < plan.hi_wanted; ++p) prio[static_cast<std::size_t>(p)] = lt::download_priority_t{7};
  atp.piece_priorities = prio;
  atp.peers = peers;
  atp.trackers = trackers_;
  h_ = eng_.session().add_torrent(atp);
  { std::lock_guard<std::mutex> g(cfg_mu_); web_policy_.reset(); }
  for (auto const& u : web_urls_) h_.remove_url_seed(u);
  paused_by_disk_ = false;
  last_web_total_ = last_peer_total_ = 0;
  last_reset_ = Clock::now();
  prio_sig_ = -1;
  ++resets_;
  reset_requested_ = false;
  resetting_ = false;
}

StreamState StreamSession::compute_state(const lt::torrent_status& st, int blocked, double since_peer) {
  if (paused_by_disk_) return StreamState::DiskPaused;
  const int conns = st.num_peers;                                   // includes web seed connections
  if (conns == 0) return since_peer > 15.0 ? StreamState::NoPeers : StreamState::Connecting;
  if (blocked > 0) {
    if (secs(Clock::now() - last_data_) > 20.0) return StreamState::Stalled;
    bool pins_missing = false;
    for (int p = planner_.pin_head_first(); p <= planner_.pin_head_last(); ++p) if (!store_->has(p)) pins_missing = true;
    // The reader is waiting for container metadata when the piece it needs is a pinned head or tail piece (an MP4/MOV without "faststart"
    // keeps its index at the END of the file). That is "Preparing", however long it takes; it is not ordinary playback buffering.
    const WaitKind wk = classify_wait_piece(demand_piece_.load(), planner_.pin_head_first(), planner_.pin_head_last(), planner_.pin_tail_first(), planner_.pin_tail_last());
    const bool waiting_for_index = wk == WaitKind::HeadIndex || wk == WaitKind::TailIndex;
    return (waiting_for_index || (pins_missing && secs(Clock::now() - t0_) < 60.0)) ? StreamState::Preparing : StreamState::Buffering;
  }
  return StreamState::Ready;
}

void StreamSession::tick() {
  if (resetting_.load() || paused_by_user_start_.load()) return;
  const auto now = Clock::now();
  const double t = secs(now - t0_);
  lt::torrent_status s = h_.status();
  const bool checking = s.state == lt::torrent_status::checking_files || s.state == lt::torrent_status::checking_resume_data;
  static thread_local int tick_no = 0; ++tick_no;
  if (s.num_peers > 0) last_peer_seen_ = now;
  if (s.download_payload_rate > 0) last_data_ = now;

  if (tick_no % 10 == 0) {                                          // ~1 Hz: traffic split + web-seed policy
    update_traffic_split();
    WebSeedSample ws;
    ws.now = t; ws.demand_pending = blocked_.load() > 0 || demand_piece_.load() >= 0;
    ws.real_peers = std::max(0, s.num_peers - web_conns_.load()); ws.rate = s.download_payload_rate;
    ws.window_ready = false;
    { int head; { std::lock_guard<std::mutex> g(est_mu_); head = geo_.piece_of_file_offset(est_.offset()); }
      int have_n = 0; for (int p = head; p < head + 4 && p <= geo_.last_piece(); ++p) have_n += store_->has(p);
      ws.window_ready = have_n >= 4; }
    WebSeedAction act;
    { std::lock_guard<std::mutex> g(cfg_mu_); act = web_policy_.update(ws); }
    if (!strip_web_always_) {
      if (act == WebSeedAction::Enable) { web_enabled_ = true; for (auto const& u : web_urls_) if (!web_seed_off(u)) h_.add_url_seed(u); log_event("web seed policy: ENABLE"); }
      else if (act == WebSeedAction::Disable) { web_enabled_ = false; for (auto const& u : web_urls_) h_.remove_url_seed(u); log_event("web seed policy: DISABLE"); }
    }
  }
  if (tick_no % 10 == 5 && s.num_peers > 0) update_known_peers();                 // every second while any peer is connected
  if (tick_no % 10 == 7 && !checking) { try { release_stalled_web_seeds(t); } catch (std::exception const& e) { log_event(std::string("web seed stall check exception: ") + e.what()); } }
  if (tick_no % 10 == 3 && !checking) { try { update_wait_info(t); } catch (std::exception const& e) { log_event(std::string("wait diagnostics exception: ") + e.what()); } }

  // ---- window plan, protection, eviction, reconciliation
  int head; { std::lock_guard<std::mutex> g(est_mu_); head = geo_.piece_of_file_offset(est_.offset()); }
  WindowPlan plan;
  { std::lock_guard<std::mutex> g(cfg_mu_); plan = planner_.plan(head, [&](int p) { return store_->has(p); }); }
  Protection pr;
  pr.head = plan.head; pr.lo = plan.lo; pr.hi = plan.hi_full;
  pr.pin_a_first = planner_.pin_head_first(); pr.pin_a_last = planner_.pin_head_last();
  pr.pin_b_first = planner_.pin_tail_first(); pr.pin_b_last = planner_.pin_tail_last();
  store_->set_protection(pr);
  store_->sweep_outside_window(std::chrono::nanoseconds(static_cast<std::int64_t>(cfg_.touch_guard_seconds * 1e9)), 2);
  store_->reconcile_step(256);

  // ---- stale "have" for a demanded piece => un-have reset (debounced)
  const int dm = demand_piece_.load();
  if (!checking && dm >= 0 && !store_->has(dm) && secs(now - last_reset_) > 1.0 && h_.have_piece(lt::piece_index_t(dm))) { ++stale_; reset_requested_ = true; }
  if (enospc_.load() > 0) {                                         // a write was refused: the window did not fit; shrink it and reset
    enospc_ = 0;
    { std::lock_guard<std::mutex> g(cfg_mu_); planner_.shrink_ahead(0.75); }
    log_event("ENOSPC handling: ahead window shrunk to 75%, resetting");
    reset_requested_ = true;
  }
  {
    const bool need = blocked_.load() > 0 || plan.refilling;
    bool missing = false;
    if (need) { for (int p = plan.head; p < plan.hi_wanted && !missing; ++p) missing = !store_->has(p); }
    if (!paused_by_disk_ && !checking) connectivity_watchdog(s, need && missing, t);
  }
  if (reset_requested_.load()) { do_reset_have(); return; }

  // ---- priorities and deadlines
  if (!checking && !paused_by_disk_) {
    const std::int64_t sig = static_cast<std::int64_t>(plan.head) * 1'000'003LL + plan.hi_wanted * 7919LL + (dm + 1) * 31LL + (store_->count_present());
    if (sig != prio_sig_ || secs(now - last_prio_) > 2.0) { apply_priorities(plan, dm); prio_sig_ = sig; last_prio_ = now; }
  }

  // ---- disk policy (every 2 s)
  DiskState ds;
  if (tick_no % 20 == 0) {
    std::int64_t freeb;
    std::function<std::int64_t()> probe;
    { std::lock_guard<std::mutex> g(cfg_mu_); probe = free_probe_; }
    freeb = probe ? probe() : volume_free_bytes(eng_.config().cache_root);
    { std::lock_guard<std::mutex> g(cfg_mu_); ds = disk_policy_.update(freeb); }
    if (ds == DiskState::Paused && !paused_by_disk_) { h_.pause(); paused_by_disk_ = true; log_event("disk space low: downloads paused"); }
    else if (ds != DiskState::Paused && paused_by_disk_) { h_.resume(); paused_by_disk_ = false; log_event("disk space recovered: downloads resumed"); }
  }

  // ---- status snapshot
  StreamStatus st;
  st.peers = std::max(0, s.num_peers - web_conns_.load()); st.seeds = s.num_seeds; st.web_connections = web_conns_.load();
  st.download_rate = s.download_payload_rate; st.upload_rate = s.upload_payload_rate;
  st.web_bytes = web_bytes_.load(); st.peer_bytes = peer_bytes_.load();
  st.head = plan.head; st.lo = plan.lo; st.hi_full = plan.hi_full; st.hi_wanted = plan.hi_wanted; st.contiguous_ahead = plan.contiguous_ahead; st.refilling = plan.refilling;
  st.blocked_readers = blocked_.load();
  st.store = store_->metrics();
  { std::lock_guard<std::mutex> g(cfg_mu_); st.disk_state = disk_policy_.state(); st.web_seed_active = web_policy_.enabled(); }
  st.connect_kicks = kicks_.load(); st.watchdog_resets = wd_resets_.load();
  st.un_have_resets = resets_.load(); st.stale_have_detections = stale_.load(); st.file_errors = file_errors_.load(); st.enospc_events = enospc_.load(); st.peer_bans = bans_.load();
  const auto& c = shadow_->counters();
  st.hash_failures = c.failures.load(); st.hash_storage = c.storage.load(); st.hash_arrived = c.arrived.load(); st.hash_undetermined = c.undetermined.load(); st.pieces_verified = c.verified_ok.load();
  st.seconds_without_peers = s.num_peers == 0 ? secs(now - last_peer_seen_) : 0.0;
  st.wait = wait_cache_;
  st.web_seed_errors = web_alerts_.load(); st.web_disconnects = web_disconnects_.load(); st.request_drops = request_drops_.load(); st.block_timeouts = block_timeouts_.load();
  { std::lock_guard<std::mutex> dg(diag_mu_); st.web_seed_last_error = web_last_error_; }
  st.state = compute_state(s, st.blocked_readers, st.seconds_without_peers);
  st.detail = to_string(st.state);
  std::lock_guard<std::mutex> g(status_mu_);
  status_ = st;
}

StreamStatus StreamSession::status() const { std::lock_guard<std::mutex> g(status_mu_); return status_; }

}  // namespace tp
