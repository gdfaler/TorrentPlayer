#!/usr/bin/env python3
"""Pre-registered analysis for the DIAGNOSTIC G3 variance study (docs/proposals/G3_DIAGNOSTIC_PROPOSAL.md rev 2). NOT the G3 gate; nothing printed here is a G3 verdict.
usage: analyze_g3_diag.py <run dir with transfers.jsonl>      -> writes <run dir>/report.txt
Definitions fixed in the plan BEFORE data collection (do not edit after seeing data):
  d_k       = ln(T_on,k / T_off,k), T = MiB/s over the 160 MiB sequential read window (same quantity as the gate); a pair is VALID only if both transfers are valid
  CI        = 95% two-sided percentile bootstrap over valid pairs, B = 10000, RNG seed 20261016
  margin    = ln(0.90) = -0.10536
  verdicts  = (i) CI entirely above the margin; (ii) CI entirely below the margin; (iii) otherwise INCONCLUSIVE (never reported as 'no effect')
  churn     = per transfer: Z = zero-peer samples / samples (50 ms) over the read window, W = watchdog re-add count in the window; HIGH if W >= 1 or Z >= 0.70, else LOW
"""
import json, math, random, sys, statistics as st, collections

D = sys.argv[1]
rows = [json.loads(l) for l in open(D + "/transfers.jsonl") if l.strip()]
mode = rows[0]["mode"] if rows else "?"
MARGIN = math.log(0.90); B = 10000; SEED = 20261016
out = []
def P(*a): out.append(" ".join(str(x) for x in a))

pairs = collections.defaultdict(dict)
for r in rows: pairs[r["pair"]][r["cond"]] = r
all_pairs = sorted(pairs)
valid = [k for k in all_pairs if "on" in pairs[k] and "off" in pairs[k] and pairs[k]["on"]["valid"] and pairs[k]["off"]["valid"]]
invalid = [k for k in all_pairs if k not in valid]
P("DIAGNOSTIC REPORT (not gate G3) -- mode %s -- %s" % (mode, D))
P("pairs attempted %d, valid %d, invalid/incomplete %d" % (len(all_pairs), len(valid), len(invalid)))
for k in invalid:
    why = [("%s: %s" % (c, pairs[k][c]["reason"] or "other")) for c in ("on", "off") if c in pairs[k] and not pairs[k][c]["valid"]] or ["incomplete pair (run halted)"]
    P("  EXCLUDED pair %d: %s" % (k, "; ".join(why)))
try: P("run status:", open(D + "/finished.txt").read().strip())
except OSError: P("run status: finished.txt missing (run did not end normally)")

def boot_ci(vals, fn=st.mean, b=B, seed=SEED):
    rnd = random.Random(seed); n = len(vals); est = []
    for _ in range(b): est.append(fn([vals[rnd.randrange(n)] for _ in range(n)]))
    est.sort(); return est[int(0.025 * b)], est[int(0.975 * b) - 1]
def sign_p(vals):
    pos = sum(1 for v in vals if v > 0); neg = sum(1 for v in vals if v < 0); n = pos + neg
    if n == 0: return 1.0
    k = min(pos, neg); p = sum(math.comb(n, i) for i in range(0, k + 1)) / 2 ** n
    return min(1.0, 2 * p)
def verdict(lo, hi):
    if lo > MARGIN: return "(i) CI entirely above the 10%-loss margin"
    if hi < MARGIN: return "(ii) CI entirely below the 10%-loss margin"
    return "(iii) INCONCLUSIVE at the 10%-loss margin (this is not evidence of no effect)"
def summarize(name, d, sd_assumed=None):
    if len(d) < 3: P("%s: too few valid pairs (%d) to estimate" % (name, len(d))); return None
    m = st.mean(d); lo, hi = boot_ci(d)
    P("%s: n=%d  mean d=%.4f (geometric-mean ratio %.3f)  95%% bootstrap CI d [%.4f, %.4f] = ratio [%.3f, %.3f]  SD(d)=%.3f  half-width %.3f" % (name, len(d), m, math.exp(m), lo, hi, math.exp(lo), math.exp(hi), st.stdev(d), (hi - lo) / 2))
    P("   exact two-sided sign test p=%.4f ; fraction of pairs with ratio < 0.90: %.1f%% ; fraction < 1: %.1f%%" % (sign_p(d), 100 * sum(1 for v in d if v < MARGIN) / len(d), 100 * sum(1 for v in d if v < 0) / len(d)))
    P("   margin verdict:", verdict(lo, hi))
    return m, lo, hi

if mode == "C1B":
    cal = [json.loads(l) for l in open(D + "/calibration.jsonl") if l.strip()] if __import__("os").path.exists(D + "/calibration.jsonl") else []
    bad = [r for r in rows + cal if not r.get("hard_ok", True)]
    try: fin = open(D + "/finished.txt").read().strip()
    except OSError: fin = ""
    if bad or "HARD" in fin or "CALIBRATION" in fin:
        P("*** FAILED DIAGNOSTIC CHECK / HALT: correctness (or calibration) takes precedence over performance. NO performance result from this run is interpreted. ***")
        for r in bad: P("  failing transfer %s: %s" % (r.get("tag"), r.get("hard_failures")))
        P("  finished.txt: " + fin)
        open(D + "/report.txt", "w").write("\n".join(out) + "\n"); print("\n".join(out)); sys.exit(0)
lbl = {"A": ("auditors ON vs OFF", "protocol A, minimal instrumentation in both arms"), "C2": ("auditors ON vs OFF", "protocol C2, full churn instrumentation in both arms"),
       "OVH": ("instrumentation ON vs OFF (auditors OFF in both)", "instrumentation-overhead pairs"),
       "C1B": ("auditors ON vs OFF", "protocol C1b: production SegmentStore NEAR THE CAP (reservation + forced eviction), synthetic sequential stream, NO peers/network; NOT an end-to-end G3 equivalent and cannot resolve G3"),
       "C1": ("auditors ON vs OFF", "protocol C1: production SegmentStore path under a synthetic piece stream, NO peers/network; NOT an end-to-end G3 equivalent and cannot resolve G3")}.get(mode, ("on vs off", ""))
P("comparison: %s; %s" % lbl)
d = [math.log(pairs[k]["on"]["mibps"] / pairs[k]["off"]["mibps"]) for k in valid]
res = summarize("PRIMARY paired log-ratio of throughput", d)
if d:
    sd = st.stdev(d) if len(d) > 1 else float("nan")
    P("   observed SD(d)=%.3f versus the planning value 0.65 (empirical SD of the 85 historical G3 rounds; the first proposal assumed 0.50). Planned N=120 gives an expected CI half-width of about 0.116 at SD 0.65." % sd)
    off_first = [x for k, x in zip(valid, d) if pairs[k]["off"]["wall"] < pairs[k]["on"]["wall"]]; on_first = [x for k, x in zip(valid, d) if pairs[k]["on"]["wall"] < pairs[k]["off"]["wall"]]
    if len(off_first) > 2: P("   order OFF-first: n=%d mean d=%.4f" % (len(off_first), st.mean(off_first)))
    if len(on_first) > 2: P("   order ON-first : n=%d mean d=%.4f" % (len(on_first), st.mean(on_first)))
    dt = [pairs[k]["on"]["ttfb_s"] - pairs[k]["off"]["ttfb_s"] for k in valid]; m = st.mean(dt); lo, hi = boot_ci(dt)
    P("SECONDARY ttfb difference (on - off): mean %.3f s, 95%% CI [%.3f, %.3f]" % (m, lo, hi))
    dc = [pairs[k]["on"]["cpu_s"] - pairs[k]["off"]["cpu_s"] for k in valid]; m = st.mean(dc); lo, hi = boot_ci(dc)
    P("SECONDARY process CPU time difference (on - off): mean %.3f s per transfer, 95%% CI [%.3f, %.3f]" % (m, lo, hi))
    P("arm means: throughput off %.1f MiB/s, on %.1f MiB/s (descriptive)" % (st.mean(pairs[k]["off"]["mibps"] for k in valid), st.mean(pairs[k]["on"]["mibps"] for k in valid)))
    onr = [pairs[k]["on"] for k in valid if pairs[k]["on"]["dir_scans"] > 0]
    if onr: P("auditor sampling (on arm): directory scans accepted %.1f%% (median), 1 ms volume sampler peak median %.1f MiB" % (100 * st.median(r["dir_accepted"] / r["dir_scans"] for r in onr), st.median(r["vol_peak"] for r in onr) / 1048576))
    # informational only: how the unchanged G3 formula would have judged consecutive blocks of 5 valid pairs from THIS data
    blocks = [valid[i:i + 5] for i in range(0, len(valid) - 4, 5)]; bv = []
    for blk in blocks:
        mo = st.mean(pairs[k]["on"]["mibps"] for k in blk); mf = st.mean(pairs[k]["off"]["mibps"] for k in blk)
        to = st.mean(pairs[k]["on"]["ttfb_s"] for k in blk); tf = st.mean(pairs[k]["off"]["ttfb_s"] for k in blk)
        bv.append(to <= tf * 1.10 + 0.30 and mo >= mf * 0.90)
    if blocks: P("INFORMATIONAL ONLY (NOT a G3 result, different harness/protocol): the G3 formula applied to %d consecutive 5-pair blocks would give %d pass / %d fail" % (len(blocks), sum(bv), len(bv) - sum(bv)))
    P("slow-mode check (descriptive): transfers below 20 MiB/s: off %d of %d, on %d of %d" % (sum(1 for k in valid if pairs[k]["off"]["mibps"] < 20), len(valid), sum(1 for k in valid if pairs[k]["on"]["mibps"] < 20), len(valid)))

# ---- C1b secondary: eviction-regime evidence, reservation latency, correctness summary
if mode == "C1B" and valid:
    T = [pairs[k][c] for k in valid for c in ("on", "off")]
    P("REGIME EVIDENCE (every valid transfer): forced evictions min/median/max %d/%d/%d ; sweep evictions min/median/max %d/%d/%d ; max charged %.2f-%.2f MiB (internal cap %.2f, limit %.0f) ; final present pieces %s ; allocated bytes max %.2f MiB" % (
        min(r["forced_evictions"] for r in T), st.median(r["forced_evictions"] for r in T), max(r["forced_evictions"] for r in T), min(r["sweep_evictions"] for r in T), st.median(r["sweep_evictions"] for r in T), max(r["sweep_evictions"] for r in T),
        min(r["max_charged_mib"] for r in T), max(r["max_charged_mib"] for r in T), T[0]["internal_cap_mib"], T[0]["limit_mib"], sorted(set(r["present_end"] for r in T)), max(r["allocated_mib"] for r in T)))
    P("CORRECTNESS (all transfers, hard checks): hard failures %d, cap violations %d, refused writes %d, set differences %d, read-back mismatches %d, absent-piece-readable %d, reserve_calls==writes in all: %s" % (
        sum(1 for r in T if not r["hard_ok"]), sum(r["cap_violations"] for r in T), sum(r["refused_writes"] for r in T), sum(r["set_diff"] for r in T), sum(r["integrity_bad"] for r in T), sum(r["absent_bad"] for r in T), all(r["reserve_calls"] == r["writes"] for r in T)))
    for key, nm in (("evict_phase_mibps", "eviction-phase throughput (pieces 460-899)"), ):
        dd = [math.log(pairs[k]["on"][key] / pairs[k]["off"][key]) for k in valid if pairs[k]["on"][key] > 0 and pairs[k]["off"][key] > 0]; m_ = st.mean(dd); lo_, hi_ = boot_ci(dd)
        P("SECONDARY paired log-ratio of %s (on/off): mean %.4f (ratio %.3f), 95%% CI [%.4f, %.4f] = ratio [%.3f, %.3f], n=%d" % (nm, m_, math.exp(m_), lo_, hi_, math.exp(lo_), math.exp(hi_), len(dd)))
    for key, nm in (("first_write_p50_us", "first-block write() p50 (reservation/eviction path)"), ("first_write_p99_us", "first-block write() p99 (reservation/eviction path)"), ("other_write_p99_us", "other-block write() p99"), ("read_p99_us", "read() p99")):
        dd = [math.log(max(pairs[k]["on"][key], 1e-3) / max(pairs[k]["off"][key], 1e-3)) for k in valid]; m_ = st.mean(dd); lo_, hi_ = boot_ci(dd)
        P("SECONDARY paired log-ratio of %s (on/off; >0 = slower with auditors): mean %.4f (ratio %.3f), 95%% CI [%.4f, %.4f] = ratio [%.3f, %.3f]" % (nm, m_, math.exp(m_), lo_, hi_, math.exp(lo_), math.exp(hi_)))
    P("C1b LIMITATION: store-only, strictly sequential oldest-first workload, no libtorrent/peers/hashing/network/churn; NOT an end-to-end G3 equivalent; cannot resolve G3 or say anything about the real swarm.")

# ---- C1 secondary: store-operation latency and store sanity
if mode == "C1" and valid:
    for key, nm in (("write_p50_us", "write() p50 latency"), ("write_p99_us", "write() p99 latency"), ("read_p99_us", "read() p99 latency")):
        dd = [math.log(max(pairs[k][ "on"][key], 1e-3) / max(pairs[k]["off"][key], 1e-3)) for k in valid]; m_ = st.mean(dd); lo_, hi_ = boot_ci(dd)
        P("SECONDARY paired log-ratio of %s (on/off; >0 means slower with auditors): mean %.4f (ratio %.3f), 95%% CI [%.4f, %.4f] = ratio [%.3f, %.3f]" % (nm, m_, math.exp(m_), lo_, hi_, math.exp(lo_), math.exp(hi_)))
    P("store sanity over all valid transfers: cap violations %d, refused writes %d, max charged %.1f MiB of limit %.0f MiB, forced evictions median %d, sweep evictions median %d" % (
        sum(pairs[k][c]["cap_violations"] for k in valid for c in ("on", "off")), sum(pairs[k][c]["refused_writes"] for k in valid for c in ("on", "off")),
        max(pairs[k][c]["max_charged_mib"] for k in valid for c in ("on", "off")), pairs[valid[0]]["on"]["limit_mib"], st.median(pairs[k][c]["forced_evictions"] for k in valid for c in ("on", "off")), st.median(pairs[k][c]["sweep_evictions"] for k in valid for c in ("on", "off"))))
    P("C1 LIMITATION: this measures the store path only (no libtorrent, no peers, no hashing, no network); it cannot say anything about the end-to-end G3 workload or the gate.")

# ---- C2: observational stratification (pre-registered churn definition)
if mode in ("C2", "OVH"):
    def Z(r): return r["zero_peer_samples"] / r["samples"] if r["samples"] else float("nan")
    def stratum(r): return "HIGH" if (r["wd_resets"] >= 1 or Z(r) >= 0.70) else "LOW"
    P("")
    P("CHURN COVARIATES (observational; definition pre-registered: HIGH if watchdog re-adds >= 1 or zero-peer fraction >= 0.70, else LOW)")
    cat = collections.defaultdict(list)
    for k in valid:
        a, b = pairs[k]["off"], pairs[k]["on"]
        if not (a["instrumented"] and b["instrumented"]): continue
        cat[stratum(a) + "/" + stratum(b)].append(k)
    for c in ("LOW/LOW", "HIGH/HIGH", "LOW/HIGH", "HIGH/LOW"): P("  pairs (off-stratum/on-stratum) %-9s: %d" % (c, len(cat.get(c, []))))
    for c, nm in (("LOW/LOW", "concordant LOW"), ("HIGH/HIGH", "concordant HIGH")):
        ks = cat.get(c, []); dd = [math.log(pairs[k]["on"]["mibps"] / pairs[k]["off"]["mibps"]) for k in ks]
        if len(ks) >= 20: summarize("  within %s" % nm, dd)
        else: P("  within %s: n=%d < 20 (pre-registered minimum): NOT estimated" % (nm, len(ks)))
    disc = cat.get("LOW/HIGH", []) + cat.get("HIGH/LOW", [])
    if disc:
        dd = [math.log(pairs[k]["on"]["mibps"] / pairs[k]["off"]["mibps"]) for k in disc]
        P("  discordant pairs: n=%d, mean d=%.3f (churn differs between the arms, so d here mixes churn and condition)" % (len(disc), st.mean(dd)))
    ks = [k for k in valid if pairs[k]["off"]["instrumented"] and pairs[k]["on"]["instrumented"] and pairs[k]["on"]["samples"] and pairs[k]["off"]["samples"]]
    if len(ks) >= 10:
        x = [Z(pairs[k]["on"]) - Z(pairs[k]["off"]) for k in ks]; y = [math.log(pairs[k]["on"]["mibps"] / pairs[k]["off"]["mibps"]) for k in ks]
        mx, my = st.mean(x), st.mean(y); sxx = sum((a - mx) ** 2 for a in x); sxy = sum((a - mx) * (b - my) for a, b in zip(x, y)); slope = sxy / sxx if sxx else float("nan")
        r2 = (sxy ** 2) / (sxx * sum((b - my) ** 2 for b in y)) if sxx and sum((b - my) ** 2 for b in y) else float("nan")
        rnd = random.Random(SEED); sl = []
        for _ in range(2000):
            idx = [rnd.randrange(len(ks)) for _ in ks]; xs = [x[i] for i in idx]; ys = [y[i] for i in idx]; a = st.mean(xs); b = st.mean(ys); s2 = sum((v - a) ** 2 for v in xs)
            if s2: sl.append(sum((u - a) * (v - b) for u, v in zip(xs, ys)) / s2)
        sl.sort(); P("  regression of d on (zero-peer fraction on - off): slope %.3f (95%% bootstrap CI [%.3f, %.3f]), R^2 %.3f, n=%d; intercept at equal churn: d = %.3f" % (slope, sl[int(0.025 * len(sl))], sl[int(0.975 * len(sl)) - 1], r2, len(ks), my - slope * mx))
    P("  churn descriptives: zero-peer fraction mean %.2f; watchdog re-adds in %d of %d valid instrumented transfers" % (st.mean(Z(pairs[k][c]) for k in valid for c in ("on", "off") if pairs[k][c]["samples"]), sum(1 for k in valid for c in ("on", "off") if pairs[k][c]["wd_resets"] >= 1), 2 * len(valid)))
P("")
P("LIMITATIONS: single machine, loopback payload, one build, AC power; diagnostic design, not the gate; an INCONCLUSIVE margin verdict is not evidence of no auditor effect; strata are observational (churn is not randomised).")
open(D + "/report.txt", "w").write("\n".join(out) + "\n"); print("\n".join(out))
