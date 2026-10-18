#!/bin/bash
# usage: sleep_check.sh <start_epoch> <end_epoch>
# Prints every system Sleep/DarkWake/Wake event (pmset log) inside [start,end]. A test session that overlaps one is INVALID for gate purposes
# (the machine was asleep: wall-clock deadlines expire and peers time out; see docs/MILESTONE1_STATUS.md, stuck-seek investigation).
# Exit 0 = no sleep events, 1 = sleep events found. (pmset -g log takes ~1-2 minutes.)
S=$1; E=$2
pmset -g log 2>/dev/null | grep -E '^[0-9-]+ [0-9:]+ [+-][0-9]+ +(Sleep|Wake|DarkWake) ' | while read -r d t rest; do
  ep=$(date -j -f '%Y-%m-%d %H:%M:%S' "$d $t" +%s 2>/dev/null) || continue
  [ "$ep" -ge "$S" ] && [ "$ep" -le "$E" ] && echo "$d $t $rest" | cut -c1-170
done | tee /tmp/sleep_check_$$.txt
if [ -s /tmp/sleep_check_$$.txt ]; then rm -f /tmp/sleep_check_$$.txt; exit 1; fi; rm -f /tmp/sleep_check_$$.txt; exit 0
