#!/usr/bin/env python3
"""Evidence-preserving cleanup of the ring directories created by ONE diag_churn_f run (approved procedure, 2026-10-17).
usage: python3 -I cleanup_churn_ring.py <run_dir> <volume>
Deletes ONLY the directories enumerated in the run's manifest_intended.json whose ownership is proven (exact name diagchurn_<runid>_slot<0-9>, direct child of the volume,
not a symlink, inode equal to the inode the harness recorded when it created it, listed in manifest_final.json). Before deleting it saves a full inventory (relative path, size,
allocated bytes, sha256 of every file) OUTSIDE the volume, verifies the saved files are readable and match their recorded checksums, re-verifies the volume files against the inventory,
and records the exact paths and sizes. If anything is uncertain it deletes NOTHING and exits non-zero. Never touches overhead_* , diagc1b_calib_off or any other entry."""
import hashlib, json, os, re, shutil, stat, sys, time

def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''): h.update(c)
    return h.hexdigest()

def walk_inventory(root):
    inv = []; total = 0; alloc = 0
    for dp, dn, fn in os.walk(root):
        dn.sort()
        for n in sorted(fn):
            p = os.path.join(dp, n); s = os.lstat(p)
            if not stat.S_ISREG(s.st_mode): raise RuntimeError('non-regular file in ring: ' + p)
            inv.append({'path': os.path.relpath(p, root), 'size': s.st_size, 'allocated': s.st_blocks * 512, 'sha256': sha256_file(p)}); total += s.st_size; alloc += s.st_blocks * 512
    return inv, total, alloc

def tree_bytes(root):
    t = 0
    for dp, dn, fn in os.walk(root):
        for n in fn:
            try: t += os.lstat(os.path.join(dp, n)).st_blocks * 512
            except OSError: pass
    return t

def other_entries(vol, ours):
    out = {}
    for n in sorted(os.listdir(vol)):
        if n in ours: continue
        p = os.path.join(vol, n); out[n] = tree_bytes(p) if os.path.isdir(p) and not os.path.islink(p) else os.lstat(p).st_size
    return out

def die(msg, code=7):
    print('CLEANUP ABORTED, NOTHING DELETED: ' + msg); sys.exit(code)

def main():
    d, vol = os.path.abspath(sys.argv[1]), os.path.realpath(sys.argv[2])
    if os.stat(d).st_dev == os.stat(vol).st_dev: die('evidence directory is on the test volume')
    mi = json.load(open(os.path.join(d, 'manifest_intended.json'))); mf = json.load(open(os.path.join(d, 'manifest_final.json')))
    fin = open(os.path.join(d, 'finished.txt')).read()
    if 'complete' not in fin or 'HALTED' in fin: die('run did not finish normally: ' + fin.strip())
    rid = mi['runid']; intended = [os.path.realpath(p) for p in mi['intended_slot_dirs']]
    rows = [json.loads(l) for l in open(os.path.join(d, 'transfers.jsonl')) if l.strip()]
    last_inode = {}
    for r in sorted(rows, key=lambda r: r['k']): last_inode[r['slot']] = r['slot_inode_new']
    forbidden = re.compile(r'^(overhead_|diagc1b)')
    targets = []
    for s in mf['slots']:
        if not s['exists']: continue
        p = s['path']; real = os.path.realpath(p); name = os.path.basename(p); j = s['slot']
        if forbidden.match(name): die('forbidden name ' + name)
        if not re.fullmatch(r'diagchurn_' + re.escape(rid) + r'_slot[0-9]', name): die('name does not match run: ' + name)
        if os.path.dirname(real) != vol or real not in intended or os.path.islink(p) or not os.path.isdir(p): die('ownership uncertain (location/symlink/manifest): ' + p)
        ino = os.stat(p).st_ino
        if ino != s['inode'] or ino != last_inode.get(j): die('inode differs from the one the harness created (%d vs %d/%s): %s' % (ino, s['inode'], last_inode.get(j), p))
        targets.append((j, p))
    unlisted = [n for n in os.listdir(vol) if n.startswith('diagchurn_' + rid + '_') and os.path.realpath(os.path.join(vol, n)) not in {os.path.realpath(t[1]) for t in targets}]
    if unlisted: die('volume has run-prefixed entries not listed in the final manifest: ' + str(unlisted))
    if not targets: die('no ring directories to clean (nothing exists)', 0)
    ev = os.path.join(d, 'ring_evidence'); os.makedirs(ev, exist_ok=True)
    ours = {os.path.basename(p) for _, p in targets}
    before_other = other_entries(vol, ours); free_before = shutil.disk_usage(vol).free
    plan = []; sums = []
    for j, p in targets:
        inv, tot, alloc = walk_inventory(p)
        fn = os.path.join(ev, 'inventory_slot%d.json' % j); json.dump({'path': p, 'files': inv}, open(fn, 'w'), indent=0)
        sums.append((sha256_file(fn), os.path.basename(fn)))
        plan.append({'slot': j, 'path': p, 'files': len(inv), 'bytes': tot, 'allocated_bytes': alloc, 'inode': os.stat(p).st_ino})
    json.dump({'planned_deletions': plan, 'other_entries_before': before_other, 'free_bytes_before': free_before, 'time': time.time()}, open(os.path.join(ev, 'cleanup_plan.json'), 'w'), indent=1)
    sums.append((sha256_file(os.path.join(ev, 'cleanup_plan.json')), 'cleanup_plan.json'))
    with open(os.path.join(ev, 'SHA256SUMS'), 'w') as f:
        for h, n in sums: f.write('%s  %s\n' % (h, n))
    # ---- verification of the saved evidence (re-read from disk; checksums must match) --------------------------------------------------------------------------------
    for l in open(os.path.join(ev, 'SHA256SUMS')):
        h, n = l.split(); 
        if sha256_file(os.path.join(ev, n)) != h: die('evidence checksum mismatch: ' + n)
    for j, p in targets:
        inv = json.load(open(os.path.join(ev, 'inventory_slot%d.json' % j)))['files']
        now = {}; 
        for dp, dn, fn in os.walk(p):
            for n in fn: now[os.path.relpath(os.path.join(dp, n), p)] = os.path.join(dp, n)
        if set(now) != {e['path'] for e in inv}: die('volume contents differ from saved inventory (file set) in slot %d' % j)
        for e in inv:
            if os.lstat(now[e['path']]).st_size != e['size'] or sha256_file(now[e['path']]) != e['sha256']: die('volume file differs from saved inventory: ' + now[e['path']])
    print('evidence saved and verified (%d directories, %d files); deleting exactly these directories:' % (len(plan), sum(x['files'] for x in plan)))
    for x in plan: print('  ', x['path'], x['files'], 'files', x['bytes'], 'bytes')
    # ---- deletion of the enumerated run-owned directories only -------------------------------------------------------------------------------------------------------
    outcome = []
    for j, p in targets:
        shutil.rmtree(p); outcome.append({'path': p, 'exists_after': os.path.exists(p)})
    after_other = other_entries(vol, set()); after_other = {k: v for k, v in after_other.items() if k not in ours}
    res = {'outcome': outcome, 'other_entries_unchanged': after_other == before_other, 'other_entries_after': after_other, 'free_bytes_after': shutil.disk_usage(vol).free, 'free_bytes_before': free_before, 'time': time.time()}
    json.dump(res, open(os.path.join(ev, 'cleanup_outcome.json'), 'w'), indent=1)
    print('cleanup outcome:', json.dumps({'all_removed': all(not o['exists_after'] for o in outcome), 'other_entries_unchanged': res['other_entries_unchanged'], 'free_before_GiB': round(free_before / 2**30, 3), 'free_after_GiB': round(res['free_bytes_after'] / 2**30, 3)}))
    sys.exit(0 if all(not o['exists_after'] for o in outcome) and res['other_entries_unchanged'] else 8)

main()
