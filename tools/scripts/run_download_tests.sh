#!/bin/bash
# Builds and runs the Download & Play core tests (tests/integration/it_download.cpp): loopback seeders, synthetic content, no external network.
# Usage: tools/scripts/run_download_tests.sh [build-dir=dist/core_build] [scenario ...]    (no scenario: all 17, about 3 minutes)
# Scenarios: selected_file whole_torrent honest_progress magnet duplicate resume_new_session resume_missing resume_garbage resume_modified cancel destination_errors
#            network_loss_and_restore rapid_flaps pause_survives_network_change no_sources_then_sources auto_resume_disabled no_background_ui_needed
set -euo pipefail
cd "$(dirname "$0")/../.."
B=${1:-dist/core_build}; [ $# -gt 0 ] && shift
cmake -G Ninja -S . -B "$B" > /dev/null
cmake --build "$B" --target it_download
"$B/it_download" "$@"
