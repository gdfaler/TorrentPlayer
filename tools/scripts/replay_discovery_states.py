#!/usr/bin/env python3
"""OFFLINE replay of the proposed discovery diagnostic states (docs/proposals/M3_DISCOVERY_STATE_PROPOSAL.md) over RECORDED real-swarm runs.
Uses only existing evidence (net_diag.log + telemetry JSONL in each run directory). No network, no app. Nothing here is the implementation of the state machine;
it is an analysis tool that states, for each run, which states the proposed rules WOULD have produced and what could not be reconstructed.

usage: replay_discovery_states.py [--silence S] [--need S] <run dir> [<run dir> ...]   (defaults: silence 30 s, need 30 s; per proposal rev 2)
Reconstruction rules and their limits are printed in the report ("NOT RECONSTRUCTABLE")."""
import re, json, glob, sys, collections, argparse

ap = argparse.ArgumentParser(); ap.add_argument("dirs", nargs="+"); ap.add_argument("--silence", type=float, default=30.0); ap.add_argument("--need", type=float, default=30.0); ap.add_argument("--conn-hold", type=float, default=20.0); ap.add_argument("--dht-min", type=int, default=20)
args = ap.parse_args()

def load(D):
    ev = [json.loads(l) for f in sorted(glob.glob(D + "/telemetry/*.jsonl")) for l in open(f)]
    t_engine = next((e["t"] for e in ev if e["event"] == "engine_started"), 0.0)
    rows = []
    for l in open(D + "/net_diag.log"):
        m = re.match(r"\[\s*([\d.]+)\] (\w+): (.*)", l.rstrip("\n"))
        if m: rows.append((float(m.group(1)) + t_engine, m.group(2), m.group(3)))
    return ev, rows

def replay(D):
    ev, rows = load(D)
    of = lambda t: [e for e in ev if e["event"] == t]
    samples = of("sample")
    t_open = next((e["t"] for e in of("open_magnet")), None)
    t_meta_end = next((e["t"] for e in of("metadata_ready") + of("metadata_failed")), None)
    t_end = max([e["t"] for e in ev] + [0]) if ev else 0
    mode = "magnet" if t_open is not None else "torrent"
    # ---- sent announces: an announce is NOT sent if a 'skipping ... unreachable' error for the same (url, interface) follows within 1 s
    ann = [(t, re.search(r"\((\w+://[^)]+)\)", m).group(1), re.search(r"\)\[([^\]]+?)-v1\]", m).group(1)) for t, w, m in rows if w == "tracker_announce" and re.search(r"\((\w+://[^)]+)\)", m) and re.search(r"\)\[([^\]]+?)-v1\]", m)]
    unreach = collections.defaultdict(list)
    for t, w, m in rows:
        if w == "tracker_error" and "unreachable" in m:
            u = re.search(r"\((\w+://[^)]+)\)", m); i = re.search(r"\)\[([^\]]+?)-v1\]", m)
            if u and i: unreach[(u.group(1), i.group(1))].append(t)
    sent_udp = []
    for t, u, i in ann:
        if not u.startswith("udp://"): continue
        if any(0 <= x - t <= 1.0 for x in unreach.get((u, i), [])): continue
        sent_udp.append((t, u))
    udp_tr_replies = [(t, int(re.search(r"received peers: (\d+)", m).group(1))) for t, w, m in rows if w == "tracker_reply" and "udp://" in m and re.search(r"received peers: (\d+)", m)]
    other_tr_replies = [(t, int(re.search(r"received peers: (\d+)", m).group(1)), re.search(r"\((\w+)://", m).group(1)) for t, w, m in rows if w == "tracker_reply" and "udp://" not in m and re.search(r"received peers: (\d+)", m)]
    dht_q = [t for t, w, m in rows if w == "dht_outgoing_get_peers"]
    dht_boot = next((t for t, w, m in rows if w == "dht_bootstrap"), None)
    dht_rep = [(t, int(re.search(r"received DHT peers: (\d+)", m).group(1))) for t, w, m in rows if w == "dht_reply" and re.search(r"received DHT peers: (\d+)", m)]
    attempts = [t for t, w, m in rows if w == "peer_connect"]
    incoming = [t for t, w, m in rows if w == "incoming_connection"]
    meta_recv = next((t for t, w, m in rows if w == "metadata_received"), None)
    # ---- 1 Hz evaluation
    st = {"t": [], "need": [], "headline": [], "flags": []}
    need_since = None; last_udp_reply = None; first_state_time = {}
    prev = None; transitions = []; misleading = 0; problem_secs = 0; web_cover = 0
    for sec in range(0, int(t_end) + 1):
        t = float(sec)
        smp = next((x for x in reversed(samples) if x["t"] <= t), None)
        meta_pending = mode == "magnet" and t_open is not None and t >= t_open and (t_meta_end is None or t < t_meta_end)
        blocked = bool(smp and smp["blocked_readers"] > 0)
        low = bool(smp and smp["ahead_s"] >= 0 and smp["ahead_s"] < 37.5)        # low-water ~ half of the 75 s default target: APPROXIMATION (refilling flag not recorded)
        need = meta_pending or blocked or (smp is not None and low and mode == "torrent" and False)   # buffer-below-low-water alone is not used: not reconstructable reliably
        need_since = (need_since if need_since is not None else t) if need else None
        need_for = (t - need_since) if need_since is not None else 0.0
        replies_udp = sorted([x[0] for x in udp_tr_replies if x[0] <= t] + [x[0] for x in dht_rep if x[0] <= t])
        last_udp_reply = replies_udp[-1] if replies_udp else None
        since_disc = t - (min([a[0] for a in sent_udp] or [t]))
        sent_n = [a for a in sent_udp if a[0] <= t]
        cycles_ok = len({u for tt, u in sent_n}) >= 3 and len(sent_n) >= 6                # >= 3 distinct UDP trackers, >= 2 announce cycles each (6 sent announces)
        dht_ok = len([x for x in dht_q if x <= t]) >= args.dht_min and dht_boot is not None and dht_boot <= t
        quiet_for = t - (last_udp_reply if last_udp_reply is not None else min([a[0] for a in sent_udp] or [t]))
        f_udp_silent = need and need_for >= args.need and cycles_ok and dht_ok and quiet_for >= args.silence
        zero_nonudp = any(x[0] <= t and x[1] == 0 for x in other_tr_replies)
        any_peers_reply = any(x[0] <= t and x[1] > 0 for x in udp_tr_replies + [(a, b) for a, b, c in other_tr_replies] + dht_rep)
        f_tracker_zero = need and need_for >= args.need and zero_nonudp and not any_peers_reply
        f_known_addr = any_peers_reply or any(x <= t for x in incoming)
        att_n = len([x for x in attempts if x <= t])
        connected = bool(smp and smp["peers"] > 0) or (meta_recv is not None and meta_recv <= t) or any(x <= t for x in incoming)   # proxy: handshake success is NOT recorded
        first_att = attempts[0] if attempts else None
        f_conn_fail = f_known_addr and att_n >= 10 and not connected and need and first_att is not None and (t - first_att) >= args.conn_hold
        if connected: head = "CONNECTED"
        elif f_conn_fail: head = "CONNECTIONS_FAILING"
        elif f_udp_silent: head = "UDP_UNRESPONSIVE"
        elif f_tracker_zero: head = "TRACKERS_REPORT_NO_PEERS"
        elif need: head = "SEARCHING"
        else: head = "NOT_NEEDED"
        if head != prev: transitions.append((t, head)); prev = head
        if head in ("UDP_UNRESPONSIVE", "TRACKERS_REPORT_NO_PEERS", "CONNECTIONS_FAILING"):
            problem_secs += 1
            if smp and (smp["web_bytes"] > 0 and smp["download_Bps"] > 0 or (not blocked and smp["ahead_s"] > 0)): misleading += 1
        if head in ("UDP_UNRESPONSIVE", "TRACKERS_REPORT_NO_PEERS", "CONNECTIONS_FAILING") and head not in first_state_time: first_state_time[head] = t
    first_udp_reply = min([x[0] for x in udp_tr_replies] + [x[0] for x in dht_rep] or [None]) if (udp_tr_replies or dht_rep) else None
    return dict(dir=D, mode=mode, t_end=t_end, sent_udp=len(sent_udp), distinct_udp=len({u for _, u in sent_udp}), udp_replies=len(udp_tr_replies), udp_peer_replies=sum(1 for _, n in udp_tr_replies if n > 0),
                dht_queries=len(dht_q), dht_bootstrap=dht_boot, dht_replies=len(dht_rep), dht_peer_replies=sum(1 for _, n in dht_rep if n > 0), wss_replies=[(round(a, 1), b, c) for a, b, c in other_tr_replies][:3],
                attempts=len(attempts), meta_recv=meta_recv, first_udp_reply=first_udp_reply, transitions=transitions, problem_secs=problem_secs, misleading_secs=misleading, t_open=t_open, t_meta_end=t_meta_end)

if __name__ == "__main__":
    print("REPLAY RULES: silence %.0f s, need-hold %.0f s, connection-failing hold %.0f s from first attempt, DHT queries >= %d; NEED = metadata pending OR reader blocked (1 Hz sample); sent-announce = not followed by an 'unreachable' skip; connected = sample peers>0 or metadata_received or incoming (proxy)\n" % (args.silence, args.need, args.conn_hold, args.dht_min))
    for D in args.dirs:
        r = replay(D)
        print("RUN", D, "(%s, %.0f s of telemetry)" % (r["mode"], r["t_end"]))
        print("   UDP announces sent %d to %d trackers, UDP tracker replies %d (with peers %d) | DHT bootstrap %s, queries %d, replies %d (with peers %d) | non-UDP tracker replies %s | connection attempts %d | first UDP reply %s | metadata_received %s" % (
            r["sent_udp"], r["distinct_udp"], r["udp_replies"], r["udp_peer_replies"], r["dht_bootstrap"] and round(r["dht_bootstrap"], 1), r["dht_queries"], r["dht_replies"], r["dht_peer_replies"], r["wss_replies"], r["attempts"],
            r["first_udp_reply"] and round(r["first_udp_reply"], 1), r["meta_recv"] and round(r["meta_recv"], 1)))
        print("   headline transitions:", [(round(t, 1), h) for t, h in r["transitions"]])
        print("   seconds in a problem state: %d ; of which while buffered or web-seed playback could continue: %d" % (r["problem_secs"], r["misleading_secs"]))
