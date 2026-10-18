#!/bin/bash
# DIAGNOSTIC protocol C1b runner (docs/proposals/G3_DIAGNOSTIC_PROPOSAL.md G3_C1B_PLAN.md rev 1.1). NOT gate G3. usage: run_g3_c1b.sh [pairs=96] [seed=20261021] [tag=C1B]   (tag SMOKE_C1B for the smoke test)
cd "$(dirname "$0")/../.." || exit 1
if [ -z "$TP_CAFFEINATED" ]; then export TP_CAFFEINATED=1; exec caffeinate -i "$0" "$@"; fi
export TP_BUILD_DIR=$PWD/build
N=${1:-96}; SEED=${2:-20261021}; TAG=${3:-C1B}
VOL=$(tools/scripts/test_volume.sh attach) || exit 1
if ! pmset -g batt | head -1 | grep -q 'AC Power'; then echo "REFUSING: not on AC power"; exit 3; fi
FK=$(df -k $VOL | tail -1 | awk '{print $4}'); if [ "$FK" -lt 8388608 ]; then echo "REFUSING: test volume below 8 GiB free"; exit 4; fi
if ! hdiutil info | grep -q "tp_test_volume.sparseimage"; then echo "REFUSING: expected image not mounted"; exit 5; fi
D=docs/gate_results/G3DIAG_${TAG}_$(date +%Y%m%d_%H%M%S); mkdir -p $D
{ echo "mode=C1B pairs=$N seed=$SEED"; shasum -a 256 docs/proposals/G3_C1B_PLAN.md docs/proposals/G3_C1B_PLAN_REV2_PROPOSAL.md docs/proposals/G3_C1B_REV2_CODE_REVIEW.md docs/proposals/G3_DIAGNOSTIC_PROPOSAL.md tests/integration/diag_c1b_store.cpp tools/scripts/analyze_g3_diag.py tools/scripts/run_g3_c1b.sh build/diag_c1b_store; } > $D/PLAN_LOCK.txt
{ date; pmset -g batt | head -2; uptime; df -k $VOL | tail -1; sysctl kern.sleeptime kern.waketime; } > $D/conditions_before.txt
{ echo "preserved rev 1.1 failure evidence (must NOT be deleted, overwritten or reused):"; ls -ld /Volumes/TPTestVol/diagc1b_calib_off; du -sk /Volumes/TPTestVol/diagc1b_calib_off; ls /Volumes/TPTestVol/diagc1b_calib_off | wc -l; echo "free space now:"; df -k $VOL | tail -1; } > $D/preserved_failure_evidence.txt 2>&1
S=$(date +%s)
./build/diag_c1b_store pairs=$N seed=$SEED out=$D volume=$VOL > $D/console.txt 2>&1
{ date; pmset -g batt | head -2; df -k $VOL | tail -1; sysctl kern.sleeptime kern.waketime; } > $D/conditions_after.txt
tools/scripts/sleep_check.sh $S $(date +%s) > $D/sleep_check.txt && echo "no sleep/wake events" || echo "SLEEP/WAKE EVENTS (see sleep_check.txt)"
python3 -I tools/scripts/analyze_g3_diag.py $D > /dev/null 2>&1
echo "$D: $(cat $D/finished.txt)"
