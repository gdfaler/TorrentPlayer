#!/bin/bash
# Approved peer-discovery experiment: EXACTLY three cold app starts about 10 minutes apart (start to start), magnet link of the same torrent, existing network
# diagnostics (TP_NET_DIAG), unchanged engine/torrent configuration, unique cold cache per run, no seeks, 60 s of playback after it starts. Requires AC power.
# Does not touch routing, tunnel, firewall or engine behaviour. Not to be extended beyond three runs without approval.
cd "$(dirname "$0")/../.." || exit 1
if [ -z "$TP_CAFFEINATED" ]; then export TP_CAFFEINATED=1; exec caffeinate -i "$0" "$@"; fi
EXP=docs/gate_results/M2_discovery_experiment_$(date +%Y%m%d_%H%M%S); mkdir -p $EXP
echo "experiment dir: $EXP" | tee $EXP/experiment_log.txt
for i in 1 2 3; do
  T0=$(date +%s)
  echo "=== run $i start $(date)" | tee -a $EXP/experiment_log.txt
  MODE=magnet RUN_TAG=disc$i NO_SEEKS=1 EXTRA_ARGS="--cache-dir $PWD/build/it_run/rs_cache_disc$i" tools/scripts/run_m2_realswarm.sh 60 > $EXP/run${i}_console.txt 2>&1
  RD=$(ls -d docs/gate_results/M2_realswarm_magnet_disc${i}_* | tail -1); echo "$RD" >> $EXP/run_dirs.txt; echo "    run $i dir: $RD ; exit $(tail -1 $EXP/run${i}_console.txt)" | tee -a $EXP/experiment_log.txt
  if [ $i -lt 3 ]; then W=$(( 600 - ($(date +%s) - T0) )); [ $W -gt 0 ] && sleep $W; fi
done
echo "=== finished $(date)" | tee -a $EXP/experiment_log.txt
