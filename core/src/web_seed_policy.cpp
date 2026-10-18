#include "tpcore/web_seed_policy.hpp"
namespace tp {

WebSeedAction WebSeedPolicy::update(const WebSeedSample& s) {
  if (c_.mode == WebSeedMode::Off) { if (on_) { on_ = false; return WebSeedAction::Disable; } return WebSeedAction::None; }
  if (!started_) { started_ = true; start_ = s.now; }
  if (c_.mode == WebSeedMode::Always) { if (!on_) { on_ = true; return WebSeedAction::Enable; } return WebSeedAction::None; }
  // ---- Assist
  const bool boosting = !s.window_ready && (s.now - start_) < c_.boost_seconds;
  const bool starved = s.demand_pending && s.real_peers < c_.starve_peers_below && s.rate < c_.starve_rate_below;
  const bool healthy = s.real_peers >= c_.healthy_peers_at_least && s.rate > c_.healthy_rate_above;
  if (starved) { if (starve_since_ < 0) starve_since_ = s.now; } else starve_since_ = -1;
  if (healthy) { if (healthy_since_ < 0) healthy_since_ = s.now; } else healthy_since_ = -1;
  if (!on_) {
    if (boosting || (starve_since_ >= 0 && s.now - starve_since_ >= c_.starve_seconds)) { on_ = true; healthy_since_ = -1; return WebSeedAction::Enable; }
  } else if (!boosting && healthy_since_ >= 0 && s.now - healthy_since_ >= c_.healthy_seconds) {
    on_ = false; return WebSeedAction::Disable;
  }
  return WebSeedAction::None;
}

}  // namespace tp
