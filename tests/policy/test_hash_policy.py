#!/usr/bin/env python3
"""Automated tests for the G8 hash-failure stop rule (tools/scripts/hash_policy.py). Exit codes: 0 continue, 3 halt."""
import hashlib, os, shutil, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "tools", "scripts", "hash_policy.py")
BLOCK, NB = 16384, 16
sha1 = lambda b: hashlib.sha1(b).hexdigest()

def make(root, piece=7, corrupt_blocks=(5,), disk_differs=False, verdict="ARRIVED CORRUPT (disk == received bytes, but ...)", attribute=True, two_endpoints_on=None,
         good_copy=True, good_wrong=False, drop_attr_block=None, bad_block_hash=False):
    good = bytes((i * 31 + 7) % 251 for i in range(BLOCK * NB))
    rec = bytearray(good)
    for b in corrupt_blocks: rec[b * BLOCK + 100: b * BLOCK + 113] = b"\xff" * 13
    rec = bytes(rec); disk = bytearray(rec)
    if disk_differs: disk[5 * BLOCK + 3] ^= 1
    base = os.path.join(root, "ev", "piece_%d_t1" % piece); os.makedirs(os.path.dirname(base), exist_ok=True)
    lines = ["piece %d" % piece, "verdict: %s" % verdict, "expected sha1: %s" % sha1(good)]
    for i in range(NB):
        h = sha1(rec[i * BLOCK:(i + 1) * BLOCK]); lines.append("block %d sha1(received)=%s" % (i, ("0" * 40) if (bad_block_hash and i == 2) else h))
    open(base + ".txt", "w").write("\n".join(lines) + "\n")
    open(base + ".received.bin", "wb").write(rec); open(base + ".disk.bin", "wb").write(bytes(disk))
    if attribute:
        d = ["piece %d later verified OK" % piece]
        for i in range(NB):
            if drop_attr_block == i: continue
            d.append("failed-copy block %d delivered by 10.0.0.%d:5000" % (i, i + 1))
            if two_endpoints_on == i: d.append("failed-copy block %d delivered by 10.9.9.9:6000" % i)
        open(base + ".diff.txt", "w").write("\n".join(d) + "\n")
    cache = os.path.join(root, "cache"); os.makedirs(cache, exist_ok=True)
    if good_copy: open(os.path.join(cache, "piece_%d.seg" % piece), "wb").write((good[:-1] + b"\x00") if good_wrong else good)
    return os.path.join(root, "ev"), cache

def run(ev, cache, failures=1, arrived=1, storage=0, undetermined=0):
    p = subprocess.run([sys.executable, "-I", TOOL, "--session", "t", "--hash-failures", str(failures), "--arrived", str(arrived), "--storage", str(storage),
                        "--undetermined", str(undetermined), "--evidence-dir", ev, "--cache-dir", cache], capture_output=True, text=True)
    return p.returncode, p.stdout

fails = []
def case(name, expect, **kw):
    counts = {k: kw.pop(k) for k in ("failures", "arrived", "storage", "undetermined") if k in kw}
    root = tempfile.mkdtemp()
    try:
        ev, cache = make(root, **kw); rc, out = run(ev, cache, **counts)
    finally: shutil.rmtree(root, ignore_errors=True)
    ok = rc == expect
    print("%s %-62s exit=%d (expected %d) %s" % ("ok  " if ok else "FAIL", name, rc, expect, out.strip().splitlines()[0][:90] if out.strip() else ""))
    if not ok: fails.append(name)

case("peer-delivered corruption, conclusive evidence -> continue", 0)
case("peer-delivered corruption in two blocks, both attributed -> continue", 0, corrupt_blocks=(3, 9))
case("session reports a STORAGE verdict -> halt", 3, storage=1, arrived=0)
case("classifier says arrived but disk bytes != received bytes -> halt", 3, disk_differs=True)
case("classifier verdict is STORAGE CORRUPTION in the evidence -> halt", 3, verdict="STORAGE CORRUPTION (disk != received)")
case("UNDETERMINED hash failure -> halt", 3, undetermined=1, arrived=0)
case("verdict text says UNDETERMINED -> halt", 3, verdict="UNDETERMINED (no received bytes)")
case("no attribution file -> halt", 3, attribute=False)
case("differing block has no attribution line -> halt", 3, drop_attr_block=5)
case("differing block attributed to two endpoints (contradictory) -> halt", 3, two_endpoints_on=5)
case("no good re-downloaded copy (recovery unverified) -> halt", 3, good_copy=False)
case("re-downloaded copy does not match the hash -> halt", 3, good_wrong=True)
case("recorded block sha1 contradicts the received bytes -> halt", 3, bad_block_hash=True)
case("evidence count differs from reported failures -> halt", 3, failures=2, arrived=2)
case("counts inconsistent (failures 1, arrived 0, storage 0) -> halt", 3, arrived=0)

# ---- the series script itself: extract its real stop_check()/hash_gate() and run them against synthetic sessions
SCRIPT = open(os.path.join(ROOT, "tools", "scripts", "run_series_g8.sh")).read()
FUNCS = SCRIPT[SCRIPT.index("hash_gate() {"):SCRIPT.index("valid=$(awk")]
def stop_case(name, expect, hf=1, ar=1, st=0, un=0, res="PASS", cv=0, stk=0, **kw):
    root = tempfile.mkdtemp()
    try:
        ev, cache = make(root, **kw)
        run_dir = os.path.join(root, "run"); os.makedirs(run_dir)
        os.rename(ev, os.path.join(run_dir, "evidence_t1"))                       # as preserve_evidence lays it out
        if os.path.exists(cache) and os.listdir(cache):
            for f in os.listdir(cache):
                shutil.copy(os.path.join(cache, f), os.path.join(run_dir, "evidence_t1", f.replace(".seg", "_good_copy.seg")))
        prog = os.path.join(run_dir, "progress.txt")
        sh = "cd %s; RUN=%s; PROG=%s; SESS_VALID=1; SESS_RES=%s; SESS_HF=%d; SESS_AR=%d; SESS_ST=%d; SESS_UN=%d; SESS_CV=%d; SESS_STK=%d\n%s\nstop_check t1; echo CONTINUED" % (ROOT, run_dir, prog, res, hf, ar, st, un, cv, stk, FUNCS)
        p = subprocess.run(["bash", "-c", sh], capture_output=True, text=True)
        cont = "CONTINUED" in p.stdout
        csvrow = os.path.exists(os.path.join(run_dir, "hash_failures.csv"))
    finally: shutil.rmtree(root, ignore_errors=True)
    ok = (cont == (expect == 0))
    print("%s %-70s %s" % ("ok  " if ok else "FAIL", "series stop_check: " + name, "continued" if cont else "HALTED (exit %d)" % p.returncode))
    if not ok: fails.append("stop_check " + name)
    return csvrow
r = stop_case("conclusive peer-delivered corruption -> series continues", 0)
if not r: print("FAIL hash_failures.csv row not written"); fails.append("csv row")
else: print("ok   series stop_check: failure recorded in hash_failures.csv")
stop_case("confirmed storage verdict -> series halts", 3, st=1, ar=0)
stop_case("undetermined hash failure -> series halts", 3, un=1, ar=0)
stop_case("ambiguous attribution (missing block) -> series halts", 3, drop_attr_block=5)
stop_case("contradictory attribution -> series halts", 3, two_endpoints_on=5)
stop_case("no re-downloaded good copy -> series halts", 3, good_copy=False)
stop_case("cap violation (with clean hashes) -> series halts", 3, hf=0, ar=0, cv=1)
stop_case("stuck seek (with clean hashes) -> series halts", 3, hf=0, ar=0, stk=1)
stop_case("clean session -> series continues", 0, hf=0, ar=0)

# regression on the real preserved r031 evidence (skipped if the cache copy of the good piece is gone: the preserved .seg is used instead)
ev31 = os.path.join(ROOT, "docs", "gate_results", "series_20261015_102303", "evidence_r031")
if os.path.isdir(ev31):
    rc, out = run(ev31, "/nonexistent")
    ok = rc == 0; print("%s real r031 evidence (piece 1052, peer-delivered) -> continue    exit=%d" % ("ok  " if ok else "FAIL", rc)); fails += [] if ok else ["real r031"]
print("RESULT: %s (%d failing)" % ("PASS" if not fails else "FAIL", len(fails)))
sys.exit(1 if fails else 0)
