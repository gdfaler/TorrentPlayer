#!/usr/bin/env python3
"""RECOVERY cleanup for ONE specific halted CHURN-F smoke run (prepared 2026-10-17; NOT authorized to run until the user's separate written approval exists).
Scope: exclusively the ten ring directories diagchurn_<runid>_slot0..9 of the run identified by the expected-identity file. It does NOT modify or replace cleanup_churn_ring.py (whose
refusal of halted runs stays in force). It is not a general tool: it refuses any run whose identity, evidence hashes or halt signature differ from the expected-identity file.
usage: python3 -I recover_halted_churn_ring.py <run_dir> <volume> --expected <expected.json> --approval <approval_file> [--plan-only]
  --plan-only : performs every read-only verification and writes the evidence package, but deletes NOTHING.
Order (every step aborts with exit 7 and NOTHING deleted on any doubt): identity -> approval file -> process check -> device/ownership/structure/nlink checks -> preserved-entry snapshot ->
inventory with sha256 written OUTSIDE the volume -> re-read of the saved evidence and checksum verification -> independent re-hash of the volume files against the inventory -> final
re-check of ownership -> deletion of the enumerated paths only (shutil.rmtree on exact paths; no shell, no glob) -> post-verification (gone; preserved entries byte-for-byte identical in
name, size, mtime, inode) -> outcome record."""
import hashlib, json, os, re, shutil, stat, subprocess, sys, time

def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''): h.update(c)
    return h.hexdigest()

def die(msg, code=7):
    print('RECOVERY ABORTED, NOTHING DELETED: ' + msg); sys.exit(code)

def inventory(root):
    inv = []; alloc = 0; size = 0
    for dp, dn, fn in os.walk(root, followlinks=False):
        dn.sort()
        for n in sorted(fn):
            p = os.path.join(dp, n); s = os.lstat(p)
            if not stat.S_ISREG(s.st_mode) or s.st_nlink != 1: raise RuntimeError('not a plain single-link regular file: ' + p)
            inv.append({'path': os.path.relpath(p, root), 'size': s.st_size, 'allocated': s.st_blocks * 512, 'ino': s.st_ino, 'sha256': sha256_file(p)}); alloc += s.st_blocks * 512; size += s.st_size
    return inv, size, alloc

def snapshot(path):
    """name/size/mtime/inode of every entry below path (no hashing): used to prove the preserved entries are untouched."""
    out = {}
    if os.path.islink(path): return {'.': 'symlink'}
    for dp, dn, fn in os.walk(path, followlinks=False):
        for n in sorted(dn + fn):
            p = os.path.join(dp, n); s = os.lstat(p); out[os.path.relpath(p, path)] = (s.st_size, s.st_mtime_ns, s.st_ino, s.st_mode)
    s = os.lstat(path); out['.'] = (s.st_size, s.st_mtime_ns, s.st_ino, s.st_mode)
    return out

def main():
    a = sys.argv[1:]
    if len(a) < 6 or '--expected' not in a or '--approval' not in a: die('usage: recover_halted_churn_ring.py <run_dir> <volume> --expected <json> --approval <file> [--plan-only]')
    d = os.path.abspath(a[0]); vol = os.path.realpath(a[1]); exp = json.load(open(a[a.index('--expected') + 1])); appr = a[a.index('--approval') + 1]; plan_only = '--plan-only' in a
    rid = exp['runid']
    # ---- 1 identity: run directory, hash lock, evidence file hashes, halt signature --------------------------------------------------------------------------------------------
    if os.path.basename(d) != exp['run_dir_name']: die('run directory name differs from the expected identity')
    for name, h in exp['evidence_sha256'].items():
        if not os.path.exists(os.path.join(d, name)) or sha256_file(os.path.join(d, name)) != h: die('evidence file differs from the expected identity: ' + name)
    fin = open(os.path.join(d, 'finished.txt')).read().strip()
    if fin != exp['finished_txt']: die('finished.txt differs from the expected halt signature: ' + fin)
    mi = json.load(open(os.path.join(d, 'manifest_intended.json'))); mf = json.load(open(os.path.join(d, 'manifest_final.json')))
    if mi['runid'] != rid or mf['runid'] != rid: die('manifest run id differs from the expected identity')
    if os.path.realpath(mi['volume']) != vol: die('volume differs from the manifest volume')
    # ---- 2 explicit written approval file must exist and name this exact run + hash lock ---------------------------------------------------------------------------------------
    if not os.path.exists(appr): die('approval file missing: ' + appr)
    if ('APPROVED ' + rid + ' ' + exp['evidence_sha256']['PLAN_LOCK.txt']) not in open(appr).read(): die('approval file does not name this run id and hash lock')
    # ---- 3 no harness process running ----------------------------------------------------------------------------------------------------------------------------------------
    if subprocess.run(['pgrep', '-x', 'diag_churn_f'], capture_output=True).stdout.strip(): die('the harness process diag_churn_f is running')
    # ---- 4 device / ownership / structure checks -----------------------------------------------------------------------------------------------------------------------------
    if os.stat(d).st_dev == os.stat(vol).st_dev: die('evidence directory is on the test volume')
    ev = os.path.join(d, 'recovery_evidence_planonly' if plan_only else 'recovery_evidence')
    if os.path.exists(ev): die(os.path.basename(ev) + ' already exists (never overwritten)')
    rows = [json.loads(l) for l in open(os.path.join(d, 'transfers.jsonl')) if l.strip()]
    last_inode = {}
    for r in sorted(rows, key=lambda r: r['k']): last_inode[r['slot']] = r['slot_inode_new']
    intended = [os.path.realpath(p) for p in mi['intended_slot_dirs']]
    forbidden = re.compile(r'^(overhead_|diagc1b)')
    entries = sorted(os.listdir(vol)); ours_names = [n for n in entries if n.startswith('diagchurn_')]
    expected_names = ['diagchurn_%s_slot%d' % (rid, j) for j in range(10)]
    if ours_names != sorted(expected_names): die('volume diagchurn_* entries differ from exactly the ten expected ring directories: %s' % ours_names)
    targets = []
    for j in range(10):
        p = os.path.join(vol, expected_names[j]); s = mf['slots'][j]
        if s['slot'] != j or s['path'] != p or not s['exists']: die('manifest_final slot entry differs: %d' % j)
        if forbidden.match(expected_names[j]) or os.path.islink(p) or not os.path.isdir(p): die('ownership uncertain (name/symlink/type): ' + p)
        st = os.lstat(p)
        if st.st_dev != os.stat(vol).st_dev or os.path.dirname(os.path.realpath(p)) != vol or os.path.realpath(p) not in intended: die('device/location/manifest mismatch: ' + p)
        if st.st_ino != s['inode'] or st.st_ino != last_inode.get(j): die('inode differs from the one recorded by the harness: ' + p)
        top = os.listdir(p)
        if len(top) != 1 or not re.fullmatch(r'[0-9a-f]{40}', top[0]) or not os.path.isdir(os.path.join(p, top[0])) or os.path.islink(os.path.join(p, top[0])): die('unexpected slot structure (expect exactly one 40-hex store directory): ' + p)
        for n in os.listdir(os.path.join(p, top[0])):
            if not re.fullmatch(r'piece_\d+\.seg', n): die('unexpected entry in store directory: %s/%s' % (p, n))
        targets.append((j, p))
    # ---- 5 snapshot of the preserved entries (names/sizes/mtimes/inodes), before anything else ------------------------------------------------------------------------------------
    preserved = [n for n in entries if forbidden.match(n)]
    if len(preserved) != 11 or sorted(preserved) != sorted(exp['preserved_entries']): die('preserved entries differ from the expected 11: %s' % preserved)
    snap_before = {n: snapshot(os.path.join(vol, n)) for n in preserved}
    free_before = shutil.disk_usage(vol).free
    # ---- 6 inventory + checksums OUTSIDE the volume --------------------------------------------------------------------------------------------------------------------------
    os.makedirs(ev); plan = []; sums = []
    try:
        for j, p in targets:
            inv, size, alloc = inventory(p)
            if not (1 <= len(inv) <= 480 and 0 < alloc <= (128 << 20)): die('inventory out of bounds for slot %d: %d files, %d bytes' % (j, len(inv), alloc))
            fn = os.path.join(ev, 'inventory_slot%d.json' % j); json.dump({'path': p, 'inode': os.lstat(p).st_ino, 'files': inv}, open(fn, 'w'), indent=0)
            sums.append((sha256_file(fn), os.path.basename(fn))); plan.append({'slot': j, 'path': p, 'files': len(inv), 'apparent_bytes': size, 'allocated_bytes': alloc, 'inode': os.lstat(p).st_ino})
    except RuntimeError as e: die(str(e))
    json.dump({'runid': rid, 'planned_deletions': plan, 'preserved_entries': preserved, 'preserved_snapshot_entries': {n: len(v) for n, v in snap_before.items()}, 'free_bytes_before': free_before, 'time': time.time()}, open(os.path.join(ev, 'recovery_plan.json'), 'w'), indent=1)
    json.dump({n: {k: list(v) if isinstance(v, tuple) else v for k, v in snap.items()} for n, snap in snap_before.items()}, open(os.path.join(ev, 'preserved_snapshot_before.json'), 'w'))
    for nm in ('recovery_plan.json', 'preserved_snapshot_before.json'): sums.append((sha256_file(os.path.join(ev, nm)), nm))
    with open(os.path.join(ev, 'SHA256SUMS'), 'w') as f:
        for h, n in sums: f.write('%s  %s\n' % (h, n))
    # ---- 7 verify the saved evidence (re-read), then re-hash the volume files independently against it -------------------------------------------------------------------------
    for l in open(os.path.join(ev, 'SHA256SUMS')):
        h, n = l.split()
        if sha256_file(os.path.join(ev, n)) != h: die('saved evidence checksum mismatch: ' + n)
    for j, p in targets:
        inv = json.load(open(os.path.join(ev, 'inventory_slot%d.json' % j)))['files']
        now = {}
        for dp, dn, fn in os.walk(p, followlinks=False):
            for n in fn: now[os.path.relpath(os.path.join(dp, n), p)] = os.path.join(dp, n)
        if set(now) != {e['path'] for e in inv}: die('volume file set differs from the saved inventory in slot %d' % j)
        for e in inv:
            s = os.lstat(now[e['path']])
            if s.st_size != e['size'] or s.st_ino != e['ino'] or sha256_file(now[e['path']]) != e['sha256']: die('volume file differs from the saved inventory: ' + now[e['path']])
    print('evidence saved to %s and verified: %d directories, %d files, %d bytes' % (ev, len(plan), sum(x['files'] for x in plan), sum(x['allocated_bytes'] for x in plan)))
    if plan_only: print('PLAN ONLY: nothing deleted'); sys.exit(0)
    # ---- 8 last ownership re-check, then delete the enumerated paths only ---------------------------------------------------------------------------------------------------
    for j, p in targets:
        if os.path.islink(p) or os.lstat(p).st_ino != plan[j]['inode']: die('ownership changed during verification: ' + p)
    if sorted(n for n in os.listdir(vol) if n.startswith('diagchurn_')) != sorted(expected_names): die('volume diagchurn_* entries changed during verification')
    print('deleting exactly these directories:'); [print('  ', x['path'], x['files'], 'files', x['allocated_bytes'], 'bytes') for x in plan]
    outcome = []
    for j, p in targets:
        try: shutil.rmtree(p); outcome.append({'path': p, 'exists_after': os.path.exists(p)})
        except Exception as e:
            outcome.append({'path': p, 'error': str(e)}); json.dump({'partial_outcome': outcome}, open(os.path.join(ev, 'recovery_outcome_PARTIAL.json'), 'w'), indent=1); print('DELETION ERROR (stopped; saved evidence intact):', p, e); sys.exit(8)
    snap_after = {n: snapshot(os.path.join(vol, n)) for n in preserved}
    same = snap_after == snap_before; remaining = sorted(n for n in os.listdir(vol) if n.startswith('diagchurn_'))
    res = {'outcome': outcome, 'all_removed': all(not o['exists_after'] for o in outcome), 'diagchurn_remaining': remaining, 'preserved_unchanged': same, 'free_bytes_before': free_before, 'free_bytes_after': shutil.disk_usage(vol).free, 'time': time.time()}
    json.dump(res, open(os.path.join(ev, 'recovery_outcome.json'), 'w'), indent=1)
    with open(os.path.join(ev, 'SHA256SUMS_outcome'), 'w') as f: f.write('%s  recovery_outcome.json\n' % sha256_file(os.path.join(ev, 'recovery_outcome.json')))
    print('recovery outcome:', json.dumps({k: res[k] for k in ('all_removed', 'diagchurn_remaining', 'preserved_unchanged')}), 'free before/after GiB: %.3f / %.3f' % (free_before / 2**30, res['free_bytes_after'] / 2**30))
    sys.exit(0 if res['all_removed'] and same and not remaining else 8)

main()
