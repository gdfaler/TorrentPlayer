#!/bin/bash
# DIAGNOSTIC protocol C1 runner (docs/proposals/G3_DIAGNOSTIC_PROPOSAL.md section 12). NOT gate G3. usage: run_g3_c1.sh [pairs=60] [seed=20261020] [tag=C1]   (tag SMOKE_C1 for the smoke test)
cd "$(dirname "$0")/../.." || exit 1
if [ -z "$TP_CAFFEINATED" ]; then export TP_CAFFEINATED=1; exec caffeinate -i "$0" "$@"; fi
export TP_BUILD_DIR=$PWD/build
N=${1:-60}; SEED=${2:-20261020}; TAG=${3:-C1}
VOL=$(tools/scripts/test_volume.sh attach) || exit 1
if ! pmset -g batt | head -1 | grep -q 'AC Power'; then echo "REFUSING: not on AC power"; exit 3; fi
FK=$(df -k $VOL | tail -1 | awk '{print $4}'); if [ "$FK" -lt 8388608 ]; then echo "REFUSING: test volume below 8 GiB free"; exit 4; fi
if ! hdiutil info | grep -q "tp_test_volume.sparseimage"; then echo "REFUSING: expected image not mounted"; exit 5; fi
D=docs/gate_results/G3DIAG_${TAG}_$(date +%Y%m%d_%H%M%S); mkdir -p $D
{ echo "mode=C1 pairs=$N seed=$SEED"; shasum -a 256 docs/proposals/G3_DIAGNOSTIC_PROPOSAL.md tests/integration/diag_c1_store.cpp tools/scripts/analyze_g3_diag.py tools/scripts/run_g3_c1.sh build/diag_c1_store; } > $D/PLAN_LOCK.txt
{ date; pmset -g batt | head -2; uptime; df -k $VOL | tail -1; sysctl kern.sleeptime kern.waketime; } > $D/conditions_before.txt
S=$(date +%s)
./build/diag_c1_store pairs=$N seed=$SEED out=$D volume=$VOL > $D/console.txt 2>&1
{ date; pmset -g batt | head -2; df -k $VOL | tail -1; sysctl kern.sleeptime kern.waketime; } > $D/conditions_after.txt
tools/scripts/sleep_check.sh $S $(date +%s) > $D/sleep_check.txt && echo "no sleep/wake events" || echo "SLEEP/WAKE EVENTS (see sleep_check.txt)"
python3 -I tools/scripts/analyze_g3_diag.py $D > /dev/null 2>&1
echo "$D: $(cat $D/finished.txt)"
