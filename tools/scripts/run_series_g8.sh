#!/bin/bash
# Real-swarm strict-cap series (criterion 2 release gate; spec 16): >= 30 VALID sessions + one long (>= 1 h) session, cache on the dedicated APFS test volume,
# 48 MiB strict cap. usage: run_series_g8.sh [valid_target=30] [pause_s=120] [long_secs=3600]
# Policy (documented in docs/MILESTONE1_STATUS.md):
#  * a session is INVALID if the system slept or woke during it (kern.sleeptime/waketime changed; confirmed at the end by the pmset log). Invalid sessions are kept
#    (log + record) and rerun; never discarded. More than 6 invalid sessions in total aborts the series.
#  * the series STOPS at once on a valid session with a cap violation, hash failure, storage verdict, stuck seek or any tp_realswarm FAIL: investigate before continuing.
#  * every session keeps its full log and one machine-readable CSV row; the power source is recorded per session.
cd "$(dirname "$0")/../.." || exit 1
if [ -z "$TP_CAFFEINATED" ]; then export TP_CAFFEINATED=1; exec caffeinate -i "$0" "$@"; fi
TARGET=${1:-30}; PAUSE=${2:-120}; LONG=${3:-3600}
VOL=$(tools/scripts/test_volume.sh attach) || exit 1
T=../TorrentPlayer-Research/S5-swarm/torrents/big-buck-bunny.torrent
# RESUME: TP_RESUME_DIR=docs/gate_results/series_<stamp> continues an existing series in place. Existing rows/logs are never touched or renumbered; the valid count is
# taken from results.csv (valid=1 rows only), new ids continue after the highest existing id, the invalid count carries over, and the long session is skipped if a valid one exists.
RUN=${TP_RESUME_DIR:-docs/gate_results/series_$(date +%Y%m%d_%H%M%S)}; mkdir -p $RUN/sessions
CSV=$RUN/results.csv; PROG=$RUN/progress.txt; START=$(date +%s)
[ -f $CSV ] || echo "id,kind,valid,invalid_reason,start_epoch,end_epoch,power,startup_s,pieces_verified,hash_failures,arrived,storage,undetermined,vol_peak_mib,dir_peak_mib,max_charged_mib,limit_mib,cap_violations,refused_writes,stalls,stall_total_s,seeks,seek_median_s,seek_max_s,stuck,peer_bans,web_mib,peer_mib,result" > $CSV
echo "series $([ -n "$TP_RESUME_DIR" ] && echo RESUMED || echo started) $(date); target $TARGET valid + long ${LONG}s; pause ${PAUSE}s; run dir $RUN" | tee $PROG
stamp() { sysctl -n kern.sleeptime kern.waketime | tr -d '\n '; }
power() { pmset -g batt | head -1 | grep -q 'AC Power' && echo AC || echo BATTERY; }
batt_pct() { pmset -g batt | grep -Eo '[0-9]+%' | head -1 | tr -d '%'; }
# Battery guard (series may run on battery): stop CLEANLY between sessions when low; everything stays resumable (TP_RESUME_DIR). MIN_BATT default 15 %, long session needs 30 %.
battery_guard() { # $1 = minimum percent
  [ "$(power)" = AC ] && return 0
  local b=$(batt_pct); b=${b:-100}
  if [ "$b" -lt "$1" ]; then echo "$(date +%T) STOPPED CLEANLY: battery ${b}% < ${1}% on battery power. Nothing was discarded; resume later with TP_RESUME_DIR=$RUN" | tee -a $PROG; exit 5; fi
}
num() { sed -E "s/$1/\\1/" ; }
# Hash-failure evidence is preserved BEFORE the next session wipes the cache: classifier files, received/disk bytes, and the re-downloaded good copy of each failed piece.
preserve_evidence() { # $1 = session id
  local src=$VOL/series_cache/evidence dst=$RUN/evidence_$1
  [ -d "$src" ] || return 0
  rm -rf "$dst"; cp -R "$src" "$dst"
  for t in $(find "$dst" -name 'piece_*.txt' ! -name '*.diff.txt'); do
    local b=$(basename $t .txt); local piece=$(echo $b | sed -E 's/piece_([0-9]+)_t.*/\1/'); local ih=$(basename $(dirname $t))
    [ -f "$VOL/series_cache/$ih/piece_$piece.seg" ] && cp "$VOL/series_cache/$ih/piece_$piece.seg" "$(dirname $t)/piece_${piece}_good_copy.seg"
  done
}
run_session() { # $1 id  $2 kind(normal|long)  $3 secs  $4 seeks
  local id=$1 kind=$2 secs=$3 seeks=$4 log=$RUN/sessions/$1.log
  local s0=$(stamp) e0=$(date +%s) pw=$(power)
  rm -rf $VOL/series_cache
  ./build/tp_realswarm torrent=$T cache=$VOL/series_cache volume=$VOL limit_mib=48 secs=$secs bps_kib=1024 web=off seeks=$seeks > $log 2>&1
  local e1=$(date +%s) s1=$(stamp) valid=1 why=""
  [ "$id" = "$TP_FAKE_SLEEP_ID" ] && s1="fake-sleep-for-testing-the-invalidation-path"
  [ "$s0" != "$s1" ] && { valid=0; why="system sleep/wake during the session (sysctl)"; }
  g() { grep -m1 "$1" $log; }
  local su=$(g 'STARTUP' | sed -E 's/.*after ([0-9.]+) s.*/\1/')
  local iv=$(g 'INTEGRITY'); local bd=$(g 'BOUND:'); local pb=$(g 'PLAYBACK'); local tr=$(g 'TRAFFIC')
  local pv=$(echo "$iv" | sed -E 's/.*verified ([0-9]+),.*/\1/') hf=$(echo "$iv" | sed -E 's/.*hash failures ([0-9]+) .*/\1/') ar=$(echo "$iv" | sed -E 's/.*ARRIVED-CORRUPT ([0-9]+),.*/\1/') st=$(echo "$iv" | sed -E 's/.*STORAGE-CORRUPTION ([0-9]+),.*/\1/') un=$(echo "$iv" | sed -E 's/.*UNDETERMINED ([0-9]+)\).*/\1/') bans=$(echo "$iv" | sed -E 's/.*peer bans ([0-9]+).*/\1/')
  local vp=$(echo "$bd" | sed -E 's/.*volume auditor peak ([0-9.]+) MiB.*/\1/') dp=$(echo "$bd" | sed -E 's/.*dir auditor peak ([0-9.]+) MiB.*/\1/') mc=$(echo "$bd" | sed -E 's/.*max charged ([0-9.]+) MiB.*/\1/') lim=$(echo "$bd" | sed -E 's/.*limit ([0-9]+) MiB.*/\1/') cv=$(echo "$bd" | sed -E 's/.*violations ([0-9]+) \|.*/\1/') rw=$(echo "$bd" | sed -E 's/.*refused writes ([0-9]+).*/\1/')
  local sl=$(echo "$pb" | sed -E 's/.*stalls ([0-9]+) \(([0-9.]+) s total\).*/\1/') stt=$(echo "$pb" | sed -E 's/.*stalls ([0-9]+) \(([0-9.]+) s total\).*/\2/') sk=$(echo "$pb" | sed -E 's/.*seeks ([0-9]+) \(median.*/\1/') smed=$(echo "$pb" | sed -E 's/.*median (-?[0-9.]+) s.*/\1/') smax=$(echo "$pb" | sed -E 's/.*max (-?[0-9.]+) s.*/\1/') stk=$(echo "$pb" | sed -E 's/.*stuck ([0-9]+),.*/\1/')
  local web=$(echo "$tr" | sed -E 's/.*web seeds ([0-9.]+) MiB.*/\1/') peer=$(echo "$tr" | sed -E 's/.*peers ([0-9.]+) MiB.*/\1/')
  local res=$(grep -c '\] PASS' $log); [ "$res" = 1 ] && res=PASS || res=FAIL
  if [ "${hf:-1}" != 0 ]; then preserve_evidence $id; fi
  [ -z "$su" ] && { why="${why:+$why; }no startup line (no data)"; }
  echo "$id,$kind,$valid,\"$why\",$e0,$e1,$pw,${su:-NA},${pv:-NA},${hf:-NA},${ar:-NA},${st:-NA},${un:-NA},${vp:-NA},${dp:-NA},${mc:-NA},${lim:-NA},${cv:-NA},${rw:-NA},${sl:-NA},${stt:-NA},${sk:-NA},${smed:-NA},${smax:-NA},${stk:-NA},${bans:-NA},${web:-NA},${peer:-NA},$res" >> $CSV
  echo "$(date +%T) $id ($kind, $pw) valid=$valid${why:+ ($why)} result=$res startup=${su:-NA}s verified=${pv:-NA} hash_failed=${hf:-NA} storage=${st:-NA} vol_peak=${vp:-NA}MiB charged=${mc:-NA}MiB cap_viol=${cv:-NA} stuck=${stk:-NA} seeks=${sk:-NA} seek_med=${smed:-NA}s max=${smax:-NA}s stalls=${sl:-NA}" | tee -a $PROG
  SESS_AR=${ar:-1}; SESS_UN=${un:-1}; SESS_VALID=$valid; SESS_RES=$res; SESS_HF=${hf:-1}; SESS_ST=${st:-1}; SESS_CV=${cv:-1}; SESS_STK=${stk:-1}; SESS_SU=${su:-NA}
}
# STOP RULE (approved policy, docs/MILESTONE1_STATUS.md): a valid session halts the series on a failed result, cap violation, stuck seek, STORAGE verdict, UNDETERMINED
# hash failure, or ANY hash failure whose evidence is not conclusive peer-delivered corruption (tools/scripts/hash_policy.py). A conclusive peer-delivered corruption is
# recorded in hash_failures.csv (and stays a failure in results.csv) and the series continues.
hash_gate() { # $1 = session id
  python3 -I tools/scripts/hash_policy.py --session $1 --hash-failures "$SESS_HF" --arrived "$SESS_AR" --storage "$SESS_ST" --undetermined "$SESS_UN" --evidence-dir $RUN/evidence_$1 --csv $RUN/hash_failures.csv | tee -a $PROG
  return ${PIPESTATUS[0]}
}
stop_check() { # genuine problem in a VALID session => stop and investigate
  if [ "$SESS_VALID" = 1 ]; then
    if [ "$SESS_RES" != PASS ] || [ "$SESS_ST" != 0 ] || [ "$SESS_CV" != 0 ] || [ "$SESS_STK" != 0 ]; then
      echo "$(date +%T) STOP: valid session $1 shows a failure (result=$SESS_RES hash_failures=$SESS_HF storage=$SESS_ST cap_violations=$SESS_CV stuck=$SESS_STK). Series halted for investigation; nothing was discarded." | tee -a $PROG; exit 3; fi
    if [ "$SESS_HF" != 0 ]; then
      if hash_gate $1; then echo "$(date +%T) NOTE: session $1 had $SESS_HF hash failure(s), conclusively peer-delivered; recorded in hash_failures.csv, series continues (failure NOT relabelled)." | tee -a $PROG
      else echo "$(date +%T) STOP: session $1 hash failure(s) not conclusively peer-delivered (see above). Series halted for investigation; nothing was discarded." | tee -a $PROG; exit 3; fi
    fi
  fi
}
valid=$(awk -F, 'NR>1 && $2=="normal" && $3==1' $CSV | wc -l | tr -d ' ')
invalid=$(awk -F, 'NR>1 && $3==0' $CSV | wc -l | tr -d ' ')
n=$(awk -F, 'NR>1{sub(/^r/,"",$1); if ($1+0>m) m=$1+0} END{print m+0}' $CSV)
echo "state at start: $valid valid normal sessions already recorded, $invalid invalid (kept), next id r$(printf %03d $((n+1)))" | tee -a $PROG
while [ $valid -lt $TARGET ]; do
  battery_guard ${MIN_BATT:-15}
  n=$((n+1)); id=$(printf "r%03d" $n)
  run_session $id normal ${NORMAL_SECS:-240} 8
  if [ "$SESS_VALID" = 1 ]; then stop_check $id; valid=$((valid+1)); else invalid=$((invalid+1)); echo "$(date +%T) $id INVALID, will be rerun ($invalid invalid so far)" | tee -a $PROG; [ $invalid -gt ${MAX_INVALID:-6} ] && { echo "ABORT: more than ${MAX_INVALID:-6} invalid sessions (machine keeps sleeping); fix the environment first" | tee -a $PROG; exit 4; }; fi
  [ $valid -lt $TARGET ] && sleep $PAUSE
done
if awk -F, 'NR>1 && $2=="long" && $3==1' $CSV | grep -q .; then echo "valid long session already recorded; skipping" | tee -a $PROG; else
sleep $PAUSE
battery_guard ${MIN_BATT_LONG:-30}
LID=long$(printf %02d $(( $(awk -F, 'NR>1 && $2=="long"' $CSV | wc -l) + 1 )))
run_session $LID long $LONG 60
if [ "$SESS_VALID" = 1 ]; then stop_check $LID; else echo "$(date +%T) $LID INVALID: rerun the long session by hand after fixing the cause" | tee -a $PROG; fi
fi
echo "$(date +%T) running the final power-log sleep check over the whole series window (takes ~2 min)" | tee -a $PROG
if tools/scripts/sleep_check.sh $START $(date +%s) > $RUN/sleep_check.txt; then echo "sleep check: no sleep/wake events during the series" | tee -a $PROG; else echo "sleep check: EVENTS FOUND (see sleep_check.txt); sessions overlapping them must be treated as invalid" | tee -a $PROG; fi
echo "series finished $(date): valid sessions $valid, invalid $invalid (kept, rerun)" | tee -a $PROG
