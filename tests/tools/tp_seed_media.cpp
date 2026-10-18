// Test helper: seeds a real media file on loopback ports and writes its .torrent. usage: tp_seed_media <media-file> <torrent-out> <port> [port...]
// The payload is copied into build/it_run (the source file is never modified). Prints READY and runs until killed.
#include <libtorrent/torrent_info.hpp>
#include "loopback_fixture.hpp"
#include "media_fixture.hpp"
using namespace tpit;
int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: tp_seed_media <media-file> <torrent-out> <port> [port...]\n"); return 2; }
  MediaPayload m = media_payload(data_dir(), argv[1]);
  { std::ofstream f(argv[2], std::ios::binary); f.write(m.p.torrent_bytes.data(), static_cast<std::streamsize>(m.p.torrent_bytes.size())); }
  std::vector<std::unique_ptr<Seeder>> seeders;
  for (int i = 3; i < argc; ++i) seeders.push_back(std::make_unique<Seeder>(m.p, std::atoi(argv[i]), std::getenv("TP_SEED_RATE") ? std::atoi(std::getenv("TP_SEED_RATE")) : 0));
  { lt::torrent_info ti(m.p.torrent_bytes.data(), static_cast<int>(m.p.torrent_bytes.size())); std::string hx; for (auto b : ti.info_hashes().get_best()) { char t[3]; std::snprintf(t, 3, "%02x", static_cast<unsigned char>(b)); hx += t; } std::printf("INFOHASH %s\n", hx.c_str()); }
  std::printf("READY\n"); std::fflush(stdout);
  for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
}
