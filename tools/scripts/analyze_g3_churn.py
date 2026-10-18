#!/usr/bin/env python3
"""Analyzer for the CHURN-F diagnostic (docs/proposals/G3_CHURN_VARIABILITY_PROPOSAL.md rev 3). DIAGNOSTIC; never a gate verdict.
usage: analyze_g3_churn.py <run_dir> [--selftest]
Reads <run_dir>/transfers.jsonl and net_*.log; writes analysis.txt and analysis.json.
Smoke runs (< 80 blocks): protocol/ring/gap/manipulation/resource checks only; the inferential estimands E1-E4 are NOT computed (no conclusion about the hypotheses is possible).
Pure standard library. The estimands implement proposal sections 5, 6.0 and 6.2 exactly (see comments)."""
import json, math, os, random, re, statistics as st, sys

B_BOOT = 10000; SEED = 20261022

def load(d):
    rows = [json.loads(l) for l in open(os.path.join(d, 'transfers.jsonl')) if l.strip()]
    return rows

def lifetimes(netlog):
    """connection lifetimes (s) from alert log: peer_connect opens, next peer_disconnected for the same endpoint closes."""
    openc = {}; out = []
    if not os.path.exists(netlog): return out
    for l in open(netlog):
        m = re.match(r'\[\s*([\d.]+)\] (peer_connect|peer_disconnected): \S+ peer (\d+\.\d+\.\d+\.\d+:\d+)', l)
        if not m: continue
        t = float(m.group(1)); ev = m.group(2); ep = m.group(3)
        if ev == 'peer_connect': openc.setdefault(ep, []).append(t)
        elif openc.get(ep): out.append(t - openc[ep].pop(0))
    return out

def med(v): return st.median(v) if v else float('nan')
def pct(v, p):
    v = sorted(v);
    if not v: return float('nan')
    i = (len(v) - 1) * p; lo = int(math.floor(i)); hi = min(lo + 1, len(v) - 1); return v[lo] + (v[hi] - v[lo]) * (i - lo)

# ---------------------------------------------------------------- estimands ------------------------------------------------------------------------------------------------
def sd(v): return st.stdev(v) if len(v) > 1 else float('nan')
def ref_medians(rows):
    """M_X per X level: median of T over ALL valid transfers of the level, G pooled, ring-fill included (proposal 6, E2 item 1). Computed once on the frozen valid set."""
    return {x: med([r['mibps'] for r in rows if r['valid'] and r['x'] == x]) for x in ('default', 'suppressed')}
def estimands(rows):
    v = [r for r in rows if r['valid']]
    d = [math.log(r['mibps']) for r in v if r['x'] == 'default']; s = [math.log(r['mibps']) for r in v if r['x'] == 'suppressed']
    E = {}
    E['E1_R_SD'] = sd(d) / sd(s) if len(d) > 1 and len(s) > 1 and sd(s) > 0 else float('nan')
    M = ref_medians(rows)
    def slow_share(x):
        t = [r['mibps'] for r in v if r['x'] == x]
        return sum(1 for q in t if q < 0.70 * M[x]) / len(t) if t else float('nan')
    E['M_default'] = M['default']; E['M_suppressed'] = M['suppressed']
    E['share_default'] = slow_share('default'); E['share_suppressed'] = slow_share('suppressed')
    E['E2_dSlow'] = E['share_default'] - E['share_suppressed']
    abs_def = [r['mibps'] for r in v if r['x'] == 'default']
    E['abs20_share_default'] = sum(1 for q in abs_def if q < 20.0) / len(abs_def) if abs_def else float('nan')
    abs_sup = [r['mibps'] for r in v if r['x'] == 'suppressed']
    E['abs20_share_suppressed_NOT_USED'] = sum(1 for q in abs_sup if q < 20.0) / len(abs_sup) if abs_sup else float('nan')
    E['unreliable_M_default'] = E['abs20_share_default'] > 0.40
    E['unreliable_M_suppressed'] = False   # the absolute 20 MiB/s valley applies to the default regime only; suppressed arm judged by its own share below 0.70*M (reported)
    E['E3_mean_lnT_diff'] = (st.mean(d) - st.mean(s)) if d and s else float('nan')
    # E4 (within default cells): effect of G
    for g in ('gate-like', 'spaced'):
        t = [r['mibps'] for r in v if r['x'] == 'default' and r['g'] == g]
        E['E4_abs20_share_default_' + g] = sum(1 for q in t if q < 20.0) / len(t) if t else float('nan')
        E['E4_mean_lnT_default_' + g] = st.mean([math.log(q) for q in t]) if t else float('nan')
        lt = [math.log(q) for q in t]; E['E4_SD_lnT_default_' + g] = sd(lt) if len(lt) > 1 else float('nan')
    E['E4_dAbs20'] = E['E4_abs20_share_default_spaced'] - E['E4_abs20_share_default_gate-like']
    E['E4_dMeanLnT'] = E['E4_mean_lnT_default_spaced'] - E['E4_mean_lnT_default_gate-like']
    return E

def boot(rows, B=B_BOOT, seed=SEED):
    """cluster bootstrap over blocks, percentile 95% CIs; the whole pipeline (M_X, classification, dSlow) is recomputed inside every replicate (proposal E2 item 3)."""
    blocks = {}
    for r in rows: blocks.setdefault(r['block'], []).append(r)
    ids = sorted(blocks); rng = random.Random(seed); keys = None; acc = {}
    for _ in range(B):
        samp = []
        for _i in range(len(ids)): samp.extend(blocks[ids[rng.randrange(len(ids))]])
        e = estimands(samp); e.update(secondary(samp))
        for k, val in e.items():
            if isinstance(val, bool): continue
            acc.setdefault(k, []).append(val)
    ci = {}
    for k, vs in acc.items():
        vs = [x for x in vs if x == x]
        ci[k] = (pct(vs, 0.025), pct(vs, 0.975)) if vs else (float('nan'), float('nan'))
    return ci

def reading(E, ci, manip):
    """Joint decision table of proposal 6.2 (E1 and E2 co-primary). Returns a text reading; never 'no effect'."""
    if manip == 'FAILED': return 'NOT INTERPRETABLE (manipulation FAILED)'
    lo1, hi1 = ci['E1_R_SD']; lo2, hi2 = ci['E2_dSlow']
    if E.get('unreliable_M_default'): e2 = 'E2 not interpretable (reference median unreliable: >40% of default transfers below 20 MiB/s)'; lo2 = float('nan')
    else: e2 = None
    if lo1 > 1.0 and (lo2 == lo2 and lo2 > 0):
        r = 'SUPPORTED: churn policy contributes to dispersion and to the slow mode' + (' ("major" claim allowed: E1 lower bound >= 1.25)' if lo1 >= 1.25 else ' (magnitude: see point estimates; "major" not claimed)')
        if manip == 'WEAK': r = 'ASSOCIATION with the intervention only (manipulation WEAK; no mechanism claim): ' + r
        return r
    if lo1 > 1.0: return 'MIXED: dispersion contribution (E1 lower > 1.0); no evidence on the slow share ' + ('(' + e2 + ')' if e2 else '(E2 interval includes 0)')
    if hi1 < 1.25:
        if manip == 'WEAK': return 'INCONCLUSIVE (a WEAK manipulation cannot exclude an effect; E1 upper bound < 1.25 downgraded)'
        return 'NOT A MAJOR DRIVER in this regime (E1 upper bound < 1.25); smaller contributions are not excluded; this is not evidence of no effect'
    return 'INCONCLUSIVE (E1 interval includes 1.0 and upper bound >= 1.25); not evidence of no effect'


# ---------------------------------------------------------------- secondary estimands E3-E6 and sensitivities (proposal 6, E1 sensitivities, E2 item 2/4, E4-E6) -------------
def iqr_sd(v): return (pct(v, .75) - pct(v, .25)) / 1.349 if len(v) > 3 else float('nan')
def ratio(a, b): return a / b if b == b and b > 0 else float('nan')
def ols(y, X):
    """least squares via normal equations (k <= 4); returns coefficients and R^2 (stdlib only)."""
    n = len(y); k = len(X[0]) if X else 0
    if n <= k: return [float('nan')] * k, float('nan')
    A = [[sum(X[i][a] * X[i][b] for i in range(n)) for b in range(k)] + [sum(X[i][a] * y[i] for i in range(n))] for a in range(k)]
    for c in range(k):
        piv = max(range(c, k), key=lambda r: abs(A[r][c]))
        if abs(A[piv][c]) < 1e-12: return [float('nan')] * k, float('nan')
        A[c], A[piv] = A[piv], A[c]
        for r in range(k):
            if r != c:
                f = A[r][c] / A[c][c]
                for j in range(c, k + 1): A[r][j] -= f * A[c][j]
    beta = [A[i][k] / A[i][i] for i in range(k)]
    my = sum(y) / n; sst = sum((q - my) ** 2 for q in y); sse = sum((y[i] - sum(beta[j] * X[i][j] for j in range(k))) ** 2 for i in range(n))
    return beta, (1 - sse / sst) if sst > 0 else float('nan')

def secondary(rows):
    v = [r for r in rows if r['valid']]; S = {}
    lnT = lambda g: [math.log(r['mibps']) for r in g]
    d = [r for r in v if r['x'] == 'default']; s_ = [r for r in v if r['x'] == 'suppressed']
    # E1 sensitivities: robust SD, within-block-centred SD, per-G ratio
    S['E1s_robust_R_SD'] = ratio(iqr_sd(lnT(d)), iqr_sd(lnT(s_)))
    bm = {}
    for r in v: bm.setdefault(r['block'], []).append(math.log(r['mibps']))
    cen = lambda grp: [math.log(r['mibps']) - st.mean(bm[r['block']]) for r in grp if len(bm[r['block']]) > 1]
    S['E1s_blockcentred_R_SD'] = ratio(sd(cen(d)), sd(cen(s_)))
    for g in ('gate-like', 'spaced'):
        S['E1s_R_SD_' + g] = ratio(sd(lnT([r for r in d if r['g'] == g])), sd(lnT([r for r in s_ if r['g'] == g])))
    S['E4_R_SD_default_spaced_over_gatelike'] = ratio(sd(lnT([r for r in d if r['g'] == 'spaced'])), sd(lnT([r for r in d if r['g'] == 'gate-like'])))
    # interaction X x G on mean ln T (descriptive)
    mm = lambda grp, g: st.mean(lnT([r for r in grp if r['g'] == g])) if [r for r in grp if r['g'] == g] else float('nan')
    S['E4_interaction_meanLnT'] = (mm(d, 'spaced') - mm(d, 'gate-like')) - (mm(s_, 'spaced') - mm(s_, 'gate-like'))
    S['E4_interaction_logRSD'] = math.log(S['E1s_R_SD_spaced'] / S['E1s_R_SD_gate-like']) if S['E1s_R_SD_spaced'] == S['E1s_R_SD_spaced'] and S['E1s_R_SD_gate-like'] == S['E1s_R_SD_gate-like'] and S['E1s_R_SD_gate-like'] > 0 and S['E1s_R_SD_spaced'] > 0 else float('nan')
    # E2 sensitivities: multipliers 0.60 / 0.80 ; hung transfers kept at the censored value 160 MiB/120 s and counted ; absolute 18 / 22 (default only)
    M = ref_medians(rows)
    for m in (0.60, 0.80):
        sh = lambda x: (lambda t: sum(q < m * M[x] for q in t) / len(t) if t else float('nan'))([r['mibps'] for r in v if r['x'] == x])
        S['E2s_dSlow_mult_%0.2f' % m] = sh('default') - sh('suppressed')
    cens = [dict(r, mibps=160.0 / 120.0, valid=True) if (not r['valid'] and str(r.get('reason', '')).startswith('hang')) else r for r in rows]
    Mc = ref_medians(cens); vc = [r for r in cens if r['valid']]
    shc = lambda x: (lambda t: sum(q < 0.70 * Mc[x] for q in t) / len(t) if t else float('nan'))([r['mibps'] for r in vc if r['x'] == x])
    S['E2s_dSlow_hung_censored'] = shc('default') - shc('suppressed'); S['hung_default'] = sum(1 for r in rows if (not r['valid']) and str(r.get('reason', '')).startswith('hang') and r['x'] == 'default'); S['hung_suppressed'] = sum(1 for r in rows if (not r['valid']) and str(r.get('reason', '')).startswith('hang') and r['x'] == 'suppressed')
    for th in (18.0, 22.0):
        S['E2s_abs%d_share_default' % th] = sum(r['mibps'] < th for r in d) / len(d) if d else float('nan')
    # PP sensitivity: exclude suppressed breaches and gap deviations (E1, E2)
    pp = [r for r in rows if not (r.get('manip') == 'breach' or r.get('gap_deviation'))]
    Epp = estimands(pp); S['PP_E1_R_SD'] = Epp['E1_R_SD']; S['PP_E2_dSlow'] = Epp['E2_dSlow']
    # E5 (observational, default cells only): ln T ~ W, Z, Rc
    if len(d) > 5:
        y = lnT(d)
        b, r2 = ols(y, [[1.0, float(r['wd_resets'] >= 1), r['Z'], float(r['Rc'])] for r in d]); S['E5_R2_W_Z_Rc'] = r2; S['E5_beta_W'], S['E5_beta_Z'], S['E5_beta_Rc'] = b[1], b[2], b[3]
        S['E5_R2_Windicator_only'] = ols(y, [[1.0, float(r['wd_resets'] >= 1)] for r in d])[1]; S['E5_R2_Z_only'] = ols(y, [[1.0, r['Z']] for r in d])[1]; S['E5_R2_Rc_only'] = ols(y, [[1.0, float(r['Rc'])] for r in d])[1]
        slow = [r for r in d if r['mibps'] < 20.0]
        S['E5_slow_abs20_n'] = len(slow); S['E5_slow_abs20_without_stall_frac'] = (sum(1 for r in slow if r['wd_resets'] == 0) / len(slow)) if slow else float('nan')
        S['E5_stall_W1_n'] = sum(1 for r in d if r['wd_resets'] >= 1); S['E5_stall_W1_slow_frac'] = (sum(1 for r in d if r['wd_resets'] >= 1 and r['mibps'] < 20.0) / S['E5_stall_W1_n']) if S['E5_stall_W1_n'] else float('nan')
    # E6 (descriptive): CPU, A, duration per cell ; position in block ; carry-over from the previous transfer's X
    for x, grp in (('default', d), ('suppressed', s_)):
        for g in ('gate-like', 'spaced'):
            c = [r for r in grp if r['g'] == g]
            if c:
                S['E6_cpu_median_%s_%s' % (x, g)] = med([r['cpu_s'] for r in c]); S['E6_A_median_%s_%s' % (x, g)] = med([r['A'] for r in c]); S['E6_duration_median_%s_%s' % (x, g)] = med([r['read_s'] for r in c])
    for pos in range(4):
        c = [r for r in v if r['pos'] == pos]
        if c: S['E6_meanLnT_pos%d' % pos] = st.mean(lnT(c))
    byk = {r['k']: r for r in rows}
    for xcur in ('default', 'suppressed'):
        for xprev in ('default', 'suppressed'):
            c = [r for r in v if r['x'] == xcur and (r['k'] - 1) in byk and byk[r['k'] - 1]['x'] == xprev]
            if c: S['E6_meanLnT_cur_%s_after_%s' % (xcur, xprev)] = st.mean(lnT(c))
    return S

# ---------------------------------------------------------------- checks ----------------------------------------------------------------------------------------------------
REQUIRED_FIELDS = ('k', 'block', 'pos', 'x', 'g', 'tag', 'slot', 'valid', 'mibps', 'slot_prev_use', 'slot_found', 'slot_found_files', 'slot_found_bytes', 'slot_found_files_indep', 'slot_found_bytes_indep', 'slot_found_agree',
                   'slot_inode_found', 'slot_inode_new', 'del_start_s', 'del_end_s', 'empty_after_create', 'retained_files', 'retained_bytes', 'retained_files_indep', 'retained_bytes_indep', 'retained_agree', 'retained_non_regular',
                   'sleep_gap_s', 'realised_gap_s', 'construct_start_s', 'gap_deviation', 'setting_close_redundant')

def walk_stats(path):
    """independent post-run measurement (python): recursive regular-file count and allocated bytes; None if unreadable. Does not follow symlinks."""
    files = 0; alloc = 0
    if not os.path.isdir(path) or os.path.islink(path): return None
    for dp, dn, fn in os.walk(path, followlinks=False):
        for n in fn:
            try: s = os.lstat(os.path.join(dp, n))
            except OSError: return None
            files += 1; alloc += s.st_blocks * 512
    return files, alloc

def checks(rows, d=None, volume_walk=True):
    """ring/state checks. FAIL CLOSED: a missing, zero, inconsistent or unreadable required measurement is a failure, never a pass (regression fix of the 2026-10-17 smoke halt)."""
    out = {}; fails = []
    if not rows: fails.append('no transfers recorded')
    last_inode = {}; ret_by_k = {}
    for r in sorted(rows, key=lambda r: r.get('k', -1)):
        miss = [f for f in REQUIRED_FIELDS if f not in r or r[f] is None]
        if miss: fails.append('k=%s missing required fields %s' % (r.get('k'), miss)); continue
        k = r['k']; sl = r['slot']; ret_by_k[k] = (r['retained_files'], r['retained_bytes'])
        if sl != k % 10: fails.append('k=%d slot %d != k mod 10' % (k, sl))
        if not (r['retained_files'] > 0 and r['retained_bytes'] > 0): fails.append('k=%d retained contents zero (%s files, %s bytes): measurement invalid' % (k, r['retained_files'], r['retained_bytes']))
        if r['retained_files'] > 480 or r['retained_bytes'] > (128 << 20): fails.append('k=%d retained contents out of bounds (%d files, %d bytes)' % (k, r['retained_files'], r['retained_bytes']))
        if not r['retained_agree'] or r['retained_files'] != r['retained_files_indep'] or r['retained_bytes'] != r['retained_bytes_indep']: fails.append('k=%d the two recursive measurement methods disagree after the transfer' % k)
        if r['retained_non_regular'] != 0: fails.append('k=%d non-regular entries in the retained directory' % k)
        if k >= 10:
            if r['slot_prev_use'] != k - 10: fails.append('k=%d slot_prev_use %d != %d' % (k, r['slot_prev_use'], k - 10))
            if not r['slot_found'] or r['slot_found_files'] <= 0: fails.append('k=%d wrap slot directory missing/empty' % k)
            if not r['slot_found_agree'] or r['slot_found_files'] != r['slot_found_files_indep'] or r['slot_found_bytes'] != r['slot_found_bytes_indep']: fails.append('k=%d found-directory measurement methods disagree' % k)
            prev = ret_by_k.get(k - 10)
            if prev is None: fails.append('k=%d no recorded retained measurement for transfer %d' % (k, k - 10))
            elif (r['slot_found_files'], r['slot_found_bytes']) != prev: fails.append('k=%d found directory (%s files, %s bytes) differs from what transfer %d left behind %s' % (k, r['slot_found_files'], r['slot_found_bytes'], k - 10, prev))
            if last_inode.get(sl) != r['slot_inode_found']: fails.append('k=%d found inode %s != inode created by transfer %d (%s)' % (k, r['slot_inode_found'], k - 10, last_inode.get(sl)))
        else:
            if r['slot_found']: fails.append('k=%d ring-fill slot already existed' % k)
        if not r['empty_after_create']: fails.append('k=%d slot not empty after create' % k)
        if not (r['del_end_s'] <= r['construct_start_s']): fails.append('k=%d deletion not finished before construction' % k)
        last_inode[sl] = r['slot_inode_new']
        if r['valid'] and r['setting_close_redundant'] != (0 if r['x'] == 'suppressed' else 1): fails.append('k=%d setting readback mismatch' % k)
    # ---- manifests and the independent post-run walk of the real directories -----------------------------------------------------------------------------------------------
    if d is not None:
        mi = mf = None
        for name in ('manifest_intended.json', 'manifest_final.json', 'finished.txt'):
            if not os.path.exists(os.path.join(d, name)): fails.append('missing ' + name)
        try: mi = json.load(open(os.path.join(d, 'manifest_intended.json')))
        except Exception as e: fails.append('manifest_intended.json unreadable: %s' % e)
        try: mf = json.load(open(os.path.join(d, 'manifest_final.json')))
        except Exception as e: fails.append('manifest_final.json unreadable: %s' % e)
        try:
            fin = open(os.path.join(d, 'finished.txt')).read()
            if 'HALTED' in fin or 'complete' not in fin: fails.append('run did not finish normally: ' + fin.strip())
        except Exception: pass
        if mf is not None and mi is not None:
            last_k = {}
            for r in rows:
                if 'slot' in r and 'k' in r: last_k[r['slot']] = max(last_k.get(r['slot'], -1), r['k'])
            intended = {os.path.realpath(p) for p in mi.get('intended_slot_dirs', [])}
            slots = {s_['slot']: s_ for s_ in mf.get('slots', [])}
            for j in range(10):
                s_ = slots.get(j)
                if s_ is None: fails.append('manifest_final lacks slot %d' % j); continue
                lk = last_k.get(j, -1)
                if lk >= 0 and not s_.get('exists'): fails.append('slot %d should exist after transfer %d' % (j, lk)); continue
                if not s_.get('exists'): continue
                if os.path.realpath(s_['path']) not in intended: fails.append('slot %d path not in manifest_intended' % j)
                if s_.get('last_transfer') != lk: fails.append('slot %d last_transfer %s != %s' % (j, s_.get('last_transfer'), lk))
                if not s_.get('methods_agree') or s_.get('files', 0) <= 0 or s_.get('allocated_bytes', 0) <= 0 or s_.get('files') != s_.get('files_indep') or s_.get('allocated_bytes') != s_.get('allocated_bytes_indep'): fails.append('slot %d final measurement zero/inconsistent: %s' % (j, {k_: s_.get(k_) for k_ in ('files', 'allocated_bytes', 'files_indep', 'allocated_bytes_indep', 'methods_agree')}))
                if lk in ret_by_k and (s_.get('files'), s_.get('allocated_bytes')) != ret_by_k[lk]: fails.append('slot %d final contents differ from the post-transfer measurement of transfer %d' % (j, lk))
                lastrow = next((r for r in rows if r.get('k') == lk), None)
                if lastrow is not None and s_.get('inode') != lastrow.get('slot_inode_new'): fails.append('slot %d final inode differs from the inode created by transfer %d' % (j, lk))
                if volume_walk:
                    w = walk_stats(s_['path'])
                    if w is None: fails.append('slot %d independent walk failed/unreadable: %s' % (j, s_['path']))
                    elif w != (s_.get('files'), s_.get('allocated_bytes')): fails.append('slot %d independent python walk %s != manifest_final (%s, %s)' % (j, w, s_.get('files'), s_.get('allocated_bytes')))
    out['volume_walk'] = 'performed' if (d is not None and volume_walk) else 'skipped'
    out['ring_and_state_failures'] = fails
    ok_rows = [r for r in rows if 'sleep_gap_s' in r and 'realised_gap_s' in r]
    sp = [r['sleep_gap_s'] for r in ok_rows if r.get('g') == 'spaced']; gl = [r['realised_gap_s'] for r in ok_rows if r.get('g') == 'gate-like']
    out['spaced_sleep_gap_min_max'] = (min(sp), max(sp)) if sp else None
    out['gate_like_realised_gap_min_median_max'] = (min(gl), med(gl), max(gl)) if gl else None
    out['gap_deviations'] = [r['k'] for r in rows if r.get('gap_deviation')]
    dels = [r['del_end_s'] - r['del_start_s'] for r in ok_rows if 'del_end_s' in r and 'del_start_s' in r]
    out['slot_deletion_seconds_min_median_max'] = (min(dels), med(dels), max(dels)) if dels else None
    out['realised_gap_by_g'] = {g: [round(r['realised_gap_s'], 3) for r in ok_rows if r.get('g') == g] for g in ('gate-like', 'spaced')}
    return out

def manipulation(rows, d):
    v = [r for r in rows if r['valid']]
    sup = [r for r in v if r['x'] == 'suppressed']; de = [r for r in v if r['x'] == 'default']
    n_intact = sum(r['manip'] == 'intact' for r in sup); n_pres = sum(r['manip'] == 'churn_present' for r in de)
    md = med([r['Rc'] for r in de]); ms = med([r['Rc'] for r in sup])
    fi = n_intact / len(sup) if sup else float('nan'); fp = n_pres / len(de) if de else float('nan')
    if not sup or not de: verdict = 'UNDETERMINED (a cell has no valid transfers)'
    elif (1 - fi) > 0.25 or fp < 0.50: verdict = 'FAILED'
    elif fi >= 0.90 and fp >= 0.90 and (md - ms) >= 4: verdict = 'OK'
    else: verdict = 'WEAK'
    stats = {}
    for x, grp in (('default', de), ('suppressed', sup)):
        lt = []
        for r in grp: lt.extend(lifetimes(os.path.join(d, 'net_' + r['tag'] + '.log')))
        stats[x] = {'n': len(grp), 'Rc_median': med([r['Rc'] for r in grp]), 'Rc_all': [r['Rc'] for r in grp], 'Dc_median': med([r['disconnects'] for r in grp]), 'Z_median': med([r['Z'] for r in grp]), 'Z_all': [round(r['Z'], 3) for r in grp],
                    'A_median': med([r['A'] for r in grp]), 'W_total': sum(r['wd_resets'] for r in grp), 'lifetime_n': len(lt), 'lifetime_median_s': med(lt), 'lifetime_p10_p90_s': (pct(lt, .1), pct(lt, .9)),
                    'mibps_all': [round(r['mibps'], 1) for r in grp], 'duration_median_s': med([r['read_s'] for r in grp]), 'cpu_median_s': med([r['cpu_s'] for r in grp])}
    return {'verdict': verdict, 'suppressed_intact': '%d/%d' % (n_intact, len(sup)), 'default_churn_present': '%d/%d' % (n_pres, len(de)), 'median_Rc_diff': md - ms, 'by_x': stats}

def main():
    if '--selftest' in sys.argv: return selftest()
    d = sys.argv[1]; rows = load(d) if os.path.exists(os.path.join(d, 'transfers.jsonl')) else []; nblocks = len({r.get('block') for r in rows})
    res = {'dir': d, 'transfers': len(rows), 'blocks': nblocks, 'valid': sum(bool(r.get('valid')) for r in rows), 'invalid': [(r.get('k'), r.get('reason')) for r in rows if not r.get('valid')]}
    res['checks'] = checks(rows, d, volume_walk='--no-volume-walk' not in sys.argv); res['fail_closed'] = bool(res['checks']['ring_and_state_failures'])
    try: res['manipulation'] = manipulation([r for r in rows if all(f in r for f in ('Rc', 'Z', 'A', 'manip'))], d)
    except Exception as e: res['manipulation'] = {'verdict': 'UNAVAILABLE (%s)' % e, 'suppressed_intact': 'n/a', 'default_churn_present': 'n/a', 'median_Rc_diff': float('nan'), 'by_x': {}}
    fs_ = [r['free_start_gib'] for r in rows if 'free_start_gib' in r] + [r['free_end_gib'] for r in rows if 'free_end_gib' in r]
    res['free_gib_min_max'] = (min(fs_), max(fs_)) if fs_ else None
    res['cell_counts'] = {}
    for r in rows:
        if 'x' in r and 'g' in r: res['cell_counts'][r['x'] + '/' + r['g']] = res['cell_counts'].get(r['x'] + '/' + r['g'], 0) + 1
    if nblocks >= 80:
        E = estimands(rows); E.update(secondary(rows)); ci = boot(rows); res['estimands'] = E; res['ci'] = ci; res['reading'] = reading(E, ci, res['manipulation']['verdict'])
        sub_ = [r for r in rows if r['block'] >= 3]; sens = estimands(sub_); sens.update(secondary(sub_)); res['sensitivity_drop_blocks_1_to_3'] = sens
    else:
        res['estimands'] = 'NOT COMPUTED: %d blocks < 80 (smoke / partial run); no conclusion about the hypotheses is possible' % nblocks
    json.dump(res, open(os.path.join(d, 'analysis.json'), 'w'), indent=1, default=str)
    with open(os.path.join(d, 'analysis.txt'), 'w') as f:
        f.write(json.dumps(res, indent=1, default=str) + '\n')
    print(json.dumps({k: res[k] for k in ('transfers', 'blocks', 'valid', 'invalid', 'cell_counts', 'free_gib_min_max')}, default=str))
    print('ring/state failures:', res['checks']['ring_and_state_failures'])
    print('manipulation:', res['manipulation']['verdict'], res['manipulation']['suppressed_intact'], res['manipulation']['default_churn_present'], 'median Rc diff', res['manipulation']['median_Rc_diff'])
    print('FAIL-CLOSED: %d failure(s)' % len(res['checks']['ring_and_state_failures']) if res['fail_closed'] else 'ring/state checks: all passed')
    sys.exit(1 if res['fail_closed'] else 0)

def synth_rows(nblocks, seed=1, ring_ok=True):
    """synthetic transfers with every field the harness writes (planted effect; used only to test the analyzer pipeline, never a result)."""
    rng = random.Random(seed); rows = []; k = 0; ino = {}
    for b in range(nblocks):
        cells = [('default', 'gate-like'), ('default', 'spaced'), ('suppressed', 'gate-like'), ('suppressed', 'spaced')]; rng.shuffle(cells)
        for pos, (x, g) in enumerate(cells):
            stall = (x == 'default' and rng.random() < 0.2)
            lnT = math.log(80 if x == 'suppressed' else 32) + rng.gauss(0, 0.2 if x == 'suppressed' else 0.25) - (0.8 if stall else 0)
            rc = rng.choice([0, 1]) if x == 'suppressed' else rng.choice([6, 8, 8, 9]); sl = k % 10; new = 1000 + k
            rows.append({'k': k, 'block': b, 'pos': pos, 'x': x, 'g': g, 'tag': 'CF_%03d_%s_%s' % (k, x, g), 'slot': sl, 'ring_fill': k < 10, 'valid': True, 'reason': '', 'ttfb_s': .5, 'mibps': math.exp(lnT), 'read_s': 160 / math.exp(lnT),
                         'cpu_s': 2.0, 'free_start_gib': 8.5 - 0.1 * (k % 10), 'free_end_gib': 8.4 - 0.1 * (k % 10), 'samples': 80, 'zero_peer_samples': 0, 'peers_sum': 160, 'Rc': rc, 'Z': ((0.5 + 0.2 * rng.random() + (0.2 if stall else 0)) if x == 'default' else 0.02 * rng.random()), 'A': 1.6, 'kicks': 0,
                         'wd_resets': 1 if stall else 0, 'unhave_resets': 0, 'connects': rc + 2, 'disconnects': rc + 2, 'manip': ('intact' if rc <= 2 else 'breach') if x == 'suppressed' else 'churn_present', 'setting_close_redundant': 0 if x == 'suppressed' else 1,
                         'slot_prev_use': (k - 10) if k >= 10 else -1, 'slot_found': k >= 10, 'slot_found_files': 460 if k >= 10 else 0, 'slot_found_bytes': 120000000 if k >= 10 else 0, 'slot_inode_found': ino.get(sl, 0) if ring_ok else 7, 'slot_inode_new': new,
                         'del_start_s': k * 10.0, 'del_end_s': k * 10.0 + .05, 'empty_after_create': True, 'retained_files': 460, 'retained_bytes': 120000000, 'retained_files_indep': 460, 'retained_bytes_indep': 120000000, 'retained_agree': True, 'retained_non_regular': 0, 'slot_found_files_indep': 460 if k >= 10 else 0, 'slot_found_bytes_indep': 120000000 if k >= 10 else 0, 'slot_found_agree': True, 'sleep_gap_s': 5.0 if g == 'spaced' else 0.0, 'realised_gap_s': 5.1 if g == 'spaced' else 0.1,
                         'construct_start_s': k * 10.0 + .1, 'gap_deviation': False})
            ino[sl] = new; k += 1
    return rows

def selftest():
    """pipeline test on synthetic data with a planted effect (not a result about any hypothesis); also checks the ring/state check detects an inode mismatch."""
    rows = synth_rows(80); E = estimands(rows); E.update(secondary(rows)); ci = boot(rows, B=300)
    print('selftest E1 point %.3f CI %s ; E2 point %.3f CI %s ; E5 R2 (W,Z,Rc) %.3f ; secondary keys %d' % (E['E1_R_SD'], ci['E1_R_SD'], E['E2_dSlow'], ci['E2_dSlow'], E['E5_R2_W_Z_Rc'], len(E)))
    print('reading (manipulation OK):', reading(E, ci, 'OK'))
    print('checks on consistent ring data:', checks(synth_rows(3))['ring_and_state_failures'])
    print('checks on tampered ring data  :', checks(synth_rows(3, ring_ok=False))['ring_and_state_failures'][:2])
    if '--write-synth' in sys.argv:
        d = sys.argv[sys.argv.index('--write-synth') + 1]; os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, 'transfers.jsonl'), 'w') as f:
            for r in synth_rows(int(sys.argv[sys.argv.index('--write-synth') + 2])): f.write(json.dumps(r) + '\n')

if __name__ == '__main__': main()
