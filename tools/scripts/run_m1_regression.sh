#!/bin/bash
# Re-runs the M1 mandatory gates (G1-G7) and the stuck-seek regression tests against the CURRENT build (used for the M2 acceptance criterion 3).
# Output: docs/gate_results/M2_m1_regression_<stamp>/ (one file per gate, unmodified tool output) + summary.txt. Runs under caffeinate; lid open, no other heavy work.
cd "$(dirname "$0")/../.." || exit 1
if [ -z "$TP_CAFFEINATED" ]; then export TP_CAFFEINATED=1; exec caffeinate -i "$0" "$@"; fi
export TP_BUILD_DIR=$PWD/build
VOL=$(tools/scripts/test_volume.sh attach) || exit 1
D=docs/gate_results/M2_m1_regression_$(date +%Y%m%d_%H%M%S); mkdir -p $D; S=$D/summary.txt; START=$(date +%s)
echo "M1 regression on the M2 build, started $(date); build $(git -C . rev-parse --short HEAD 2>/dev/null || echo 'no-git')" | tee $S
step() { local name=$1; shift; local t0=$(date +%s); echo "=== $name: $*" | tee -a $S; "$@" > $D/$name.txt 2>&1; local rc=$?; echo "    exit=$rc in $(( $(date +%s) - t0 )) s -> $(tail -2 $D/$name.txt | tr '\n' ' ' | cut -c1-200)" | tee -a $S; }
step G1_unit ctest --test-dir build -L unit --output-on-failure
step G7_policy_tests ctest --test-dir build -R policy_hash --output-on-failure
step G5_seeks_2seeders ./build/it_seek_script 2 120
step G5_seeks_1seeder ./build/it_seek_script 1 120
step G6_crash_recovery ./build/it_crash_recovery cycles=20
step G4_integrity ./build/it_integrity pieces=5000
step G3_auditor_overhead ./build/it_auditor_overhead volume=$VOL rounds=5
step M2_capi ./build/it_capi
step M2_mpv_e2e ./build/it_mpv_e2e 20
step stuck_seek_outage_2 ./build/it_outage 2 30 20
step stuck_seek_outage_1 ./build/it_outage 1 30 20
step stuck_seek_freeze ./build/it_freeze 3 100 30
echo "=== G2 strict-cap matrix (tools/scripts/run_gate_g2.sh)" | tee -a $S
tools/scripts/run_gate_g2.sh > $D/G2_matrix_stdout.txt 2>&1; echo "    exit=$? ; detailed result file: $(ls -t docs/gate_results/M1-G2_2*.txt | head -1)" | tee -a $S
cp "$(ls -t docs/gate_results/M1-G2_2*.txt | head -1)" $D/G2_matrix.txt
echo "=== power-log sleep check over the whole regression" | tee -a $S
tools/scripts/sleep_check.sh $START $(date +%s) > $D/sleep_check.txt && echo "    no sleep/wake events" | tee -a $S || echo "    SLEEP EVENTS FOUND: results of overlapping steps are invalid, rerun them" | tee -a $S
echo "finished $(date)" | tee -a $S
