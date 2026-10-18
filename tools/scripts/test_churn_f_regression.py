#!/usr/bin/env python3
"""Regression tests for the CHURN-F fix package (2026-10-17): analyzer fail-closed behaviour and the halted-run recovery script. Pure standard library.
Recovery-script tests run on a throw-away scratch disk image (hdiutil), NEVER on the test volume. usage: python3 -I test_churn_f_regression.py"""
import hashlib, json, os, shutil, subprocess, sys, tempfile, time, unittest
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
import importlib.util
spec = importlib.util.spec_from_file_location('an', os.path.join(HERE, 'analyze_g3_churn.py')); an = importlib.util.module_from_spec(spec); spec.loader.exec_module(an)
REC = os.path.join(HERE, 'recover_halted_churn_ring.py')

def sha(p): return hashlib.sha256(open(p, 'rb').read()).hexdigest()

class AnalyzerFailClosed(unittest.TestCase):
    def rows(self, n=3): return an.synth_rows(n)
    def test_consistent_passes(self): self.assertEqual(an.checks(self.rows())['ring_and_state_failures'], [])
    def test_zero_retained_fails(self):
        r = self.rows(); r[0]['retained_files'] = 0; r[0]['retained_bytes'] = 0
        self.assertTrue(any('retained contents zero' in f for f in an.checks(r)['ring_and_state_failures']))
    def test_missing_measurement_fails(self):
        r = self.rows(); del r[2]['retained_files']
        self.assertTrue(any('missing required fields' in f for f in an.checks(r)['ring_and_state_failures']))
    def test_none_measurement_fails(self):
        r = self.rows(); r[1]['retained_bytes'] = None
        self.assertTrue(any('missing required fields' in f for f in an.checks(r)['ring_and_state_failures']))
    def test_method_disagreement_fails(self):
        r = self.rows(); r[3]['retained_files_indep'] = 459
        self.assertTrue(any('disagree' in f for f in an.checks(r)['ring_and_state_failures']))
    def test_incorrect_count_vs_previous_use_fails(self):
        r = self.rows(); r[10]['slot_found_files'] = 455; r[10]['slot_found_files_indep'] = 455     # wrap: found directory differs from what transfer 0 left behind
        self.assertTrue(any('differs from what transfer' in f for f in an.checks(r)['ring_and_state_failures']))
    def test_wrap_empty_found_fails(self):
        r = self.rows(); r[11]['slot_found'] = False; r[11]['slot_found_files'] = 0
        self.assertTrue(any('missing/empty' in f for f in an.checks(r)['ring_and_state_failures']))
    def test_no_rows_fails(self): self.assertTrue(an.checks([])['ring_and_state_failures'])
    def test_out_of_bounds_fails(self):
        r = self.rows(); r[0]['retained_files'] = r[0]['retained_files_indep'] = 481
        self.assertTrue(any('out of bounds' in f for f in an.checks(r)['ring_and_state_failures']))

class AnalyzerManifestsAndWalk(unittest.TestCase):
    def setUp(self):
        self.t = tempfile.mkdtemp(); self.vol = os.path.join(self.t, 'vol'); os.makedirs(self.vol); self.d = os.path.join(self.t, 'run'); os.makedirs(self.d)
        self.rows = an.synth_rows(3); self.paths = []
        for j in range(10):                                                   # nested layout identical to the real store: <slot>/<40hex>/piece_N.seg
            p = os.path.join(self.vol, 'diagchurn_T_slot%d' % j); st = os.path.join(p, 'a' * 40); os.makedirs(st); self.paths.append(p)
            for i in range(5): open(os.path.join(st, 'piece_%d.seg' % i), 'wb').write(b'x' * 4096)
        files, alloc = an.walk_stats(self.paths[0]); self.fa = (files, alloc)
        # make the synthetic rows consistent with the fixture (every transfer left 5 files)
        for r in self.rows: r['retained_files'] = r['retained_files_indep'] = files; r['retained_bytes'] = r['retained_bytes_indep'] = alloc; r['slot_found_files'] = r['slot_found_files_indep'] = files if r['k'] >= 10 else 0; r['slot_found_bytes'] = r['slot_found_bytes_indep'] = alloc if r['k'] >= 10 else 0
        self.write()
    def write(self, final_override=None, drop=None):
        json.dump({'runid': 'T', 'intended_slot_dirs': self.paths}, open(os.path.join(self.d, 'manifest_intended.json'), 'w'))
        slots = []
        last = {}
        for r in self.rows: last[r['slot']] = r
        for j in range(10):
            lr = last[j]; s = {'slot': j, 'path': self.paths[j], 'exists': True, 'last_transfer': lr['k'], 'inode': lr['slot_inode_new'], 'files': self.fa[0], 'dirs': 1, 'non_regular': 0, 'allocated_bytes': self.fa[1], 'apparent_bytes': 0, 'files_indep': self.fa[0], 'allocated_bytes_indep': self.fa[1], 'methods_agree': True}
            slots.append(s)
        if final_override: final_override(slots)
        json.dump({'runid': 'T', 'slots': slots}, open(os.path.join(self.d, 'manifest_final.json'), 'w'))
        open(os.path.join(self.d, 'finished.txt'), 'w').write('transfers_run 12 of 12 invalid 0 gap_deviations 0 complete\n')
        if drop: os.remove(os.path.join(self.d, drop))
    def tearDown(self): shutil.rmtree(self.t, ignore_errors=True)
    def test_nested_walk_counts_files(self): self.assertEqual(self.fa[0], 5)
    def test_old_nonrecursive_style_zero_is_rejected(self):
        self.write(final_override=lambda s: [x.update(files=0, allocated_bytes=0) for x in s])
        f = an.checks(self.rows, self.d)['ring_and_state_failures']; self.assertTrue(any('zero/inconsistent' in x for x in f)); self.assertTrue(any('independent python walk' in x for x in f))
    def test_walk_mismatch_detected(self):
        open(os.path.join(self.paths[3], 'a' * 40, 'extra.seg'), 'wb').write(b'y' * 10)
        self.assertTrue(any('independent python walk' in x for x in an.checks(self.rows, self.d)['ring_and_state_failures']))
    def test_missing_manifest_fails(self):
        self.write(drop='manifest_final.json'); self.assertTrue(any('manifest_final.json' in x for x in an.checks(self.rows, self.d)['ring_and_state_failures']))
        self.write(drop='manifest_intended.json'); self.assertTrue(any('manifest_intended.json' in x for x in an.checks(self.rows, self.d)['ring_and_state_failures']))
    def test_all_consistent_passes(self): self.assertEqual(an.checks(self.rows, self.d)['ring_and_state_failures'], [])
    def test_halted_run_fails(self):
        self.write(); open(os.path.join(self.d, 'finished.txt'), 'w').write('transfers_run 10 of 12 HALTED x\n')
        self.assertTrue(any('did not finish normally' in x for x in an.checks(self.rows, self.d)['ring_and_state_failures']))
    def test_main_exit_code_nonzero_on_failure(self):
        self.write(drop='manifest_final.json')
        with open(os.path.join(self.d, 'transfers.jsonl'), 'w') as f:
            for r in self.rows: f.write(json.dumps(r) + '\n')
        p = subprocess.run([sys.executable, '-I', os.path.join(HERE, 'analyze_g3_churn.py'), self.d], capture_output=True, text=True)
        self.assertEqual(p.returncode, 1); self.assertIn('FAIL-CLOSED', p.stdout)

@unittest.skipUnless(shutil.which('hdiutil'), 'hdiutil needed for the scratch-image recovery tests')
class RecoveryScript(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = tempfile.mkdtemp(); cls.img = os.path.join(cls.t, 's.sparseimage'); cls.mnt = os.path.join(cls.t, 'mnt'); os.makedirs(cls.mnt)
        subprocess.run(['hdiutil', 'create', '-size', '128m', '-fs', 'APFS', '-volname', 'RCTEST', '-type', 'SPARSE', cls.img], capture_output=True, check=True)
        subprocess.run(['hdiutil', 'attach', cls.img, '-mountpoint', cls.mnt, '-nobrowse'], capture_output=True, check=True); cls.vol = os.path.realpath(cls.mnt)
    @classmethod
    def tearDownClass(cls):
        subprocess.run(['chflags', '-R', 'nouchg', cls.mnt], capture_output=True); subprocess.run(['hdiutil', 'detach', cls.mnt], capture_output=True); shutil.rmtree(cls.t, ignore_errors=True)
    def setUp(self):
        subprocess.run(['chflags', '-R', 'nouchg', self.vol], capture_output=True)
        for n in os.listdir(self.vol):
            p = os.path.join(self.vol, n); shutil.rmtree(p) if os.path.isdir(p) and not os.path.islink(p) else os.remove(p)
        rid = 'SMOKE_X_1'; self.rid = rid; self.run = os.path.join(self.t, 'G3DIAG_' + rid); shutil.rmtree(self.run, ignore_errors=True); os.makedirs(self.run)
        names = ['overhead_%d_%s' % (i, k) for i in range(5) for k in ('aud', 'noaud')] + ['diagc1b_calib_off']
        for n in names: os.makedirs(os.path.join(self.vol, n, 'sub')); open(os.path.join(self.vol, n, 'sub', 'f'), 'wb').write(n.encode() * 50)
        self.slots = []; rows = []
        for j in range(10):
            p = os.path.join(self.vol, 'diagchurn_%s_slot%d' % (rid, j)); st = os.path.join(p, ('%040x' % (j + 1))); os.makedirs(st)
            for i in range(6): open(os.path.join(st, 'piece_%d.seg' % i), 'wb').write(os.urandom(5000))
            ino = os.stat(p).st_ino; self.slots.append({'slot': j, 'path': p, 'exists': True, 'last_transfer': j, 'inode': ino, 'files': 0, 'allocated_bytes': 0}); rows.append({'k': j, 'slot': j, 'slot_inode_new': ino})
        json.dump({'runid': rid, 'volume': self.vol, 'intended_slot_dirs': [s['path'] for s in self.slots]}, open(os.path.join(self.run, 'manifest_intended.json'), 'w'))
        json.dump({'runid': rid, 'slots': self.slots}, open(os.path.join(self.run, 'manifest_final.json'), 'w'))
        open(os.path.join(self.run, 'transfers.jsonl'), 'w').write('\n'.join(json.dumps(r) for r in rows) + '\n')
        open(os.path.join(self.run, 'finished.txt'), 'w').write('transfers_run 10 of 12 invalid 0 gap_deviations 0 HALTED sig\n')
        for n in ('PLAN_LOCK.txt', 'console.txt', 'order_sequence.txt'): open(os.path.join(self.run, n), 'w').write(n + '\n')
        self.exp = os.path.join(self.t, 'expected.json'); self.names = names; self.mkexp(); self.sync = None
    def mkexp(self):
        json.dump({'runid': self.rid, 'run_dir_name': os.path.basename(self.run), 'evidence_sha256': {n: sha(os.path.join(self.run, n)) for n in ['PLAN_LOCK.txt', 'transfers.jsonl', 'manifest_final.json', 'manifest_intended.json', 'finished.txt', 'console.txt', 'order_sequence.txt']},
                   'finished_txt': open(os.path.join(self.run, 'finished.txt')).read().strip(), 'preserved_entries': sorted(self.names)}, open(self.exp, 'w'))
    def rec(self, *extra, env=None):
        return subprocess.run([sys.executable, '-I', REC, self.run, self.vol, '--expected', self.exp, *extra], capture_output=True, text=True, env=env)
    def plan(self):
        p = self.rec('--plan-only'); self.assertEqual(p.returncode, 0, p.stdout + p.stderr); time.sleep(1.1)
        return sorted(g for g in os.listdir(self.run) if g.startswith('recovery_evidence_planonly_'))[-1]
    def approval(self, **over):
        pdir = self.plan(); plan = json.load(open(os.path.join(self.run, pdir, 'recovery_plan.json')))
        a = {'type': 'RECOVERY_DELETION_APPROVAL', 'runid': self.rid, 'plan_lock_sha256': sha(os.path.join(self.run, 'PLAN_LOCK.txt')), 'target_paths': [s['path'] for s in self.slots], 'plan_only_evidence_dir': pdir,
             'plan_only_sha256sums_sha256': sha(os.path.join(self.run, pdir, 'SHA256SUMS')), 'preserved_content_inventory_sha256': plan['preserved_content_inventory_sha256'], 'script_sha256': sha(REC), 'expected_json_sha256': sha(self.exp),
             'expires_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime(time.time() + 3600))}
        a.update(over); f = os.path.join(self.t, 'deletion_approval.json'); json.dump({k: v for k, v in a.items() if v is not None}, open(f, 'w')); return f
    def ring(self): return sorted(n for n in os.listdir(self.vol) if n.startswith('diagchurn_') or n.startswith('.recovery_quarantine_'))
    def vol_snapshot(self): return sorted(os.listdir(self.vol))
    def assertAborted(self, p, ring=10):
        self.assertEqual(p.returncode, 7, p.stdout + p.stderr); self.assertIn('NOTHING DELETED', p.stdout); self.assertEqual(len(self.ring()), ring)
        for n in self.names: self.assertTrue(os.path.isdir(os.path.join(self.vol, n)))
    # ---- plan-only / deletion separation ---------------------------------------------------------------------------------------------------------------------------------
    def test_plan_only_needs_no_approval_and_changes_nothing(self):
        before = self.vol_snapshot(); p = self.rec('--plan-only'); self.assertEqual(p.returncode, 0, p.stdout + p.stderr); self.assertEqual(self.vol_snapshot(), before); self.assertEqual(len(self.ring()), 10)
        self.assertIn('nothing written to the volume', p.stdout); self.assertTrue([g for g in os.listdir(self.run) if g.startswith('recovery_evidence_planonly_')])
    def test_plan_only_rejects_an_approval_file(self): self.assertAborted(self.rec('--plan-only', '--approval', self.approval()))
    def test_no_mode_rejected(self): self.assertAborted(self.rec())
    def test_both_modes_rejected(self): self.assertAborted(self.rec('--plan-only', '--delete', '--approval', self.approval()))
    def test_delete_without_approval_rejected(self): self.assertAborted(self.rec('--delete'))
    def test_delete_approval_missing_file(self): self.assertAborted(self.rec('--delete', '--approval', os.path.join(self.t, 'nope.json')))
    def test_old_style_text_approval_rejected(self):
        f = os.path.join(self.t, 'old.txt'); open(f, 'w').write('APPROVED %s %s\n' % (self.rid, sha(os.path.join(self.run, 'PLAN_LOCK.txt')))); self.assertAborted(self.rec('--delete', '--approval', f))
    def test_malformed_approval_rejected(self):
        f = os.path.join(self.t, 'bad.json'); open(f, 'w').write('{not json'); self.assertAborted(self.rec('--delete', '--approval', f))
    def test_wrong_type_rejected(self): self.assertAborted(self.rec('--delete', '--approval', self.approval(type='PLAN_ONLY_AUTHORISATION')))
    def test_missing_key_rejected(self): self.assertAborted(self.rec('--delete', '--approval', self.approval(preserved_content_inventory_sha256=None)))
    def test_extra_key_rejected(self): self.assertAborted(self.rec('--delete', '--approval', self.approval(surprise='x')))
    def test_expired_approval_rejected(self): self.assertAborted(self.rec('--delete', '--approval', self.approval(expires_utc='2020-01-01T00:00:00Z')))
    def test_malformed_expiry_rejected(self): self.assertAborted(self.rec('--delete', '--approval', self.approval(expires_utc='tomorrow')))
    def test_wrong_runid_rejected(self): self.assertAborted(self.rec('--delete', '--approval', self.approval(runid='SMOKE_OTHER')))
    def test_wrong_plan_lock_rejected(self): self.assertAborted(self.rec('--delete', '--approval', self.approval(plan_lock_sha256='0' * 64)))
    def test_target_paths_not_exactly_the_ten(self):
        ps = [s['path'] for s in self.slots]; self.assertAborted(self.rec('--delete', '--approval', self.approval(target_paths=ps[:9])))
        self.assertAborted(self.rec('--delete', '--approval', self.approval(target_paths=list(reversed(ps))))); self.assertAborted(self.rec('--delete', '--approval', self.approval(target_paths=ps[:9] + [os.path.join(self.vol, 'overhead_0_aud')])))
    def test_stale_script_hash_rejected(self): self.assertAborted(self.rec('--delete', '--approval', self.approval(script_sha256='1' * 64)))
    def test_stale_expected_json_hash_rejected(self): self.assertAborted(self.rec('--delete', '--approval', self.approval(expected_json_sha256='2' * 64)))
    def test_plan_only_evidence_changed_since_approval(self):
        f = self.approval(); pdir = json.load(open(f))['plan_only_evidence_dir']; open(os.path.join(self.run, pdir, 'SHA256SUMS'), 'a').write('\n'); self.assertAborted(self.rec('--delete', '--approval', f))
    def test_plan_only_evidence_file_changed_since_approval(self):
        f = self.approval(); pdir = json.load(open(f))['plan_only_evidence_dir']; open(os.path.join(self.run, pdir, 'recovery_plan.json'), 'a').write(' '); self.assertAborted(self.rec('--delete', '--approval', f))
    def test_preserved_inventory_hash_mismatch(self): self.assertAborted(self.rec('--delete', '--approval', self.approval(preserved_content_inventory_sha256='3' * 64)))
    def test_preserved_file_contents_changed_same_size_and_mtime(self):
        f = self.approval(); p = os.path.join(self.vol, 'overhead_2_aud', 'sub', 'f'); st = os.stat(p); data = bytearray(open(p, 'rb').read()); data[0] ^= 0xFF
        open(p, 'wb').write(bytes(data)); os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns)); self.assertAborted(self.rec('--delete', '--approval', f))
    def test_ring_contents_changed_same_size_and_mtime_since_plan(self):
        f = self.approval(); p = os.path.join(self.slots[4]['path'], '%040x' % 5, 'piece_3.seg'); st = os.stat(p); data = bytearray(open(p, 'rb').read()); data[10] ^= 0xFF
        open(p, 'wb').write(bytes(data)); os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns)); self.assertAborted(self.rec('--delete', '--approval', f))
    # ---- ownership / structure (both modes share these checks) ---------------------------------------------------------------------------------------------------
    def test_evidence_hash_mismatch(self): open(os.path.join(self.run, 'console.txt'), 'a').write('tamper'); self.assertAborted(self.rec('--plan-only'))
    def test_wrong_halt_signature(self):
        open(os.path.join(self.run, 'finished.txt'), 'w').write('transfers_run 12 of 12 invalid 0 gap_deviations 0 complete\n'); self.assertAborted(self.rec('--plan-only'))
    def test_extra_diagchurn_directory(self): os.makedirs(os.path.join(self.vol, 'diagchurn_OTHER_slot0')); p = self.rec('--plan-only'); self.assertEqual(p.returncode, 7); self.assertEqual(len(self.ring()), 11)
    def test_symlink_slot(self):
        s = self.slots[2]['path']; os.rename(s, s + '_m'); os.symlink(s + '_m', s); p = self.rec('--plan-only'); self.assertEqual(p.returncode, 7); self.assertTrue(os.path.isdir(s + '_m'))
    def test_inode_mismatch(self):
        m = json.load(open(os.path.join(self.run, 'manifest_final.json'))); m['slots'][4]['inode'] += 1; json.dump(m, open(os.path.join(self.run, 'manifest_final.json'), 'w')); self.mkexp(); self.assertAborted(self.rec('--plan-only'))
    def test_unexpected_file_in_store(self): open(os.path.join(self.slots[1]['path'], '%040x' % 2, 'surprise.txt'), 'w').write('x'); self.assertAborted(self.rec('--plan-only'))
    def test_unexpected_second_top_entry(self): os.makedirs(os.path.join(self.slots[5]['path'], 'second')); self.assertAborted(self.rec('--plan-only'))
    def test_hardlinked_file(self): os.link(os.path.join(self.slots[0]['path'], '%040x' % 1, 'piece_0.seg'), os.path.join(self.vol, 'loose_hardlink')); self.assertAborted(self.rec('--plan-only'))
    def test_missing_preserved_entry(self): shutil.rmtree(os.path.join(self.vol, 'overhead_3_aud')); p = self.rec('--plan-only'); self.assertEqual(p.returncode, 7); self.assertEqual(len(self.ring()), 10)
    def test_process_holding_a_file_open_blocks_deletion(self):
        f = self.approval(); hold = subprocess.Popen([sys.executable, '-c', 'import sys,time; f=open(sys.argv[1],"rb"); print("ok",flush=True); time.sleep(60)', os.path.join(self.slots[7]['path'], '%040x' % 8, 'piece_1.seg')], stdout=subprocess.PIPE, text=True)
        try:
            hold.stdout.readline(); self.assertAborted(self.rec('--delete', '--approval', f))
        finally: hold.kill(); hold.wait()
    # ---- successful deletion: only the ring is removed; preserved entries identical in CONTENT and metadata ----------------------------------------------------------
    def test_execute_removes_only_the_ring_and_preserves_everything_else(self):
        f = self.approval(); before = {n: sorted((os.path.join(dp, x), sha(os.path.join(dp, x))) for dp, dn, fn in os.walk(os.path.join(self.vol, n)) for x in fn) for n in self.names}
        p = self.rec('--delete', '--approval', f); self.assertEqual(p.returncode, 0, p.stdout + p.stderr); self.assertEqual(self.ring(), [])
        after = {n: sorted((os.path.join(dp, x), sha(os.path.join(dp, x))) for dp, dn, fn in os.walk(os.path.join(self.vol, n)) for x in fn) for n in self.names}; self.assertEqual(before, after)
        ev = sorted(g for g in os.listdir(self.run) if g.startswith('recovery_evidence_2'))[-1]; out = json.load(open(os.path.join(self.run, ev, 'recovery_outcome.json')))
        self.assertTrue(out['preserved_content_identical'] and out['preserved_metadata_identical_except_atime']); self.assertTrue(all(v['state'] == 'removed' for v in out['targets'].values()))
        for nm in ('preserved_inventory_before.json', 'preserved_inventory_after.json', 'SHA256SUMS', 'SHA256SUMS_outcome'): self.assertTrue(os.path.exists(os.path.join(self.run, ev, nm)))
        inv = json.load(open(os.path.join(self.run, ev, 'inventory_slot0.json'))); self.assertEqual(len(inv['files']), 6)
    # ---- race / replacement / partial failure -------------------------------------------------------------------------------------------------------------------------
    def run_with_hooks(self, f, actions):
        """start the deletion with test hooks; actions = {(tag, j): callable} executed while the script waits at that point."""
        sd = os.path.join(self.t, 'sync'); shutil.rmtree(sd, ignore_errors=True); os.makedirs(sd); env = dict(os.environ, TP_RECOVERY_TEST_SYNC_DIR=sd)
        for (tag, j) in actions: open(os.path.join(sd, 'arm_%s_%d' % (tag, j)), 'w').write('x')
        pr = subprocess.Popen([sys.executable, '-I', REC, self.run, self.vol, '--expected', self.exp, '--delete', '--approval', f, '--allow-test-hooks'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
        for (tag, j), fn in sorted(actions.items(), key=lambda kv: kv[0][1]):
            t0 = time.time()
            while not os.path.exists(os.path.join(sd, 'at_%s_%d' % (tag, j))) and time.time() - t0 < 60 and pr.poll() is None: time.sleep(0.05)
            fn(); open(os.path.join(sd, 'go_%s_%d' % (tag, j)), 'w').write('x')
        out, err = pr.communicate(timeout=120); return pr.returncode, out + err
    def replace_slot(self, j):
        p = self.slots[j]['path']; os.rename(p, p + '_ASIDE'); os.makedirs(os.path.join(p, '%040x' % (j + 1)))
        for i in range(6): open(os.path.join(p, '%040x' % (j + 1), 'piece_%d.seg' % i), 'wb').write(os.urandom(5000))
    def outcome_partial(self):
        ev = sorted(g for g in os.listdir(self.run) if g.startswith('recovery_evidence_2'))[-1]; return json.load(open(os.path.join(self.run, ev, 'recovery_outcome_PARTIAL.json'))), ev
    def test_target_replaced_before_revalidation_first_target(self):
        rc, out = self.run_with_hooks(self.approval(), {('pre', 0): lambda: self.replace_slot(0)}); self.assertEqual(rc, 8, out)
        self.assertEqual(len([n for n in self.ring() if n.startswith('diagchurn_') and not n.endswith('_ASIDE')]), 10); self.assertTrue(os.path.isdir(self.slots[0]['path'] + '_ASIDE')); self.assertFalse([n for n in os.listdir(self.vol) if n.startswith('.recovery_quarantine_')])
        part, ev = self.outcome_partial(); self.assertTrue(part['targets']['0']['state'].startswith('not_attempted: target changed')); self.assertTrue(all(v['state'].startswith('not_attempted') for k, v in part['targets'].items()))
    def test_target_replaced_between_validation_and_rename_is_renamed_back(self):
        rc, out = self.run_with_hooks(self.approval(), {('post_validate', 0): lambda: self.replace_slot(0)}); self.assertEqual(rc, 8, out)
        self.assertFalse([n for n in os.listdir(self.vol) if n.startswith('.recovery_quarantine_')]); self.assertTrue(os.path.isdir(self.slots[0]['path'])); self.assertTrue(os.path.isdir(self.slots[0]['path'] + '_ASIDE'))
        part, ev = self.outcome_partial(); self.assertIn('renamed back', part['targets']['0']['state']); self.assertEqual(len(os.listdir(self.slots[0]['path'])), 1)
    def test_replacement_after_some_targets_removed_records_exact_state(self):
        rc, out = self.run_with_hooks(self.approval(), {('pre', 3): lambda: self.replace_slot(3)}); self.assertEqual(rc, 8, out)
        part, ev = self.outcome_partial(); st = {int(k): v['state'] for k, v in part['targets'].items()}
        self.assertTrue(all(st[j] == 'removed' for j in range(3))); self.assertTrue(st[3].startswith('not_attempted: target changed')); self.assertTrue(all(st[j].startswith('not_attempted') for j in range(4, 10)))
        for j in range(3): self.assertFalse(os.path.exists(self.slots[j]['path']))
        for j in range(4, 10): self.assertTrue(os.path.isdir(self.slots[j]['path']))
        self.assertTrue(os.path.exists(os.path.join(self.run, ev, 'SHA256SUMS')))
    def test_partial_deletion_failure_records_exact_state_and_keeps_evidence(self):
        f = self.approval(); victim = os.path.join(self.slots[5]['path'], '%040x' % 6, 'piece_2.seg'); subprocess.run(['chflags', 'uchg', victim], check=True)
        p = self.rec('--delete', '--approval', f); self.assertEqual(p.returncode, 8, p.stdout + p.stderr)
        part, ev = self.outcome_partial(); st = {int(k): v['state'] for k, v in part['targets'].items()}
        self.assertTrue(all(st[j] == 'removed' for j in range(5))); self.assertTrue(st[5].startswith('partially_removed_or_failed')); self.assertIn('.recovery_quarantine_', st[5]); self.assertTrue(all(st[j].startswith('not_attempted') for j in range(6, 10)))
        self.assertTrue([n for n in os.listdir(self.vol) if n.startswith('.recovery_quarantine_')]); self.assertTrue(os.path.exists(os.path.join(self.run, ev, 'inventory_slot5.json'))); self.assertTrue(os.path.exists(os.path.join(self.run, ev, 'SHA256SUMS')))
        for n in self.names: self.assertTrue(os.path.isdir(os.path.join(self.vol, n)))

if __name__ == '__main__': unittest.main(verbosity=1)
