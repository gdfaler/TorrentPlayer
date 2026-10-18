#!/bin/bash
# M2 first real-swarm playback through the APP (Big Buck Bunny, official WebTorrent torrent; read-only use of the research torrent file).
# Requires AC power and an open lid (refuses to start otherwise); runs under caffeinate; validates the power log afterwards; preserves telemetry + app output.
# usage: run_m2_realswarm.sh [play_seconds=300]
cd "$(dirname "$0")/../.." || exit 1
if [ -z "$TP_CAFFEINATED" ]; then export TP_CAFFEINATED=1; exec caffeinate -i "$0" "$@"; fi
PLAY=${1:-300}
MODE=${MODE:-torrent}                       # torrent | magnet (magnet built from the SAME torrent's infohash, trackers and web seed)
EXTRA_ARGS=${EXTRA_ARGS:-}                  # e.g. "--web-seeds off" (diagnostic variants; the standard run uses the approved Assist default)
if ! pmset -g batt | head -1 | grep -q 'AC Power'; then echo "REFUSING: not on AC power (the approved test conditions are AC power + lid open)."; exit 3; fi
D=docs/gate_results/M2_realswarm_${MODE}${RUN_TAG:+_$RUN_TAG}_$(date +%Y%m%d_%H%M%S); mkdir -p $D     # unique run id: never overwrites earlier evidence
T=../TorrentPlayer-Research/S5-swarm/torrents/big-buck-bunny.torrent
START=$(date +%s)
{ echo "start $(date)"; pmset -g batt | head -2; sysctl kern.sleeptime kern.waketime; echo "default cache dir: ~/Library/Caches/TorrentPlayer/stream"; echo "MODE=$MODE EXTRA_ARGS=$EXTRA_ARGS RUN_TAG=$RUN_TAG"; netstat -rn -f inet | grep -E "^default"; ifconfig utun7 2>/dev/null | head -2; scutil --dns 2>/dev/null | head -12; }  > $D/conditions_before.txt
if [ "$MODE" = magnet ]; then
  OPEN=$(python3 -I - "$T" <<'PY'
import sys,hashlib,urllib.parse
def dec(b,i=0):
    c=b[i:i+1]
    if c==b'i': j=b.index(b'e',i); return int(b[i+1:j]),j+1
    if c==b'l':
        i+=1; r=[]
        while b[i:i+1]!=b'e': v,i=dec(b,i); r.append(v)
        return r,i+1
    if c==b'd':
        i+=1; r={}
        while b[i:i+1]!=b'e': k,i=dec(b,i); v,i=dec(b,i); r[k]=v
        return r,i+1
    j=b.index(b':',i); n=int(b[i:j]); return b[j+1:j+1+n],j+1+n
b=open(sys.argv[1],'rb').read(); d,_=dec(b); i=b.index(b'4:info')+6; _,j=dec(b,i)
m='magnet:?xt=urn:btih:'+hashlib.sha1(b[i:j]).hexdigest()+'&dn='+urllib.parse.quote(d[b'info'][b'name'].decode())
for t in d[b'announce-list']: m+='&tr='+urllib.parse.quote(t[0].decode(),safe='')
for u in d.get(b'url-list',[]): m+='&ws='+urllib.parse.quote(u.decode(),safe='')
print(m)
PY
)
  echo "$OPEN" > $D/magnet_link.txt
else OPEN=$T; fi
export TP_NET_DIAG=$PWD/$D/net_diag.log      # diagnostic-only: peer discovery / connection stages (tracker, DHT, LSD, connect, disconnect, metadata)
export TP_LOG_DIR=$PWD/$D/telemetry TP_SNAPSHOT_DIR=$PWD/$D/snapshots
# the BBB mp4 is file index 1 in this torrent; default cache dir (no --cache-dir) and default 2 GiB limit; web seeds in the approved default mode (assist); audio muted
./build/TorrentPlayer.app/Contents/MacOS/TorrentPlayer --open "$OPEN" --file-index 1 --autoplay --mute --exit-after $PLAY --start-timeout 240 \
  $([ "$NO_SEEKS" = 1 ] || echo --seek-script 60:300,110:60,160:450,210:20) $EXTRA_ARGS > $D/app_stdout.txt 2>&1 &
APP_PID=$!
# a failed open (e.g. magnet without peers) leaves the app idle: stop it after the longest expected duration and record that the operator script did so
( sleep $((PLAY + 300)); kill $APP_PID 2>/dev/null && echo "script watchdog killed the app after $((PLAY + 300)) s" > $D/watchdog_note.txt ) &
WD=$!
wait $APP_PID; kill $WD 2>/dev/null
{ echo "end $(date)"; pmset -g batt | head -2; sysctl kern.sleeptime kern.waketime; } > $D/conditions_after.txt
tools/scripts/sleep_check.sh $START $(date +%s) > $D/sleep_check.txt && echo "no sleep/wake events during the run" | tee $D/sleep_verdict.txt || echo "SLEEP EVENTS FOUND: this run is INVALID for performance analysis" | tee $D/sleep_verdict.txt
cat $D/app_stdout.txt | tail -3
echo "results: $D"
