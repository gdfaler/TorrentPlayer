#!/bin/bash
# Compiles and runs the Swift test suites that do not need the AppKit application, mpv or macOS 27 (tests/app/*_test.swift except wait_text_test.swift,
# which imports the C bridge headers). Each suite must report a positive number of checks and 0 failures. Usage: tools/scripts/ci_swift_tests.sh [out-dir]
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${1:-$(mktemp -d)}; mkdir -p "$OUT/scratch"; OUT=$(cd "$OUT" && pwd)      # absolute: the recents suite creates symbolic links in its scratch folder
TARGET="$(uname -m)-apple-macosx14.0"
FLAGS=(-parse-as-library -swift-version 5 -target "$TARGET")
run() {                                                                      # run <name> <sources...>
  local name=$1; shift
  echo "== $name"
  swiftc "${FLAGS[@]}" "$@" -o "$OUT/$name"
  local scratch="$OUT/scratch/$name"; mkdir -p "$scratch"
  "$OUT/$name" "$scratch" | tee "$OUT/$name.out"
  local line; line=$(grep -E "[0-9]+ checks, [0-9]+ failures" "$OUT/$name.out" | tail -1)
  [[ "$line" =~ ([0-9]+)\ checks,\ 0\ failures ]] && [ "${BASH_REMATCH[1]}" -gt 0 ] || { echo "suite $name did not report a passing result" >&2; exit 1; }
}
run glass_metrics   app/Sources/GlassMetrics.swift tests/app/glass_metrics_test.swift
run home_model      app/Sources/GlassMetrics.swift app/Sources/HomeModel.swift tests/app/home_model_test.swift
run open_target     app/Sources/OpenTarget.swift tests/app/open_target_test.swift
run playback_keys   app/Sources/PlaybackKeys.swift tests/app/playback_keys_test.swift
run playback_telem  app/Sources/PlaybackTelemetry.swift tests/app/playback_telemetry_test.swift
run recent_files    app/Sources/RecentFiles.swift tests/app/recent_files_test.swift
run seek_tracker    app/Sources/SeekTracker.swift tests/app/seek_tracker_test.swift
run download_model   app/Sources/DownloadModel.swift tests/app/download_model_test.swift
echo "swift unit tests: all suites passed"
