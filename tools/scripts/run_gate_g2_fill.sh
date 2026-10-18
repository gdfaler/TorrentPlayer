#!/bin/bash
# M1-G2 fill-to-cap variants: window fractions raised so the cache SATURATES the internal cap at 2 GiB scale (the default windows leave headroom).
cd "$(dirname "$0")/../.." || exit 1
export TP_BUILD_DIR=$PWD/build
VOL=$(tools/scripts/test_volume.sh attach)
F=docs/gate_results/M1-G2_fill_$(date +%Y%m%d_%H%M%S).txt
echo "M1-G2 fill-to-cap, started $(date)" | tee $F
run() { echo "=== $*" | tee -a $F; ./build/it_strict_cap "$@" volume=$VOL 2>&1 | grep -E 'FILL|SATURATION|NOT SATURATED|config:|margin usage|VOLUME|SUMMARY|PASS|FAIL|STUCK|MISMATCH|BEFORE any' | tee -a $F; }
run limit_mib=2048 payload_mib=6144 secs=240 seeders=2 fault=none       bps_mib=32 fill=1
run limit_mib=2048 payload_mib=6144 secs=240 seeders=3 fault=overalloc3 bps_mib=32 fill=1
echo "M1-G2 fill-to-cap finished $(date)" | tee -a $F
