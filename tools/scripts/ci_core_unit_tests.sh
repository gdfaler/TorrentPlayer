#!/bin/bash
# Builds and runs the portable C++ core unit tests (tests/unit/*.cpp, CTest label "unit") in a separate build directory.
# Needs cmake, ninja, pkg-config, boost, libtorrent-rasterbar and openssl@3 from Homebrew (the project CMake reads /opt/homebrew).
# No network, no media, no BitTorrent peers. Usage: tools/scripts/ci_core_unit_tests.sh [build-dir=ci_build]
set -euo pipefail
cd "$(dirname "$0")/../.."
B=${1:-ci_build}
cmake -G Ninja -S . -B "$B" -DCMAKE_BUILD_TYPE=RelWithDebInfo
targets=()
for f in tests/unit/*.cpp; do targets+=("$(basename "$f" .cpp)"); done
[ "${#targets[@]}" -gt 0 ] || { echo "no unit tests found" >&2; exit 1; }
cmake --build "$B" --target "${targets[@]}"
ctest --test-dir "$B" -L unit --output-on-failure --timeout 120 | tee "$B/ctest_unit.log"
ran=$(grep -cE "Test +#[0-9]+: .* Passed" "$B/ctest_unit.log" || true)
[ "$ran" -ge "${#targets[@]}" ] || { echo "expected at least ${#targets[@]} passing unit tests, got $ran" >&2; exit 1; }
echo "core unit tests: $ran passed (ctest label unit)"
