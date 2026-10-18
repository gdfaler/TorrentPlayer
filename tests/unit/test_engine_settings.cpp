// The engine applies the web-seed request-size limit to its libtorrent session (no torrent is added, no peer or tracker is contacted;
// the session only listens on a loopback ephemeral port with DHT/LSD disabled).
#include <filesystem>
#include "tp_test.hpp"
#include "tpcore/engine.hpp"
#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
using namespace tp;

static EngineConfig quiet_config(const char* dir) {
  EngineConfig c;
  c.cache_root = std::filesystem::temp_directory_path() / dir;
  c.listen_interfaces = "127.0.0.1:0";
  c.enable_dht = false; c.enable_lsd = false; c.enable_upnp = false; c.enable_natpmp = false;
  return c;
}

TP_TEST(default_config_limits_web_seed_requests_below_the_libtorrent_default) {
  const int ltdefault = lt::default_settings().get_int(lt::settings_pack::urlseed_max_request_bytes);
  TP_CHECK(ltdefault == 16 * 1024 * 1024);                                   // documents the value being replaced
  EngineConfig cfg = quiet_config("tp_engine_settings_default");
  TP_CHECK(cfg.web_seed_max_request_bytes == 4 * 1024 * 1024);
  Engine e(cfg);
  const int applied = e.session().get_settings().get_int(lt::settings_pack::urlseed_max_request_bytes);
  TP_CHECK(applied == 4 * 1024 * 1024);
  TP_CHECK(applied < ltdefault);
  std::error_code ec; std::filesystem::remove_all(cfg.cache_root, ec);
}

TP_TEST(zero_keeps_the_libtorrent_default) {
  EngineConfig cfg = quiet_config("tp_engine_settings_zero");
  cfg.web_seed_max_request_bytes = 0;
  Engine e(cfg);
  TP_CHECK(e.session().get_settings().get_int(lt::settings_pack::urlseed_max_request_bytes) == 16 * 1024 * 1024);
  std::error_code ec; std::filesystem::remove_all(cfg.cache_root, ec);
}

TP_TEST(explicit_value_is_applied) {
  EngineConfig cfg = quiet_config("tp_engine_settings_value");
  cfg.web_seed_max_request_bytes = 8 * 1024 * 1024;
  Engine e(cfg);
  TP_CHECK(e.session().get_settings().get_int(lt::settings_pack::urlseed_max_request_bytes) == 8 * 1024 * 1024);
  std::error_code ec; std::filesystem::remove_all(cfg.cache_root, ec);
}
TP_MAIN()
