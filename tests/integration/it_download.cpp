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
void fill(int file, std::int64_t off, std::int64_t len, char* out) {
  for (std::int64_t i = 0; i < len; ++i) { const std::int64_t p = off + i; out[i] = static_cast<char>(mix(static_cast<std::uint64_t>(file) * 0x1000193ULL + static_cast<std::uint64_t>(p / 8)) >> ((p % 8) * 8)); }
}
bool file_matches(const fs::path& p, int file) {
  std::ifstream f(p, std::ios::binary); if (!f) return false;
  std::vector<char> got(1 << 20), want(1 << 20);
  for (std::int64_t off = 0; off < kFiles[file].size; off += static_cast<std::int64_t>(got.size())) {
    const auto n = std::min<std::int64_t>(static_cast<std::int64_t>(got.size()), kFiles[file].size - off);
    f.read(got.data(), n); if (f.gcount() != n) return false;
    fill(file, off, n, want.data()); if (std::memcmp(got.data(), want.data(), static_cast<std::size_t>(n)) != 0) return false;
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
  static Content make(const fs::path& base) {
    Content c; c.root = base / "seed_content"; fs::create_directories(c.root / "t");
    lt::file_storage fsr;
    for (int i = 0; i < 3; ++i) {
      std::ofstream o(c.root / kFiles[i].path, std::ios::binary | std::ios::trunc); std::vector<char> b(1 << 20);
      for (std::int64_t off = 0; off < kFiles[i].size; off += static_cast<std::int64_t>(b.size())) { const auto n = std::min<std::int64_t>(b.size(), kFiles[i].size - off); fill(i, off, n, b.data()); o.write(b.data(), n); }
      fsr.add_file(kFiles[i].path, kFiles[i].size);
    }
    lt::create_torrent ct(fsr, kPiece, lt::create_flags_t{} | lt::create_torrent::v1_only);
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
    TP_CHECK(m.cancel(id, true));                                         // explicit deletion of a completed download
    TP_CHECK(!fs::exists(file_of(dest, kVideo)));
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

int main(int argc, char** argv) {
  std::signal(SIGPIPE, SIG_IGN);
  g_base = fs::path(TP_BUILD_DIR) / "it_download_run"; fs::create_directories(g_base);
  static const Content content = Content::make(g_base); g_content = &content;
  struct Sc { const char* name; void (*fn)(); };
  const Sc all[] = {{"selected_file", sc_selected_file}, {"whole_torrent", sc_whole_torrent}, {"honest_progress", sc_honest_progress}, {"magnet", sc_magnet}, {"duplicate", sc_duplicate},
                    {"resume_new_session", sc_resume_new_session}, {"resume_missing", sc_resume_missing}, {"resume_garbage", sc_resume_garbage}, {"resume_modified", sc_resume_modified},
                    {"cancel", sc_cancel}, {"destination_errors", sc_destination_errors}, {"network_loss_and_restore", sc_network_loss_and_restore}, {"rapid_flaps", sc_rapid_flaps},
                    {"pause_survives_network_change", sc_pause_survives_network_change}, {"no_sources_then_sources", sc_no_sources_then_sources},
                    {"auto_resume_disabled", sc_auto_resume_disabled}, {"no_background_ui_needed", sc_no_background_ui_needed}};
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
