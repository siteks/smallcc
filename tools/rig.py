#!/usr/bin/env python3
"""Random instruction generator (RIG) and lockstep checker, for any ISA defined
by an <arch>/isa.py.

Generates constrained-random assembly programs from the ISA definition and
runs each on two executors, comparing them instruction by instruction through
their retirement traces:

  sim_c -arch A   the C executor generated from A/isa.py SEMANTICS (A/exec_gen.h)
  pysim --arch A  the Python executor generated from the same lines (A/exec_gen.py)

Both come from one description, so agreement checks the two generators, the
two primitive implementations (for cpu4 the float model, fpu_model.h and its
port fpu_model.py) and the two memory/state models against each other; a
disagreement names the instruction and the operands. The retirement trace
(one line per retired instruction: pc, bytes, every register and memory
change, next pc) is also the contract a retirement port on the RTL bench is
meant to produce, which is how the RTL joins the comparison.

Nothing here names an instruction. Each one is placed in a group from what
its semantics line touches (isatool/sem.py analyse) and its operand shapes:
alu (registers only), load/store (bp- or register-relative, from the address
expression), branch (conditional pc-relative or absolute), loop (a branch that
also writes its register: dbnz), call/icall/ijump/return, push/pop, adjust
(SP by an immediate), prologue (writes SP, BP and memory from LR: enter),
setsp, out and halt. `--show` prints the groups. An instruction with no
semantics yet is left out and listed.

Constraints keep every program terminating and every access legal, while
leaving operand values and instruction mixes as random as possible:
  * forward branches only, plus bounded loops;
  * bp-relative accesses land, aligned, in a data window around BP (0x9000);
  * register-relative accesses set their base register immediately before,
    so the address lands in a second window whatever the offset;
  * calls go to generated leaf functions (prologue ... return);
  * push/pop and SP adjustments come in balanced pairs; setsp only in the preamble.
Registers start from a mix of random and edge values (0, 1, -1, INT_MIN,
INT_MAX, float specials) so the edge cases of the semantics are reached.

  tools/rig.py -n 200 -len 300 -seed 1           # 200 programs, cpu4
  tools/rig.py --arch cpu5 -n 50                  # another ISA
  tools/rig.py --show                             # the groups and a sample program
  tools/rig.py --keep out/                        # save every program that fails
"""
import argparse, collections, contextlib, io, os, random, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
from isatool import model, sem, pysim

BP_ADDR, STACK = 0x9000, 0xF000
BP_WIN = (0x8800, 0x9800)                          # bp-relative accesses land here
REG_WIN = (0x9C00, 0xB800)                         # register-relative accesses land here
WINDOWS = [BP_WIN, REG_WIN]                        # data initialised with random bytes
EDGE = [0, 1, 2, 0xffffffff, 0xfffffffe, 0x80000000, 0x7fffffff, 0x0000ffff, 0xffff0000, 0x00008000,
        0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x00800000, 0x7f7fffff, 0x00000001, 0x4b800000]


# ---------------------------------------------------------------- classification from the semantics
class Unevaluable(Exception):
    pass


def evaluate(n, env):
    """Value of a semantics expression; env holds 'SP'/'BP'/... , ('R', operand) and operand names (raw fields)."""
    k = n[0]
    if k == 'num': return n[1]
    if k == 'spec': return env[n[1]]
    if k == 'operand': return env[n[1]]
    if k == 'reg': return env[('R', n[1][1])]
    if k == 'bin':
        a, b = evaluate(n[2], env), evaluate(n[3], env)
        ops = {'+': lambda: a + b, '-': lambda: a - b, '*': lambda: a * b, '<<': lambda: a << b,
               '&': lambda: a & b, '|': lambda: a | b}
        if n[1] not in ops: raise Unevaluable(n[1])
        return ops[n[1]]() & 0xffffffff
    if k == 'call' and n[1] == 'sext':
        name = n[2][0][1]; bits = env['#bits'][name]; v = env[name] & ((1 << bits) - 1)
        return (v - (1 << bits) if v >> (bits - 1) else v) & 0xffffffff
    if k == 'call' and n[1] == 'lo16':
        return evaluate(n[2][0], env) & 0xffff
    raise Unevaluable(k)


def signed(v):
    return v - (1 << 32) if v & 0x80000000 else v


class Kind:
    """What rig does with each instruction, derived from sem.analyse and the operand shapes."""
    def __init__(self, isa):
        self.isa = isa
        self.groups = collections.defaultdict(list)
        self.info = {}
        for i in isa.instrs:
            a = sem.analyse(isa, i)
            self.info[i.name] = a
            g = self.classify(i, a)
            if g:
                self.groups[g].append(i)
        self.set16 = self.pick_set16()
        self.sethi = self.pick_sethi()
        self.sp_deltas = {i.name: self.sp_table(i) for i in self.groups['adjust'] + self.groups['prologue']}
        self.mem = {}
        for g in ('load-bp', 'store-bp', 'load-reg', 'store-reg', 'load-abs', 'store-abs'):
            for i in self.groups[g]:
                self.mem[i.name] = next(_mems(sem.parse(isa, i)))

    def classify(self, i, a):
        if a.get('unspecified'):
            return None
        w, r = a['writes'], a['reads']
        regs = [o for o in i.ops if o.reg]
        imms = [o for o in i.ops if not o.reg]
        if 'H' in w: return 'halt' if not i.ops else None
        if 'out' in w: return 'out'
        if 'PC' in w:
            f = a['pc_forms']
            if f == {'mem'}: return 'return' if not i.ops else None
            if f == {'rel'} and a['cond'] and any(o.kind == 'pcrel' for o in imms):
                if 'R' in w: return 'loop' if len(regs) == 1 and len(imms) == 1 else None
                return 'branch'
            if f == {'imm'} and any(o.kind in ('abs', 'pcrel', 'raw') for o in imms):
                if 'LR' in w: return 'call'
                return 'branch'
            if f == {'reg'} and len(regs) == 1 and not imms:
                return 'icall' if 'LR' in w else 'ijump'
            return None
        if 'SP' in w:
            if w == {'SP'} and r == {'R'} and len(regs) == 1 and not imms: return 'setsp'
            if w == {'SP', 'M'} and 'R' in r and len(regs) == 1 and not imms: return 'push'
            if w == {'SP', 'R'} and 'M' in r and len(regs) == 1 and not imms: return 'pop'
            if w == {'SP'} and r == {'SP'} and len(imms) == 1 and not regs: return 'adjust'
            if w == {'SP', 'BP', 'M'} and 'LR' in r and len(imms) == 1 and not regs: return 'prologue'
            return None
        if w - {'R', 'M'} or 'LR' in r or 'PC' in r:
            return None
        if 'M' in w or 'M' in r:
            if len(a['mem_addr']) != 1: return None
            addr = next(_mems(sem.parse(self.isa, i)))[0]
            base = 'bp' if 'BP' in r else 'reg' if next(_regs_in(addr), None) else 'abs'
            return ('store' if 'M' in w else 'load') + '-' + base
        return 'alu'

    def pick_set16(self):
        """An instruction loading a register with a 16-bit (or wider) constant: one register, one immediate, reads nothing."""
        for i in self.groups['alu']:
            imms = [o for o in i.ops if not o.reg]
            if len([o for o in i.ops if o.reg]) == 1 and len(imms) == 1 and imms[0].bits >= 16 \
                    and not self.info[i.name]['reads'] and imms[0].kind in model.LABEL_KINDS + ('uimm',):
                return i
        raise SystemExit(f"rig: {self.isa.name} has no instruction that loads a 16-bit constant")

    def pick_sethi(self):
        for i in self.groups['alu']:
            imms = [o for o in i.ops if not o.reg]
            sm = self.isa.semantics[i.name]      # sets the high half and keeps the low (immwh, not a lui)
            if len(i.ops) == 2 and len(imms) == 1 and imms[0].bits >= 16 and '<< 16' in sm and sm.count('R[') >= 2:
                return i
        return None

    def sp_table(self, i):
        """{SP delta: source operand value} for small word-multiple deltas."""
        o = next(o for o in i.ops if not o.reg)
        stmt = next(s for s in sem.parse(self.isa, i) if s[0] != 'if' and s[1] == ('spec', 'SP') and s[2] != ('spec', 'BP'))
        lo, hi = o.field_range()
        out = {}
        for f in range(max(lo, -512), min(hi, 512) + 1):
            try:
                d = signed(evaluate(stmt[2], {'SP': 0, o.name: f & ((1 << o.bits) - 1), '#bits': {o.name: o.bits}}))
            except Unevaluable:
                return {}
            if d % 4 == 0 and -64 <= d <= 64 and d not in out:
                out[d] = f * o.scale
        return out


class Prog:
    def __init__(self, kind, rnd, length):
        self.k, self.isa, self.r, self.length, self.lab, self.funcs = kind, kind.isa, rnd, length, 0, []
        self.regs = [f"r{i}" for i in range(self.isa.ngpr)]

    def label(self):
        self.lab += 1
        return f"L{self.lab}"

    def reg(self, avoid=()):
        return self.r.choice([x for x in self.regs if x not in avoid])

    def imm(self, o):
        """A source value for an immediate operand, biased to the edges of its range."""
        lo, hi = o.field_range()
        if o.kind == 'raw':
            lo = 0
        v = self.r.choice([lo, hi, -1 if lo < 0 else 1, 0, 1, self.r.randint(lo, hi), self.r.randint(lo, hi)])
        return v * o.scale

    def value(self):
        return self.r.choice(EDGE) if self.r.random() < 0.4 else self.r.getrandbits(32)

    def set16(self, reg, v):
        return [f"{self.k.set16.name} {reg}, {v}"]

    def load32(self, reg, v):
        out = self.set16(reg, f"0x{v & 0xffff:04x}")
        if self.k.sethi:
            out.append(f"{self.k.sethi.name} {reg}, 0x{(v >> 16) & 0xffff:04x}")
        return out

    def operands(self, i, avoid_dst=(), fixed=None):
        fixed = fixed or {}
        out = []
        for o in i.ops:
            if o.name in fixed: out.append(str(fixed[o.name]))
            elif o.reg: out.append(self.reg(avoid_dst if o.name == i.ops[0].name else ()))
            else: out.append(str(self.imm(o)))
        return f"{i.name} " + ", ".join(out) if out else i.name

    def mem_item(self, i, base, avoid_dst):
        """A load or store whose address lands, naturally aligned, in its data window."""
        addr, size = self.k.mem[i.name]
        imm_ops = [o for o in i.ops if not o.reg]
        is_store = 'M' in self.k.info[i.name]['writes']
        rop = next(_regs_in(addr), None) if base == 'reg' else None
        if base == 'abs':                  # the immediate is the address: pick one in the window
            if len(imm_ops) != 1: return None
            a = self.r.randrange(REG_WIN[0], REG_WIN[1] - size, size)
            o = imm_ops[0]
            env = {'BP': BP_ADDR, '#bits': {o.name: o.bits}, o.name: (a // o.scale) & ((1 << o.bits) - 1)}
            try:
                if signed(evaluate(addr, env)) & 0xffff != a: return None
            except Unevaluable:
                return None
            fixed = {o.name: f"0x{a:04x}"}
            for q in i.ops:
                if q.reg: fixed[q.name] = self.reg() if is_store else self.reg(avoid_dst)
            return [self.operands(i, fixed=fixed)]
        for _ in range(40):
            vals = {o.name: self.imm(o) for o in imm_ops}
            env = {'BP': BP_ADDR, '#bits': {o.name: o.bits for o in imm_ops}}
            for o in imm_ops:
                env[o.name] = (vals[o.name] // o.scale) & ((1 << o.bits) - 1)
            if rop:
                env[('R', rop)] = 0
            try:
                off = signed(evaluate(addr, env))
            except Unevaluable:
                return None
            fixed = dict(vals)
            if base == 'bp':
                if BP_WIN[0] <= off <= BP_WIN[1] - size and off % size == 0:     # off is the address here
                    return [self.operands(i, () if is_store else avoid_dst, fixed)]
                continue
            b = self.r.randrange(REG_WIN[0], REG_WIN[1] - size, size) - off
            if 0 <= b <= 0xffff:
                fixed[rop] = self.reg(avoid_dst)
                for o in i.ops:
                    if o.reg and o.name != rop:
                        fixed[o.name] = self.reg() if is_store else self.reg(avoid_dst)
                return self.set16(fixed[rop], f"0x{b:04x}") + [self.operands(i, fixed=fixed)]
        return None

    # ---- one item = a list of assembly lines
    def simple(self, avoid_dst=(), in_function=False):
        """An item with no control flow and no stack effect."""
        g = self.k.groups
        choices = [('alu', 0.62), ('load-bp', 0.08), ('store-bp', 0.0 if in_function else 0.05),
                   ('load-reg', 0.13), ('store-reg', 0.12), ('load-abs', 0.03), ('store-abs', 0.03)]
        for _ in range(20):
            x = self.r.random() * sum(w for _, w in choices)
            for name, w in choices:
                x -= w
                if x < 0: break
            if not g[name]:
                continue
            i = self.r.choice(g[name])
            if name == 'alu':
                return [self.operands(i, avoid_dst)]
            item = self.mem_item(i, name.split('-')[1], avoid_dst)
            if item:
                return item
        return [self.operands(self.r.choice(g['alu']), avoid_dst)]

    def branch(self, target):
        i = self.r.choice(self.k.groups['branch'] + self.k.groups['ijump'])
        if i in self.k.groups['ijump']:
            t = self.reg(); return self.set16(t, target) + [f"{i.name} {t}"]
        fixed = {o.name: target for o in i.ops if o.kind in model.LABEL_KINDS and not o.reg}
        return [self.operands(i, fixed=fixed)]

    def body(self, n, in_function=False):
        """n items with forward branches, loops, calls and balanced stack pairs."""
        r, g, out, pending = self.r, self.k.groups, [], collections.defaultdict(list)
        i = 0
        while i < n:
            for lab in pending.pop(i, []):
                out.append(f"{lab}:")
            k = r.random()
            if k < 0.12 and i + 1 < n and (g['branch'] or g['ijump']):
                lab = self.label(); pending[min(n, i + r.randint(1, 6))].append(lab); out += self.branch(lab)
            elif k < 0.15 and not in_function and g['loop']:
                c = self.reg(); lab = self.label(); lp = r.choice(g['loop'])
                out += self.set16(c, r.randint(1, 5)) + [f"{lab}:"]
                for _ in range(r.randint(1, 3)):
                    out += self.simple(avoid_dst=(c,))
                out.append(f"{lp.name} {c}, {lab}")
            elif k < 0.19 and not in_function and g['prologue'] and g['return'] and (g['call'] or g['icall']):
                f = f"F{len(self.funcs)}"; self.funcs.append(f)
                calls = g['call'] + g['icall']; c = r.choice(calls)
                if c in g['call']: out.append(f"{c.name} {f}")
                else: t = self.reg(); out += self.set16(t, f) + [f"{c.name} {t}"]
            elif k < 0.22 and not in_function and g['push'] and g['pop']:
                out.append(f"{r.choice(g['push']).name} {self.reg()}")
                for _ in range(r.randint(0, 2)):
                    out += self.simple()
                out.append(f"{r.choice(g['pop']).name} {self.reg()}")
            elif k < 0.24 and not in_function and g['adjust']:
                adj = r.choice(g['adjust']); t = self.k.sp_deltas[adj.name]
                ds = [d for d in t if d < 0 and -d in t]
                if ds:
                    d = r.choice(ds); out.append(f"{adj.name} {t[d]}")
                    for _ in range(r.randint(0, 2)):
                        out += self.simple()
                    out.append(f"{adj.name} {t[-d]}")
            elif k < 0.245 and g['out']:
                out.append(self.operands(r.choice(g['out'])))
            else:
                out += self.simple(in_function=in_function)
            i += 1
        for idx in sorted(pending):
            for lab in pending[idx]:
                out.append(f"{lab}:")
        return out

    def prologue(self, frame_max):
        p = self.r.choice(self.k.groups['prologue']); t = self.k.sp_deltas[p.name]
        ds = [d for d in t if -4 - frame_max <= d < 0]
        return f"{p.name} {t[self.r.choice(ds)]}", t

    def program(self):
        r, g = self.r, self.k.groups
        if not (g['setsp'] and g['halt']):
            raise SystemExit(f"rig: {self.isa.name} needs a set-SP and a halt instruction")
        ssp, halt = g['setsp'][0].name, g['halt'][0].name
        lines = [".text=0"] + self.set16('r0', f"0x{BP_ADDR + 4:04x}") + [f"{ssp} r0"]
        if g['prologue']:
            p = g['prologue'][0]; t = self.k.sp_deltas[p.name]
            lines.append(f"{p.name} {t[-4]}")
        lines += self.set16('r0', f"0x{STACK:04x}") + [f"{ssp} r0"]
        for reg in self.regs:
            lines += self.load32(reg, self.value())
        lines += self.body(self.length)
        lines.append(halt)
        k = 0
        while k < len(self.funcs):   # functions may be added while generating (they do not call)
            f = self.funcs[k]; k += 1
            lines += [f"{f}:", self.prologue(32)[0]] + self.body(r.randint(2, 8), in_function=True) + [g['return'][0].name]
        for lo, hi in WINDOWS:
            lines.append(f".data=0x{lo:04x}")
            words = [f"0x{r.getrandbits(32):08x}" for _ in range((hi - lo) // 4)]
            for j in range(0, len(words), 8):
                lines.append("long " + " ".join(words[j:j + 8]))
        return "\n".join(l if l.endswith(':') or l.startswith('.') else "    " + l for l in lines) + "\n"


def _mems(n):
    """(address, bytes) of every memory access in a parsed semantics line."""
    if isinstance(n, tuple):
        if n and n[0] == 'mem':
            yield n[2], n[1] // 8 if n[1] > 4 else n[1]
        for c in n[1:]:
            yield from _mems(c)
    elif isinstance(n, list):
        for c in n:
            yield from _mems(c)


def _regs_in(n):
    """Register operand names read by an address expression."""
    if isinstance(n, tuple):
        if n[0] == 'reg':
            yield n[1][1]
        for c in n[1:]:
            yield from _regs_in(c)
    elif isinstance(n, list):
        for c in n:
            yield from _regs_in(c)


# ---------------------------------------------------------------- running and comparing
SIM = os.path.join(ROOT, 'sim_c')


def mnemonic(isa, hexbytes):
    b = bytes.fromhex(hexbytes)
    i, _ = isa.decode_bytes(lambda a: b[a] if a < len(b) else 0, 0)
    return i.name if i else '?'


def run_sim(arch, asm, maxsteps):
    with tempfile.NamedTemporaryFile(suffix='.ret', delete=False) as t:
        ret = t.name
    p = subprocess.run([SIM, '-arch', arch, '-maxsteps', str(maxsteps), '-retire', ret, asm],
                       capture_output=True, text=True, errors="replace")
    trace = open(ret).read().splitlines(); os.unlink(ret)
    final = next((l for l in p.stdout.splitlines() if l.startswith('r0:')), '')
    return p.returncode, trace, final, p.stderr


def run_pysim(arch, asm, maxsteps):
    """The Python simulator in-process (its putchar output is discarded)."""
    buf = io.StringIO()
    try:
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            st = pysim.runfile(asm, maxsteps=maxsteps, retire=buf, arch=arch)
        code = 0
    except SystemExit as e:
        st, code = None, e.code or 1
    except Exception as e:
        return 1, buf.getvalue().splitlines(), 'EXC ' + type(e).__name__ + ': ' + str(e)[:80]
    return code, buf.getvalue().splitlines(), (repr(st) if st is not None else '')


def first_diff(a, b):
    for k, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return k, x, y
    return (min(len(a), len(b)), a[len(b)] if len(a) > len(b) else '<end>', b[len(a)] if len(b) > len(a) else '<end>')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-n', type=int, default=100, help='programs to generate')
    ap.add_argument('-len', type=int, default=300, help='items per program body')
    ap.add_argument('-seed', type=int, default=1)
    ap.add_argument('--keep', metavar='DIR', help='save each failing program here')
    ap.add_argument('--maxsteps', type=int, default=200000)
    ap.add_argument('--arch', default='cpu4', help='ISA (a directory holding isa.py)')
    ap.add_argument('--show', action='store_true', help='print how each instruction is used and one program, then stop')
    a = ap.parse_args()
    kinds = Kind(model.load(a.arch)); isa = kinds.isa
    if a.show:
        for g, l in sorted(kinds.groups.items()):
            print(f"{g:10s} {' '.join(i.name for i in l)}")
        print(Prog(kinds, random.Random(a.seed), 40).program().split('.data')[0])
        return
    cover = collections.Counter(); fails = collections.Counter(); retired = 0; examples = {}
    for k in range(a.n):
        seed = a.seed * 1000003 + k
        src = Prog(kinds, random.Random(seed), a.len).program()
        with tempfile.NamedTemporaryFile('w', suffix='.s', delete=False) as f:
            f.write(src); asm = f.name
        g = run_sim(a.arch, asm, a.maxsteps); c = run_pysim(a.arch, asm, a.maxsteps)
        problems = []
        if g[0] != 0:
            problems.append(('sim_c-error', g[3].strip().splitlines()[-1:] if g[3].strip() else [f'exit {g[0]}']))
        if g[1] != c[1] or not g[2].startswith(c[2][:c[2].find(' H:') + 4] if ' H:' in c[2] else '\x00'):
            if c[2].startswith('EXC') or c[0]:
                problems.append(('pysim-error', [c[2] or f'exit {c[0]}', f'after {len(c[1])} instructions']))
            else:
                i, x, y = first_diff(g[1], c[1])
                problems.append(('sim_c-vs-pysim', [f'instruction {i} (' + (mnemonic(isa, x.split()[1]) if x != '<end>' else '?') + ')',
                                                    f'sim_c  {x}', f'pysim  {y}']))
        for line in g[1]:
            cover[mnemonic(isa, line.split()[1])] += 1
        retired += len(g[1])
        for kind, detail in problems:
            fails[kind] += 1
            examples.setdefault(kind, (seed, detail))
            if a.keep:
                os.makedirs(a.keep, exist_ok=True)
                open(os.path.join(a.keep, f'{kind}-{seed}.s'), 'w').write(src)
        os.unlink(asm)
    names = [i.name for i in isa.instrs if i.name in isa.semantics]
    missing = [n for n in names if not cover[n]]
    print(f"{a.arch}: {a.n} programs, {retired:,} retired instructions; {len(names) - len(missing)}/{len(names)} "
          f"specified instructions executed" + (f" (never: {', '.join(missing)})" if missing else ""))
    if isa.missing_semantics:
        print(f"not generated, no semantics yet: {', '.join(isa.missing_semantics)}")
    if not fails:
        print("no differences")
    for kind, n in fails.items():
        seed, detail = examples[kind]
        print(f"{kind}: {n} program(s); first at seed {seed}:")
        for d in detail:
            print("    " + d)
    sys.exit(1 if fails else 0)


if __name__ == '__main__':
    main()
