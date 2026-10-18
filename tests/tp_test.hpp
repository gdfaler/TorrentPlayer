// Minimal dependency-free test harness (no Xcode/XCTest needed).
#pragma once
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace tptest {
struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& cases() { static std::vector<Case> c; return c; }
inline int& failures() { static int f = 0; return f; }
inline int& checks() { static int c = 0; return c; }
struct Reg { Reg(const char* n, std::function<void()> f) { cases().push_back({n, std::move(f)}); } };
inline int run_all() {
  for (auto& c : cases()) {
    int before = failures();
    std::printf("[ RUN  ] %s\n", c.name);
    c.fn();
    std::printf("[ %s ] %s\n", failures() == before ? " OK " : "FAIL", c.name);
  }
  std::printf("%d checks, %d failures\n", checks(), failures());
  return failures() ? 1 : 0;
}
}  // namespace tptest

#define TP_TEST(name) static void name(); static tptest::Reg reg_##name(#name, name); static void name()
#define TP_CHECK(cond) do { ++tptest::checks(); if (!(cond)) { ++tptest::failures(); std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define TP_CHECK_EQ(a, b) do { ++tptest::checks(); auto _a = (a); auto _b = (b); if (!(_a == _b)) { ++tptest::failures(); std::printf("  CHECK_EQ FAILED %s:%d: %s == %s (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, (long long)_a, (long long)_b); } } while (0)
#define TP_MAIN() int main() { return tptest::run_all(); }
