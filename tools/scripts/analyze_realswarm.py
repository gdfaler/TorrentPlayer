#!/usr/bin/env python3
"""Stage-by-stage report for one real-swarm app run directory (telemetry JSONL + net_diag.log). usage: analyze_realswarm.py <run dir> ; writes <run dir>/analysis.txt"""
import re, json, glob, sys, collections, statistics as st
D = sys.argv[1]; out = []
def P(*a): out.append(" ".join(str(x) for x in a))
ev = [json.loads(l) for f in sorted(glob.glob(D + "/telemetry/*.jsonl")) for l in open(f)]
of = lambda t: [e for e in ev if e["event"] == t]
P("RUN", D); P(open(D + "/sleep_verdict.txt").read().strip() if glob.glob(D + "/sleep_verdict.txt") else "no sleep verdict")
P("conditions:", open(D + "/conditions_before.txt").read().splitlines()[1:3])
ds = of("metadata_ready"); ps = of("playback_started"); fp = of("first_peer"); f1 = of("first_piece")
P("-- startup (since app open): metadata_ready %s | first_piece %s | first_peer(>=1 non-web peer) %s | file_loaded %s | playback_started %s" % (
  [round(e.get("fetch_s", 0), 1) for e in ds], [round(e["since_open_s"], 1) for e in f1], [round(e["since_open_s"], 1) for e in fp], [round(e["since_open_s"], 1) for e in of("file_loaded")], [round(e["since_open_s"], 1) for e in ps]))
for e in of("seek_issued"): P("seek_issued t=%.1f target=%.0f from=%.1f cached@target=%s" % (e["t"], e["target_s"], e["from_s"], e["cache_coverage_at_target_0_255"]))
for e in of("seek_done"): P("seek_done  t=%.1f target=%.0f latency=%.2f s landed=%.1f" % (e["t"], e["target_s"], e["latency_s"], e["landed_s"]))
P("-- stalls:", [(round(e["t"], 1), round(e["duration_s"], 2)) for e in of("stall_end")], "starts", len(of("stall_start")))
P("-- state transitions (raw -> qualified):"); [P("   %.1f %s -> %s" % (e["t"], e["to"], e.get("to_qualified", ""))) for e in of("state_change")]
P("-- banners:", [(round(e["t"]), e["title"]) for e in of("ui_banner")])
s = of("sample")
if s:
    P("-- samples %d | peers max %d, median %s | web_conn max %d | peer MiB %.1f | web MiB %.1f (source attribution at end)" % (len(s), max(x["peers"] for x in s), st.median(x["peers"] for x in s), max(x["web_connections"] for x in s), s[-1]["peer_bytes"] / 1048576, s[-1]["web_bytes"] / 1048576))
    P("   buffer ahead s (min/median/max): %.0f / %.0f / %.0f" % (min(x["ahead_s"] for x in s), st.median(x["ahead_s"] for x in s), max(x["ahead_s"] for x in s)), "| cache charged max MiB %.0f of %.0f" % (max(x["cache_charged"] for x in s) / 1048576, s[0]["cache_limit"] / 1048576))
P("-- errors:", of("error"), "| hash failures:", [e.get("total") for e in of("hash_failure")], "| session_end:", of("session_end")[:1])
try:
    L = [l.rstrip("\n") for l in open(D + "/net_diag.log")]
    T = collections.Counter(re.match(r"\[\s*[\d.]+\] (\w+):", l).group(1) for l in L if re.match(r"\[\s*[\d.]+\] (\w+):", l))
    P("-- NETWORK STAGES (net_diag.log, diagnostic-only alerts):", dict(T.most_common()))
    def first(p):
        for l in L:
            if re.search(r"\] " + p, l): return l[:190]
    for p in ["dht_bootstrap", "tracker_reply", "dht_reply", "lsd_peer", "peer_connect", "incoming_connection", "metadata_received"]: P("   first", p, ":", first(p))
    P("   tracker replies:", [l[:140] for l in L if "] tracker_reply" in l][:6])
    errs = collections.Counter()
    for l in L:
        m = re.match(r"\[\s*[\d.]+\] tracker_error: \S+ \((\w+://[^)]+)\)\[([^\]]*)\]\S* (.*)", l)
        if m: errs[(m.group(1)[:45], "unreachable-iface" if "unreachable" in m.group(3) else re.sub(r"\s*\"\".*", "", m.group(3))[:40])] += 1
    P("   tracker errors by (tracker, kind):", dict(errs.most_common(12)))
    pc = [l for l in L if "] peer_connect" in l]
    P("   connection attempts: total %d, uTP %d, TCP %d, SSL/TCP %d; distinct peers %d" % (len(pc), sum("(uTP)" in l for l in pc), sum("(TCP)" in l for l in pc), sum("(SSL" in l for l in pc), len(set(re.findall(r"\d+\.\d+\.\d+\.\d+:\d+", " ".join(pc))))))
    dk = collections.Counter(re.sub(r"\(reason.*", "", re.search(r"\)\s*(\[[^\]]+\] \[[^\]]+\]: .*)", l).group(1))[:70] for l in L if "] peer_disconnected" in l and re.search(r"\)\s*(\[[^\]]+\] \[[^\]]+\]: .*)", l))
    P("   disconnect reasons:", dk.most_common(6))
except FileNotFoundError: P("(no net_diag.log in this run)")
open(D + "/analysis.txt", "w").write("\n".join(out) + "\n"); print("\n".join(out))
