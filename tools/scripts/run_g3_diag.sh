#!/bin/bash
# DIAGNOSTIC G3 variance study runner (docs/proposals/G3_DIAGNOSTIC_PROPOSAL.md rev 2). NOT gate G3. Runs protocols A, C2, OVH in that order, each in its own
# run directory with its own ID, after recording hashes of the plan, harness, analysis script and seeds (PLAN_LOCK.txt). Requires AC power; lid open.
# usage: run_g3_diag.sh [A_pairs=120] [C2_pairs=120] [OVH_pairs=30]
cd "$(dirname "$0")/../.." || exit 1
if [ -z "$TP_CAFFEINATED" ]; then export TP_CAFFEINATED=1; exec caffeinate -i "$0" "$@"; fi
export TP_BUILD_DIR=$PWD/build
NA=${1:-120}; NC=${2:-120}; NO=${3:-30}
VOL=$(tools/scripts/test_volume.sh attach) || exit 1
STAMP=$(date +%Y%m%d_%H%M%S)
if ! pmset -g batt | head -1 | grep -q 'AC Power'; then echo "REFUSING: not on AC power"; exit 3; fi
FREE_KB=$(df -k $VOL | tail -1 | awk '{print $4}'); if [ "$FREE_KB" -lt 8388608 ]; then echo "REFUSING: test volume has less than 8 GiB free"; exit 4; fi
if ! hdiutil info | grep -q "tp_test_volume.sparseimage"; then echo "REFUSING: $VOL is not mounted from the expected sparse image"; exit 5; fi
MAIN=docs/gate_results/G3DIAG_MASTER_$STAMP; mkdir -p $MAIN; LOG=$MAIN/master_log.txt
echo "G3 diagnostic study (NOT gate G3) started $(date)" | tee $LOG
run_one() { # $1 mode  $2 pairs  $3 seed
  local M=$1 N=$2 SEED=$3 D=docs/gate_results/G3DIAG_${1}_$STAMP
  # precondition re-checked BEFORE EVERY protocol (approved constraint): AC power, mount, and >= 8 GiB free; otherwise stop without starting this protocol
  local FK=$(df -k $VOL | tail -1 | awk '{print $4}')
  echo "precondition before $M: free $(python3 -c "print(round($FK/1048576,3))") GiB; $(pmset -g batt | head -1)" | tee -a $LOG
  if [ "$FK" -lt 8388608 ]; then echo "STOP: test volume below 8 GiB free before protocol $M; not started, nothing deleted" | tee -a $LOG; exit 6; fi
  if ! pmset -g batt | head -1 | grep -q 'AC Power'; then echo "STOP: not on AC power before protocol $M" | tee -a $LOG; exit 3; fi
  if ! hdiutil info | grep -q "tp_test_volume.sparseimage"; then echo "STOP: expected image not mounted before protocol $M" | tee -a $LOG; exit 5; fi
  mkdir -p $D
  { echo "mode=$M pairs=$N seed=$SEED"; shasum -a 256 docs/proposals/G3_DIAGNOSTIC_PROPOSAL.md tests/integration/diag_g3_paired.cpp tools/scripts/analyze_g3_diag.py tools/scripts/run_g3_diag.sh build/diag_g3_paired; } > $D/PLAN_LOCK.txt
  { date; pmset -g batt | head -2; uptime; df -k $VOL | tail -1; sysctl kern.sleeptime kern.waketime; } > $D/conditions_before.txt
  local S=$(date +%s)
  echo "=== $M start $(date) -> $D" | tee -a $LOG
  ./build/diag_g3_paired mode=$M pairs=$N seed=$SEED out=$D volume=$VOL > $D/console.txt 2>&1
  { date; pmset -g batt | head -2; df -k $VOL | tail -1; sysctl kern.sleeptime kern.waketime; } > $D/conditions_after.txt
  tools/scripts/sleep_check.sh $S $(date +%s) > $D/sleep_check.txt && echo "no sleep/wake events during $M" | tee -a $LOG || echo "SLEEP/WAKE EVENTS during $M (see $D/sleep_check.txt)" | tee -a $LOG
  python3 -I tools/scripts/analyze_g3_diag.py $D > /dev/null 2>&1; echo "=== $M finished $(date); $(cat $D/finished.txt 2>/dev/null)" | tee -a $LOG
  sleep 30
}
run_one A $NA 20261017
run_one C2 $NC 20261018
run_one OVH $NO 20261019
echo "all protocols finished $(date)" | tee -a $LOG
