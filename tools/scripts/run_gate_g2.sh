#!/bin/bash
# Mandatory gate M1-G2 matrix (strict cache bound). Results: docs/gate_results/M1-G2_<timestamp>.txt
# Defined conditions: dedicated APFS test volume, independent auditors (lock-free dir scan + 1 ms volume free-space delta), faults none and
# calibrated over-allocation (overalloc3 = ~3x the average observed on the real swarm), 2-3 seeders (duplicate/partial deliveries), seeks, recovery with a smaller limit.
cd "$(dirname "$0")/../.." || exit 1
export TP_BUILD_DIR=$PWD/build
VOL=$(tools/scripts/test_volume.sh attach)
OUT=docs/gate_results; mkdir -p $OUT; F=$OUT/M1-G2_$(date +%Y%m%d_%H%M%S).txt
echo "M1-G2 strict-cap matrix, started $(date)" | tee $F
run() { echo "=== $*" | tee -a $F; ./build/it_strict_cap "$@" volume=$VOL 2>&1 | grep -E 'FILL|SATURATION|NOT SATURATED|config:|margin usage|VOLUME|SUMMARY|PASS|FAIL|STUCK|MISMATCH|BEFORE any' | tee -a $F; }
run limit_mib=64   payload_mib=256  secs=60  seeders=2 fault=none      bps_mib=4
run limit_mib=64   payload_mib=256  secs=60  seeders=3 fault=overalloc3 bps_mib=4
run limit_mib=512  payload_mib=2048 secs=120 seeders=2 fault=none      bps_mib=16
run limit_mib=512  payload_mib=2048 secs=120 seeders=3 fault=overalloc3 bps_mib=16
run limit_mib=2048 payload_mib=6144 secs=240 seeders=2 fault=none      bps_mib=32
run limit_mib=2048 payload_mib=6144 secs=240 seeders=3 fault=overalloc3 bps_mib=32
run limit_mib=2048 payload_mib=6144 secs=240 seeders=2 fault=none      bps_mib=32 fill=1
run limit_mib=2048 payload_mib=6144 secs=240 seeders=3 fault=overalloc3 bps_mib=32 fill=1
run limit_mib=512  payload_mib=2048 secs=120 seeders=3 fault=overalloc3 bps_mib=16 fill=1
echo "=== INFORMATIONAL (beyond defined conditions: ~6x observed over-allocation)" | tee -a $F
run limit_mib=64 payload_mib=256 secs=60 seeders=2 fault=overalloc10 bps_mib=4 informational=1
echo "M1-G2 matrix finished $(date)" | tee -a $F
