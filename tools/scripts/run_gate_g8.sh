#!/bin/bash
# M1-G8: >=5 PACED real-swarm sessions (cache on the dedicated APFS test volume, 48 MiB strict cap). Pauses between sessions so trackers/peers are not hammered.
cd "$(dirname "$0")/../.." || exit 1
VOL=$(tools/scripts/test_volume.sh attach)
# Keep the machine awake (lid OPEN, ideally on AC): a sleeping machine invalidates a session. caffeinate holds a user-level assertion only; no system setting is changed.
if [ -z "$TP_CAFFEINATED" ]; then export TP_CAFFEINATED=1; exec caffeinate -i "$0" "$@"; fi
START_EPOCH=$(date +%s)
N=${1:-5}; PAUSE=${2:-120}
T=../TorrentPlayer-Research/S5-swarm/torrents/big-buck-bunny.torrent
F=docs/gate_results/M1-G8_$(date +%Y%m%d_%H%M%S).txt
echo "M1-G8 real swarm, $N sessions, pause ${PAUSE}s, started $(date)" | tee $F
for i in $(seq 1 $N); do
  echo "=== session $i $(date)" | tee -a $F
  rm -rf $VOL/g8cache; ./build/tp_realswarm torrent=$T cache=$VOL/g8cache volume=$VOL limit_mib=48 secs=240 bps_kib=1024 web=off seeks=8 2>&1 | tee build/g8_s$i.log | grep -E 'STARTUP|SEEK|seek|SUMMARY|VOLUME|margin|STORAGE|PASS|FAIL|violation|stall' | tail -25 | tee -a $F
  [ $i -lt $N ] && sleep $PAUSE
done
echo "M1-G8 finished $(date)" | tee -a $F
tools/scripts/sleep_check.sh $START_EPOCH $(date +%s) | tee -a $F && echo "sleep check: no system sleep/wake events during the run" | tee -a $F || echo "sleep check: SLEEP EVENTS FOUND - affected sessions are invalid, rerun them" | tee -a $F
