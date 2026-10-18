#!/usr/bin/env python3
"""G8 hash-failure stop rule (approved policy; docs/MILESTONE1_STATUS.md, "Stop rule for peer-delivered corruption").

usage: hash_policy.py --session ID --hash-failures N --arrived A --storage S --undetermined U --evidence-dir DIR [--cache-dir DIR] [--csv hash_failures.csv]
Exit 0  = CONTINUE: every hash failure is CONCLUSIVE peer-delivered corruption (recorded, never relabelled as a pass).
Exit 3  = HALT: storage verdict, undetermined failure, or any missing / incomplete / contradictory evidence or attribution.
Every failure is preserved as a failure: this tool only decides whether the series may continue; results.csv is never edited.

A failure is CONCLUSIVE only if ALL hold (otherwise it is treated as UNDETERMINED and halts):
  1. classifier verdict line says ARRIVED CORRUPT;
  2. received.bin and disk.bin exist and are byte-identical (the storage layer wrote exactly what it received);
  3. sha1(received.bin) != the expected sha1 recorded for the piece (the received bytes really are wrong);
  4. the per-block sha1s recorded by the classifier match the received bytes;
  5. a good copy of the piece exists (re-download) and sha1(good) == expected sha1 (recovery outcome verified);
  6. at least one block differs between received and good copy, and EVERY differing block has exactly one delivering endpoint in the attribution;
  7. the attribution covers every block of the piece (no gaps).
The number of evidence records must equal the number of hash failures reported by the session.
"""
import argparse, csv, glob, hashlib, os, re, sys

BLOCK = 16384

def sha1(b): return hashlib.sha1(b).hexdigest()

def analyse_one(txt_path, cache_dir):
    base = txt_path[:-4]
    why = []
    try:
        txt = open(txt_path).read()
    except OSError as e:
        return None, ["cannot read evidence text: %s" % e], {}
    m = re.search(r"^piece (\d+)", txt, re.M); piece = int(m.group(1)) if m else -1
    info = {"piece": piece, "evidence": base, "verdict": "", "endpoints": "", "differing_blocks": "", "recovery": "unknown"}
    vm = re.search(r"^verdict: (.*)$", txt, re.M); verdict = vm.group(1) if vm else ""
    info["verdict"] = verdict
    if "ARRIVED CORRUPT" not in verdict: why.append("classifier verdict is not ARRIVED CORRUPT (%r)" % verdict[:60])
    em = re.search(r"^expected sha1: ([0-9a-f]{40})", txt, re.M)
    if not em: why.append("expected sha1 missing")
    rec_p, disk_p, diff_p = base + ".received.bin", base + ".disk.bin", base + ".diff.txt"
    if not (os.path.exists(rec_p) and os.path.exists(disk_p)): why.append("received/disk bytes missing")
    if why: return False, why, info
    rec, disk = open(rec_p, "rb").read(), open(disk_p, "rb").read()
    if rec != disk: why.append("STORAGE MISMATCH: disk bytes differ from received bytes")
    exp = em.group(1)
    if sha1(rec) == exp: why.append("received bytes actually match the expected hash (contradiction)")
    nblocks = (len(rec) + BLOCK - 1) // BLOCK
    recorded = {int(i): h for i, h in re.findall(r"^block (\d+) sha1\(received\)=([0-9a-f]{40})", txt, re.M)}
    if len(recorded) != nblocks: why.append("recorded block hashes incomplete (%d of %d)" % (len(recorded), nblocks))
    for i, h in recorded.items():
        if sha1(rec[i * BLOCK:(i + 1) * BLOCK]) != h: why.append("recorded block %d sha1 does not match the received bytes" % i); break
    # recovery: good copy
    good = None
    if cache_dir:
        gp = os.path.join(cache_dir, "piece_%d.seg" % piece)
        if os.path.exists(gp): good = open(gp, "rb").read()
    if good is None:
        gl = glob.glob(base.rsplit("_t", 1)[0] + "_good_copy*")
        if gl: good = open(gl[0], "rb").read()
    if good is None: why.append("no good re-downloaded copy available (recovery not verified)")
    elif sha1(good) != exp: why.append("re-downloaded copy does not match the expected hash")
    else: info["recovery"] = "piece re-downloaded and verified"
    # attribution
    att = {}
    if os.path.exists(diff_p):
        for i, ep in re.findall(r"^failed-copy block (\d+) delivered by (\S+)", open(diff_p).read(), re.M): att.setdefault(int(i), []).append(ep)
    else: why.append("attribution file missing")
    if sorted(att) != list(range(nblocks)): why.append("attribution does not cover every block (%d of %d)" % (len(att), nblocks))
    if any(len(v) != 1 for v in att.values()): why.append("a block has contradictory attribution")
    if good is not None and sha1(good) == exp and len(good) == len(rec):
        diff = [i for i in range(nblocks) if good[i * BLOCK:(i + 1) * BLOCK] != rec[i * BLOCK:(i + 1) * BLOCK]]
        info["differing_blocks"] = " ".join(map(str, diff))
        if not diff: why.append("no differing block found (cannot attribute)")
        eps = []
        for i in diff:
            if len(att.get(i, [])) != 1: why.append("differing block %d has no unique attribution" % i)
            else: eps.append(att[i][0])
        info["endpoints"] = " ".join(sorted(set(eps)))
    return (not why), why, info

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--session", required=True); ap.add_argument("--hash-failures", type=int, required=True)
    ap.add_argument("--arrived", type=int, default=0); ap.add_argument("--storage", type=int, default=0); ap.add_argument("--undetermined", type=int, default=0)
    ap.add_argument("--evidence-dir"); ap.add_argument("--cache-dir"); ap.add_argument("--csv")
    a = ap.parse_args()
    if a.hash_failures == 0: print("no hash failures"); return 0
    if a.storage > 0: print("HALT: %d STORAGE-CORRUPTION verdict(s)" % a.storage); return 3
    if a.undetermined > 0: print("HALT: %d UNDETERMINED hash failure(s)" % a.undetermined); return 3
    if a.arrived != a.hash_failures: print("HALT: classifier counts do not add up (failures %d, arrived %d)" % (a.hash_failures, a.arrived)); return 3
    txts = sorted(t for t in glob.glob(os.path.join(a.evidence_dir or "", "*", "piece_*.txt")) + glob.glob(os.path.join(a.evidence_dir or "", "piece_*.txt")) if not t.endswith(".diff.txt"))
    if len(txts) != a.hash_failures: print("HALT: %d hash failure(s) reported but %d evidence record(s) found" % (a.hash_failures, len(txts))); return 3
    rows, halt = [], False
    for t in txts:
        ok, why, info = analyse_one(t, a.cache_dir)
        info["conclusive"] = bool(ok); info["why_not"] = "; ".join(why)
        rows.append(info)
        print(("CONCLUSIVE peer-delivered corruption: piece %s, differing block(s) [%s], endpoint(s) %s, recovery: %s" % (info["piece"], info["differing_blocks"], info["endpoints"], info["recovery"])) if ok else ("HALT: not conclusive for piece %s: %s" % (info["piece"], "; ".join(why))))
        halt |= not ok
    if a.csv:
        new = not os.path.exists(a.csv)
        with open(a.csv, "a", newline="") as f:
            w = csv.writer(f)
            if new: w.writerow(["session", "piece", "differing_blocks", "endpoints", "evidence", "classifier_verdict", "recovery_outcome", "conclusive", "why_not", "series_decision"])
            for r in rows: w.writerow([a.session, r["piece"], r["differing_blocks"], r["endpoints"], r["evidence"], r["verdict"], r["recovery"], int(r["conclusive"]), r["why_not"], "HALT" if halt else "CONTINUE (failure kept, session stays counted as valid for cap/sleep)"])
    return 3 if halt else 0

if __name__ == "__main__": sys.exit(main())
