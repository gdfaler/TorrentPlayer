// tpcore/web_seed_policy.hpp: pure decision logic for HTTP web seeds (spec 4.6). Off / Assist (default) / Always.
// Assist = startup boost (web seed on from the start until the first window is buffered or boost_seconds), then on again when real peers
// are scarce or throughput is starved for starve_seconds, and off after healthy_seconds of a healthy swarm.
#pragma once
namespace tp {

enum class WebSeedMode { Off, Assist, Always };
enum class WebSeedAction { None, Enable, Disable };

struct WebSeedPolicyConfig {
  WebSeedMode mode = WebSeedMode::Assist;
  double boost_seconds = 20.0;
  double starve_seconds = 2.0;
  double healthy_seconds = 10.0;
  int starve_peers_below = 3;
  int healthy_peers_at_least = 10;
  double starve_rate_below = 50'000;          // bytes/s
  double healthy_rate_above = 500'000;        // bytes/s
};

struct WebSeedSample {
  double now = 0;                // seconds, monotonic
  bool window_ready = false;     // the initial playback window is buffered
  bool demand_pending = false;   // the player is waiting for data
  int real_peers = 0;            // BitTorrent peers (web seeds excluded)
  double rate = 0;               // total payload download rate, bytes/s
};

class WebSeedPolicy {
 public:
  explicit WebSeedPolicy(WebSeedPolicyConfig c) : c_(c) {}
  // Call roughly once per second. Returns the action to apply to the torrent's web seeds.
  WebSeedAction update(const WebSeedSample& s);
  bool enabled() const { return on_; }
  void reset() { *this = WebSeedPolicy(c_); }
 private:
  WebSeedPolicyConfig c_;
  bool on_ = false, started_ = false;
  double start_ = 0, starve_since_ = -1, healthy_since_ = -1;
};

}  // namespace tp
