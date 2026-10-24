#!/bin/bash
# Builds and runs the offline web-seed cross-file regression (tests/integration/it_webseed_crossfile.cpp) in an isolated dist/ folder.
# Needs dist/core_build/libtpcore.a (tools/scripts/build_release.sh builds it). No network: everything is on 127.0.0.1.
# Usage: tools/scripts/run_webseed_crossfile.sh [seconds-per-run=90] [direct|xfile_delayed|xfileB_delayed|xfile_stalled|xfileB_stalled]
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD; OUT=$ROOT/dist/webseed_crossfile; mkdir -p "$OUT"
[ -f "$ROOT/dist/core_build/libtpcore.a" ] || { echo "build the core first: tools/scripts/build_release.sh <name>" >&2; exit 1; }
export PKG_CONFIG_PATH=/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig:${PKG_CONFIG_PATH:-}
LTC=$(pkg-config --cflags libtorrent-rasterbar); LTL=$(pkg-config --libs libtorrent-rasterbar)
# shellcheck disable=SC2086
clang++ -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -arch arm64 -DTP_BUILD_DIR="\"$OUT\"" -I core/include -I tests/integration -I/opt/homebrew/include $LTC \
  tests/integration/it_webseed_crossfile.cpp "$ROOT/dist/core_build/libtpcore.a" $LTL -L/opt/homebrew/lib -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto -lpthread -o "$OUT/it_webseed_crossfile"
"$OUT/it_webseed_crossfile" "$@"
