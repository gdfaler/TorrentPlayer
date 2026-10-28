// Loopback regression: cross-file demand piece with redirecting and plain web seeds (production configuration, no test switches).
// Geometry (synthetic): the selected "MP4" starts in the MIDDLE of piece 3, which also holds the tail of the previous file (the "demand piece"). A redirecting seed answers every request with a
// 302 to a hidden healthy server (libtorrent then learns that server per file; it advertises only the files it was redirected for); the plain seed holds every file.
// Scenarios: direct (control); xfile_delayed* (plain seed answers its first request after 14 s: it must NOT be released and the piece must finish near the 14 s mark);
// xfile_redirect_only (only a redirecting seed: the demand piece must complete; needs the web-seed refresh); xfile_stalled* (plain seed never answers: informational, completion is not required, connection churn must stay bounded); *B variants list the plain seed first.
// Fully local (127.0.0.1), synthetic data, no BitTorrent peers. Build/run: tools/scripts/run_webseed_crossfile.sh [seconds-per-run] [scenario]. Exit status 1 if an assertion fails.
#include "loopback_fixture.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <csignal>
#include <map>
#include <mutex>
#include <set>
using namespace tpit;

namespace {
constexpr int kPiece = 524288;
constexpr std::int64_t kA = 3LL * kPiece + 32502, kB = 8LL * 1048576 + 12345, kC = 9034;   // previous file ends mid piece 3; the selected "MP4" starts there
constexpr std::uint64_t kSeed = 20261025;
struct FileDef { const char* path; std::int64_t start, size; };
const FileDef kFiles[] = {{"item/a.bin", 0, kA}, {"item/b.mp4", kA, kB}, {"item/c.jpg", kA + kB, kC}};

void fill(std::int64_t off, std::int64_t len, char* out) {
  for (std::int64_t i = 0; i < len;) {
    const std::int64_t pos = off + i, word = pos / 8; const int sh = static_cast<int>(pos % 8);
    const std::uint64_t w = splitmix(kSeed * 0x1000193ULL + static_cast<std::uint64_t>(word));
    const int n = static_cast<int>(std::min<std::int64_t>(8 - sh, len - i));
    std::memcpy(out + i, reinterpret_cast<const char*>(&w) + sh, static_cast<std::size_t>(n)); i += n;
  }
}
struct Ev { double t; int server; int conn; std::string what; };
std::mutex g_mu; std::vector<Ev> g_ev;
void ev(int server, int conn, const std::string& what) { std::lock_guard<std::mutex> g(g_mu); g_ev.push_back({now_s(), server, conn, what}); }

enum class Mode { Healthy, Stalled, Redirect, Delayed };   // Delayed: the first request is answered only delay_s seconds after it arrived (then valid data, and every later request at once)
class Server {
 public:
  Server(int idx, int port, Mode m, int redirect_port, double delay_s = 0, double rate_bps = 0) : idx_(idx), port_(port), mode_(m), redirect_port_(redirect_port), delay_s_(delay_s), rate_(rate_bps) {
    lfd_ = ::socket(AF_INET, SOCK_STREAM, 0); int one = 1; ::setsockopt(lfd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(static_cast<uint16_t>(port)); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(lfd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(lfd_, 16) != 0) { std::perror("bind/listen"); std::exit(2); }
    th_ = std::thread([this] { loop(); });
  }
  ~Server() { stop_ = true; ::shutdown(lfd_, SHUT_RDWR); ::close(lfd_); if (th_.joinable()) th_.join(); for (auto& t : conns_) if (t.joinable()) t.join(); }
  std::atomic<std::int64_t> bytes{0};
 private:
  void loop() { while (!stop_) { int fd = ::accept(lfd_, nullptr, nullptr); if (fd < 0) break; const int id = ++next_; ev(idx_, id, "connect"); conns_.emplace_back([this, fd, id] { serve(fd, id); }); } }
  void serve(int fd, int id) {
    std::string buf; char tmp[4096];
    for (;;) {
      while (buf.find("\r\n\r\n") == std::string::npos) { ssize_t n = ::recv(fd, tmp, sizeof tmp, 0); if (n <= 0 || stop_) goto done; buf.append(tmp, static_cast<std::size_t>(n)); }
      const auto end = buf.find("\r\n\r\n"); const std::string head = buf.substr(0, end); buf.erase(0, end + 4);
      char path[512] = {0}; std::sscanf(head.c_str(), "GET %511s", path);
      long long a = 0, b = -1; const auto rp = head.find("Range: bytes="); if (rp != std::string::npos) std::sscanf(head.c_str() + rp, "Range: bytes=%lld-%lld", &a, &b);
      const FileDef* f = nullptr; for (auto& d : kFiles) if (std::string(path).find(d.path) != std::string::npos) f = &d;
      char desc[200]; std::snprintf(desc, sizeof desc, "GET %s bytes=%lld-%lld", f ? f->path : path, a, b); ev(idx_, id, desc);
      if (mode_ == Mode::Delayed) {
        double due; { std::lock_guard<std::mutex> g(dmu_); if (first_due_ < 0) first_due_ = now_s() + delay_s_; due = first_due_; }
        while (!stop_ && now_s() < due) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (stop_) goto done;
      }
      if (mode_ == Mode::Stalled) { while (!stop_) std::this_thread::sleep_for(std::chrono::milliseconds(100)); goto done; }
      if (mode_ == Mode::Redirect) {
        char r[400]; std::snprintf(r, sizeof r, "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:%d%s\r\nContent-Length: 0\r\n\r\n", redirect_port_, path);
        ::send(fd, r, std::strlen(r), 0); ev(idx_, id, std::string("302 for ") + (f ? f->path : path)); continue;
      }
      if (!f) { const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"; ::send(fd, nf, std::strlen(nf), 0); continue; }
      if (b < 0 || b >= f->size) b = f->size - 1;
      const std::int64_t len = b - a + 1;
      char hdr[400]; std::snprintf(hdr, sizeof hdr, "HTTP/1.1 206 Partial Content\r\nContent-Length: %lld\r\nContent-Range: bytes %lld-%lld/%lld\r\nAccept-Ranges: bytes\r\n\r\n", (long long)len, a, b, (long long)f->size);
      ::send(fd, hdr, std::strlen(hdr), 0);
      std::vector<char> chunk(16384);
      for (std::int64_t sent = 0; sent < len && !stop_;) {
        const auto n = std::min<std::int64_t>(static_cast<std::int64_t>(chunk.size()), len - sent); fill(f->start + a + sent, n, chunk.data());
        ssize_t w = ::send(fd, chunk.data(), static_cast<std::size_t>(n), 0); if (w <= 0) { ev(idx_, id, "send failed (client closed)"); goto done; }
        sent += w; bytes += w;
        if (rate_ > 0) std::this_thread::sleep_for(std::chrono::microseconds(static_cast<long long>(1e6 * static_cast<double>(w) / rate_)));   // paced: the source keeps delivering for a known time
      }
      ev(idx_, id, "response complete");
    }
  done:
    ev(idx_, id, "closed"); ::close(fd);
  }
  double delay_s_, rate_; std::mutex dmu_; double first_due_ = -1; int idx_, port_; Mode mode_; int redirect_port_; int lfd_ = -1; std::atomic<bool> stop_{false}; std::thread th_; std::vector<std::thread> conns_; std::atomic<int> next_{0};
};

struct Spec { Mode mode; int redirect_to = -1; bool hidden = false; double delay_s = 0; double rate_bps = 0; };       // hidden: not listed in the torrent (a redirect target)
struct Run { bool done = false; double done_s = -1; int verified_end = 0, resets = 0, wd_resets = 0, kicks = 0, redirects = 0, prio_min = 99, prio_max = -1, deadline_nonzero_samples = 0, samples_missing = 0; double max_no_progress = 0;
             std::int64_t served[4] = {0, 0, 0, 0}; std::set<std::string> redirected_files; int piece3_a_requests = 0; int releases = 0, readds_after_penalty = 0; int conns[4] = {0, 0, 0, 0}; };

std::string make_torrent(const fs::path& dir, std::vector<std::string> const& urls) {
  fs::create_directories(dir / "item");
  for (auto& f : kFiles) { std::ofstream o(dir / f.path, std::ios::binary | std::ios::trunc); std::vector<char> b(1 << 20);
    for (std::int64_t off = 0; off < f.size; off += static_cast<std::int64_t>(b.size())) { const auto n = std::min<std::int64_t>(b.size(), f.size - off); fill(f.start + off, n, b.data()); o.write(b.data(), n); } }
  lt::file_storage fsr; for (auto& f : kFiles) fsr.add_file(f.path, f.size);
  lt::create_torrent ct(fsr, kPiece, lt::create_flags_t{} | lt::create_torrent::v1_only);
  for (auto& u : urls) ct.add_url_seed(u);
  lt::set_piece_hashes(ct, dir.string());
  std::vector<char> out; lt::bencode(std::back_inserter(out), ct.generate());
  return std::string(out.begin(), out.end());
}

Run run(const char* label, const std::vector<Spec>& sv, double budget_s, int base_port) {
  ITLOG("==== %s ====", label);
  { std::lock_guard<std::mutex> g(g_mu); g_ev.clear(); }
  std::vector<std::unique_ptr<Server>> srv; std::vector<std::string> urls;
  for (std::size_t i = 0; i < sv.size(); ++i) { srv.push_back(std::make_unique<Server>(static_cast<int>(i), base_port + static_cast<int>(i), sv[i].mode, sv[i].redirect_to >= 0 ? base_port + sv[i].redirect_to : 0, sv[i].delay_s, sv[i].rate_bps)); if (!sv[i].hidden) urls.push_back("http://127.0.0.1:" + std::to_string(base_port + static_cast<int>(i)) + "/"); }
  const fs::path dir = fresh_dir(std::string("webseed_crossfile_") + label);
  const std::string tb = make_torrent(dir / "payload", urls);
  tp::Engine eng(engine_config(dir / "cache", base_port + 50));
  tp::TorrentSource src; src.torrent_bytes.assign(tb.begin(), tb.end()); src.strip_trackers = true; src.strip_web_seeds = false;
  std::string err; auto ss = eng.open_stream(src, 1, tp::CacheConfig{}, &err);
  Run r; if (!ss) { ITLOG("open_stream failed: %s", err.c_str()); return r; }
  ss->set_web_seed_mode(tp::WebSeedMode::Always); ss->set_bitrate(1.0 * 1048576);
  const int demand = static_cast<int>(kA / kPiece);
  const auto t0 = Clock::now(); std::atomic<bool> rd{false}; std::atomic<double> first_read{-1};
  std::thread reader([&] { std::vector<char> buf(65536); std::int64_t pos = 0;
    while (pos < 262144 && secs(Clock::now() - t0) < budget_s) { std::atomic<bool> c{false}; std::thread k([&] { while (!c && secs(Clock::now() - t0) < budget_s) std::this_thread::sleep_for(std::chrono::milliseconds(50)); c = true; });
      const auto n = ss->read(pos, buf.data(), static_cast<std::int64_t>(buf.size()), &c); c = true; k.join(); if (n <= 0) break; pos += n; if (first_read < 0) first_read = secs(Clock::now() - t0); }
    rd = true; });
  double next_print = 0;
  while (secs(Clock::now() - t0) < budget_s && !rd) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const double t = secs(Clock::now() - t0); const auto st = ss->status();
    if (st.wait.piece == demand) { r.prio_min = std::min(r.prio_min, st.wait.priority); r.prio_max = std::max(r.prio_max, st.wait.priority); if (st.wait.deadline_ms != 0) ++r.deadline_nonzero_samples; }
    r.max_no_progress = std::max(r.max_no_progress, st.wait.no_progress_s);
    if (ss->store().has(demand) && !r.done) { r.done = true; r.done_s = t; }
    if (t >= next_print) { next_print = t + 3.0;
      ITLOG("t=%5.1f verified %2d | demand piece %d: %2d/%d blocks, requested %2d, in queue %d | web conns %d web bytes %lld | web requesting %d | watchdog kicks %d re-adds %d", t, st.store.present, demand, st.wait.blocks_done, st.wait.blocks_total, st.wait.blocks_requested,
            st.wait.in_queue, st.web_connections, (long long)st.web_bytes, st.wait.web_requesting, st.connect_kicks, st.un_have_resets); }
  }
  rd = true; reader.join();
  const auto st = ss->status(); r.verified_end = st.store.present; r.resets = st.un_have_resets; r.wd_resets = st.watchdog_resets; r.kicks = st.connect_kicks;
  { std::lock_guard<std::mutex> g(g_mu);
    for (auto& e : g_ev) { if (e.what.rfind("302 for", 0) == 0) { ++r.redirects; r.redirected_files.insert(e.what.substr(8)); }
      if (e.what == "GET item/a.bin bytes=1572864-1605365") ++r.piece3_a_requests; }
    for (std::size_t i = 0; i < srv.size() && i < 4; ++i) r.served[i] = srv[i]->bytes;
    for (auto& e : g_ev) if (e.what == "connect" && e.server < 4) ++r.conns[e.server];
    std::map<int, std::pair<int, std::vector<double>>> life; std::map<std::pair<int, int>, double> opened;
    for (auto& e : g_ev) { if (e.what == "connect") opened[{e.server, e.conn}] = e.t; else if (e.what == "closed") life[e.server].second.push_back(e.t - opened[{e.server, e.conn}]); if (e.what == "connect") ++life[e.server].first; }
    for (auto& kv : life) { double mx = 0, mn = 1e9; for (double d : kv.second.second) { mx = std::max(mx, d); mn = std::min(mn, d); }
      ITLOG("server S%d (%s): %d connections, %zu closed (lifetime %.1f..%.1f s), %lld bytes sent", kv.first, sv[kv.first].mode == Mode::Redirect ? "redirector" : sv[kv.first].mode == Mode::Stalled ? "STALLED" : sv[kv.first].mode == Mode::Delayed ? "DELAYED" : "healthy", kv.second.first, kv.second.second.size(), kv.second.second.empty() ? 0.0 : mn, mx, (long long)srv[kv.first]->bytes.load()); }
    int shown = 0; for (auto& e : g_ev) if ((e.what.rfind("GET", 0) == 0 || e.what.rfind("302", 0) == 0) && shown++ < 40) ITLOG("  t=%6.2f S%d conn%d %s", e.t, e.server, e.conn, e.what.c_str()); }
  for (auto const& e : ss->recent_events()) if (e.find("released stalled") != std::string::npos || e.find("refreshed") != std::string::npos || e.find("web seed back") != std::string::npos || e.find("web seed policy") != std::string::npos || e.find("re-adding") != std::string::npos || e.find("WEB SEED ALERT") != std::string::npos || e.find("read blocked") != std::string::npos) ITLOG("  session event %s", e.c_str());
  for (auto const& e : ss->recent_events()) { if (e.find("released stalled") != std::string::npos) ++r.releases; if (e.find("web seed back") != std::string::npos) ++r.readds_after_penalty; }
  ITLOG("%s: demand piece %s%s | verified pieces at end %d | actual torrent re-adds %d (watchdog-initiated %d), re-dial checks %d | redirects %d for %zu file(s) | demand priority %d..%d, deadline!=0 in %d samples | max no-progress %.1f s | stalled-seed releases %d, re-added after penalty %d", label,
        r.done ? "COMPLETE at t=" : "NOT complete", r.done ? std::to_string(r.done_s).substr(0, 5).c_str() : "", r.verified_end, r.resets, r.wd_resets, r.kicks, r.redirects, r.redirected_files.size(), r.prio_min, r.prio_max, r.deadline_nonzero_samples, r.max_no_progress, r.releases, r.readds_after_penalty);
  return r;
}
}  // namespace

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);   // a test server writing to a connection the client just closed must not kill the harness
  const double budget = argc > 1 ? std::atof(argv[1]) : 60.0; const std::string only = argc > 2 ? argv[2] : "";
  const Spec paced{Mode::Healthy, -1, true, 0, 250000.0};     // hidden redirect target; paced so that it keeps delivering for ~15 s (a source that is busy with OTHER pieces)
  struct Sc { const char* key; const char* label; std::vector<Spec> servers; bool delayed, stalled; };
  const std::vector<Sc> all = {
    {"direct", "direct_healthy_seed", {{Mode::Healthy}}, false, false},
    {"xfile_redirect_only", "xfile_redirecting_seed_only", {{Mode::Redirect, 1}, paced}, false, false},
    {"xfile_delayed", "xfile_plain_seed_delayed_14s", {{Mode::Redirect, 2}, {Mode::Delayed, -1, false, 14.0}, paced}, true, false},
    {"xfileB_delayed", "xfileB_plain_first_delayed_14s", {{Mode::Delayed, -1, false, 14.0}, {Mode::Redirect, 2}, paced}, true, false},
    {"xfile_stalled", "xfile_plain_seed_stalled", {{Mode::Redirect, 2}, {Mode::Stalled}, paced}, false, true},
    {"xfileB_stalled", "xfileB_plain_first_stalled", {{Mode::Stalled}, {Mode::Redirect, 2}, paced}, false, true},
  };
  int port = 44400, failures = 0;
  std::vector<std::pair<std::string, Run>> res;
  for (auto& sc : all) {
    if (!only.empty() ? only != sc.key : false) continue;
    Run r = run(sc.label, sc.servers, budget, port); port += 100; res.push_back({sc.label, r});
    if (std::string(sc.key) == "direct" && (!r.done || r.done_s > 10)) { ITLOG("ASSERT FAILED: direct control must finish within 10 s"); ++failures; }
    if (sc.delayed) {
      if (!r.done || r.done_s > 25.0) { ITLOG("ASSERT FAILED: delayed plain seed: the demand piece must complete within 25 s (release-disabled baseline 15.6 s, with the old release policy 40.8 s)"); ++failures; }
      if (r.releases != 0) { ITLOG("ASSERT FAILED: the only source able to serve the cross-file piece was released (%d)", r.releases); ++failures; }
    }
    if (std::string(sc.key) == "xfile_redirect_only") {      // fixed by refreshing unclaimed web seeds: without the refresh the cross-file demand piece is claimed by no seed and never completes (0 of 3 runs in 60-90 s)
      if (!r.done || r.done_s > 75.0) { ITLOG("ASSERT FAILED: redirecting seed only: the demand piece must complete within 75 s (measured 22-47 s; did not complete in 70-90 s without the refresh)"); ++failures; }
      for (int i = 0; i < 3; ++i) if (r.conns[i] > 24) { ITLOG("ASSERT FAILED: connection churn: server S%d saw %d connections (bound 24; measured up to 21)", i, r.conns[i]); ++failures; }
    }
    if (sc.stalled) {      // informational: completion is not required; churn must stay bounded and the harness must not claim success it did not see
      if (r.releases > 1) { ITLOG("ASSERT FAILED: stalled plain seed released %d times within one run (penalties must space releases out)", r.releases); ++failures; }
      for (int i = 0; i < 3; ++i) if (r.conns[i] > 12) { ITLOG("ASSERT FAILED: connection churn: server S%d saw %d connections", i, r.conns[i]); ++failures; }
      ITLOG("INFO %s: demand piece %s (not a requirement)", sc.label, r.done ? "completed" : "did not complete in the budget");
    }
  }
  ITLOG("==== SUMMARY ====");
  for (auto& kv : res) { const Run& r = kv.second; ITLOG("%-40s demand piece %-10s %-8s verified %2d | stalled-seed releases %d | torrent re-adds %d | connections S0/S1/S2 %d/%d/%d", kv.first.c_str(), r.done ? "complete" : "INCOMPLETE", r.done ? (std::to_string(r.done_s).substr(0, 4) + " s").c_str() : "", r.verified_end, r.releases, r.resets, r.conns[0], r.conns[1], r.conns[2]); }
  ITLOG("%s (%d assertion failure(s))", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
