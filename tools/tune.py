#!/usr/bin/env python3
"""Sweep the optimiser's register-pressure budgets (-Oparam) for one ISA.

Each evaluation compiles and runs the workloads with a set of -Oparam
values and measures cycles (sim_c; cycles equal instructions on the barrel
core, so this is the whole cost model), checking every result:

  corpus     every tests/cases program with EXPECT_R0 (r0 checked), scored
             as the geometric mean of per-program cycle ratios to the
             baseline, so each program counts equally
  coremark   CRCs checked
  raytracer  frame checked against the baseline frame
  jpeg       r0 checked

The score is the geometric mean of the four ratios (lower is better). The
search is coordinate descent from the target's defaults: each parameter in
turn over its range, keeping a value only if it lowers the score and makes
no workload worse than --tolerance; rounds repeat until nothing changes.

  tools/tune.py --arch cpu5
  tools/tune.py --arch cpu4 --rounds 1 --only licm_reserve,lsr_reserve
"""
import argparse, concurrent.futures as cf, hashlib, math, os, re, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MONO = os.path.dirname(os.path.dirname(ROOT))
SMALLCC, SIM = os.path.join(ROOT, 'smallcc'), os.path.join(ROOT, 'sim_c')

PARAMS = ['lc_reserve', 'lc_reserve_large', 'lc_large_body', 'lc_cap_reserve', 'lc_max_hoist',
          'lc_min_uses', 'licm_reserve', 'licm_max', 'licm_dense_hi', 'licm_dense_lo', 'lsr_reserve',
          'ipra', 'ipra_reserve', 'inline_cf_nodes', 'frame_promote', 'spill_cost']


def ranges(K):
    return {
        'lc_reserve': range(0, K), 'lc_reserve_large': range(0, K), 'lc_cap_reserve': range(0, K),
        'lc_large_body': [8, 12, 16, 24, 32, 48, 64], 'lc_max_hoist': range(0, 9),
        'lc_min_uses': range(0, 4), 'licm_reserve': range(1, K), 'licm_max': range(0, 9),
        'licm_dense_hi': [4, 6, 8, 10, 12, 16, 20, 30, 1000], 'licm_dense_lo': [2, 3, 4, 6, 8, 10, 14, 20, 1000],
        'lsr_reserve': range(1, K), 'ipra': range(0, 2), 'ipra_reserve': range(0, K // 2 + 1),
        'inline_cf_nodes': [0, 40, 80, 120, 160, 200, 240],
        'frame_promote': range(0, 2), 'spill_cost': range(0, 2),
    }


def corpus():
    out = []
    for d, _, files in os.walk(os.path.join(ROOT, 'tests', 'cases')):
        for f in sorted(files):
            if not f.endswith('.c'):
                continue
            p = os.path.join(d, f)
            meta = {}
            for line in open(p):
                if not line.startswith('//'):
                    break
                m = re.match(r'//\s*([A-Z0-9_]+):?\s*(.*)', line)
                if m:
                    meta[m.group(1)] = m.group(2).strip()
            if 'EXPECT_R0' not in meta or 'XFAIL' in meta or 'SIM_ARGS' in meta:
                continue
            if f == 'jpeg_test.c':          # a workload of its own
                continue
            srcs = [os.path.join(d, x) for x in meta['FILES'].split()] if 'FILES' in meta else [p]
            out.append((os.path.relpath(p, ROOT), srcs, meta.get('CFLAGS', '').split(),
                        meta.get('TARGET', 'sim'), int(meta['EXPECT_R0'])))
    return out


def run_one(arch, params, srcs, cflags, target, maxsteps, fb=False):
    with tempfile.TemporaryDirectory() as tmp:
        asm = os.path.join(tmp, 'p.s')
        cmd = [SMALLCC, '-arch', arch, '-target', target, *cflags, *[f'-Oparam={k}={v}' for k, v in params],
               '-o', asm, *srcs]
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
        if p.returncode:
            return None, None, None, 'compile: ' + p.stderr[:200]
        sim = [SIM, '-arch', arch, '-maxsteps', str(maxsteps)]
        fbp = os.path.join(tmp, 'f.ppm')
        if fb:
            sim += ['-fb', fbp]
        s = subprocess.run(sim + [asm], capture_output=True, text=True, timeout=300)
        m = re.search(r'r0:([0-9a-f]{8}).*cycles:(\d+)', s.stdout)
        if not m:
            return None, None, None, 'no result'
        r0 = int(m.group(1), 16)
        r0 = r0 - (1 << 32) if r0 & 0x80000000 else r0
        frame = hashlib.md5(open(fbp, 'rb').read()).hexdigest() if fb and os.path.exists(fbp) else None
        return int(m.group(2)), r0, frame, s.stderr


class Evaluator:
    def __init__(self, arch, jobs):
        self.arch, self.jobs = arch, jobs
        self.corpus = corpus()
        self.cache = {}
        self.base = None

    def evaluate(self, params):
        key = tuple(sorted(params.items()))
        if key in self.cache:
            return self.cache[key]
        pl = sorted(params.items())
        res, bad = {}, []
        with cf.ThreadPoolExecutor(self.jobs) as ex:
            futs = {ex.submit(run_one, self.arch, pl, srcs, cf_, tg, 10_000_000): (name, want)
                    for name, srcs, cf_, tg, want in self.corpus}
            cm = ex.submit(run_one, self.arch, pl, [os.path.join(ROOT, 'bench', 'coremark', 'coremark_single.c')],
                           [], 'sim', 4_000_000)
            rt = ex.submit(run_one, self.arch, pl, [os.path.join(MONO, 'sw', 'raytracer', 'src', 'raytracer.c')],
                           [], 'sim', 200_000_000, True)
            jp = ex.submit(run_one, self.arch, pl, [os.path.join(ROOT, 'tests', 'cases', 'jpeg', 'jpeg_test.c')],
                           [], 'sim', 100_000_000)
            corpus_cycles = {}
            for f, (name, want) in futs.items():
                cyc, r0, _, err = f.result()
                if cyc is None or r0 != want:
                    bad.append(f'{name}: {err if cyc is None else f"r0 {r0} != {want}"}')
                else:
                    corpus_cycles[name] = cyc
            cyc, _, _, err = cm.result()
            if cyc is None or not all(c in (err or '') for c in ('0xe9f5', '0xe714', '0x1fd7', '0x8e3a')):
                bad.append('coremark: CRC mismatch or no result')
            res['coremark'] = cyc
            cyc, _, frame, _ = rt.result()
            res['raytracer'] = cyc
            res['_frame'] = frame
            cyc, r0, _, _ = jp.result()
            if cyc is None:
                bad.append('jpeg: no result')
            res['jpeg'] = cyc
            res['_corpus'] = corpus_cycles
        if self.base and res['_frame'] != self.base['_frame']:
            bad.append('raytracer: frame differs')
        res['_bad'] = bad
        self.cache[key] = res
        return res

    def ratios(self, res):
        b = self.base
        cr = [res['_corpus'][n] / b['_corpus'][n] for n in b['_corpus'] if n in res['_corpus']]
        r = {'corpus': math.exp(sum(math.log(x) for x in cr) / len(cr)) if cr else 9.9}
        for w in ('coremark', 'raytracer', 'jpeg'):
            r[w] = res[w] / b[w] if res[w] and b[w] else 9.9
        r['score'] = math.exp(sum(math.log(r[w]) for w in ('corpus', 'coremark', 'raytracer', 'jpeg')) / 4)
        return r


def fmt(r):
    return ' '.join(f'{w} {100 * (r[w] - 1):+.2f}%' for w in ('corpus', 'coremark', 'raytracer', 'jpeg', 'score'))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--arch', default='cpu4')
    ap.add_argument('--rounds', type=int, default=3)
    ap.add_argument('--only', help='comma-separated parameters to sweep (default all)')
    ap.add_argument('--tolerance', type=float, default=0.5, help='max %% regression on any workload')
    ap.add_argument('-j', '--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    listing = subprocess.run([SMALLCC, '-arch', a.arch, '-Oparam=list', 'x'], capture_output=True, text=True).stderr
    defaults = {k: int(v) for k, v in (l.split('=') for l in listing.split())}
    K = {'cpu4': 8, 'cpu5': 16}.get(a.arch, 8)
    ev = Evaluator(a.arch, a.jobs)
    base = ev.evaluate(defaults)
    if base['_bad']:
        sys.exit('baseline fails: ' + '; '.join(base['_bad'][:5]))
    ev.base = base
    print(f'{a.arch} baseline: coremark {base["coremark"]:,}  raytracer {base["raytracer"]:,}  '
          f'jpeg {base["jpeg"]:,}  corpus {len(base["_corpus"])} programs', flush=True)
    cur = dict(defaults)
    cur_r = ev.ratios(base)
    names = a.only.split(',') if a.only else PARAMS
    rng = ranges(K)
    for rnd in range(a.rounds):
        changed = False
        for p in names:
            best_v, best_r = cur[p], cur_r
            for v in rng[p]:
                if v == cur[p]:
                    continue
                trial = dict(cur, **{p: v})
                res = ev.evaluate(trial)
                if res['_bad']:
                    print(f'  {p}={v}: INCORRECT: {res["_bad"][0]}', flush=True)
                    continue
                r = ev.ratios(res)
                worst = max(r[w] for w in ('corpus', 'coremark', 'raytracer', 'jpeg'))
                ok = worst <= 1 + a.tolerance / 100 and r['score'] < best_r['score'] - 1e-6
                print(f'  {p}={v}: {fmt(r)}{"  *" if ok else ""}', flush=True)
                if ok:
                    best_v, best_r = v, r
            if best_v != cur[p]:
                print(f'round {rnd + 1}: {p} {cur[p]} -> {best_v}: {fmt(best_r)}', flush=True)
                cur[p], cur_r, changed = best_v, best_r, True
        if not changed:
            break
    print(f'\n{a.arch} result: ' + ' '.join(f'{k}={v}' for k, v in cur.items() if v != defaults[k])
          + f'\n  {fmt(cur_r)}')
    res = ev.evaluate(cur)
    print(f'  coremark {res["coremark"]:,}  raytracer {res["raytracer"]:,}  jpeg {res["jpeg"]:,}')


if __name__ == '__main__':
    main()
