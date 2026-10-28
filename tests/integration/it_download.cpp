// Download & Play regression tests (tpcore/download.hpp). Fully local: BitTorrent seeders on 127.0.0.1, synthetic multi-file content, no trackers, no DHT, no external network.
// Usage: it_download [scenario ...]   (no argument: all scenarios). Exit status 1 if any check fails.
// Content: torrent folder "t" with a.bin (ends 32,502 bytes into piece 3), v.mp4 (the "video", starts inside piece 3) and c.srt; 256 KiB pieces.
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/torrent_info.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include "tp_test.hpp"
#include "tpcore/download.hpp"
#include "tpcore/engine.hpp"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static double secs_since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
#define LOG(...) do { std::printf("[%7.2f] ", secs_since(g_t0)); std::printf(__VA_ARGS__); std::printf("\n"); std::fflush(stdout); } while (0)
static Clock::time_point g_t0 = Clock::now();

namespace {
constexpr int kPiece = 262144;
struct FileDef { const char* path; std::int64_t size; };
const FileDef kFiles[] = {{"t/a.bin", 3LL * kPiece + 32502}, {"t/v.mp4", 6LL * 1048576 + 777}, {"t/c.srt", 9034}};
constexpr int kVideo = 1;

std::uint64_t mix(std::uint64_t x) { x += 0x9E3779B97F4A7C15ULL; x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL; x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL; return x ^ (x >> 31); }
void fill(int file, std::int64_t off, std::int64_t len, char* out, int salt = 0) {
  for (std::int64_t i = 0; i < len; ++i) { const std::int64_t p = off + i; out[i] = static_cast<char>(mix(static_cast<std::uint64_t>(file) * 0x1000193ULL + static_cast<std::uint64_t>(salt) * 0x9E3779B1ULL + static_cast<std::uint64_t>(p / 8)) >> ((p % 8) * 8)); }
}
bool file_matches(const fs::path& p, int file, int salt = 0) {
  std::ifstream f(p, std::ios::binary); if (!f) return false;
  std::vector<char> got(1 << 20), want(1 << 20);
  for (std::int64_t off = 0; off < kFiles[file].size; off += static_cast<std::int64_t>(got.size())) {
    const auto n = std::min<std::int64_t>(static_cast<std::int64_t>(got.size()), kFiles[file].size - off);
    f.read(got.data(), n); if (f.gcount() != n) return false;
    fill(file, off, n, want.data(), salt); if (std::memcmp(got.data(), want.data(), static_cast<std::size_t>(n)) != 0) return false;
  }
  return true;
}
bool first_bytes_are_zero(const fs::path& p, int n = 4096) {
  std::ifstream f(p, std::ios::binary); if (!f) return true;
  std::vector<char> b(static_cast<std::size_t>(n)); f.read(b.data(), n);
  for (std::streamsize i = 0; i < f.gcount(); ++i) if (b[static_cast<std::size_t>(i)] != 0) return false;
  return true;
}

struct Content {
  fs::path root;                       // contains t/
  std::vector<char> torrent;
  std::string hex;
  static Content make(const fs::path& base, int salt = 0, const char* dir = "seed_content", const std::string& tracker = "", const std::string& web_seed = "") {
    Content c; c.root = base / dir; fs::create_directories(c.root / "t");
    lt::file_storage fsr;
    for (int i = 0; i < 3; ++i) {
      std::ofstream o(c.root / kFiles[i].path, std::ios::binary | std::ios::trunc); std::vector<char> b(1 << 20);
      for (std::int64_t off = 0; off < kFiles[i].size; off += static_cast<std::int64_t>(b.size())) { const auto n = std::min<std::int64_t>(b.size(), kFiles[i].size - off); fill(i, off, n, b.data(), salt); o.write(b.data(), n); }
      fsr.add_file(kFiles[i].path, kFiles[i].size);
    }
    lt::create_torrent ct(fsr, kPiece, lt::create_flags_t{} | lt::create_torrent::v1_only);
    if (!tracker.empty()) ct.add_tracker(tracker);
    if (!web_seed.empty()) ct.add_url_seed(web_seed);
    lt::set_piece_hashes(ct, c.root.string());
    lt::bencode(std::back_inserter(c.torrent), ct.generate());
    lt::torrent_info ti(c.torrent.data(), static_cast<int>(c.torrent.size()));
    const auto h = ti.info_hashes().get_best(); static const char* d = "0123456789abcdef";
    for (int i = 0; i < 20; ++i) { auto u = static_cast<unsigned char>(h[i]); c.hex += d[u >> 4]; c.hex += d[u & 15]; }
    return c;
  }
};

// A genuine BitTorrent seeder on loopback that can be stopped and started again (a connectivity outage as seen from the downloader).
struct Seeder {
  const Content& c; int port; int limit; std::unique_ptr<lt::session> ses; lt::torrent_handle h;
  Seeder(const Content& c_, int p, int upload_limit) : c(c_), port(p), limit(upload_limit) { start(); }
  void start() {
    lt::settings_pack sp;
    sp.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:" + std::to_string(port));
    sp.set_bool(lt::settings_pack::enable_dht, false); sp.set_bool(lt::settings_pack::enable_lsd, false); sp.set_bool(lt::settings_pack::enable_upnp, false); sp.set_bool(lt::settings_pack::enable_natpmp, false);
    sp.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
    sp.set_int(lt::settings_pack::out_enc_policy, lt::settings_pack::pe_disabled); sp.set_int(lt::settings_pack::in_enc_policy, lt::settings_pack::pe_disabled);
    sp.set_int(lt::settings_pack::alert_mask, 0);
    ses = std::make_unique<lt::session>(lt::session_params(sp));
    lt::add_torrent_params atp; atp.ti = std::make_shared<lt::torrent_info>(c.torrent.data(), static_cast<int>(c.torrent.size()));
    atp.save_path = c.root.string(); atp.flags |= lt::torrent_flags::seed_mode;
    h = ses->add_torrent(atp);
    if (limit > 0) h.set_upload_limit(limit);
  }
  void stop() { h = lt::torrent_handle(); ses.reset(); }
  std::int64_t uploaded() { return h.is_valid() ? h.status().total_upload : 0; }
  std::string ep() const { return "127.0.0.1:" + std::to_string(port); }
};

tp::DownloadConfig make_cfg(const fs::path& state, int port, bool auto_resume = true) {
  tp::DownloadConfig c; c.state_dir = state; c.listen_interfaces = "127.0.0.1:" + std::to_string(port);
  c.enable_dht = false; c.enable_lsd = false; c.loopback_test_mode = true; c.min_reconnect_time_s = 1; c.auto_resume_on_network = auto_resume; c.resume_save_interval_s = 3;
  return c;
}
tp::DownloadRequest make_req(const Content& c, const fs::path& dest, std::vector<int> files, const std::string& peer) {
  tp::DownloadRequest r; r.torrent_bytes = c.torrent; r.save_path = dest; r.files = std::move(files); if (!peer.empty()) r.extra_peers = {peer}; r.strip_trackers = true;
  return r;
}
bool wait_for(double timeout_s, const std::function<bool()>& pred) {
  const auto t = Clock::now();
  while (secs_since(t) < timeout_s) { if (pred()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
  return pred();
}
tp::DownloadStatus st(tp::DownloadManager& m, const std::string& id) { tp::DownloadStatus s; if (!m.status(id, &s)) s.state = tp::DlState::Failed, s.error = "no such download"; return s; }
bool wait_state(tp::DownloadManager& m, const std::string& id, tp::DlState want, double timeout_s) { return wait_for(timeout_s, [&] { return st(m, id).state == want; }); }
fs::path fresh(const fs::path& base, const std::string& name) { fs::path p = base / name; fs::remove_all(p); fs::create_directories(p); return p; }
fs::path file_of(const fs::path& dest, int f) { return dest / kFiles[f].path; }
constexpr int kSlow = 1000000;     // ~1 MB/s from the seeder: a 6 MB video takes ~6 s, long enough to interrupt

}  // namespace

static fs::path g_base;
static const Content* g_content = nullptr;
static int g_port = 46100;
static int next_port() { return g_port += 3; }

// ---------------------------------------------------------------- scenarios
static void sc_selected_file() {
  const auto root = fresh(g_base, "selected"); Seeder seed(*g_content, next_port(), 0);
  tp::DownloadManager m(make_cfg(root / "state", next_port())); const auto dest = root / "dest";
  std::string err; const auto id = m.add(make_req(*g_content, dest, {kVideo}, seed.ep()), &err);
  TP_CHECK(!id.empty()); if (id.empty()) { LOG("add failed: %s", err.c_str()); return; }
  TP_CHECK(wait_state(m, id, tp::DlState::Completed, 60));
  const auto s = st(m, id);
  TP_CHECK(s.completed); TP_CHECK_EQ(s.verified_bytes, kFiles[kVideo].size); TP_CHECK(s.progress >= 0.999);
  TP_CHECK(file_matches(file_of(dest, kVideo), kVideo));
  TP_CHECK(!s.primary_file.empty() && s.primary_file == file_of(dest, kVideo).string());
  TP_CHECK(first_bytes_are_zero(file_of(dest, 0)));                      // the unrelated file's data was not fetched (only the bytes it shares with the video's first piece)
  TP_CHECK(!fs::exists(file_of(dest, 2)) || first_bytes_are_zero(file_of(dest, 2)));
  TP_CHECK_EQ(m.live_torrents(), 0);                                     // finished: no longer transferring or seeding
  LOG("selected file complete, %lld bytes verified", (long long)s.verified_bytes);
}
static void sc_whole_torrent() {
  const auto root = fresh(g_base, "whole"); Seeder seed(*g_content, next_port(), 0);
  tp::DownloadManager m(make_cfg(root / "state", next_port())); const auto dest = root / "dest";
  const auto id = m.add(make_req(*g_content, dest, {}, seed.ep()));
  TP_CHECK(!id.empty()); if (id.empty()) return;
  TP_CHECK(wait_state(m, id, tp::DlState::Completed, 60));
  for (int f = 0; f < 3; ++f) TP_CHECK(file_matches(file_of(dest, f), f));
  TP_CHECK_EQ(st(m, id).files.size(), std::size_t(3));
}
static void sc_honest_progress() {                                       // progress never exceeds verified data and never claims completion early; stale files on disk are not trusted
  const auto root = fresh(g_base, "honest"); Seeder seed(*g_content, next_port(), kSlow);
  const auto dest = root / "dest"; fs::create_directories(dest / "t");
  { std::ofstream o(file_of(dest, kVideo), std::ios::binary); std::vector<char> z(1 << 20, 'Z'); for (std::int64_t w = 0; w < kFiles[kVideo].size; w += (1 << 20)) o.write(z.data(), std::min<std::int64_t>(1 << 20, kFiles[kVideo].size - w)); }   // full-size file with WRONG content
  tp::DownloadManager m(make_cfg(root / "state", next_port()));
  const auto id = m.add(make_req(*g_content, dest, {kVideo}, seed.ep()));
  TP_CHECK(!id.empty()); if (id.empty()) return;
  double last = 0; bool ever_complete_early = false, monotonic = true;
  const auto t = Clock::now();
  while (secs_since(t) < 60) {
    const auto s = st(m, id);
    if (s.progress + 1e-9 < last) monotonic = false;
    last = std::max(last, s.progress);
    if (s.state == tp::DlState::Completed) { if (!file_matches(file_of(dest, kVideo), kVideo)) ever_complete_early = true; break; }
    if (s.verified_bytes > s.selected_bytes) monotonic = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  TP_CHECK(monotonic); TP_CHECK(!ever_complete_early);
  TP_CHECK(st(m, id).completed); TP_CHECK(file_matches(file_of(dest, kVideo), kVideo));        // wrong bytes were detected and replaced
}
static void sc_magnet() {
  const auto root = fresh(g_base, "magnet"); Seeder seed(*g_content, next_port(), 0);
  tp::EngineConfig ec; ec.cache_root = root / "streamcache"; ec.listen_interfaces = "127.0.0.1:" + std::to_string(next_port());
  ec.enable_dht = false; ec.enable_lsd = false; ec.enable_upnp = false; ec.enable_natpmp = false; ec.loopback_test_mode = true; ec.min_reconnect_time_s = 1;
  tp::Engine eng(ec);
  std::string err; auto job = eng.start_metadata("magnet:?xt=urn:btih:" + g_content->hex + "&x.pe=" + seed.ep(), &err);
  TP_CHECK(job != nullptr); if (!job) { LOG("start_metadata: %s", err.c_str()); return; }
  tp::MetadataPoll mp; TP_CHECK(wait_for(30, [&] { mp = eng.poll_metadata(*job); return mp.ready; }));
  if (!mp.ready) return;
  TP_CHECK(!mp.bytes.empty());
  tp::DownloadManager m(make_cfg(root / "state", next_port())); const auto dest = root / "dest";
  tp::DownloadRequest r; r.torrent_bytes = mp.bytes; r.save_path = dest; r.files = {kVideo}; r.extra_peers = {seed.ep()}; r.strip_trackers = true;
  const auto id = m.add(r, &err);
  TP_CHECK(id == g_content->hex); if (id.empty()) { LOG("add: %s", err.c_str()); return; }
  TP_CHECK(wait_state(m, id, tp::DlState::Completed, 60));
  TP_CHECK(file_matches(file_of(dest, kVideo), kVideo));
}
static void sc_duplicate() {
  const auto root = fresh(g_base, "dup"); Seeder seed(*g_content, next_port(), kSlow);
  tp::DownloadManager m(make_cfg(root / "state", next_port())); const auto dest = root / "dest";
  const auto a = m.add(make_req(*g_content, dest, {kVideo}, seed.ep())), b = m.add(make_req(*g_content, dest, {kVideo}, seed.ep()));
  TP_CHECK(!a.empty() && a == b); TP_CHECK_EQ(m.live_torrents(), 1); TP_CHECK_EQ(m.list().size(), std::size_t(1));
  m.cancel(a, false);
}
static void sc_resume_new_session() {
  const auto root = fresh(g_base, "resume"); Seeder seed(*g_content, next_port(), kSlow);
  const auto state = root / "state", dest = root / "dest"; std::string id; std::int64_t at_quit = 0;
  {
    tp::DownloadManager a(make_cfg(state, next_port()));
    id = a.add(make_req(*g_content, dest, {kVideo}, seed.ep()));
    TP_CHECK(!id.empty()); if (id.empty()) return;
    TP_CHECK(wait_for(40, [&] { return st(a, id).verified_bytes >= kFiles[kVideo].size * 35 / 100; }));
    at_quit = st(a, id).verified_bytes;
    LOG("session A quits at %lld of %lld bytes", (long long)at_quit, (long long)kFiles[kVideo].size);
  }                                                                       // ~DownloadManager: graceful, writes resume data
  TP_CHECK(fs::exists(state / id / "resume.dat")); TP_CHECK(fs::exists(state / id / "state.bencode")); TP_CHECK(fs::exists(state / id / "meta.torrent"));
  const auto uploaded_before = seed.uploaded();
  tp::DownloadManager b(make_cfg(state, next_port()));                    // a brand-new core session
  TP_CHECK_EQ(b.restore(), 1);
  TP_CHECK(wait_for(20, [&] { auto s = st(b, id); return s.state != tp::DlState::Checking && s.verified_bytes > 0; }));
  const auto after_restore = st(b, id).verified_bytes;
  TP_CHECK(after_restore >= at_quit * 9 / 10);                            // did not start from zero
  TP_CHECK(wait_state(b, id, tp::DlState::Completed, 60));
  TP_CHECK(file_matches(file_of(dest, kVideo), kVideo));
  const auto fetched_again = seed.uploaded() - uploaded_before;
  TP_CHECK(fetched_again < kFiles[kVideo].size * 75 / 100);               // only the missing part was transferred in session B
  LOG("restored at %lld, session B fetched %lld bytes", (long long)after_restore, (long long)fetched_again);
}
static void resume_variant(const char* name, const std::function<void(const fs::path& statedir, const fs::path& dest, const std::string& id)>& damage) {
  const auto root = fresh(g_base, name); Seeder seed(*g_content, next_port(), kSlow);
  const auto state = root / "state", dest = root / "dest"; std::string id;
  {
    tp::DownloadManager a(make_cfg(state, next_port()));
    id = a.add(make_req(*g_content, dest, {kVideo}, seed.ep()));
    TP_CHECK(!id.empty()); if (id.empty()) return;
    TP_CHECK(wait_for(40, [&] { return st(a, id).verified_bytes >= kFiles[kVideo].size * 40 / 100; }));
  }
  damage(state / id, dest, id);
  tp::DownloadManager b(make_cfg(state, next_port()));
  TP_CHECK_EQ(b.restore(), 1);
  TP_CHECK(wait_state(b, id, tp::DlState::Completed, 90));
  TP_CHECK(file_matches(file_of(dest, kVideo), kVideo));                  // correct bytes whatever happened to the saved state
}
static void sc_resume_missing() { resume_variant("resume_missing", [](const fs::path& s, const fs::path&, const std::string&) { fs::remove(s / "resume.dat"); }); }
static void sc_resume_garbage() { resume_variant("resume_garbage", [](const fs::path& s, const fs::path&, const std::string&) { std::ofstream o(s / "resume.dat", std::ios::binary | std::ios::trunc); o << "this is not bencoded resume data"; }); }
static void sc_resume_modified() {                                        // a verified region was changed after the resume data was written
  resume_variant("resume_modified", [](const fs::path&, const fs::path& dest, const std::string&) {
    std::this_thread::sleep_for(std::chrono::seconds(3));                 // make the modification clearly newer than the resume file
    std::fstream f(file_of(dest, kVideo), std::ios::in | std::ios::out | std::ios::binary); f.seekp(1 << 20); const char x[16] = {'B','A','D'}; f.write(x, sizeof x);
  });
}
static void sc_cancel() {
  const auto root = fresh(g_base, "cancel"); Seeder seed(*g_content, next_port(), kSlow);
  const auto dest = root / "dest";
  {
    tp::DownloadManager m(make_cfg(root / "state", next_port()));
    const auto id = m.add(make_req(*g_content, dest, {kVideo}, seed.ep()));
    TP_CHECK(wait_for(30, [&] { return st(m, id).verified_bytes >= kFiles[kVideo].size / 4; }));
    TP_CHECK(m.cancel(id, false));                                        // keep partial data
    TP_CHECK(fs::exists(file_of(dest, kVideo))); TP_CHECK(m.list().empty()); TP_CHECK_EQ(m.live_torrents(), 0); TP_CHECK(!fs::exists(root / "state" / id));
  }
  {
    tp::DownloadManager m(make_cfg(root / "state2", next_port()));        // adding it again picks up the kept data instead of starting from zero
    const auto id = m.add(make_req(*g_content, dest, {kVideo}, seed.ep()));
    TP_CHECK(wait_state(m, id, tp::DlState::Completed, 60)); TP_CHECK(file_matches(file_of(dest, kVideo), kVideo));
    TP_CHECK(m.cancel(id, true));                                         // "delete" on data that was already in the folder when this download started (the partial data kept above): left alone
    TP_CHECK(fs::exists(file_of(dest, kVideo)));
  }
}
static void sc_destination_errors() {
  const auto root = fresh(g_base, "dest_err"); Seeder seed(*g_content, next_port(), 0);
  tp::DownloadManager m(make_cfg(root / "state", next_port())); std::string err;
  const auto ro = root / "readonly"; fs::create_directories(ro); ::chmod(ro.c_str(), 0555);
  if (::access(ro.c_str(), W_OK) != 0) {                                   // (skipped when running as a user that ignores permissions)
    TP_CHECK(m.add(make_req(*g_content, ro / "sub", {kVideo}, seed.ep()), &err).empty()); TP_CHECK(!err.empty()); LOG("unwritable destination: %s", err.c_str());
  }
  ::chmod(ro.c_str(), 0755);
  auto cfg = make_cfg(root / "state2", next_port()); cfg.free_space = [](const fs::path&) { return std::int64_t(1) << 20; };      // 1 MiB free
  tp::DownloadManager m2(cfg); err.clear();
  TP_CHECK(m2.add(make_req(*g_content, root / "full", {kVideo}, seed.ep()), &err).empty()); TP_CHECK(err.find("free space") != std::string::npos); LOG("low disk space: %s", err.c_str());
  TP_CHECK(m2.list().empty());
}
static void sc_network_loss_and_restore() {
  const auto root = fresh(g_base, "net"); Seeder seed(*g_content, next_port(), kSlow);
  tp::DownloadManager m(make_cfg(root / "state", next_port())); const auto dest = root / "dest";
  const auto id = m.add(make_req(*g_content, dest, {kVideo}, seed.ep()));
  TP_CHECK(wait_for(30, [&] { return st(m, id).verified_bytes >= kFiles[kVideo].size / 4; }));
  seed.stop(); m.set_network_available(false);                            // the network disappears
  TP_CHECK(wait_state(m, id, tp::DlState::WaitingForNetwork, 5));
  const auto frozen = st(m, id).verified_bytes; std::this_thread::sleep_for(std::chrono::seconds(3));
  auto s = st(m, id); TP_CHECK(s.state == tp::DlState::WaitingForNetwork); TP_CHECK(s.verified_bytes >= frozen); TP_CHECK(!s.completed); TP_CHECK_EQ(m.live_torrents(), 1);
  seed.start(); m.set_network_available(true);                            // it comes back, together with the source
  TP_CHECK(wait_state(m, id, tp::DlState::Completed, 90));
  TP_CHECK(file_matches(file_of(dest, kVideo), kVideo)); TP_CHECK(m.network_restores_applied() >= 1);
  LOG("recovered automatically; restores applied: %d", m.network_restores_applied());
}
static void sc_rapid_flaps() {
  const auto root = fresh(g_base, "flaps"); Seeder seed(*g_content, next_port(), kSlow);
  tp::DownloadManager m(make_cfg(root / "state", next_port())); const auto id = m.add(make_req(*g_content, root / "dest", {kVideo}, seed.ep()));
  TP_CHECK(wait_for(30, [&] { return st(m, id).verified_bytes > 0; }));
  const int before = m.network_restores_applied();
  for (int i = 0; i < 20; ++i) { m.set_network_available(false); m.set_network_available(true); std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
  TP_CHECK(wait_state(m, id, tp::DlState::Completed, 90));
  TP_CHECK(m.network_restores_applied() - before <= 3);                   // coalesced: not one restore per flap
  TP_CHECK_EQ(m.list().size(), std::size_t(1)); TP_CHECK(file_matches(file_of(root / "dest", kVideo), kVideo));
  LOG("20 flaps -> %d restore actions", m.network_restores_applied() - before);
}
static void sc_pause_survives_network_change() {
  const auto root = fresh(g_base, "pause"); Seeder seed(*g_content, next_port(), kSlow);
  const auto state = root / "state", dest = root / "dest"; std::string id; std::int64_t at_pause = 0;
  {
    tp::DownloadManager m(make_cfg(state, next_port())); id = m.add(make_req(*g_content, dest, {kVideo}, seed.ep()));
    TP_CHECK(wait_for(30, [&] { return st(m, id).verified_bytes >= kFiles[kVideo].size / 5; }));
    TP_CHECK(m.pause(id)); TP_CHECK(wait_state(m, id, tp::DlState::PausedByUser, 5)); at_pause = st(m, id).verified_bytes;
    m.set_network_available(false); m.set_network_available(true); std::this_thread::sleep_for(std::chrono::seconds(4));
    auto s = st(m, id); TP_CHECK(s.state == tp::DlState::PausedByUser); TP_CHECK(s.verified_bytes <= at_pause + kPiece * 2);   // nothing downloaded while paused (in-flight pieces at most)
  }
  tp::DownloadManager b(make_cfg(state, next_port())); TP_CHECK_EQ(b.restore(), 1);                  // the pause survives a restart
  std::this_thread::sleep_for(std::chrono::seconds(3));
  TP_CHECK(st(b, id).state == tp::DlState::PausedByUser);
  TP_CHECK(b.resume(id)); TP_CHECK(wait_state(b, id, tp::DlState::Completed, 90)); TP_CHECK(file_matches(file_of(dest, kVideo), kVideo));
}
static void sc_no_sources_then_sources() {
  const auto root = fresh(g_base, "nosrc");
  tp::DownloadManager m(make_cfg(root / "state", next_port())); const int sport = next_port();
  std::unique_ptr<Seeder> seed;
  const auto id = m.add(make_req(*g_content, root / "dest", {kVideo}, "127.0.0.1:" + std::to_string(sport)));
  TP_CHECK(wait_for(40, [&] { auto s = st(m, id); return s.state == tp::DlState::WaitingForSources; }));      // honest: nothing to download from yet
  TP_CHECK(st(m, id).verified_bytes == 0);
  seed = std::make_unique<Seeder>(*g_content, sport, 0);                  // a source appears later; the known peer is re-dialled with bounded backoff
  {
    const auto t = Clock::now(); int last = -1;
    while (secs_since(t) < 120 && !st(m, id).completed) {
      const auto s = st(m, id); const int tick = static_cast<int>(secs_since(t) / 5);
      if (tick != last) { last = tick; LOG("state=%s peers=%d web=%d verified=%lld", tp::dl_state_name(s.state), s.peers, s.web_seeds, (long long)s.verified_bytes); }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  }
  TP_CHECK(st(m, id).completed); TP_CHECK(file_matches(file_of(root / "dest", kVideo), kVideo));
}
static void sc_auto_resume_disabled() {
  const auto root = fresh(g_base, "noauto"); Seeder seed(*g_content, next_port(), kSlow);
  tp::DownloadManager m(make_cfg(root / "state", next_port(), false)); const auto id = m.add(make_req(*g_content, root / "dest", {kVideo}, seed.ep()));
  TP_CHECK(wait_for(30, [&] { return st(m, id).verified_bytes > 0; }));
  m.set_network_available(false); m.set_network_available(true); std::this_thread::sleep_for(std::chrono::seconds(4));
  TP_CHECK(st(m, id).state == tp::DlState::WaitingForNetwork);            // the user opted out: nothing resumes by itself
  TP_CHECK(m.resume(id)); TP_CHECK(wait_state(m, id, tp::DlState::Completed, 90));
}
static void sc_no_background_ui_needed() {                                // transfers need no window/UI object: the manager alone keeps working (the app only owns it, closing a window does not touch it)
  const auto root = fresh(g_base, "headless"); Seeder seed(*g_content, next_port(), kSlow);
  tp::DownloadManager m(make_cfg(root / "state", next_port())); const auto id = m.add(make_req(*g_content, root / "dest", {kVideo}, seed.ep()));
  const auto a = st(m, id).verified_bytes; std::this_thread::sleep_for(std::chrono::seconds(3));
  TP_CHECK(st(m, id).verified_bytes > a || st(m, id).completed);
  TP_CHECK(wait_state(m, id, tp::DlState::Completed, 60));
}

// ---------------------------------------------------------------- completed downloads across restarts, and what "delete" may touch
static void complete_then_close(const fs::path& state, const fs::path& dest, const Seeder& seed, std::string* id) {
  tp::DownloadManager a(make_cfg(state, next_port()));
  *id = a.add(make_req(*g_content, dest, {kVideo}, seed.ep()));
  TP_CHECK(wait_state(a, *id, tp::DlState::Completed, 60));
}
static void flip_byte(const fs::path& p, std::int64_t at) { std::fstream f(p, std::ios::in | std::ios::out | std::ios::binary); f.seekg(at); char c = 0; f.read(&c, 1); c = static_cast<char>(c ^ 0x5A); f.seekp(at); f.write(&c, 1); }
static void sc_completed_modified_between_sessions() {   // same size, different bytes: must never be reported (or auto-played) as a verified completed file
  const auto root = fresh(g_base, "cmod"); Seeder seed(*g_content, next_port(), 0); const auto state = root / "state", dest = root / "dest"; std::string id;
  complete_then_close(state, dest, seed, &id); if (id.empty()) return;
  std::this_thread::sleep_for(std::chrono::seconds(2)); flip_byte(file_of(dest, kVideo), 3 * 1024 * 1024 + 11);
  TP_CHECK(!file_matches(file_of(dest, kVideo), kVideo));
  tp::DownloadManager b(make_cfg(state, next_port())); TP_CHECK_EQ(b.restore(), 1);
  bool false_completed = false;
  for (int i = 0; i < 400 && !st(b, id).completed; ++i) {               // watch the whole repair: "Completed" may only ever be shown for correct bytes
    const auto s = st(b, id); if (s.state == tp::DlState::Completed && !file_matches(file_of(dest, kVideo), kVideo)) false_completed = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (i == 0) TP_CHECK(s.state != tp::DlState::Completed);          // not trusted right after the restart
  }
  TP_CHECK(!false_completed); TP_CHECK(wait_state(b, id, tp::DlState::Completed, 60)); TP_CHECK(file_matches(file_of(dest, kVideo), kVideo));      // repaired from the source
}
static void sc_completed_modified_no_source() {
  const auto root = fresh(g_base, "cmod2"); std::string id; const auto state = root / "state", dest = root / "dest";
  { Seeder seed(*g_content, next_port(), 0); complete_then_close(state, dest, seed, &id); }
  if (id.empty()) return;
  std::this_thread::sleep_for(std::chrono::seconds(2)); flip_byte(file_of(dest, kVideo), 1234567);
  tp::DownloadManager b(make_cfg(state, next_port())); TP_CHECK_EQ(b.restore(), 1);
  for (int i = 0; i < 80; ++i) { TP_CHECK(st(b, id).state != tp::DlState::Completed); std::this_thread::sleep_for(std::chrono::milliseconds(100)); }   // 8 s, nothing to repair it with
  TP_CHECK(!st(b, id).completed); TP_CHECK(!st(b, id).primary_file.empty());
}
static void sc_completed_unchanged_restart() {            // ordinary fast path: unchanged data is trusted at once and nothing is transferred
  const auto root = fresh(g_base, "cok"); Seeder seed(*g_content, next_port(), 0); const auto state = root / "state", dest = root / "dest"; std::string id;
  complete_then_close(state, dest, seed, &id); if (id.empty()) return;
  const auto up = seed.uploaded();
  tp::DownloadManager b(make_cfg(state, next_port())); TP_CHECK_EQ(b.restore(), 1);
  TP_CHECK(st(b, id).state == tp::DlState::Completed); TP_CHECK(st(b, id).completed); TP_CHECK_EQ(b.live_torrents(), 0);
  std::this_thread::sleep_for(std::chrono::seconds(1)); TP_CHECK_EQ(seed.uploaded(), up);
}
static void sc_completed_touched_not_redownloaded() {     // only the modification time changed: re-checked against the piece hashes, no data fetched again
  const auto root = fresh(g_base, "ctouch"); Seeder seed(*g_content, next_port(), 0); const auto state = root / "state", dest = root / "dest"; std::string id;
  complete_then_close(state, dest, seed, &id); if (id.empty()) return;
  std::this_thread::sleep_for(std::chrono::seconds(2)); fs::last_write_time(file_of(dest, kVideo), fs::file_time_type::clock::now());
  const auto up = seed.uploaded();
  tp::DownloadManager b(make_cfg(state, next_port())); TP_CHECK_EQ(b.restore(), 1);
  TP_CHECK(st(b, id).state != tp::DlState::Completed);
  TP_CHECK(wait_state(b, id, tp::DlState::Completed, 30)); TP_CHECK(file_matches(file_of(dest, kVideo), kVideo));
  TP_CHECK_EQ(seed.uploaded(), up);
}
static void sc_cancel_delete_scope() {                     // "delete" removes this download's own files only
  const auto root = fresh(g_base, "delscope"); Seeder seed(*g_content, next_port(), kSlow); const auto dest = root / "dest";
  fs::create_directories(dest / "t"); { std::ofstream(dest / "notes.txt") << "mine"; std::ofstream(dest / "t" / "notes.txt") << "mine too"; }
  {
    tp::DownloadManager m(make_cfg(root / "state", next_port())); const auto id = m.add(make_req(*g_content, dest, {kVideo}, seed.ep()));
    TP_CHECK(wait_for(30, [&] { return st(m, id).verified_bytes >= kFiles[kVideo].size / 3; }));
    TP_CHECK(m.cancel(id, true));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  TP_CHECK(fs::exists(dest / "notes.txt")); TP_CHECK(fs::exists(dest / "t" / "notes.txt")); TP_CHECK(fs::is_directory(dest));          // unrelated files and the folder itself stay
  TP_CHECK(!fs::exists(file_of(dest, kVideo))); TP_CHECK(!fs::exists(file_of(dest, 0)));                                              // the download's own files (and its boundary file) are gone
  // a file that was already in the folder before the download started is never deleted
  const auto dest2 = root / "dest2"; fs::create_directories(dest2 / "t"); fs::copy_file(g_content->root / kFiles[0].path, file_of(dest2, 0));
  {
    tp::DownloadManager m(make_cfg(root / "state2", next_port())); const auto id = m.add(make_req(*g_content, dest2, {kVideo}, seed.ep()));
    TP_CHECK(wait_for(30, [&] { return st(m, id).verified_bytes >= kFiles[kVideo].size / 3; })); TP_CHECK(m.cancel(id, true));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  TP_CHECK(file_matches(file_of(dest2, 0), 0));
}
static void sc_overlapping_destinations() {               // two different torrents that use the same file paths
  const auto root = fresh(g_base, "overlap"); static const Content other = Content::make(g_base, 7, "seed_content2");
  Seeder s1(*g_content, next_port(), 0); Seeder s2(other, next_port(), 0); const auto dest = root / "dest";
  tp::DownloadManager m(make_cfg(root / "state", next_port())); std::string err;
  auto r1 = make_req(*g_content, dest, {kVideo}, s1.ep());
  const auto a = m.add(r1); TP_CHECK(!a.empty());
  tp::DownloadRequest r2; r2.torrent_bytes = other.torrent; r2.save_path = dest; r2.files = {kVideo}; r2.extra_peers = {s2.ep()}; r2.strip_trackers = true;
  TP_CHECK(m.add(r2, &err).empty()); TP_CHECK(err.find("already used by another download") != std::string::npos); LOG("overlap refused: %s", err.c_str());
  TP_CHECK(wait_state(m, a, tp::DlState::Completed, 60)); TP_CHECK(file_matches(file_of(dest, kVideo), kVideo));
  // B selects another file of the same layout, but its boundary data would be written into v.mp4, which A owns: refused as well, A's file stays intact
  tp::DownloadRequest r3 = r2; r3.files = {2};
  TP_CHECK(m.add(r3, &err).empty()); TP_CHECK(err.find("already used by another download") != std::string::npos);
  std::this_thread::sleep_for(std::chrono::milliseconds(500)); TP_CHECK(file_matches(file_of(dest, kVideo), kVideo));
  TP_CHECK(m.cancel(a, true)); TP_CHECK(!fs::exists(file_of(dest, kVideo)));           // deleting a completed download removes the files it created
}

// ---------------------------------------------------------------- production source discovery (no extra_peers): a tracker, and a web seed that sends no "Server" header
struct Tracker {                                           // HTTP tracker on loopback: answers every announce with one compact peer
  int fd = -1; int port; std::string peer_ip; int peer_port; std::atomic<bool> stop{false}; std::atomic<int> announces{0}; std::thread th; std::vector<std::thread> conns;
  Tracker(int p, const std::string& ip, int pp) : port(p), peer_ip(ip), peer_port(pp) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0); int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(static_cast<uint16_t>(port)); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 16) != 0) { std::perror("tracker bind/listen"); std::exit(2); }
    th = std::thread([this] { while (!stop) { int c = ::accept(fd, nullptr, nullptr); if (c < 0) break; conns.emplace_back([this, c] { serve(c); }); } });
  }
  ~Tracker() { stop = true; ::shutdown(fd, SHUT_RDWR); ::close(fd); if (th.joinable()) th.join(); for (auto& t : conns) if (t.joinable()) t.join(); }
  void serve(int c) {
    char buf[4096]; std::string req; while (req.find("\r\n\r\n") == std::string::npos) { ssize_t n = ::recv(c, buf, sizeof buf, 0); if (n <= 0) { ::close(c); return; } req.append(buf, static_cast<size_t>(n)); }
    ++announces;
    std::string peers(6, '\0'); in_addr ia{}; ::inet_pton(AF_INET, peer_ip.c_str(), &ia); std::memcpy(&peers[0], &ia, 4); peers[4] = static_cast<char>(peer_port >> 8); peers[5] = static_cast<char>(peer_port & 255);
    std::string body = "d8:completei1e10:incompletei0e8:intervali30e12:min intervali10e5:peers6:" + peers + "e";
    std::string resp = "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
    ::send(c, resp.data(), resp.size(), 0); ::close(c);
  }
};
struct WebSeedServer {                                     // plain HTTP file server with Range support and, like many real servers, no "Server" header
  int fd = -1; int port; fs::path root; int first_delay_ms; std::atomic<bool> stop{false}; std::atomic<int> requests{0}; std::thread th; std::vector<std::thread> conns;
  WebSeedServer(int p, fs::path r, int first_response_delay_ms = 0) : port(p), root(std::move(r)), first_delay_ms(first_response_delay_ms) {
    fd = ::socket(AF_INET, SOCK_STREAM, 0); int one = 1; ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(static_cast<uint16_t>(port)); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(fd, 16) != 0) { std::perror("web bind/listen"); std::exit(2); }
    th = std::thread([this] { while (!stop) { int c = ::accept(fd, nullptr, nullptr); if (c < 0) break; conns.emplace_back([this, c] { serve(c); }); } });
  }
  ~WebSeedServer() { stop = true; ::shutdown(fd, SHUT_RDWR); ::close(fd); if (th.joinable()) th.join(); for (auto& t : conns) if (t.joinable()) t.join(); }
  void serve(int c) {
    std::string buf; char tmp[4096];
    for (;;) {
      while (buf.find("\r\n\r\n") == std::string::npos) { ssize_t n = ::recv(c, tmp, sizeof tmp, 0); if (n <= 0 || stop) { ::close(c); return; } buf.append(tmp, static_cast<size_t>(n)); }
      const auto e = buf.find("\r\n\r\n"); const std::string head = buf.substr(0, e); buf.erase(0, e + 4);
      char path[512] = {0}; std::sscanf(head.c_str(), "GET %511s", path);
      if (requests.fetch_add(1) == 0 && first_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(first_delay_ms));      // the server is connected but silent for a while
      long long a = 0, b = -1; const auto rp = head.find("Range: bytes="); if (rp != std::string::npos) std::sscanf(head.c_str() + rp, "Range: bytes=%lld-%lld", &a, &b);
      std::ifstream f(root / (path + 1), std::ios::binary); if (!f) { const char* nf = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n"; ::send(c, nf, strlen(nf), 0); continue; }
      f.seekg(0, std::ios::end); const long long size = f.tellg(); if (b < 0 || b >= size) b = size - 1;
      char hdr[300]; std::snprintf(hdr, sizeof hdr, "HTTP/1.1 206 Partial Content\r\nContent-Length: %lld\r\nContent-Range: bytes %lld-%lld/%lld\r\n\r\n", b - a + 1, a, b, size);
      ::send(c, hdr, strlen(hdr), 0); f.seekg(a); std::vector<char> chunk(16384);
      for (long long left = b - a + 1; left > 0;) { const auto n = std::min<long long>(left, static_cast<long long>(chunk.size())); f.read(chunk.data(), n); if (::send(c, chunk.data(), static_cast<size_t>(n), 0) <= 0) { ::close(c); return; } left -= n; std::this_thread::sleep_for(std::chrono::milliseconds(20)); }
    }
  }
};
static void sc_tracker_discovery() {                       // the torrent's own tracker is the only way to find the peer: announce -> peer list -> connection -> verified download
  const auto root = fresh(g_base, "tracker"); const int tport = next_port(), sport = next_port();
  const Content tc = Content::make(g_base, 0, "seed_content_tracker", "http://127.0.0.1:" + std::to_string(tport) + "/announce");
  Seeder seed(tc, sport, 0); Tracker trk(tport, "127.0.0.1", sport);
  tp::DownloadManager m(make_cfg(root / "state", next_port())); const auto dest = root / "dest";
  tp::DownloadRequest r; r.torrent_bytes = tc.torrent; r.save_path = dest; r.files = {kVideo};      // no extra_peers, trackers kept
  const auto id = m.add(r); TP_CHECK(!id.empty()); if (id.empty()) return;
  TP_CHECK(wait_state(m, id, tp::DlState::Completed, 60)); TP_CHECK(file_matches(file_of(dest, kVideo), kVideo));
  TP_CHECK(trk.announces.load() >= 1); LOG("tracker saw %d announce(s)", trk.announces.load());
}
static void sc_web_seed_counts_as_source() {               // a connected web seed (here silent for 4 s, and sending no "Server" header) is a source: counted, and never shown as "Waiting for sources" while it is connected or data flows
  const auto root = fresh(g_base, "webseed_src"); const int wport = next_port();
  const Content wc = Content::make(g_base, 0, "seed_content_web", "", "http://127.0.0.1:" + std::to_string(wport) + "/");
  WebSeedServer web(wport, wc.root, 4000);
  tp::DownloadManager m(make_cfg(root / "state", next_port())); const auto dest = root / "dest";
  tp::DownloadRequest r; r.torrent_bytes = wc.torrent; r.save_path = dest; r.files = {kVideo}; r.strip_trackers = true;
  const auto id = m.add(r); TP_CHECK(!id.empty()); if (id.empty()) return;
  bool saw_sources = false, waiting_while_data_flows = false, saw_progress = false, silent_connected_seen = false, silent_connected_mislabelled = false, reused_accumulates = false;
  tp::DownloadStatus reused;                                // the same out-parameter on every poll: source counters must describe the moment, not add up (found with a real swarm: 1470 "peers" from 118)
  const auto t = Clock::now();
  while (secs_since(t) < 60) {
    const int web_requests = web.requests.load();      // read BEFORE the snapshot: a request the server has seen implies a connection libtorrent already had, so the snapshot cannot predate it
    const auto s = st(m, id);
    m.status(id, &reused); if (reused.web_seeds > 1 || reused.peers > 0 || reused.connecting > 5) reused_accumulates = true;
    if (s.web_seeds >= 1) saw_sources = true;
    if (web_requests >= 1 && s.verified_bytes == 0 && !s.completed) {     // the web seed has been asked and has not answered yet: it is connected, and a connected source must be counted and not shown as "Waiting for sources"
      silent_connected_seen = true; if (s.web_seeds < 1 || s.state == tp::DlState::WaitingForSources) silent_connected_mislabelled = true;
    }
    if (s.verified_bytes > 0) saw_progress = true;
    if (s.state == tp::DlState::WaitingForSources && (s.download_rate > 0 || s.verified_bytes > 0 || s.web_seeds > 0)) waiting_while_data_flows = true;
    if (s.completed) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  TP_CHECK(saw_sources); TP_CHECK(saw_progress); TP_CHECK(!waiting_while_data_flows); TP_CHECK(!reused_accumulates); TP_CHECK(silent_connected_seen); TP_CHECK(!silent_connected_mislabelled); TP_CHECK(st(m, id).completed); TP_CHECK(file_matches(file_of(dest, kVideo), kVideo));
}

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  g_base = fs::path(TP_BUILD_DIR) / "it_download_run"; fs::create_directories(g_base);
  static const Content content = Content::make(g_base); g_content = &content;
  struct Sc { const char* name; void (*fn)(); };
  const Sc all[] = {{"selected_file", sc_selected_file}, {"whole_torrent", sc_whole_torrent}, {"honest_progress", sc_honest_progress}, {"magnet", sc_magnet}, {"duplicate", sc_duplicate},
                    {"resume_new_session", sc_resume_new_session}, {"resume_missing", sc_resume_missing}, {"resume_garbage", sc_resume_garbage}, {"resume_modified", sc_resume_modified},
                    {"cancel", sc_cancel}, {"destination_errors", sc_destination_errors}, {"network_loss_and_restore", sc_network_loss_and_restore}, {"rapid_flaps", sc_rapid_flaps},
                    {"pause_survives_network_change", sc_pause_survives_network_change}, {"no_sources_then_sources", sc_no_sources_then_sources},
                    {"auto_resume_disabled", sc_auto_resume_disabled}, {"no_background_ui_needed", sc_no_background_ui_needed},
                    {"completed_modified_between_sessions", sc_completed_modified_between_sessions}, {"completed_modified_no_source", sc_completed_modified_no_source},
                    {"completed_unchanged_restart", sc_completed_unchanged_restart}, {"completed_touched_not_redownloaded", sc_completed_touched_not_redownloaded},
                    {"cancel_delete_scope", sc_cancel_delete_scope}, {"overlapping_destinations", sc_overlapping_destinations},
                    {"tracker_discovery", sc_tracker_discovery}, {"web_seed_counts_as_source", sc_web_seed_counts_as_source}};
  int ran = 0;
  for (auto& s : all) {
    bool want = argc <= 1; for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == s.name) want = true;
    if (!want) continue;
    const int before = tptest::failures(); LOG("---- %s", s.name); s.fn(); ++ran;
    LOG("%s %s", tptest::failures() == before ? "PASS" : "FAIL", s.name);
  }
  std::printf("it_download: %d scenario(s), %d checks, %d failure(s)\n", ran, tptest::checks(), tptest::failures());
  return tptest::failures() || ran == 0 ? 1 : 0;
}
