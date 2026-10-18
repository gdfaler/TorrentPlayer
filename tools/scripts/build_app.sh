#!/bin/bash
# Builds build/TorrentPlayer.app with the Command Line Tools only (no Xcode): CMake/Ninja for the C++ core, swiftc for the AppKit app, ad-hoc codesign.
# Dependencies are Homebrew libraries linked by absolute path: the result runs on THIS Mac only (see docs/DISTRIBUTION_NOTES.md).
set -euo pipefail
cd "$(dirname "$0")/../.."
ROOT=$PWD; B=$ROOT/build; APP=$B/TorrentPlayer.app
export PKG_CONFIG_PATH=/opt/homebrew/lib/pkgconfig:/opt/homebrew/opt/openssl@3/lib/pkgconfig:${PKG_CONFIG_PATH:-}
cmake -G Ninja -S . -B build > /dev/null
ninja -C build tpcore
mkdir -p $B/app_obj
clang -c -O2 -Wall -Wextra -Wno-unused-parameter -arch arm64 -mmacosx-version-min=27.0 -I core/include -I app/Sources -I /opt/homebrew/opt/mpv/include app/Sources/tp_mpv_bridge.c -o $B/app_obj/tp_mpv_bridge.o
LT_LIBS=$(pkg-config --libs libtorrent-rasterbar)
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
swiftc -O -swift-version 5 -target arm64-apple-macosx27.0 -module-name TorrentPlayer \
  -import-objc-header app/Sources/bridge.h -Xcc -I$ROOT/core/include -Xcc -I$ROOT/app/Sources -Xcc -I/opt/homebrew/opt/mpv/include \
  app/Sources/*.swift $B/app_obj/tp_mpv_bridge.o $B/libtpcore.a \
  -L/opt/homebrew/opt/mpv/lib -lmpv $LT_LIBS -L/opt/homebrew/opt/openssl@3/lib -lssl -lcrypto -lc++ \
  -framework AppKit -framework QuartzCore -framework UniformTypeIdentifiers \
  -o "$APP/Contents/MacOS/TorrentPlayer"
cp app/Resources/Info.plist "$APP/Contents/Info.plist"
codesign --force --sign - "$APP" 2>&1 | tail -1
echo "built $APP"
