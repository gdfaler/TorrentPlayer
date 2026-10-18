// M2 acceptance support: real libmpv playing real media (H.264 MP4, AV1 MKV, VP9 WebM) THROUGH the production path: loopback swarm -> libtorrent -> tpcore
// (tp_c.h) -> tp_mpv_bridge ("tp://") -> mpv. Every byte mpv receives is compared with the source file (TPB_VERIFY). Headless output (vo=null, ao=null);
// the windowed path is exercised by the app autotest. Checks: playback starts, duration known, time advances, scripted seeks complete and land near the target,
// cancel/seek behaviour does not poison the demuxer, clean teardown (no hang) over repeated open/close cycles.
// usage: it_mpv_e2e [cycles=20]
#include <mpv/client.h>
#include "media_fixture.hpp"
#include "tpcore/tp_c.h"
#include "tp_mpv_bridge.h"
using namespace tpit;
using Clock = std::chrono::steady_clock;

static double el(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }
struct Mpv {
  mpv_handle* h = nullptr;
  Mpv(tp_stream* s) {
    h = mpv_create();
    auto o = [&](const char* k, const char* v) { mpv_set_option_string(h, k, v); };
    o("vo", "null"); o("ao", "null"); o("hwdec", "no"); o("config", "no"); o("terminal", "no"); o("idle", "yes"); o("ytdl", "no"); o("load-scripts", "no");
    o("untimed", "yes"); o("cache", "yes"); o("demuxer-readahead-secs", "10"); o("keep-open", "yes");
    mpv_initialize(h);
    tpb_register(h, s);
  }
  double num(const char* n) { double v = -1; return mpv_get_property(h, n, MPV_FORMAT_DOUBLE, &v) == 0 ? v : -1; }
  bool flag(const char* n) { int v = 0; return mpv_get_property(h, n, MPV_FORMAT_FLAG, &v) == 0 && v; }
  void cmd(std::initializer_list<const char*> a) { std::vector<const char*> v(a); v.push_back(nullptr); mpv_command(h, v.data()); }
  // waits for the next PLAYBACK_RESTART after `issued`, returning seconds, or -1 on timeout / end-file error
  double wait_restart(double timeout, std::string* why = nullptr) {
    const auto t0 = Clock::now();
    while (el(t0) < timeout) {
      mpv_event* e = mpv_wait_event(h, 0.05);
      if (e->event_id == MPV_EVENT_PLAYBACK_RESTART) return el(t0);
      if (e->event_id == MPV_EVENT_END_FILE) { auto* ef = static_cast<mpv_event_end_file*>(e->data); if (ef->reason == MPV_END_FILE_REASON_ERROR) { if (why) *why = mpv_error_string(ef->error); return -1; } }
    }
    if (why) *why = "timeout"; return -1;
  }
};

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { ++failures; ITLOG("FAIL: " __VA_ARGS__); } else ITLOG("ok:   " __VA_ARGS__); } while (0)

// returns teardown seconds, or -1 if mpv_terminate_destroy hangs (> 10 s)
static double teardown(Mpv& m) {
  auto t0 = Clock::now(); std::atomic<bool> done{false};
  std::thread th([&] { mpv_terminate_destroy(m.h); done = true; });
  while (!done && el(t0) < 10) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  if (!done) { th.detach(); return -1; }
  th.join(); return el(t0);
}

int main(int argc, char** argv) {
  const int cycles = argc > 1 ? std::atoi(argv[1]) : 20;
  const fs::path mdir = fs::path(TP_RESEARCH_MEDIA_DIR);
  struct Item { const char* file; double seek_to[6]; };
  const Item items[] = {{"sdr_1080p_23976_h264.mp4", {5, 20, 2, 40, 8, 30}}, {"av1_1080p_24_10bit.mkv", {2, 6, 1, 8, 3, 5}}, {"vp9_1080p_24.webm", {1, 5, 2, 7, 3, 4}}};
  const fs::path base = data_dir();
  std::vector<MediaPayload> mp; for (auto& it : items) { if (!fs::exists(mdir / it.file)) { ITLOG("missing research media %s", (mdir / it.file).c_str()); return 2; } mp.push_back(media_payload(base, mdir / it.file)); }
  std::vector<std::unique_ptr<Seeder>> seeders;
  for (size_t i = 0; i < mp.size(); ++i) { seeders.push_back(std::make_unique<Seeder>(mp[i].p, static_cast<int>(42601 + i * 2))); seeders.push_back(std::make_unique<Seeder>(mp[i].p, static_cast<int>(42602 + i * 2))); }

  const fs::path dir = fresh_dir("mpv_e2e");
  char err[256]; tp_engine_config ec{}; std::string cache = (dir / "cache").string(); ec.cache_root = cache.c_str(); ec.listen_interfaces = "127.0.0.1:42501"; ec.loopback_test_mode = 1;
  tp_engine* eng = tp_engine_create(&ec, err, sizeof err); if (!eng) { ITLOG("engine: %s", err); return 2; }

  auto play_one = [&](size_t idx, bool full, int cycle) {
    const MediaPayload& M = mp[idx];
    const fs::path tf = dir / (M.name + ".torrent"); { std::ofstream f(tf, std::ios::binary); f.write(M.p.torrent_bytes.data(), static_cast<std::streamsize>(M.p.torrent_bytes.size())); }
    tp_meta* meta = tp_meta_from_file(eng, tf.c_str(), err, sizeof err); if (!meta) { ITLOG("meta: %s", err); ++failures; return; }
    tp_meta_add_peer(meta, seeders[idx * 2]->endpoint().c_str()); tp_meta_add_peer(meta, seeders[idx * 2 + 1]->endpoint().c_str());
    tp_stream_options so{}; so.cache_limit_bytes = 48LL << 20; so.web_seed_mode = 0;
    tp_stream* s = tp_stream_open(eng, meta, 0, &so, err, sizeof err); if (!s) { ITLOG("stream: %s", err); ++failures; tp_meta_free(meta); return; }
    tpb_counters_reset(); tpb_set_verify_path(M.p.data_path.c_str(), 0);
    Mpv m(s);
    const auto t0 = Clock::now();
    m.cmd({"loadfile", "tp://stream"});
    std::string why; const double first = m.wait_restart(60, &why);
    if (full) CHECK(first >= 0, "[%s] playback started (first restart after %.2f s) %s", M.name.c_str(), first, why.c_str());
    else if (first < 0) { ++failures; ITLOG("FAIL: [cycle %d %s] playback did not start: %s", cycle, M.name.c_str(), why.c_str()); }
    if (full && first >= 0) {
      const double dur = m.num("duration"); CHECK(dur > 1, "[%s] duration known: %.2f s", M.name.c_str(), dur);
      std::this_thread::sleep_for(std::chrono::milliseconds(600)); const double p1 = m.num("time-pos");
      std::this_thread::sleep_for(std::chrono::milliseconds(1500)); const double p2 = m.num("time-pos");
      CHECK(p2 > p1 || p2 >= dur - 0.5, "[%s] time advances: %.2f -> %.2f", M.name.c_str(), p1, p2);
      tp_stream_set_bitrate(s, M.p.size / std::max(1.0, dur));
      for (double target : items[idx].seek_to) {
        if (target >= dur - 1) continue;
        tp_stream_seek_hint(s, static_cast<int64_t>(target / dur * M.p.size));
        char buf[32]; std::snprintf(buf, sizeof buf, "%.3f", target);
        m.cmd({"seek", buf, "absolute+exact"});
        std::string w2; const double l = m.wait_restart(60, &w2);
        const double pos = m.num("time-pos");
        CHECK(l >= 0 && std::fabs(pos - target) < 3.0, "[%s] seek to %.1f s done in %.2f s, landed at %.2f s %s", M.name.c_str(), target, l, pos, w2.c_str());
      }
      tp_stream_status ss; tp_stream_status_get(s, &ss);
      CHECK(ss.cap_violations == 0 && ss.hash_failures == 0, "[%s] cache violations %lld, hash failures %d", M.name.c_str(), (long long)ss.cap_violations, ss.hash_failures);
    } else if (first >= 0) { m.cmd({"seek", "3", "absolute+keyframes"}); m.wait_restart(30); }
    const double td = teardown(m);
    tpb_counters c; tpb_counters_get(&c);
    if (full) CHECK(td >= 0, "[%s] teardown completed in %.2f s (no hang)", M.name.c_str(), td);
    else if (td < 0) { ++failures; ITLOG("FAIL: [cycle %d] teardown hang", cycle); }
    if (c.verify_mismatches != 0) { ++failures; ITLOG("FAIL: [%s cycle %d] %lld DATA MISMATCHES in %lld reads", M.name.c_str(), cycle, (long long)c.verify_mismatches, (long long)c.reads); }
    else if (full) ITLOG("ok:   [%s] every byte mpv received matched the source file (%lld reads, %.1f MiB, %lld cancels, %lld cancelled reads, %lld opens/%lld closes)", M.name.c_str(), (long long)c.reads, c.bytes / 1048576.0, (long long)c.cancels, (long long)c.cancelled_reads, (long long)c.opens, (long long)c.closes);
    (void)t0;
    tp_stream_close(s); tp_meta_free(meta);
  };

  for (size_t i = 0; i < mp.size(); ++i) play_one(i, true, 0);
  ITLOG("--- %d open/close cycles (shutdown + resource-leak check) ---", cycles);
  for (int c = 0; c < cycles; ++c) play_one(static_cast<size_t>(c % mp.size()), false, c);
  ITLOG("cycles done: failures so far %d", failures);
  tp_engine_destroy(eng);
  ITLOG("SUMMARY: %d failure(s)", failures); ITLOG(failures ? "FAIL" : "PASS");
  return failures ? 1 : 0;
}
