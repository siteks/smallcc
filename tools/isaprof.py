#!/usr/bin/env python3
"""Instruction-set profile of compiled programs, for ISA design.

For each workload the program is compiled for the ISA (smallcc -arch), run
on sim_c (-pccount gives the execution count of every pc) and its code is
decoded through the ISA definition (isatool/model.py), so every number
comes from the real encoding, whatever the ISA:

  mnemonics    static count and bytes, dynamic share per workload
  formats      the same per format (instruction length and shape)
  immediates   for every immediate field: how many bits its values need,
               as cumulative "fits in k bits" percentages up to the field
               width, static and dynamic. A field that is rarely full is a
               candidate to narrow; one that is often full, to widen (the
               values that did not fit took a longer sequence and are not
               visible here, see `wide constants` below). An absolute jump
               target is also shown as the PC-relative displacement it would
               need (`j.imm->pcrel`)
  registers    how often every register operand is below r8 (the case for
               3-bit register fields), and the use of each register
  two-address  for three-register instructions, how often rd equals a source
  wide consts  immw + immwh pairs: 32-bit constants no immediate field held

Workloads (default: all): the corpus (every tests/cases program, each
weighted equally in the dynamic average), CoreMark, the ray tracer and the
NanoJPEG decode.

  tools/isaprof.py --arch cpu5
  tools/isaprof.py --arch cpu4 --json prof4.json --only coremark,raytracer
  tools/isaprof.py --arch cpu5 prog.c other.s    # just these programs
"""
import argparse, collections, json, os, re, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from isatool import model

MONO = os.path.dirname(os.path.dirname(ROOT))
SMALLCC, SIM = os.path.join(ROOT, 'smallcc'), os.path.join(ROOT, 'sim_c')
SIGNED = ('simm', 'index', 'bytes', 'pcrel')


def sbits(v):
    b = 1
    while not (-(1 << (b - 1)) <= v <= (1 << (b - 1)) - 1):
        b += 1
    return b


def ubits(v):
    return max(1, v.bit_length())


def bits_needed(kind, raw, width):
    """Bits the field value needs under its kind (raw = the unsigned field)."""
    sv = raw - (1 << width) if raw >> (width - 1) else raw
    if kind in SIGNED:
        return sbits(sv)
    if kind == 'uimm':
        return ubits(raw)
    return min(sbits(sv), ubits(raw))          # raw / abs: either reading


class Workload:
    def __init__(self, name, sources, cflags=(), sim_args=(), target='sim'):
        self.name, self.sources, self.cflags, self.sim_args, self.target = \
            name, list(sources), list(cflags), list(sim_args), target


def corpus_workloads():
    out = []
    for dirpath, _, files in os.walk(os.path.join(ROOT, 'tests', 'cases')):
        for f in sorted(files):
            if not f.endswith('.c'):
                continue
            p = os.path.join(dirpath, f)
            meta = {}
            for line in open(p):
                if not line.startswith('//'):
                    break
                m = re.match(r'//\s*([A-Z0-9_]+):?\s*(.*)', line)
                if m:
                    meta[m.group(1)] = m.group(2).strip()
            if not any(k.startswith('EXPECT_R0') or k == 'EXPECT_STDOUT' for k in meta):
                continue
            if 'EXPECT_COMPILE_FAIL' in meta or 'XFAIL' in meta:
                continue
            srcs = [os.path.join(dirpath, x) for x in meta['FILES'].split()] if 'FILES' in meta else [p]
            out.append(Workload(os.path.relpath(p, os.path.join(ROOT, 'tests', 'cases')), srcs,
                                meta.get('CFLAGS', '').split(), meta.get('SIM_ARGS', '').split(),
                                meta.get('TARGET', 'sim')))
    return out


def named_workloads():
    return {
        'coremark': Workload('coremark', [os.path.join(ROOT, 'bench', 'coremark', 'coremark_single.c')]),
        'raytracer': Workload('raytracer', [os.path.join(MONO, 'sw', 'raytracer', 'src', 'raytracer.c')],
                              sim_args=['-maxsteps', '200000000']),
        'jpeg': Workload('jpeg', [os.path.join(ROOT, 'tests', 'cases', 'jpeg', 'jpeg_test.c')],
                         sim_args=['-maxsteps', '100000000']),
    }


class Profiler:
    def __init__(self, arch):
        self.arch = arch
        self.isa = model.load(arch)
        self.decode_cache = {}
        # instructions whose absolute operand is a jump target (they write PC)
        from isatool import sem
        self.jumps = {i.name for i in self.isa.instrs
                      if i.name in self.isa.semantics and 'PC' in sem.analyse(self.isa, i)['writes']}

    def decode(self, bs):
        key = bytes(bs)
        if key not in self.decode_cache:
            i, w = self.isa.decode_bytes(lambda a: key[a] if a < len(key) else 0, 0)
            if i is None or i.len != len(key):
                self.decode_cache[key] = None
            else:
                fields = []
                for o in i.ops:
                    raw = self.isa.field(o, w, i.fmt.len)
                    fields.append((o, raw))
                self.decode_cache[key] = (i, fields)
        return self.decode_cache[key]

    def run(self, wl, tmp):
        asm = os.path.join(tmp, 'p.s')
        if wl.sources[0].endswith('.s'):
            asm = wl.sources[0]
        else:
            cmd = [SMALLCC, '-arch', self.arch, '-target', wl.target, *wl.cflags, '-o', asm, *wl.sources]
            p = subprocess.run(cmd, capture_output=True, text=True)
            if p.returncode:
                return None, f'compile failed: {p.stderr.strip()[:200]}'
        pc = os.path.join(tmp, 'p.pc')
        imap = os.path.join(tmp, 'p.imap')
        hexf = os.path.join(tmp, 'p.hex')
        args = [a for a in wl.sim_args]
        if '-maxsteps' not in args:
            args += ['-maxsteps', '10000000']
        # The assembler's own instruction map: exact boundaries, even where
        # data sits between instructions (a linear disassembly loses sync).
        subprocess.run([SIM, '-arch', self.arch, *args, '-pccount', pc, '-imap', imap, asm], capture_output=True)
        subprocess.run([SIM, '-arch', self.arch, '-hex', hexf, asm], capture_output=True)
        counts = {}
        if os.path.exists(pc):
            for line in open(pc):
                a, c = line.split()
                counts[int(a, 16)] = int(c)
        image = [int(x, 16) for x in open(hexf).read().split()]
        static = []
        for line in open(imap):
            a, n, _ = line.split()
            a, n = int(a, 16), int(n)
            static.append((a, image[a:a + n]))
        return (static, counts), None


class Stats:
    """Accumulated over the instructions of one workload (static and dynamic)."""
    def __init__(self):
        self.mn_static = collections.Counter(); self.mn_bytes = collections.Counter()
        self.mn_dyn = collections.Counter()
        self.fmt_static = collections.Counter(); self.fmt_bytes = collections.Counter()
        self.fmt_dyn = collections.Counter()
        # (mnemonic, operand) -> [static Counter(bits), dynamic Counter(bits), width, kind]
        self.imm = {}
        self.reg_low_static = [0, 0]; self.reg_low_dyn = [0, 0]      # [all < 8, total]
        self.reg_use_static = collections.Counter(); self.reg_use_dyn = collections.Counter()
        self.two_static = [0, 0]; self.two_dyn = [0, 0]              # [rd == a source, total]
        self.unknown = 0
        self.dyn_total = 0
        self.wide_static = 0; self.wide_dyn = 0
        self.nprog = 0; self.dyn_raw = 0

    def add(self, prof, static, counts):
        prev = None
        for addr, bs in static:
            d0 = prof.decode(bs)
            if d0 and prev and d0[0].name == 'immwh' and prev[0].name == 'immw':
                self.wide_static += 1; self.wide_dyn += counts.get(addr, 0)
            prev = d0
        for addr, bs in static:
            d = prof.decode(bs)
            n = counts.get(addr, 0)
            if d is None:
                self.unknown += 1
                continue
            i, fields = d
            self.dyn_total += n
            self.mn_static[i.name] += 1; self.mn_bytes[i.name] += i.len; self.mn_dyn[i.name] += n
            fk = f'{i.fmt.name} ({i.len}B)'
            self.fmt_static[fk] += 1; self.fmt_bytes[fk] += i.len; self.fmt_dyn[fk] += n
            regs = [raw for o, raw in fields if o.reg]
            if regs:
                low = all(r < 8 for r in regs)
                self.reg_low_static[0] += low; self.reg_low_static[1] += 1
                self.reg_low_dyn[0] += low * n; self.reg_low_dyn[1] += n
                for r in regs:
                    self.reg_use_static[r] += 1; self.reg_use_dyn[r] += n
            if len(regs) == 3:
                two = regs[0] in (regs[1], regs[2])
                self.two_static[0] += two; self.two_static[1] += 1
                self.two_dyn[0] += two * n; self.two_dyn[1] += n
            for o, raw in fields:
                if o.reg:
                    continue
                key = (i.name, o.name)
                e = self.imm.setdefault(key, [collections.Counter(), collections.Counter(), o.bits, o.kind])
                b = bits_needed(o.kind, raw, o.bits)
                e[0][b] += 1; e[1][b] += n
                if o.kind == 'abs' and i.name in prof.jumps:
                    # the same transfer as a PC-relative displacement
                    disp = raw - (addr + i.len)
                    key = (i.name, o.name + '->pcrel')
                    e = self.imm.setdefault(key, [collections.Counter(), collections.Counter(), 16, 'pcrel'])
                    b = sbits(disp)
                    e[0][b] += 1; e[1][b] += n


def ctotal(c):
    return sum(c.values())


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def fits_line(hist, width):
    tot = ctotal(hist)
    if not tot:
        return ''
    cells, run = [], 0
    for b in range(1, width + 1):
        run += hist.get(b, 0)
        cells.append(f'{pct(run, tot):5.1f}')
    return ' '.join(cells)


def report(arch, results, top):
    names = list(results)
    print(f'# ISA profile: {arch}\n')
    print('Workloads: ' + ', '.join(f'{n} ({results[n].nprog} program{"s" if results[n].nprog > 1 else ""}, '
                                   f'{results[n].dyn_raw:,} instructions)' for n in names) + '\n')
    print('The corpus column weights each program equally.\n')
    allmn = collections.Counter()
    for s in results.values():
        allmn.update({k: v for k, v in s.mn_static.items()})
    # mnemonics
    first = results[names[0]]
    print('## Mnemonics\n')
    hdr = f'{"mnemonic":10s} {"static":>7s} {"bytes":>7s} ' + ' '.join(f'{n[:10]:>10s}' for n in names)
    print('Dynamic share (%) per workload; static count and bytes over all workloads.\n')
    print('```'); print(hdr)
    order = sorted(allmn, key=lambda m: -max(pct(results[n].mn_dyn[m], results[n].dyn_total) for n in names))
    for m in order[:top]:
        st = sum(results[n].mn_static[m] for n in names); by = sum(results[n].mn_bytes[m] for n in names)
        print(f'{m:10s} {st:7d} {by:7d} ' + ' '.join(f'{pct(results[n].mn_dyn[m], results[n].dyn_total):10.2f}' for n in names))
    never = [i.name for i in model.load(arch).instrs if not any(results[n].mn_static[i.name] for n in names)]
    print('```\n')
    if never:
        print('Never emitted: ' + ', '.join(never) + '\n')
    # formats
    print('## Formats\n```')
    allf = sorted(set(k for s in results.values() for k in s.fmt_static))
    print(f'{"format":12s} {"static":>7s} {"bytes":>7s} ' + ' '.join(f'{n[:10]:>10s}' for n in names))
    for fk in allf:
        st = sum(results[n].fmt_static[fk] for n in names); by = sum(results[n].fmt_bytes[fk] for n in names)
        print(f'{fk:12s} {st:7d} {by:7d} ' + ' '.join(f'{pct(results[n].fmt_dyn[fk], results[n].dyn_total):10.2f}' for n in names))
    tot_b = sum(results[n].fmt_bytes[fk] for n in names for fk in allf)
    tot_s = sum(results[n].fmt_static[fk] for n in names for fk in allf)
    print(f'{"total":12s} {tot_s:7d} {tot_b:7d}   mean {tot_b / max(tot_s, 1):.2f} bytes/instruction')
    print('```\n')
    # immediates
    print('## Immediate fields\n')
    print('Cumulative % of values that fit in 1, 2, ... bits (up to the field width), static over all')
    print('workloads, then dynamic for the busiest workload that uses it.\n```')
    keys = sorted(set(k for s in results.values() for k in s.imm),
                  key=lambda k: -sum(ctotal(results[n].imm[k][0]) for n in names if k in results[n].imm))
    for k in keys:
        width, kind = next(results[n].imm[k][2:] for n in names if k in results[n].imm)
        st = collections.Counter()
        for n in names:
            if k in results[n].imm:
                st.update(results[n].imm[k][0])
        dn = max(names, key=lambda n: ctotal(results[n].imm[k][1]) if k in results[n].imm else -1)
        dy = results[dn].imm[k][1]
        print(f'{k[0]}.{k[1]} ({kind}{width}) static n={ctotal(st)}: {fits_line(st, width)}')
        if ctotal(dy):
            print(f'{"":{len(k[0]) + len(k[1]) + 1}} dynamic ({dn}) n={ctotal(dy):.0f}: {fits_line(dy, width)}')
    print('```\n')
    # registers
    print('## Registers\n```')
    for n in names:
        s = results[n]
        print(f'{n:12s} register operands all below r8: static {pct(*s.reg_low_static):5.1f}%  '
              f'dynamic {pct(*s.reg_low_dyn):5.1f}%   two-address fit (rd == a source): '
              f'static {pct(*s.two_static):5.1f}%  dynamic {pct(*s.two_dyn):5.1f}%')
    for n in names:
        s = results[n]
        print(f'{n:12s} 32-bit constants (immw + immwh): {s.wide_static} static, '
              f'{pct(s.wide_dyn, s.dyn_total):.2f}% of executed instructions')
    ruse = collections.Counter()
    for s in results.values():
        ruse.update(s.reg_use_static)
    tot = sum(ruse.values())
    print('static use by register: ' + ' '.join(f'r{r}:{pct(ruse[r], tot):.1f}' for r in sorted(ruse)))
    print('```')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('programs', nargs='*', help='.c or .s files (default: the standard workloads)')
    ap.add_argument('--arch', default='cpu4')
    ap.add_argument('--only', help='comma-separated: corpus,coremark,raytracer,jpeg')
    ap.add_argument('--top', type=int, default=60, help='mnemonics to list')
    ap.add_argument('--json', metavar='FILE', help='also write the numbers as JSON')
    a = ap.parse_args()
    prof = Profiler(a.arch)
    groups = collections.OrderedDict()
    if a.programs:
        for p in a.programs:
            groups[os.path.basename(p)] = [Workload(os.path.basename(p), [os.path.abspath(p)])]
    else:
        only = a.only.split(',') if a.only else ['corpus', 'coremark', 'raytracer', 'jpeg']
        named = named_workloads()
        for w in only:
            groups[w] = corpus_workloads() if w == 'corpus' else [named[w]]
    results = collections.OrderedDict()
    with tempfile.TemporaryDirectory() as tmp:
        for g, wls in groups.items():
            st = Stats()
            if g == 'corpus':
                # each program weighted equally in the dynamic numbers: scale
                # its counts to a common total
                for wl in wls:
                    r, err = prof.run(wl, tmp)
                    if not r:
                        print(f'skip {wl.name}: {err}', file=sys.stderr); continue
                    static, counts = r
                    tot = sum(counts.values()) or 1
                    st.nprog += 1; st.dyn_raw += tot
                    counts = {k: v * 1_000_000 / tot for k, v in counts.items()}
                    st.add(prof, static, counts)
            else:
                for wl in wls:
                    r, err = prof.run(wl, tmp)
                    if not r:
                        print(f'skip {wl.name}: {err}', file=sys.stderr); continue
                    st.add(prof, *r)
                    st.nprog += 1; st.dyn_raw += sum(r[1].values())
            results[g] = st
    report(a.arch, results, a.top)
    if a.json:
        out = {'arch': a.arch, 'workloads': {}}
        for n, s in results.items():
            out['workloads'][n] = {
                'dynamic_total': s.dyn_total,
                'mnemonics': {m: {'static': s.mn_static[m], 'bytes': s.mn_bytes[m], 'dynamic': s.mn_dyn[m]}
                              for m in s.mn_static},
                'formats': {f: {'static': s.fmt_static[f], 'bytes': s.fmt_bytes[f], 'dynamic': s.fmt_dyn[f]}
                            for f in s.fmt_static},
                'immediates': {f'{k[0]}.{k[1]}': {'kind': v[3], 'width': v[2],
                                                  'static_bits': dict(v[0]), 'dynamic_bits': dict(v[1])}
                               for k, v in s.imm.items()},
                'registers_below_r8': {'static': s.reg_low_static, 'dynamic': s.reg_low_dyn},
                'two_address': {'static': s.two_static, 'dynamic': s.two_dyn},
            }
        json.dump(out, open(a.json, 'w'), indent=1, default=float)


if __name__ == '__main__':
    main()
