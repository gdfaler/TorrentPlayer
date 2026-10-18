#!/usr/bin/env python3
"""RECOVERY cleanup for ONE specific halted CHURN-F smoke run (version 2, 2026-10-17). Version 1 (plan-only executed once) is kept as recover_halted_churn_ring_v1_planonly_executed.py.
Scope: exclusively the ten ring directories diagchurn_<runid>_slot0..9 of the run identified by the expected-identity file; not a general tool. cleanup_churn_ring.py (and its refusal of halted runs) is untouched.
MODES (exactly one is required; there is NO default):
  --plan-only : read-only. Needs NO approval; passing --approval is rejected. Writes the evidence package to recovery_evidence_planonly_<UTC stamp>/ (host disk). Deletes nothing, renames nothing.
  --delete    : requires --approval <file>, a dedicated JSON file of type RECOVERY_DELETION_APPROVAL naming the exact run id, plan-lock hash, the exact ten target paths, the plan-only evidence directory and
                the sha256 of its SHA256SUMS, the sha256 of the canonical preserved-content inventory, the sha256 of THIS script and of the expected-identity file, and an expiry. Any missing, malformed,
                stale, mismatched or mode-inappropriate approval aborts with nothing deleted.
usage: python3 -I recover_halted_churn_ring.py <run_dir> <volume> --expected <expected.json> (--plan-only | --delete --approval <approval.json>) [--allow-test-hooks]
Every pre-deletion failure exits 7 with NOTHING deleted or renamed. Deletion is one target at a time: lsof check (no process may hold a file open there) -> metadata revalidation against the state verified
a moment earlier -> atomic rename to a private quarantine name -> identity check of what was renamed (renamed back and abort if it is not the expected inode) -> rmtree of the quarantine path. A failure stops
immediately; every target's state is recorded (removed / partially removed with the remaining entries / quarantined / not attempted). RESIDUAL LIMITATION: deletion is not atomic and cannot be made so; a process with
write access can still create or change files inside a target between the last check and rmtree (the lsof check, the rename and the single-user nature of the volume narrow, but do not close, that window)."""
import datetime, fcntl, hashlib, json, os, re, shutil, stat, subprocess, sys, time

APPROVAL_TYPE = 'RECOVERY_DELETION_APPROVAL'
APPROVAL_KEYS = {'type', 'runid', 'plan_lock_sha256', 'target_paths', 'plan_only_evidence_dir', 'plan_only_sha256sums_sha256', 'preserved_content_inventory_sha256', 'script_sha256', 'expected_json_sha256', 'expires_utc'}

def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''): h.update(c)
    return h.hexdigest()

def die(msg, code=7):
    print('RECOVERY ABORTED, NOTHING DELETED: ' + msg); sys.exit(code)

def canon_hash(mapping): return hashlib.sha256(json.dumps(sorted(mapping.items()), separators=(',', ':')).encode()).hexdigest()

def inventory(root):
    inv = []; alloc = 0; size = 0
    for dp, dn, fn in os.walk(root, followlinks=False):
        dn.sort()
        for n in sorted(fn):
            p = os.path.join(dp, n); s = os.lstat(p)
            if not stat.S_ISREG(s.st_mode) or s.st_nlink != 1: raise RuntimeError('not a plain single-link regular file: ' + p)
            inv.append({'path': os.path.relpath(p, root), 'size': s.st_size, 'allocated': s.st_blocks * 512, 'ino': s.st_ino, 'sha256': sha256_file(p)}); alloc += s.st_blocks * 512; size += s.st_size
    return inv, size, alloc

def tree_meta(root):
    """size/ino/mtime/ctime/mode/nlink of every entry below root (atime deliberately excluded: reads change it)."""
    out = {}
    for dp, dn, fn in os.walk(root, followlinks=False):
        for n in sorted(dn + fn):
            p = os.path.join(dp, n); s = os.lstat(p); out[os.path.relpath(p, root)] = [s.st_size, s.st_ino, s.st_mtime_ns, s.st_ctime_ns, s.st_mode, s.st_nlink]
    s = os.lstat(root); out['.'] = [s.st_size, s.st_ino, s.st_mtime_ns, s.st_ctime_ns, s.st_mode, s.st_nlink]
    return out

def preserved_inventory(vol, names):
    """content (sha256 of EVERY file) and metadata of the preserved entries."""
    content = {}; meta = {}
    for n in names:
        root = os.path.join(vol, n)
        if os.path.islink(root) or not os.path.isdir(root): raise RuntimeError('preserved entry is not a plain directory: ' + n)
        for dp, dn, fn in os.walk(root, followlinks=False):
            for f in sorted(fn):
                p = os.path.join(dp, f)
                if os.path.islink(p): raise RuntimeError('symlink inside a preserved entry: ' + p)
                content[os.path.relpath(p, vol)] = sha256_file(p)
        for k, v in tree_meta(root).items(): meta[n + ('' if k == '.' else '/' + k)] = v
    return content, meta

def lsof_open(path):
    """True if any process holds a file open under path. Fails closed if lsof cannot be used."""
    if not os.path.exists('/usr/sbin/lsof'): die('lsof not available (cannot prove that no process holds files open)')
    r = subprocess.run(['/usr/sbin/lsof', '-w', '+D', path], capture_output=True, text=True)
    if r.returncode not in (0, 1): die('lsof failed (%d): %s' % (r.returncode, r.stderr.strip()))
    return bool(r.stdout.strip())

def main():
    a = sys.argv[1:]
    if len(a) < 2 or '--expected' not in a: die('usage: recover_halted_churn_ring.py <run_dir> <volume> --expected <json> (--plan-only | --delete --approval <file>) [--allow-test-hooks]')
    plan_only = '--plan-only' in a; delete = '--delete' in a
    if plan_only == delete: die('exactly one of --plan-only / --delete is required (there is no default mode)')
    if plan_only and '--approval' in a: die('mode-inappropriate: --plan-only needs no approval and an approval file must not be passed with it')
    if delete and '--approval' not in a: die('--delete requires --approval <deletion approval file>')
    d = os.path.abspath(a[0]); vol = os.path.realpath(a[1]); exp_path = a[a.index('--expected') + 1]; exp = json.load(open(exp_path)); rid = exp['runid']
    hooks = '--allow-test-hooks' in a and vol != os.path.realpath('/Volumes/TPTestVol')
    # ---- identity: run directory, hash lock, evidence files, halt signature ------------------------------------------------------------------------------------------------
    if os.path.basename(d) != exp['run_dir_name']: die('run directory name differs from the expected identity')
    for name, h in exp['evidence_sha256'].items():
        if not os.path.exists(os.path.join(d, name)) or sha256_file(os.path.join(d, name)) != h: die('evidence file differs from the expected identity: ' + name)
    if open(os.path.join(d, 'finished.txt')).read().strip() != exp['finished_txt']: die('finished.txt differs from the expected halt signature')
    mi = json.load(open(os.path.join(d, 'manifest_intended.json'))); mf = json.load(open(os.path.join(d, 'manifest_final.json')))
    if mi['runid'] != rid or mf['runid'] != rid: die('manifest run id differs from the expected identity')
    if os.path.realpath(mi['volume']) != vol: die('volume differs from the manifest volume')
    plan_lock = exp['evidence_sha256']['PLAN_LOCK.txt']
    expected_names = ['diagchurn_%s_slot%d' % (rid, j) for j in range(10)]; target_paths = [os.path.join(vol, n) for n in expected_names]
    # ---- deletion approval (deletion mode only) ----------------------------------------------------------------------------------------------------------------------------
    appr = None
    if delete:
        ap = a[a.index('--approval') + 1]
        if not os.path.exists(ap): die('approval file missing: ' + ap)
        try: appr = json.load(open(ap))
        except Exception as e: die('approval file malformed (not JSON): %s' % e)
        if not isinstance(appr, dict) or appr.get('type') != APPROVAL_TYPE: die('approval file is not of type ' + APPROVAL_TYPE + ' (a plan-only or other file can never authorize deletion)')
        if set(appr) - APPROVAL_KEYS - {'note'} or APPROVAL_KEYS - set(appr): die('approval file has missing or unexpected keys')
        if appr['runid'] != rid or appr['plan_lock_sha256'] != plan_lock: die('approval names a different run id or hash lock')
        if not isinstance(appr['target_paths'], list) or [os.path.realpath(p) for p in appr['target_paths']] != [os.path.realpath(p) for p in target_paths]: die('approval does not name exactly the ten expected target paths in order')
        if appr['script_sha256'] != sha256_file(os.path.abspath(__file__)): die('approval was issued for a different version of this script (stale)')
        if appr['expected_json_sha256'] != sha256_file(exp_path): die('approval was issued for a different expected-identity file (stale)')
        try: exp_dt = datetime.datetime.strptime(appr['expires_utc'], '%Y-%m-%dT%H:%M:%SZ').replace(tzinfo=datetime.timezone.utc)
        except Exception: die('approval expiry malformed')
        if exp_dt < datetime.datetime.now(datetime.timezone.utc): die('approval expired')
        pdir = os.path.join(d, os.path.basename(appr['plan_only_evidence_dir']))
        if not os.path.isfile(os.path.join(pdir, 'SHA256SUMS')) or sha256_file(os.path.join(pdir, 'SHA256SUMS')) != appr['plan_only_sha256sums_sha256']: die('plan-only evidence missing or changed since the approval (stale)')
        planned = {}
        for l in open(os.path.join(pdir, 'SHA256SUMS')):
            h, n = l.split()
            if sha256_file(os.path.join(pdir, n)) != h: die('plan-only evidence file changed: ' + n)
            planned[n] = h
    # ---- single instance + no harness process ------------------------------------------------------------------------------------------------------------------------
    if subprocess.run(['pgrep', '-x', 'diag_churn_f'], capture_output=True).stdout.strip(): die('the harness process diag_churn_f is running')
    lockf = open(os.path.join(d, '.recovery.lock'), 'w')
    try: fcntl.flock(lockf, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError: die('another recovery process holds the lock')
    # ---- device / ownership / structure ------------------------------------------------------------------------------------------------------------------------------
    if os.stat(d).st_dev == os.stat(vol).st_dev: die('evidence directory is on the test volume')
    stamp = time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())
    ev = os.path.join(d, ('recovery_evidence_planonly_' if plan_only else 'recovery_evidence_') + stamp)
    if os.path.exists(ev): die(os.path.basename(ev) + ' already exists (never overwritten)')
    rows = [json.loads(l) for l in open(os.path.join(d, 'transfers.jsonl')) if l.strip()]; last_inode = {}
    for r in sorted(rows, key=lambda r: r['k']): last_inode[r['slot']] = r['slot_inode_new']
    intended = [os.path.realpath(p) for p in mi['intended_slot_dirs']]; forbidden = re.compile(r'^(overhead_|diagc1b)'); vol_dev = os.stat(vol).st_dev
    entries = sorted(os.listdir(vol)); ours_names = [n for n in entries if n.startswith('diagchurn_') or n.startswith('.recovery_quarantine_')]
    if ours_names != sorted(expected_names): die('volume diagchurn_*/quarantine entries differ from exactly the ten expected ring directories: %s' % ours_names)
    targets = []
    for j in range(10):
        p = target_paths[j]; s = mf['slots'][j]
        if s['slot'] != j or s['path'] != p or not s['exists']: die('manifest_final slot entry differs: %d' % j)
        if forbidden.match(expected_names[j]) or os.path.islink(p) or not os.path.isdir(p): die('ownership uncertain (name/symlink/type): ' + p)
        st = os.lstat(p)
        if st.st_dev != vol_dev or os.path.dirname(os.path.realpath(p)) != vol or os.path.realpath(p) not in intended: die('device/location/manifest mismatch: ' + p)
        if st.st_ino != s['inode'] or st.st_ino != last_inode.get(j): die('inode differs from the one recorded by the harness: ' + p)
        top = os.listdir(p)
        if len(top) != 1 or not re.fullmatch(r'[0-9a-f]{40}', top[0]) or not os.path.isdir(os.path.join(p, top[0])) or os.path.islink(os.path.join(p, top[0])): die('unexpected slot structure (expect exactly one 40-hex store directory): ' + p)
        for n in os.listdir(os.path.join(p, top[0])):
            if not re.fullmatch(r'piece_\d+\.seg', n): die('unexpected entry in store directory: %s/%s' % (p, n))
        targets.append((j, p))
    preserved = [n for n in entries if forbidden.match(n)]
    if len(preserved) != 11 or sorted(preserved) != sorted(exp['preserved_entries']): die('preserved entries differ from the expected 11: %s' % preserved)
    # ---- preserved entries: content (sha256 of every file) + metadata, BEFORE ---------------------------------------------------------------------------------------------
    try: pres_c0, pres_m0 = preserved_inventory(vol, preserved)
    except RuntimeError as e: die(str(e))
    pres_hash0 = canon_hash(pres_c0)
    if delete and pres_hash0 != appr['preserved_content_inventory_sha256']: die('preserved-entry content inventory differs from the approved one (%s != approved)' % pres_hash0)
    free_before = shutil.disk_usage(vol).free
    # ---- ring inventory + evidence OUTSIDE the volume ------------------------------------------------------------------------------------------------------------------
    os.makedirs(ev); plan = []; sums = []
    try:
        for j, p in targets:
            inv, size, alloc = inventory(p)
            if not (1 <= len(inv) <= 480 and 0 < alloc <= (128 << 20)): die('inventory out of bounds for slot %d: %d files, %d bytes' % (j, len(inv), alloc))
            fn = os.path.join(ev, 'inventory_slot%d.json' % j); json.dump({'path': p, 'inode': os.lstat(p).st_ino, 'files': inv}, open(fn, 'w'), indent=0)
            sums.append((sha256_file(fn), os.path.basename(fn))); plan.append({'slot': j, 'path': p, 'files': len(inv), 'apparent_bytes': size, 'allocated_bytes': alloc, 'inode': os.lstat(p).st_ino})
            if delete and planned.get(os.path.basename(fn)) != sums[-1][0]: die('ring slot %d differs from the approved plan-only inventory (stale approval or changed contents)' % j)
    except RuntimeError as e: die(str(e))
    json.dump({'runid': rid, 'mode': 'plan-only' if plan_only else 'delete', 'planned_deletions': plan, 'preserved_entries': preserved, 'preserved_files_hashed': len(pres_c0), 'preserved_content_inventory_sha256': pres_hash0, 'free_bytes_before': free_before, 'time': time.time()}, open(os.path.join(ev, 'recovery_plan.json'), 'w'), indent=1)
    json.dump({'sha256': pres_c0, 'meta': pres_m0}, open(os.path.join(ev, 'preserved_inventory_before.json'), 'w'))
    for nm in ('recovery_plan.json', 'preserved_inventory_before.json'): sums.append((sha256_file(os.path.join(ev, nm)), nm))
    with open(os.path.join(ev, 'SHA256SUMS'), 'w') as f:
        for h, n in sums: f.write('%s  %s\n' % (h, n))
    for l in open(os.path.join(ev, 'SHA256SUMS')):
        h, n = l.split()
        if sha256_file(os.path.join(ev, n)) != h: die('saved evidence checksum mismatch: ' + n)
    meta_verify = {}
    for j, p in targets:
        inv = json.load(open(os.path.join(ev, 'inventory_slot%d.json' % j)))['files']; now = {}
        for dp, dn, fn in os.walk(p, followlinks=False):
            for n in fn: now[os.path.relpath(os.path.join(dp, n), p)] = os.path.join(dp, n)
        if set(now) != {e['path'] for e in inv}: die('volume file set differs from the saved inventory in slot %d' % j)
        for e in inv:
            s = os.lstat(now[e['path']])
            if s.st_size != e['size'] or s.st_ino != e['ino'] or sha256_file(now[e['path']]) != e['sha256']: die('volume file differs from the saved inventory: ' + now[e['path']])
        meta_verify[j] = tree_meta(p)
    print('evidence saved to %s and verified: %d directories, %d files, %d bytes; preserved content inventory: %d files hashed (sha256 %s)' % (ev, len(plan), sum(x['files'] for x in plan), sum(x['allocated_bytes'] for x in plan), len(pres_c0), pres_hash0))
    if plan_only: print('PLAN ONLY: nothing deleted, nothing renamed, nothing written to the volume'); sys.exit(0)
    # ---- deletion, one target at a time ---------------------------------------------------------------------------------------------------------------------------------
    for j, p in targets:                                                         # pre-pass over ALL targets: nothing is deleted or renamed if any process holds a file open anywhere in the ring
        if lsof_open(p): die('a process holds files open in ' + p)
    print('deleting exactly these directories:'); [print('  ', x['path'], x['files'], 'files', x['allocated_bytes'], 'bytes') for x in plan]
    states = {j: {'path': p, 'state': 'not_attempted (untouched at its original path)'} for j, p in targets}
    def hook(tag, j):
        if not hooks: return
        sd = os.environ.get('TP_RECOVERY_TEST_SYNC_DIR')
        if not sd or not os.path.exists(os.path.join(sd, 'arm_%s_%d' % (tag, j))): return          # a hook acts only where the test armed it
        open(os.path.join(sd, 'at_%s_%d' % (tag, j)), 'w').write('x'); t0 = time.time()
        while not os.path.exists(os.path.join(sd, 'go_%s_%d' % (tag, j))) and time.time() - t0 < 30: time.sleep(0.05)
    def partial_exit(msg):
        json.dump({'reason': msg, 'targets': states, 'evidence_dir': ev, 'time': time.time()}, open(os.path.join(ev, 'recovery_outcome_PARTIAL.json'), 'w'), indent=1)
        print('DELETION STOPPED (saved evidence intact):', msg); print(json.dumps(states, indent=1)); sys.exit(8)
    for j, p in targets:
        ino = plan[j]['inode']; q = os.path.join(vol, '.recovery_quarantine_%s_slot%d_%d' % (rid, j, os.getpid()))
        hook('pre', j)
        try:
            if lsof_open(p): states[j]['state'] = 'not_attempted: a process holds files open there'; partial_exit('open files in ' + p)
            st = os.lstat(p)
            if os.path.islink(p) or not os.path.isdir(p) or st.st_ino != ino or st.st_dev != vol_dev or tree_meta(p) != meta_verify[j]: states[j]['state'] = 'not_attempted: target changed since verification (identity/metadata)'; partial_exit('target changed since verification: ' + p)
            hook('post_validate', j)
            os.rename(p, q); states[j]['state'] = 'renamed_to_quarantine ' + q
            if os.path.islink(q) or os.lstat(q).st_ino != ino:
                os.rename(q, p); states[j]['state'] = 'quarantine identity mismatch; renamed back to the original path'; partial_exit('the directory renamed for deletion was not the verified one (replaced between validation and rename): ' + p)
            shutil.rmtree(q); states[j] = {'path': p, 'state': 'removed', 'exists_after': os.path.exists(p) or os.path.exists(q)}
            if states[j]['exists_after']: partial_exit('still exists after rmtree: ' + p)
        except SystemExit: raise
        except Exception as e:
            rem = []
            try: rem = sorted(os.listdir(q))[:20] if os.path.exists(q) else []
            except Exception: pass
            states[j]['state'] = 'partially_removed_or_failed: %s (quarantine path %s, top-level entries remaining %s)' % (e, q, rem); partial_exit('deletion error: %s' % e)
    # ---- post-verification of the preserved entries (content + metadata) ----------------------------------------------------------------------------------------------------
    pres_c1, pres_m1 = preserved_inventory(vol, preserved); same_c = pres_c1 == pres_c0; same_m = pres_m1 == pres_m0
    json.dump({'sha256': pres_c1, 'meta': pres_m1}, open(os.path.join(ev, 'preserved_inventory_after.json'), 'w'))
    remaining = sorted(n for n in os.listdir(vol) if n.startswith('diagchurn_') or n.startswith('.recovery_quarantine_'))
    res = {'targets': states, 'diagchurn_or_quarantine_remaining': remaining, 'preserved_content_identical': same_c, 'preserved_metadata_identical_except_atime': same_m, 'preserved_files_hashed': len(pres_c1), 'free_bytes_before': free_before, 'free_bytes_after': shutil.disk_usage(vol).free, 'time': time.time()}
    json.dump(res, open(os.path.join(ev, 'recovery_outcome.json'), 'w'), indent=1)
    with open(os.path.join(ev, 'SHA256SUMS_outcome'), 'w') as f:
        for nm in ('recovery_outcome.json', 'preserved_inventory_after.json'): f.write('%s  %s\n' % (sha256_file(os.path.join(ev, nm)), nm))
    print('recovery outcome:', json.dumps({k: res[k] for k in ('diagchurn_or_quarantine_remaining', 'preserved_content_identical', 'preserved_metadata_identical_except_atime')}), 'free before/after GiB: %.3f / %.3f' % (free_before / 2**30, res['free_bytes_after'] / 2**30))
    sys.exit(0 if all(s['state'] == 'removed' for s in states.values()) and same_c and same_m and not remaining else 8)

main()
