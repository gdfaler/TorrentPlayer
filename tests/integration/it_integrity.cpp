// Gate M1-G4: (a) the hash-failure classifier is validated by fault injection with 0 false positives; (b) honest-seeder soak: >= N verified pieces with
// 0 STORAGE-CORRUPTION verdicts (loopback seeders cannot send bad data, so any failure would be ours).
// usage: it_integrity [pieces=5000] [volume=...]
#include <map>
#include "loopback_fixture.hpp"
using namespace tpit;

static std::map<std::string, std::string> args(int argc, char** argv) { std::map<std::string, std::string> m; for (int i = 1; i < argc; ++i) { std::string a = argv[i]; auto e = a.find('='); if (e != std::string::npos) m[a.substr(0, e)] = a.substr(e + 1); } return m; }

static bool fault_case(const Payload& pl, const char* kind, int every) {
  std::vector<std::unique_ptr<Seeder>> seeders;
  for (int i = 0; i < 3; ++i) seeders.push_back(std::make_unique<Seeder>(pl, 42101 + i));
  auto dir = fresh_dir(std::string("integrity_fault_") + kind);
  tp::Engine eng(engine_config(dir, 42001));
  tp::TorrentSource src; src.torrent_bytes = pl.torrent_bytes; src.strip_web_seeds = true; src.strip_trackers = true;
  for (auto& s : seeders) src.extra_peers.push_back(s->endpoint());
  tp::CacheConfig cc; cc.limit_bytes = 64LL << 20;
  std::string err; auto ss = eng.open_stream(src, 0, cc, &err);
  if (!ss) { ITLOG("open failed %s", err.c_str()); return false; }
  if (std::strcmp(kind, "arrival") == 0) ss->storage().inject_arrival_every = every; else ss->storage().inject_storage_every = every;
  ss->set_bitrate(8.0 * 1048576);
  Player p(*ss, pl, 8.0 * 1048576);
  p.open_like_demuxer(); p.play(25.0, 60.0);
  auto st = ss->status();
  const int inj = std::strcmp(kind, "arrival") == 0 ? ss->storage().injected_arrival.load() : ss->storage().injected_storage.load();
  ITLOG("fault=%s every %d writes: injected=%d | hash failures=%d classified: ARRIVED=%d STORAGE=%d UNDETERMINED=%d | verified OK=%d | attribution: %s",
        kind, every, inj, st.hash_failures, st.hash_arrived, st.hash_storage, st.hash_undetermined, st.pieces_verified,
        ss->shadow().attribution(0).c_str());
  bool ok = inj > 0 && st.hash_failures > 0;
  if (std::strcmp(kind, "arrival") == 0) ok = ok && st.hash_arrived > 0 && st.hash_storage == 0;
  else ok = ok && st.hash_storage > 0 && st.hash_arrived == 0;
  ITLOG("  -> classifier %s for injected '%s' faults", ok ? "CORRECT" : "WRONG", kind);
  return ok;
}

int main(int argc, char** argv) {
  auto a = args(argc, argv);
  const long long target = a.count("pieces") ? std::atoll(a["pieces"].c_str()) : 5000;
  Payload pl = Payload::create(data_dir(), 512LL << 20, 262144, 33);
  bool ok = true;
  ITLOG("--- (a) classifier validation by fault injection (3 seeders; libtorrent will ban the faulty peers, so each case is short)");
  ok &= fault_case(pl, "arrival", 150);
  ok &= fault_case(pl, "storage", 150);
  ITLOG("--- (b) honest-seeder integrity soak: need %lld verified pieces, strict cap 64 MiB, random seeks", target);
  std::vector<std::unique_ptr<Seeder>> seeders;
  for (int i = 0; i < 3; ++i) seeders.push_back(std::make_unique<Seeder>(pl, 42201 + i));
  auto dir = fresh_dir("integrity_soak");
  tp::Engine eng(engine_config(dir, 42002));
  tp::TorrentSource src; src.torrent_bytes = pl.torrent_bytes; src.strip_web_seeds = true; src.strip_trackers = true;
  for (auto& s : seeders) src.extra_peers.push_back(s->endpoint());
  tp::CacheConfig cc; cc.limit_bytes = 64LL << 20;
  std::string err; auto ss = eng.open_stream(src, 0, cc, &err);
  if (!ss) { ITLOG("open failed %s", err.c_str()); return 2; }
  ss->set_bitrate(8.0 * 1048576);
  Player player(*ss, pl, 8.0 * 1048576);
  player.open_like_demuxer();
  std::mt19937 rng(9); int n = 0; int stuck = 0;
  while (ss->status().pieces_verified < target) {
    if (n % 2 == 0) { if (player.seek(static_cast<std::int64_t>(rng() % static_cast<std::uint64_t>(pl.size - 1048576)), 90.0) < 0) { ++stuck; ITLOG("stuck seek"); } }
    if (!player.play(1.5, 60.0)) { ++stuck; ITLOG("stuck play"); }
    if (++n % 40 == 0) { auto st = ss->status(); ITLOG("  verified pieces %d / %lld, seeks %d, resets %d, hash failures %d (storage %d)", st.pieces_verified, target, player.st.seeks, st.un_have_resets, st.hash_failures, st.hash_storage); }
    if (n > 20000) break;
  }
  auto st = ss->status();
  ITLOG("RESULT honest soak: verified pieces %d (~%.0f MiB), hash failures %d (ARRIVED %d STORAGE %d UNDETERMINED %d), data mismatches %d, stuck %d, rewrites differing %lld, cap violations %lld",
        st.pieces_verified, st.pieces_verified * 0.25, st.hash_failures, st.hash_arrived, st.hash_storage, st.hash_undetermined, player.st.mismatches, stuck, (long long)st.store.rewrites_different, (long long)st.store.cap_violations);
  ok &= st.pieces_verified >= target && st.hash_storage == 0 && st.hash_failures == 0 && player.st.mismatches == 0 && stuck == 0 && st.store.rewrites_different == 0;
  ITLOG(ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
