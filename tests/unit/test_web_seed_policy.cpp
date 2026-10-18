#include "tp_test.hpp"
#include "tpcore/web_seed_policy.hpp"
using namespace tp;

static WebSeedSample S(double t, bool ready, bool demand, int peers, double rate) { WebSeedSample s; s.now = t; s.window_ready = ready; s.demand_pending = demand; s.real_peers = peers; s.rate = rate; return s; }

TP_TEST(off_never_enables) {
  WebSeedPolicyConfig c; c.mode = WebSeedMode::Off; WebSeedPolicy p(c);
  for (int t = 0; t < 100; ++t) TP_CHECK(p.update(S(t, false, true, 0, 0)) == WebSeedAction::None);
}
TP_TEST(always_enables_once) {
  WebSeedPolicyConfig c; c.mode = WebSeedMode::Always; WebSeedPolicy p(c);
  TP_CHECK(p.update(S(0, false, false, 50, 1e6)) == WebSeedAction::Enable);
  for (int t = 1; t < 50; ++t) TP_CHECK(p.update(S(t, true, false, 50, 1e6)) == WebSeedAction::None);
}
TP_TEST(assist_startup_boost_then_off_when_healthy) {
  WebSeedPolicyConfig c; WebSeedPolicy p(c);
  TP_CHECK(p.update(S(0, false, true, 0, 0)) == WebSeedAction::Enable);          // startup boost
  TP_CHECK(p.update(S(5, false, true, 20, 2e6)) == WebSeedAction::None);        // still boosting (window not ready, < 20 s)
  TP_CHECK(p.update(S(6, true, false, 20, 2e6)) == WebSeedAction::None);        // window ready; healthy clock starts
  WebSeedAction a = WebSeedAction::None; double t = 7;
  for (; t < 30 && a == WebSeedAction::None; t += 1) a = p.update(S(t, true, false, 20, 2e6));
  TP_CHECK(a == WebSeedAction::Disable);
  TP_CHECK(t >= 16 && t <= 18);                                                  // 10 s of health
}
TP_TEST(assist_reenables_when_starved_and_demand_pending) {
  WebSeedPolicyConfig c; WebSeedPolicy p(c);
  p.update(S(0, true, false, 20, 2e6));                                           // boost skipped if window already ready? boosting requires !ready
  WebSeedAction a = WebSeedAction::None; double t = 1;
  for (; t < 12 && a != WebSeedAction::Enable; t += 1) a = p.update(S(t, true, true, 1, 1000));
  TP_CHECK(a == WebSeedAction::Enable);
  TP_CHECK(t <= 5);                                                               // starve_seconds = 2 s
}
TP_TEST(assist_does_not_enable_when_idle_even_with_few_peers) {
  WebSeedPolicyConfig c; WebSeedPolicy p(c);
  p.update(S(0, true, false, 0, 0));
  for (int t = 1; t < 60; ++t) TP_CHECK(p.update(S(t, true, false, 0, 0)) == WebSeedAction::None);   // nothing demanded: no need
}

TP_MAIN()
