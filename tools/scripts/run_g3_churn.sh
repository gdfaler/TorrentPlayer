#!/bin/bash
# DIAGNOSTIC protocol CHURN-F runner (docs/proposals/G3_CHURN_VARIABILITY_PROPOSAL.md rev 3). NOT gate G3.
# usage: run_g3_churn.sh <blocks> <seed> <TAG>   TAG must start with SMOKE for a smoke run; any other tag (registered run) REQUIRES the file docs/proposals/G3_CHURN_REGISTERED_RUN_APPROVAL.md
cd "$(dirname "$0")/../.." || exit 1
if [ -z "$TP_CAFFEINATED" ]; then export TP_CAFFEINATED=1; exec caffeinate -i "$0" "$@"; fi
export TP_BUILD_DIR=$PWD/build
N=${1:?blocks}; SEED=${2:?seed}; TAG=${3:?tag}
case "$TAG" in SMOKE*) ;; *) [ -f docs/proposals/G3_CHURN_REGISTERED_RUN_APPROVAL.md ] || { echo "REFUSING: registered run not approved (approval file missing)"; exit 9; } ;; esac
VOL=$(tools/scripts/test_volume.sh attach) || exit 1
if ! pmset -g batt | head -1 | grep -q 'AC Power'; then echo "REFUSING: not on AC power"; exit 3; fi
FK=$(df -k $VOL | tail -1 | awk '{print $4}'); if [ "$FK" -lt 8388608 ]; then echo "REFUSING: start gate: test volume below 8 GiB free ($FK KiB)"; exit 4; fi
if ! hdiutil info | grep -q "tp_test_volume.sparseimage"; then echo "REFUSING: expected image not mounted"; exit 5; fi
STAMP=$(date +%Y%m%d_%H%M%S); D=docs/gate_results/G3DIAG_${TAG}_$STAMP; RUNID=${TAG}_$STAMP; mkdir -p $D
{ echo "mode=CHURN-F blocks=$N seed=$SEED runid=$RUNID"; shasum -a 256 docs/proposals/G3_CHURN_VARIABILITY_PROPOSAL.md docs/proposals/G3_CHURN_VARIABILITY_PROPOSAL_rev1.md docs/proposals/G3_CHURN_VARIABILITY_PROPOSAL_rev2.md docs/proposals/G3_CHURN_F_CODE_REVIEW.md tests/integration/diag_churn_f.cpp tests/integration/churn_tree_stats.hpp tests/integration/test_churn_tree_stats.cpp tests/integration/loopback_fixture.hpp tools/scripts/analyze_g3_churn.py tools/scripts/cleanup_churn_ring.py tools/scripts/recover_halted_churn_ring.py tools/scripts/test_churn_f_regression.py tools/scripts/run_g3_churn.sh CMakeLists.txt build/diag_churn_f build/test_churn_tree_stats; } > $D/PLAN_LOCK.txt
{ date; pmset -g batt | head -2; uptime; df -k $VOL | tail -1; sysctl kern.sleeptime kern.waketime; } > $D/conditions_before.txt
{ echo "preserved entries (must NOT be touched):"; ls -ld $VOL/overhead_* $VOL/diagc1b_calib_off; du -sk $VOL/overhead_* $VOL/diagc1b_calib_off; } > $D/preserved_entries_before.txt 2>&1
# regression gates (fail closed): recursive content accounting unit test and analyzer/recovery regression tests must pass before any transfer
{ ./build/test_churn_tree_stats && python3 -W ignore -I tools/scripts/test_churn_f_regression.py; } > $D/preflight_tests.txt 2>&1 || { echo "REFUSING: regression tests failed (see $D/preflight_tests.txt)"; exit 6; }
S=$(date +%s)
./build/diag_churn_f blocks=$N seed=$SEED out=$PWD/$D volume=$VOL runid=$RUNID > $D/console.txt 2>&1; RC=$?
{ date; pmset -g batt | head -2; df -k $VOL | tail -1; sysctl kern.sleeptime kern.waketime; } > $D/conditions_after.txt
tools/scripts/sleep_check.sh $S $(date +%s) > $D/sleep_check.txt && echo "no sleep/wake events" || echo "SLEEP/WAKE EVENTS (see sleep_check.txt)"
python3 -I tools/scripts/analyze_g3_churn.py $D > $D/analysis_console.txt 2>&1
echo "harness exit code $RC: $(cat $D/finished.txt 2>/dev/null)"; cat $D/analysis_console.txt
FAILS=$(python3 -I -c "import json,sys; a=json.load(open('$D/analysis.json')); print(len(a['checks']['ring_and_state_failures']))" 2>/dev/null)
if [ "$RC" = "0" ] && [ "$FAILS" = "0" ]; then
  python3 -I tools/scripts/cleanup_churn_ring.py $D $VOL > $D/cleanup_console.txt 2>&1; echo "cleanup exit code $?"; cat $D/cleanup_console.txt
else
  echo "NOT cleaning up: harness exit $RC, ring/state failures '$FAILS'. All evidence (including the ring directories on the volume) is preserved."
fi
{ date; df -k $VOL | tail -1; ls -ld $VOL/overhead_* $VOL/diagc1b_calib_off; du -sk $VOL/overhead_* $VOL/diagc1b_calib_off; ls $VOL; } > $D/preserved_entries_after.txt 2>&1
echo "$D"
