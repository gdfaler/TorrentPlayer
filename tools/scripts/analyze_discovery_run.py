#!/usr/bin/env python3
"""Per-run discovery/connection timings for the peer-discovery experiment. usage: analyze_discovery_run.py <run dir> [<run dir> ...]
All times are seconds since the app's telemetry started (engine_started ~ 0.1 s). net_diag.log times are engine-relative and are aligned with the
telemetry clock using the 'engine_started' event. Definitions (stated so nothing is over-read):
  discovered   = first tracker reply with peers > 0, first DHT reply with peers > 0, or first incoming connection (a peer address became known)
  attempted    = first peer_connect alert (libtorrent posts it when an outgoing attempt starts; it is NOT proof of success)
  connected    = metadata_received (magnet: a peer really delivered data) or, after the stream starts, the first 1 Hz sample with peers > 0 (non-web)
  peer data    = first 1 Hz sample with peer_bytes > 0 (bytes that came from BitTorrent peers, not the web seed); 1 s resolution
Writes <run dir>/discovery_analysis.txt and prints it."""
import re, json, glob, sys, collections

def analyse(D):
    out = []
    P = lambda *a: out.append(" ".join(str(x) for x in a))
    ev = [json.loads(l) for f in sorted(glob.glob(D + "/telemetry/*.jsonl")) for l in open(f)]
    of = lambda t: [e for e in ev if e["event"] == t]
    t_engine = of("engine_started")[0]["t"] if of("engine_started") else 0.0
    L = [l.rstrip("\n") for l in open(D + "/net_diag.log")] if glob.glob(D + "/net_diag.log") else []
    parsed = []
    for l in L:
        m = re.match(r"\[\s*([\d.]+)\] (\w+): (.*)", l)
        if m: parsed.append((float(m.group(1)) + t_engine, m.group(2), m.group(3)))
    res = {"run": D}
    def first(pred):
        for t, w, msg in parsed:
            if pred(w, msg): return t
        return None
    t_trk = first(lambda w, m: w == "tracker_reply" and re.search(r"received peers: ([1-9]\d*)", m))
    t_dht = first(lambda w, m: w == "dht_reply" and re.search(r"received DHT peers: ([1-9]\d*)", m))
    t_inc = first(lambda w, m: w == "incoming_connection")
    cands = [(t, n) for t, n in ((t_trk, "tracker"), (t_dht, "DHT"), (t_inc, "incoming")) if t is not None]
    res["first_discovered_s"] = min(cands)[0] if cands else None
    res["first_discovered_via"] = min(cands)[1] if cands else None
    res["first_tracker_reply_with_peers_s"] = t_trk; res["first_dht_reply_with_peers_s"] = t_dht
    res["first_connect_attempt_s"] = first(lambda w, m: w == "peer_connect")
    res["metadata_received_s"] = first(lambda w, m: w == "metadata_received")
    mr = of("metadata_ready"); res["metadata_ready_ui_s"] = mr[0]["t"] if mr else None
    s = of("sample")
    res["first_connected_after_stream_s"] = next((x["t"] for x in s if x["peers"] > 0), None)
    res["first_peer_data_s"] = next((x["t"] for x in s if x["peer_bytes"] > 0), None)
    res["first_web_data_s"] = next((x["t"] for x in s if x["web_bytes"] > 0), None)
    ps = of("playback_started"); res["playback_started_s"] = (ps[0]["t"] if ps else None)
    res["peers_max"] = max([x["peers"] for x in s] or [0])
    res["peer_MiB_end"] = round((s[-1]["peer_bytes"] if s else 0) / 1048576, 1); res["web_MiB_end"] = round((s[-1]["web_bytes"] if s else 0) / 1048576, 1)
    # tracker / DHT responses
    trk = collections.Counter(); dht = collections.Counter()
    for t, w, m in parsed:
        if w == "tracker_reply":
            u = re.search(r"\((\w+://[^)]+)\)", m); n = re.search(r"received peers: (\d+)", m)
            trk["reply %s peers=%s" % (u.group(1) if u else "?", n.group(1) if n else "?")] += 1
        elif w == "tracker_error":
            u = re.search(r"\((\w+://[^)]+)\)", m); kind = "unreachable-interface" if "unreachable" in m else ("timed out" if "timed out" in m else re.sub(r"\s*\"\".*", "", m.split("] ", 1)[-1])[:40])
            trk["error %s %s" % (u.group(1)[:40] if u else "?", kind)] += 1
        elif w.startswith("dht_"):
            dht[w + (" (" + re.search(r"received DHT peers: (\d+)", m).group(0) + ")" if w == "dht_reply" and re.search(r"received DHT peers: (\d+)", m) else "")] += 1
    res["external_ips_seen"] = sorted(set(re.findall(r"external IP received: ([\d.]+)", " ".join(m for t, w, m in parsed if w == "external_ip"))))
    res["announce_source_addresses"] = dict(collections.Counter(re.search(r"\[([^\]]+?)(?::6881)?-v1\]", m).group(1) for t, w, m in parsed if w == "tracker_announce" and re.search(r"\[([^\]]+?)(?::6881)?-v1\]", m)).most_common(6))
    res["tracker_summary"] = dict(trk.most_common(14)); res["dht_summary"] = dict(dht.most_common(8))
    pc = [m for t, w, m in parsed if w == "peer_connect"]
    res["connection_attempts"] = {"total": len(pc), "uTP": sum("(uTP)" in m for m in pc), "TCP": sum("(TCP)" in m for m in pc), "SSL/TCP": sum("(SSL" in m for m in pc),
                                  "distinct_peers": len(set(re.findall(r"\d+\.\d+\.\d+\.\d+:\d+", " ".join(pc))))}
    dk = collections.Counter()
    for t, w, m in parsed:
        if w == "peer_disconnected":
            r = re.search(r"\)\s*(\[[^\]]+\] \[[^\]]+\]: [^(]*)", m)
            if r: dk[r.group(1).strip()[:70]] += 1
    res["disconnect_reasons"] = dict(dk.most_common(8))
    P("RUN", D); P(open(D + "/sleep_verdict.txt").read().strip() if glob.glob(D + "/sleep_verdict.txt") else "no sleep verdict")
    P("conditions:", " | ".join(open(D + "/conditions_before.txt").read().splitlines()[1:3]))
    for k, v in res.items():
        if k != "run": P("  %-36s %s" % (k, ("%.1f" % v) if isinstance(v, float) else v))
    open(D + "/discovery_analysis.txt", "w").write("\n".join(out) + "\n")
    return res, out

if __name__ == "__main__":
    allr = []
    for D in sys.argv[1:]:
        r, o = analyse(D); print("\n".join(o)); print(); allr.append(r)
    if len(allr) > 1:
        print("SUMMARY (seconds since app start):")
        keys = ["metadata_received_s", "first_discovered_s", "first_discovered_via", "first_connect_attempt_s", "first_connected_after_stream_s", "first_peer_data_s", "first_web_data_s", "playback_started_s", "peers_max", "peer_MiB_end", "web_MiB_end"]
        for k in keys: print("  %-34s %s" % (k, [("%.1f" % r[k]) if isinstance(r[k], float) else r[k] for r in allr]))
