// Builds a loopback torrent from a REAL media file (read-only use of the research media; nothing is modified).
#pragma once
#include "loopback_fixture.hpp"
namespace tpit {
struct MediaPayload { Payload p; fs::path src; std::string name; };
inline MediaPayload media_payload(const fs::path& base, const fs::path& src, int piece_size = 262144) {
  MediaPayload m; m.src = src; m.name = src.filename().string();
  m.p.piece_size = piece_size; m.p.size = static_cast<std::int64_t>(fs::file_size(src));
  m.p.root = base / ("media_" + m.name); fs::create_directories(m.p.root / "payload");
  m.p.data_path = m.p.root / "payload" / m.name;
  if (!fs::exists(m.p.data_path) || fs::file_size(m.p.data_path) != fs::file_size(src)) fs::copy_file(src, m.p.data_path, fs::copy_options::overwrite_existing);
  lt::file_storage fsr; fsr.add_file("payload/" + m.name, m.p.size);
  lt::create_torrent ct(fsr, piece_size, lt::create_flags_t{} | lt::create_torrent::v1_only);
  lt::set_piece_hashes(ct, m.p.root.string());
  m.p.torrent_bytes.clear(); lt::bencode(std::back_inserter(m.p.torrent_bytes), ct.generate());
  return m;
}
}  // namespace tpit
