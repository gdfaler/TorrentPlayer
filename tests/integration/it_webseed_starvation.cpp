// Loopback reproduction: does the DEMAND piece starve when some web seeds accept connections but never answer?
// (Internet Archive live test: piece 15 = tail of the previous file + head of the selected MP4 stayed at 0 blocks for 70 s while other pinned pieces progressed.)
// Fully local: synthetic multi-file torrent, three HTTP web seeds on 127.0.0.1 (one healthy, two "stalled": they accept, read the request and never answer), no BitTorrent peers.
// Instrumentation: every HTTP request each server sees (connection, path, range, time), the libtorrent download queue (which web-seed endpoint owns each block of the demand piece),
// per-server request/connection lifetime, and when the demand piece completes and the selected file becomes readable.
// Build/run: tools/scripts/run_webseed_starvation.sh   (not part of CMake: CMakeLists.txt is hash-locked)
#include "loopback_fixture.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <map>
#include <mutex>
#include <set>
using namespace tpit;

namespace {

constexpr int kPiece = 524288;
constexpr std::int64_t kA = 3LL * kPiece + 32502;       // preceding file: ends 32,502 bytes into piece 3
constexpr std::int64_t kB = 4LL * 1048576 + 12345;      // selected "MP4": starts inside piece 3
constexpr std::int64_t kC = 9034;                       // trailing small file
constexpr std::uint64_t kSeed = 20261024;
struct FileDef { const char* path; std::int64_t start, size; };
const FileDef kFiles[] = {{"item/a.bin", 0, kA}, {"item/b.mp4", kA, kB}, {"item/c.jpg", kA + kB, kC}};

void fill(std::int64_t off, std::int64_t len, char* out) {                       // byte at ABSOLUTE torrent offset: any slice reproducible
  for (std::int64_t i = 0; i < len;) {
    const std::int64_t pos = off + i, word = pos / 8; const int sh = static_cast<int>(pos % 8);
    const std::uint64_t w = splitmix(kSeed * 0x1000193ULL + static_cast<std::uint64_t>(word));
    const int n = static_cast<int>(std::min<std::int64_t>(8 - sh, len - i));
    std::memcpy(out + i, reinterpret_cast<const char*>(&w) + sh, static_cast<std::size_t>(n)); i += n;
  }
}

struct Ev { double t; int server; int conn; std::string what; };
std::mutex g_ev_mu; std::vector<Ev> g_ev;
void ev(int server, int conn, const std::string& what) { std::lock_guard<std::mutex> g(g_ev_mu); g_ev.push_back({now_s(), server, conn, what}); }

enum class Mode { Healthy, Stalled };

class WebServer {
 public:
  WebServer(int index, int port, Mode m, int rate_bps, double first_byte_delay = 0) : idx_(index), port_(port), mode_(m), rate_(rate_bps), first_delay_(first_byte_delay) {
    lfd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1; ::setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(static_cast<uint16_t>(port)); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(lfd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd_, 16) != 0) { std::perror("bind/listen"); std::exit(2); }
    th_ = std::thread([this] { loop(); });
  }
  ~WebServer() { stop_ = true; ::shutdown(lfd_, SHUT_RDWR); ::close(lfd_); if (th_.joinable()) th_.join(); for (auto& t : conns_) if (t.joinable()) t.join(); }
  int port() const { return port_; }
  std::atomic<std::int64_t> bytes_sent{0};
 private:
  void loop() {
    while (!stop_) {
      int fd = ::accept(lfd_, nullptr, nullptr);
      if (fd < 0) break;
      const int id = ++next_conn_;
      ev(idx_, id, "connect");
      conns_.emplace_back([this, fd, id] { serve(fd, id); });
    }
  }
  static bool read_headers(int fd, std::string& buf, std::atomic<bool>& stop) {
    char tmp[4096];
    while (buf.find("\r\n\r\n") == std::string::npos) {
      ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
      if (n <= 0 || stop) return false;
      buf.append(tmp, static_cast<std::size_t>(n));
    }
    return true;
  }
  void serve(int fd, int id) {
    std::string buf; bool delayed = false;
    while (!stop_ && read_headers(fd, buf, stop_)) {
      const auto end = buf.find("\r\n\r\n");
      const std::string head = buf.substr(0, end); buf.erase(0, end + 4);
      char path[512] = {0}; std::sscanf(head.c_str(), "GET %511s", path);
      long long a = 0, b = -1; const auto rp = head.find("Range: bytes="); if (rp != std::string::npos) std::sscanf(head.c_str() + rp, "Range: bytes=%lld-%lld", &a, &b);
      const FileDef* f = nullptr; for (auto& d : kFiles) if (std::string(path).find(d.path) != std::string::npos) f = &d;
      char desc[200]; std::snprintf(desc, sizeof desc, "GET %s bytes=%lld-%lld", path, a, b); ev(idx_, id, desc);
      if (mode_ == Mode::Stalled) {                                     // accept, read the request, never answer
        while (!stop_) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        break;
      }
      if (!f) { const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"; ::send(fd, nf, std::strlen(nf), 0); continue; }
      if (first_delay_ > 0 && !delayed) { delayed = true; std::this_thread::sleep_for(std::chrono::duration<double>(first_delay_)); }      // slow first byte on each connection (a legitimately slow start)
      if (b < 0 || b >= f->size) b = f->size - 1;
      const std::int64_t len = b - a + 1;
      char hdr[400]; std::snprintf(hdr, sizeof hdr, "HTTP/1.1 206 Partial Content\r\nContent-Length: %lld\r\nContent-Range: bytes %lld-%lld/%lld\r\nAccept-Ranges: bytes\r\n\r\n", (long long)len, a, b, (long long)f->size);
      ::send(fd, hdr, std::strlen(hdr), 0);
      std::vector<char> chunk(16384);
      for (std::int64_t sent = 0; sent < len && !stop_;) {
        const std::int64_t n = std::min<std::int64_t>(static_cast<std::int64_t>(chunk.size()), len - sent);
        fill(f->start + a + sent, n, chunk.data());
        ssize_t w = ::send(fd, chunk.data(), static_cast<std::size_t>(n), 0);
        if (w <= 0) { ev(idx_, id, "send failed (client closed)"); goto done; }
        sent += w; bytes_sent += w;
        if (rate_ > 0) std::this_thread::sleep_for(std::chrono::duration<double>(static_cast<double>(w) / rate_));
      }
      ev(idx_, id, "response complete");
    }
  done:
    ev(idx_, id, "closed"); ::close(fd);
  }
  int idx_, port_; Mode mode_; int rate_; double first_delay_; int lfd_ = -1;
  std::atomic<bool> stop_{false}; std::thread th_; std::vector<std::thread> conns_; std::atomic<int> next_conn_{0};
};

struct Result {
  std::vector<std::pair<int, int>> penalties;    // (seconds off, strike) in the order the core logged them
  std::set<int> released;                      // web-seed indexes the core released as stalled (from its event log)
  bool demand_done = false; double demand_done_s = -1; bool readable = false; double readable_s = -1; bool bytes_ok = true;
  int max_other_pin_blocks = 0; int demand_blocks_at_end = 0; double max_no_progress = 0;
  std::int64_t bytes_by_server[3] = {0, 0, 0};
  int demand_requests_on_stalled_peak = 0; int demand_blocks_on_healthy_peak = 0; double first_progress_other_s = -1;
};

std::string make_torrent(const fs::path& dir, std::vector<std::string> const& urls) {
  fs::create_directories(dir / "item");
  for (auto& f : kFiles) {
    std::ofstream o(dir / f.path, std::ios::binary | std::ios::trunc); std::vector<char> b(1 << 20);
    for (std::int64_t off = 0; off < f.size; off += static_cast<std::int64_t>(b.size())) { const auto n = std::min<std::int64_t>(b.size(), f.size - off); fill(f.start + off, n, b.data()); o.write(b.data(), n); }
  }
  lt::file_storage fsr; for (auto& f : kFiles) fsr.add_file(f.path, f.size);
  lt::create_torrent ct(fsr, kPiece, lt::create_flags_t{} | lt::create_torrent::v1_only);
  for (auto& u : urls) ct.add_url_seed(u);
  lt::set_piece_hashes(ct, dir.string());
  std::vector<char> out; lt::bencode(std::back_inserter(out), ct.generate());
  return std::string(out.begin(), out.end());
}

// add_at > 0: the seed is NOT in the torrent; it is added to the running torrent add_at seconds after start, i.e. it becomes usable AFTER the stalled seeds already hold requests
// (the Internet Archive HTTPS seed needed a redirect and a TLS handshake before it could serve, the dead plain-HTTP seeds were connected at once).
struct Spec { Mode mode; int rate; double add_at = 0; double first_byte_delay = 0; bool dup_url = false; };   // dup_url: the same server is listed under two URLs
Result run(const char* label, const std::vector<Spec>& servers, double budget_s, int base_port) {
  ITLOG("==== %s: %zu web seed(s): %s ====", label, servers.size(), [&] { static std::string s; s.clear(); for (auto& x : servers) s += (x.mode == Mode::Healthy ? "healthy" : "STALLED") + std::string(x.rate ? "@" + std::to_string(x.rate / 1024) + "KB/s" : "") + (x.add_at > 0 ? "(added at t=" + std::to_string(x.add_at).substr(0, 3) + "s) " : " "); return s.c_str(); }());
  { std::lock_guard<std::mutex> g(g_ev_mu); g_ev.clear(); }
  std::vector<std::unique_ptr<WebServer>> srv; std::vector<std::string> urls;
  for (std::size_t i = 0; i < servers.size(); ++i) {
    srv.push_back(std::make_unique<WebServer>(static_cast<int>(i), base_port + static_cast<int>(i), servers[i].mode, servers[i].rate, servers[i].first_byte_delay));
    if (servers[i].dup_url) urls.push_back("http://127.0.0.1:" + std::to_string(base_port + static_cast<int>(i)) + "/mirror/");
    if (servers[i].add_at <= 0) urls.push_back("http://127.0.0.1:" + std::to_string(base_port + static_cast<int>(i)) + "/");
  }
  const fs::path dir = fresh_dir(std::string("webseed_starvation_") + label);
  const std::string tbytes = make_torrent(dir / "payload", urls);
  tp::Engine eng(engine_config(dir / "cache", base_port + 50));
  tp::TorrentSource src; src.torrent_bytes.assign(tbytes.begin(), tbytes.end()); src.strip_trackers = true; src.strip_web_seeds = false;
  std::string err; auto ss = eng.open_stream(src, 1, tp::CacheConfig{}, &err);
  Result r; if (!ss) { ITLOG("open_stream failed: %s", err.c_str()); return r; }
  ss->set_web_seed_mode(tp::WebSeedMode::Always);
  ss->set_bitrate(1.0 * 1048576);
  const int demand_piece = static_cast<int>(kA / kPiece);                                    // 3
  ITLOG("geometry: piece %d, file B starts at torrent offset %lld = piece %d offset %lld; demand piece %d holds %lld bytes of a.bin and %lld of b.mp4; head pin %d..%d", kPiece, (long long)kA, demand_piece, (long long)(kA % kPiece), demand_piece,
        (long long)(kA % kPiece), (long long)(kPiece - kA % kPiece), demand_piece, demand_piece + 3);
  std::atomic<bool> reader_done{false}; std::atomic<double> read_s{-1}; std::atomic<bool> bytes_ok{true};
  const auto t0 = Clock::now();
  std::thread reader([&] {                                                                 // a demuxer reading the head of the selected file (the container index)
    std::vector<char> buf(65536); std::int64_t pos = 0;
    while (pos < 262144 && secs(Clock::now() - t0) < budget_s) {
      std::atomic<bool> cancel{false};
      std::thread killer([&] { while (!cancel && secs(Clock::now() - t0) < budget_s) std::this_thread::sleep_for(std::chrono::milliseconds(50)); cancel = true; });
      const std::int64_t n = ss->read(pos, buf.data(), static_cast<std::int64_t>(buf.size()), &cancel);
      cancel = true; killer.join();
      if (n <= 0) break;
      std::vector<char> want(static_cast<std::size_t>(n)); fill(kA + pos, n, want.data());
      if (std::memcmp(want.data(), buf.data(), static_cast<std::size_t>(n)) != 0) bytes_ok = false;
      pos += n;
      if (read_s < 0) read_s = secs(Clock::now() - t0);
    }
    reader_done = true;
  });
  std::map<std::string, std::string> owner_label;
  for (std::size_t i = 0; i < servers.size(); ++i) owner_label["127.0.0.1:" + std::to_string(base_port + static_cast<int>(i))] = "S" + std::to_string(i) + (servers[i].mode == Mode::Stalled ? "(stalled)" : "(ok)");
  double next_print = 0; int last_demand_done = 0; std::vector<bool> added(servers.size(), false);
  while (secs(Clock::now() - t0) < budget_s && !reader_done) {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    const double t = secs(Clock::now() - t0);
    for (std::size_t i = 0; i < servers.size(); ++i) if (servers[i].add_at > 0 && !added[i] && t >= servers[i].add_at) {
      added[i] = true; ss->handle().add_url_seed("http://127.0.0.1:" + std::to_string(base_port + static_cast<int>(i)) + "/"); ITLOG("t=%5.1f healthy web seed S%zu added to the running torrent", t, i);
    }
    std::map<std::string, int> demand_owner_req, demand_owner_done; int demand_done = 0, demand_req = 0, other_pin_done = 0;
    try {
      for (auto const& pp : ss->handle().get_download_queue()) {
        const int p = static_cast<int>(pp.piece_index);
        for (int b = 0; b < pp.blocks_in_piece && pp.blocks; ++b) {
          const auto& bi = pp.blocks[b];
          std::string who = bi.num_peers ? [&] { auto e = bi.peer(); return e.address().to_string() + ":" + std::to_string(e.port()); }() : "-";
          if (p == demand_piece) {
            if (bi.state == lt::block_info::finished) { ++demand_done; ++demand_owner_done[owner_label.count(who) ? owner_label[who] : who]; }
            else if (bi.state == lt::block_info::requested || bi.state == lt::block_info::writing) { ++demand_req; ++demand_owner_req[owner_label.count(who) ? owner_label[who] : who]; }
          } else if (p > demand_piece && p <= demand_piece + 3 && bi.state == lt::block_info::finished) ++other_pin_done;
        }
      }
    } catch (std::exception const&) {}
    const auto st = ss->status();
    other_pin_done = st.wait.head_pin_blocks_done - (st.wait.piece == demand_piece ? st.wait.blocks_done : 0);
    const bool have = ss->store().has(demand_piece);
    if (have && !r.demand_done) { r.demand_done = true; r.demand_done_s = t; }
    r.max_other_pin_blocks = std::max(r.max_other_pin_blocks, other_pin_done);
    if (other_pin_done > 0 && r.first_progress_other_s < 0) r.first_progress_other_s = t;
    r.max_no_progress = std::max(r.max_no_progress, st.wait.no_progress_s);
    int on_stalled = 0, on_healthy = 0; for (auto& kv : demand_owner_req) (kv.first.find("stalled") != std::string::npos ? on_stalled : on_healthy) += kv.second;
    r.demand_requests_on_stalled_peak = std::max(r.demand_requests_on_stalled_peak, on_stalled);
    r.demand_blocks_on_healthy_peak = std::max(r.demand_blocks_on_healthy_peak, on_healthy + [&] { int n = 0; for (auto& kv : demand_owner_done) if (kv.first.find("ok") != std::string::npos) n += kv.second; return n; }());
    r.demand_blocks_at_end = have ? 32 : demand_done;
    if (t >= next_print || demand_done != last_demand_done || have) {
      next_print = t + 2.0; last_demand_done = demand_done;
      std::string own; for (auto& kv : demand_owner_req) own += kv.first + ":req" + std::to_string(kv.second) + " "; for (auto& kv : demand_owner_done) own += kv.first + ":done" + std::to_string(kv.second) + " ";
      ITLOG("t=%5.1f demand piece %d: done %2d/32 requested %2d [%s] present=%d | other head-pin blocks done %3d | no_progress %.1fs | web conns %d web_bytes %lld disconnects %d", t, demand_piece, demand_done, demand_req, own.c_str(), have ? 1 : 0, other_pin_done,
            st.wait.no_progress_s, st.web_connections, (long long)st.web_bytes, st.web_disconnects);
    }
  }
  reader_done = true; reader.join();
  for (auto const& e : ss->recent_events()) { const auto p = e.find("released stalled web seed #"); if (p != std::string::npos) r.released.insert(std::atoi(e.c_str() + p + 27)); }
  for (auto const& e : ss->recent_events()) { const auto p = e.find("off for "); if (p != std::string::npos && e.find("released stalled") != std::string::npos) { int secs_off = 0, strike = 0; std::sscanf(e.c_str() + p, "off for %d s (strike %d)", &secs_off, &strike); r.penalties.push_back({secs_off, strike}); } }
  for (auto const& e : ss->recent_events()) if (e.find("released stalled") != std::string::npos || e.find("back after its penalty") != std::string::npos) ITLOG("core event: %s", e.c_str());
  r.readable = read_s >= 0 && bytes_ok; r.readable_s = read_s; r.bytes_ok = bytes_ok;
  for (std::size_t i = 0; i < srv.size() && i < 3; ++i) r.bytes_by_server[i] = srv[i]->bytes_sent;
  {  // request ownership and timing from the servers' side
    std::lock_guard<std::mutex> g(g_ev_mu);
    std::map<int, std::vector<const Ev*>> by; for (auto& e : g_ev) by[e.server].push_back(&e);
    for (auto& kv : by) {
      int conns = 0, reqs = 0, demand_reqs = 0, closed = 0; double first_close = -1;
      for (auto* e : kv.second) {
        if (e->what == "connect") ++conns; else if (e->what.rfind("GET", 0) == 0) { ++reqs; long long a = 0, b = 0; const auto p = e->what.find("bytes="); std::sscanf(e->what.c_str() + p, "bytes=%lld-%lld", &a, &b);
          const std::string path = e->what.substr(4, e->what.find(' ', 4) - 4); long long fs0 = path.find("b.mp4") != std::string::npos ? kA : path.find("c.jpg") != std::string::npos ? kA + kB : 0; const long long absa = fs0 + a, absb = fs0 + b;
          if (absb >= demand_piece * (long long)kPiece && absa < (demand_piece + 1) * (long long)kPiece) ++demand_reqs; }
        else if (e->what == "closed") { ++closed; if (first_close < 0) first_close = e->t; }
      }
      ITLOG("server S%d (%s): %d connections, %d HTTP requests (%d touch the demand piece), %d closed%s, %lld bytes sent", kv.first, servers[kv.first].mode == Mode::Healthy ? "healthy" : "STALLED", conns, reqs, demand_reqs, closed,
            first_close >= 0 ? (" (first close at t=" + std::to_string(first_close) + ")").c_str() : "", (long long)srv[kv.first]->bytes_sent.load());
    }
    int shown = 0; for (auto& e : g_ev) { if (e.what.rfind("GET", 0) == 0 && shown < 14) { ITLOG("  request t=%6.2f S%d conn%d %s", e.t, e.server, e.conn, e.what.c_str()); ++shown; } }
  }
  ITLOG("%s result: demand piece %s%s, selected file readable: %s%s, max no-progress %.1f s, other head-pin blocks done peak %d, peak demand-piece requests held by stalled seeds %d", label, r.demand_done ? "COMPLETE at t=" : "NOT complete", r.demand_done ? std::to_string(r.demand_done_s).c_str() : "",
        r.readable ? "yes" : "NO", r.readable ? (" (first 64 KiB at t=" + std::to_string(r.readable_s) + " s)").c_str() : "", r.max_no_progress, r.max_other_pin_blocks, r.demand_requests_on_stalled_peak);
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  const double budget = argc > 1 ? std::atof(argv[1]) : 90.0;
  const std::string only = argc > 2 ? argv[2] : "";
  // The healthy seed is ADDED to the running torrent 3 s after start, i.e. after the two stalled seeds (connected at once) already hold requests: the order seen in the Internet Archive
  // test, where the HTTPS seed needed a redirect and a TLS handshake. "slow" rates: 60 KB/s and 15 KB/s (the ~15 KB/s seen live).
  struct Sc { const char* key; const char* label; std::vector<Spec> servers; double max_complete_s; std::set<int> allowed_release; bool must_release; bool require_complete = true; };
  const std::vector<Sc> all = {
    {"base_15", "healthy only 15 KB/s, added at 3 s (reference)", {{Mode::Healthy, 15 * 1024, 3.0}}, 60, {}, false},
    {"stalled_15", "2 stalled + healthy 15 KB/s added at 3 s", {{Mode::Stalled, 0}, {Mode::Stalled, 0}, {Mode::Healthy, 15 * 1024, 3.0}}, 90, {0, 1}, true},
    {"flowing_15", "stalled #0 + slow but FLOWING healthy #1 in the torrent from the start", {{Mode::Stalled, 0}, {Mode::Healthy, 15 * 1024}}, 75, {0}, false},
    // two URLs on one server cannot be attributed to a single URL: the remedy must stay out of it (libtorrent's own timeouts apply); completion is not required here
    {"ambiguous_endpoint", "stalled server listed under two URLs + healthy 60 KB/s added at 3 s", {{Mode::Stalled, 0, 0.0, 0.0, true}, {Mode::Healthy, 60 * 1024, 3.0}}, 0, {}, false, false},
    {"slow_start_alone", "one healthy server whose first byte takes 12 s (sole source: must not be touched)", {{Mode::Healthy, 0, 0.0, 12.0}}, 40, {}, false},
  };
  std::vector<std::pair<std::string, Result>> res; int port = 44100; int failures = 0;
  for (auto& sc : all) if (only.empty() || only == sc.key) {
    Result r = run(sc.key, sc.servers, budget, port); port += 100;
    bool ok = r.bytes_ok && (!sc.require_complete || (r.demand_done && r.readable && r.demand_done_s <= sc.max_complete_s));
    for (std::size_t i = 0; i < r.penalties.size(); ++i) {                       // penalties double per strike (30, 60, 120, 240 s)
      const int expect = std::min(240, 30 << std::min(r.penalties[i].second - 1, 3));
      if (r.penalties[i].first != expect) { ITLOG("ASSERT: penalty %d s at strike %d, expected %d s", r.penalties[i].first, r.penalties[i].second, expect); ok = false; }
    }
    if (sc.key == std::string("stalled_15") && std::none_of(r.penalties.begin(), r.penalties.end(), [](auto& p) { return p.first == 30 && p.second == 1; })) { ITLOG("ASSERT: no first-strike 30 s penalty logged"); ok = false; }
    for (int i : r.released) if (!sc.allowed_release.count(i)) { ITLOG("ASSERT: web seed #%d was released but must not be", i); ok = false; }
    if (sc.must_release && r.released.empty()) { ITLOG("ASSERT: a stalled seed holding the demand piece was never released"); ok = false; }
    ITLOG("%s: %s (demand piece %s at t=%.1f s, limit %.0f s; released seeds: %zu; penalties logged: %zu)", sc.key, ok ? "PASS" : "FAIL", r.demand_done ? "complete" : "INCOMPLETE", r.demand_done_s, sc.max_complete_s, r.released.size(), r.penalties.size());
    if (!ok) ++failures;
    res.push_back({sc.label, r});
  }
  ITLOG("==== SUMMARY (budget %.0f s per scenario) ====", budget);
  for (auto& kv : res) { const Result& r = kv.second;
    ITLOG("%-78s demand piece %-10s %s | readable %-3s | peak demand-piece requests held by stalled seeds %2d | max no-progress %5.1f s | released %zu", kv.first.c_str(),
          r.demand_done ? "complete" : "INCOMPLETE", r.demand_done ? ("t=" + std::to_string(r.demand_done_s).substr(0, 5) + "s").c_str() : "        ", r.readable ? "yes" : "NO", r.demand_requests_on_stalled_peak, r.max_no_progress, r.released.size()); }
  ITLOG("%s (%d failing scenario(s))", failures ? "WEBSEED STARVATION TEST: FAIL" : "WEBSEED STARVATION TEST: PASS", failures);
  return failures ? 1 : 0;
}
