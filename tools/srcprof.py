#!/usr/bin/env python3
"""Profile-annotated source: executed instructions per source line.

Compiles with smallcc -g (each instruction's file, line and inline call
chain), runs sim_c with -pccount/-imap, and attributes every executed
instruction:

  self       to the line it was written on (an inlined helper's own lines)
  inclusive  to that line and to every call site it was inlined through,
             plus, for real calls (jl), the callee's inclusive cost spread
             over its call sites by call count (as gprof does)

The kernel table lists each function with its invocations (inlined
expansions and real calls counted alike), inclusive instructions and
instructions per invocation, which is what sizing an offload block needs.

  tools/srcprof.py --arch cpu5 prog.c                      # text, to stdout
  tools/srcprof.py --arch cpu5 --html prof.html prog.c     # heatmap page
  tools/srcprof.py --arch cpu5 --per 76800 --top 40 prog.c # costs per pixel
"""
import argparse, collections, html, json, os, re, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SRC_RE = re.compile(r'^\s*;\s*@src\s+(.*)$')
DIRECTIVES = ('byte', 'word', 'long', 'allocb', 'allocw', 'allocl', 'align')


def run(arch, srcs, cflags, sim_args, maxsteps, tmp):
    asm = os.path.join(tmp, 'p.s')
    subprocess.run([os.path.join(ROOT, 'smallcc'), '-arch', arch, '-g', *cflags, '-o', asm, *srcs], check=True)
    pc, im = os.path.join(tmp, 'p.pc'), os.path.join(tmp, 'p.imap')
    r = subprocess.run([os.path.join(ROOT, 'sim_c'), '-arch', arch, '-maxsteps', str(maxsteps), *sim_args,
                        '-pccount', pc, '-imap', im, asm], capture_output=True, text=True)
    if 'H:1' not in r.stdout:
        sys.exit('sim_c did not halt:\n' + r.stdout[-400:] + r.stderr[-400:])
    counts = {int(a, 16): int(c) for a, c in (l.split() for l in open(pc))}
    imap = [(int(a, 16), m) for a, n, m in (l.split() for l in open(im))]
    return asm, counts, imap


def parse_chain(text):
    """'F L @ F L @ ...' -> ((file, line), ...) innermost first."""
    out = []
    for part in text.split(' @ '):
        f, _, l = part.strip().rpartition(' ')
        out.append((os.path.realpath(f), int(l)))
    return tuple(out)


def functions_of(path):
    """[(first line, last line, name)] of the function definitions in a C file (by brace depth)."""
    try:
        text = open(path).read()
    except OSError:
        return []
    text = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), text, flags=re.S)
    text = re.sub(r'//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', '', text)
    out, depth, pending, cur, start = [], 0, None, None, 0
    hdr = re.compile(r'^(?:[A-Za-z_][\w \t\*]*?[\s\*])?\**(\w+)\s*\(')   # also a name alone on its line
    lines = text.split('\n')
    i = 0
    while i < len(lines):                    # multi-line macros: their code is named #NAME
        m = re.match(r'^\s*#\s*define\s+(\w+)', lines[i])
        if m and lines[i].rstrip().endswith('\\'):
            j = i
            while j + 1 < len(lines) and lines[j].rstrip().endswith('\\'): j += 1
            out.append((i + 1, j + 1, '#' + m.group(1)))
            i = j
        i += 1
    for i, l in enumerate(lines, 1):
        if depth == 0 and not l.startswith((' ', '\t', '#')):
            m = hdr.match(l)
            if m and m.group(1) not in ('if', 'while', 'for', 'switch', 'return', 'sizeof'):
                pending, start = m.group(1), i
        for ch in l:
            if ch == '{':
                if depth == 0 and pending: cur = pending
                depth += 1
            elif ch == '}':
                depth -= 1
                if depth == 0 and cur:
                    out.append((start, i, cur)); cur = pending = None
            elif ch == ';' and depth == 0:
                pending = None
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('srcs', nargs='+')
    ap.add_argument('--arch', default='cpu4')
    ap.add_argument('--cflags', default='', help='extra smallcc flags')
    ap.add_argument('--sim-args', default='', help='extra sim_c flags')
    ap.add_argument('--maxsteps', type=int, default=400_000_000)
    ap.add_argument('--per', type=float, default=0, help='also report costs divided by this (e.g. pixels)')
    ap.add_argument('--per-name', default='unit')
    ap.add_argument('--top', type=int, default=25, help='kernel table rows')
    ap.add_argument('--all-files', action='store_true', help='annotate library files too')
    ap.add_argument('--html', help='write a self-contained heatmap page')
    ap.add_argument('--json', help='write the numbers')
    a = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        asm, counts, imap = run(a.arch, a.srcs, a.cflags.split(), a.sim_args.split(), a.maxsteps, tmp)
        text = open(asm).read().split('\n')

    # Walk the assembly: each instruction gets the chain in force, and the
    # assembly function it is in; calls are noted for the gprof step.
    ins = []                                # (count, chain, asmfn, mnem, operands)
    chain, fn, k = (), None, 0
    for l in text:
        m = SRC_RE.match(l)
        if m:
            chain = parse_chain(m.group(1)); continue
        m = re.match(r'^([A-Za-z_]\w*):', l)
        if m:
            if not re.match(r'^_\w*_B\d+$|^_l\d+$|^_ls\d+$|^_k\d+$|^_\w+_jt\d+$|^_globals|^_bss', m.group(1)):
                fn = m.group(1); chain = ()
            continue
        t = l.split(';')[0].split()
        if not t or t[0].startswith('.') or t[0] in DIRECTIVES:
            continue
        addr, mnem = imap[k]; k += 1
        ins.append((counts.get(addr, 0), chain, fn, mnem, t[1:]))
    total = sum(c for c, *_ in ins)

    # Real calls: gprof-style inclusive cost of each assembly function.
    self_fn, entries, edges = collections.Counter(), {}, collections.defaultdict(list)
    for c, ch, fn, mnem, ops in ins:
        self_fn[fn] += c
        entries.setdefault(fn, c)           # first instruction = entry count
        if mnem == 'jl' and ops:
            edges[fn].append((ops[-1].rstrip(','), c, ch))
    memo = {}
    def incl_fn(f, stack=()):
        if f in memo: return memo[f]
        if f in stack: return self_fn[f]
        tot = self_fn[f] + sum(n / entries[g] * incl_fn(g, stack + (f,)) for g, n, _ in edges.get(f, ())
                               if entries.get(g))
        memo[f] = tot
        return tot

    # Source attribution.
    fns = {}
    def src_fn(fl):
        f, line = fl
        if f not in fns: fns[f] = functions_of(f)
        for s, e, name in fns[f]:
            if s <= line <= e: return (f, name)
        return (f, '?')
    self_l, incl_l, execs = collections.Counter(), collections.Counter(), collections.Counter()
    inst_max = collections.defaultdict(int)  # (chain) -> max count: a line's executions per inline instance
    k_incl, k_calls = collections.Counter(), collections.Counter()   # per source function
    site_incl = collections.Counter()        # (call-site chain, callee fn) -> inclusive
    site_first = {}                          # first instruction count of each inline instance
    for c, ch, fn, mnem, ops in ins:
        if not ch:
            continue
        self_l[ch[0]] += c
        inst_max[ch] = max(inst_max[ch], c)
        seen = set()
        for i, fl in enumerate(ch):
            if fl not in seen: incl_l[fl] += c; seen.add(fl)
        fset = set()
        for i, fl in enumerate(ch):
            sf = src_fn(fl)
            if sf not in fset: k_incl[sf] += c; fset.add(sf)
            if i + 1 < len(ch):              # fl is inside an inlined body called at ch[i+1:]
                key = (ch[i + 1:], sf)
                site_incl[key] += c
                site_first.setdefault(key, c)
    for ch, n in inst_max.items():
        execs[ch[0]] += n
    for key, n in site_first.items():
        k_calls[key[1]] += n
    # Real calls: the callee's inclusive cost lands on the call's chain.
    for f, lst in edges.items():
        for g, n, ch in lst:
            if not entries.get(g): continue
            share = n / entries[g] * incl_fn(g)
            seen = set()
            for fl in ch:
                if fl not in seen: incl_l[fl] += share; seen.add(fl)
            for sf in {src_fn(fl) for fl in ch}: k_incl[sf] += share
    # Real invocations: an assembly function is the source function most of
    # its code's outermost locations belong to.
    votes = collections.defaultdict(collections.Counter)
    for c, ch, fn, *_ in ins:
        if ch: votes[fn][src_fn(ch[-1])] += 1
    for f in entries:
        if votes[f]:
            k_calls[votes[f].most_common(1)[0][0]] += entries[f]

    per = a.per or 0
    def fmt(n):
        return f"{n:>12,.0f}" + (f" {n / per:9.1f}" if per else '')

    files = sorted({fl[0] for fl in self_l} | {fl[0] for fl in incl_l})
    if not a.all_files:
        user = {os.path.realpath(s) for s in a.srcs}
        files = [f for f in files if f in user] or files

    out = []
    out.append(f"{total:,} instructions executed ({a.arch})" + (f", {total / per:.1f} per {a.per_name}" if per else ''))
    out.append('')
    out.append(f"Kernels (source functions, inlined or called), by inclusive cost:")
    unit = f" per {a.per_name}" if per else ''
    out.append(f"  {'function':22} {'inclusive':>12}{' ' * 10 if per else ''} {'%':>6} {'invocations':>12} {'instr/inv':>10}")
    for sf, n in sorted(k_incl.items(), key=lambda x: -x[1])[:a.top]:
        inv = k_calls.get(sf, 0)
        out.append(f"  {sf[1]:22} {fmt(n)} {100 * n / total:6.1f} {inv:12,} {n / inv if inv else 0:10.1f}")
    out.append('')
    out.append("Inline call sites, by inclusive cost:")
    for (site, sf), n in sorted(site_incl.items(), key=lambda x: -x[1])[:a.top]:
        inv = site_first[(site, sf)]
        where = ' <- '.join(f"{os.path.basename(f)}:{l}" for f, l in site)
        out.append(f"  {sf[1]:18} at {where:48} {fmt(n)} {100 * n / total:6.1f}%  {inv:,} x {n / inv if inv else 0:.1f}")
    for f in files:
        try:
            src = open(f).read().split('\n')
        except OSError:
            continue
        out.append('')
        out.append(f"== {f}")
        out.append(f"   {'self':>12} {'inclusive':>12} {'execs':>10}  line")
        for i, l in enumerate(src, 1):
            s, n, e = self_l.get((f, i), 0), incl_l.get((f, i), 0), execs.get((f, i), 0)
            cols = f"{s:12,.0f} {n:12,.0f} {e:10,}" if (s or n) else ' ' * 36
            out.append(f"   {cols}  {i:4} {l}")
    if not a.html:
        print('\n'.join(out))

    data = {
        'arch': a.arch, 'total': total, 'per': per, 'per_name': a.per_name,
        'files': {f: open(f).read() for f in files if os.path.exists(f)},
        'lines': [[f, l, self_l.get((f, l), 0), round(incl_l.get((f, l), 0)), execs.get((f, l), 0)]
                  for (f, l) in set(self_l) | set(incl_l) if f in files],
        'kernels': [[sf[1], sf[0], round(n), k_calls.get(sf, 0),
                     next((st for st, e, nm in fns.get(sf[0], ()) if nm == sf[1]), 0)]
                    for sf, n in sorted(k_incl.items(), key=lambda x: -x[1])],
        'sites': [[sf[1], [[f, l] for f, l in site], n, site_first[(site, sf)]]
                  for (site, sf), n in sorted(site_incl.items(), key=lambda x: -x[1])],
    }
    if a.json:
        json.dump(data, open(a.json, 'w'))
    if a.html:
        tpl = open(os.path.join(ROOT, 'tools', 'srcprof.html')).read()
        stem = os.path.splitext(os.path.basename(a.srcs[-1]))[0]
        tpl = tpl.replace('<title>Cycle Heatmap</title>', f'<title>{html.escape(stem)} cycle map</title>', 1)
        open(a.html, 'w').write(tpl.replace('/*DATA*/null', json.dumps(data).replace('</', '<\\/')))
        print(f"wrote {a.html}")


if __name__ == '__main__':
    main()
