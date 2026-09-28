#!/usr/bin/env python3
"""Random instruction generator (RIG) and lockstep checker for CPU4.

Generates constrained-random CPU4 assembly programs from the instruction table
in cpu4/isa.py, runs each on two or more executors, and compares them
instruction by instruction:

  hand   sim_c's hand-written executor              (sim_c -retire)
  gen    the executor generated from isa.SEMANTICS  (sim_c -gen -retire)
  cpupy  cpu4/cpu.py, final architectural state only (--cpupy)

The retirement trace (one line per retired instruction: pc, bytes, every
register and memory change, next pc) is the comparison contract; a
retirement port on the RTL bench is meant to produce the same lines.

Constraints keep every program terminating and every access legal, while
leaving operand values and instruction mixes as random as possible:
  * forward branches only, plus bounded dbnz loops;
  * bp-relative accesses go to a data window around BP (0x9000);
  * register-relative accesses set their base register immediately before;
  * calls go to generated leaf functions (enter ... ret);
  * pushr/popr and adjw come in balanced pairs; ssp only in the preamble.
Registers start from a mix of random and edge values (0, 1, -1, INT_MIN,
INT_MAX, float specials) so the edge cases of the semantics are reached.

  tools/rig.py -n 200 -len 300 -seed 1           # 200 programs
  tools/rig.py -n 50 --cpupy                      # also compare with cpu.py
  tools/rig.py --keep out/                        # save every program that fails
"""
import argparse, collections, hashlib, os, random, re, subprocess, sys, tempfile
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'cpu4'))
import isa, sem

BP_ADDR, STACK = 0x9000, 0xF000
F3C_LO, F3C_HI = 0xA400, 0xA800                    # register-relative base range
WINDOWS = [(0x8F00, 0x9104), (0x9C00, 0xB000)]    # data initialised with random bytes
EDGE = [0, 1, 2, 0xffffffff, 0xfffffffe, 0x80000000, 0x7fffffff, 0x0000ffff, 0xffff0000, 0x00008000,
        0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x00800000, 0x7f7fffff, 0x00000001, 0x4b800000]
REGS = [f"r{i}" for i in range(8)]

G = {d['name']: d for d in isa.INSTRUCTIONS}
ALU3 = [d['name'] for d in isa.INSTRUCTIONS if d['fmt'] == 'F1a']
IMM9 = [d['name'] for d in isa.INSTRUCTIONS if d['fmt'] == 'F0b']
UNARY = ['sxb', 'sxw', 'inc', 'dec', 'zxb', 'zxw', 'itof', 'ftoi', 'neg', 'frecip', 'frsqrt']
F2ALU = ['addi', 'shli', 'andi', 'shrsi', 'imms']
F2MEM_LD, F2MEM_ST = ['lb', 'lw', 'll', 'lbx', 'lwx'], ['sb', 'sw', 'sl']
F3C_LD, F3C_ST = ['llb', 'llw', 'lll', 'llbx', 'llwx'], ['slb', 'slw', 'sll']
BR2 = ['beq', 'bne', 'blt', 'ble', 'blts', 'bles']
BR1 = ['beqz', 'bnez', 'dbnz', 'bltz', 'bgez', 'bgtz', 'blez']


class Prog:
    def __init__(self, rnd, length):
        self.r, self.length, self.lab, self.funcs = rnd, length, 0, []

    def label(self):
        self.lab += 1
        return f"L{self.lab}"

    def reg(self, avoid=()):
        return self.r.choice([x for x in REGS if x not in avoid])

    def simm(self, bits):
        lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
        return self.r.choice([lo, hi, -1, 0, 1, self.r.randint(lo, hi), self.r.randint(lo, hi)])

    def value(self):
        return self.r.choice(EDGE) if self.r.random() < 0.4 else self.r.getrandbits(32)

    def load32(self, reg, v):
        return [f"immw {reg}, 0x{v & 0xffff:04x}", f"immwh {reg}, 0x{(v >> 16) & 0xffff:04x}"]

    # ---- one item = a list of assembly lines; `pending` holds forward-branch labels
    def simple(self, avoid_dst=()):
        """An item with no control flow and no stack effect."""
        r = self.r; k = r.random()
        if k < 0.26:
            op = r.choice(ALU3); return [f"{op} {self.reg(avoid_dst)}, {self.reg()}, {self.reg()}"]
        if k < 0.44:
            op = r.choice(IMM9); imm = r.randint(0, 511) if op == 'bitex' else self.simm(9)
            return [f"{op} {self.reg(avoid_dst)}, {self.reg()}, {imm}"]
        if k < 0.54:
            return [f"{r.choice(UNARY)} {self.reg(avoid_dst)}"]
        if k < 0.62:
            op = r.choice(F2ALU); imm = r.randint(0, 127) if op == 'andi' else self.simm(7)
            return [f"{op} {self.reg(avoid_dst)}, {imm}"]
        if k < 0.72:
            op = r.choice(F2MEM_LD + F2MEM_ST)
            reg = self.reg(avoid_dst) if op in F2MEM_LD else self.reg()
            return [f"{op} {reg}, {r.randint(-64, 63)}"]
        if k < 0.84:
            op = r.choice(F3C_LD + F3C_ST); base = self.reg(avoid_dst)
            src = self.reg(avoid_dst) if op in F3C_LD else self.reg()
            return [f"immw {base}, 0x{r.randrange(F3C_LO, F3C_HI, 4):04x}", f"{op} {src}, {base}, {r.randint(-512, 511)}"]
        if k < 0.90:
            dst = self.reg(avoid_dst)
            return self.load32(dst, self.value()) if r.random() < 0.5 else [f"{r.choice(['immw', 'immwh'])} {dst}, 0x{r.getrandbits(16):04x}"]
        if k < 0.95:
            return [f"lea {self.reg(avoid_dst)}, {r.randrange(-512, 512, 4)}"]
        z = [n for n in (f"zero{i}" for i in range(8)) if f"r{n[-1]}" not in avoid_dst]
        return [r.choice(z)] if z else ["zero0"]

    def branch(self, target):
        r = self.r; k = r.random()
        if k < 0.30: return [f"{r.choice(BR2)} {self.reg()}, {self.reg()}, {target}"]
        if k < 0.55: return [f"{r.choice(BR1)} {self.reg()}, {target}"]
        if k < 0.75: return [f"{r.choice(['cbeq', 'cbne'])} {self.reg()}, {r.randint(0, 127)}, {target}"]
        if k < 0.90: return [f"{r.choice(['jz', 'jnz'])} {self.reg()}, {target}"]
        if k < 0.95: return [f"j {target}"]
        t = self.reg(); return [f"immw {t}, {target}", f"jr {t}"]

    def body(self, n, in_function=False):
        """n items with forward branches, loops, calls and balanced stack pairs."""
        r, out, pending = self.r, [], collections.defaultdict(list)
        i = 0
        while i < n:
            for lab in pending.pop(i, []):
                out.append(f"{lab}:")
            k = r.random()
            if k < 0.12 and i + 1 < n:
                lab = self.label(); pending[min(n, i + r.randint(1, 6))].append(lab); out += self.branch(lab)
            elif k < 0.15 and not in_function:
                c = self.reg(); lab = self.label()
                out += [f"imms {c}, {r.randint(1, 5)}", f"{lab}:"]
                for _ in range(r.randint(1, 3)):
                    out += self.simple(avoid_dst=(c,))
                out.append(f"dbnz {c}, {lab}")
            elif k < 0.19 and not in_function:
                f = f"F{len(self.funcs)}"; self.funcs.append(f)
                if r.random() < 0.6: out.append(f"jl {f}")
                else: t = self.reg(); out += [f"immw {t}, {f}", f"jlr {t}"]
            elif k < 0.22 and not in_function:
                out.append(f"pushr {self.reg()}")
                for _ in range(r.randint(0, 2)):
                    out += self.simple()
                out.append(f"popr {self.reg()}")
            elif k < 0.24 and not in_function:
                a = r.randrange(4, 68, 4); out.append(f"adjw {-a}")
                for _ in range(r.randint(0, 2)):
                    out += self.simple()
                out.append(f"adjw {a}")
            elif k < 0.245:
                out.append(f"putchar {self.reg()}")
            else:
                item = self.simple()
                if in_function:   # a callee must not store into its own frame word via BP
                    while item[-1].split()[0] in F2MEM_ST:
                        item = self.simple()
                out += item
            i += 1
        for idx in sorted(pending):
            for lab in pending[idx]:
                out.append(f"{lab}:")
        return out

    def program(self):
        r = self.r
        lines = [".text=0", f"immw r0, 0x{BP_ADDR + 4:04x}", "ssp r0", "enter 0", f"immw r0, 0x{STACK:04x}", "ssp r0"]
        for reg in REGS:
            lines += self.load32(reg, self.value())
        lines += self.body(self.length)
        lines.append("halt")
        k = 0
        while k < len(self.funcs):   # functions may be added while generating (they do not call)
            f = self.funcs[k]; k += 1
            lines += [f"{f}:", f"enter {r.randrange(0, 36, 4)}"] + self.body(r.randint(2, 8), in_function=True) + ["ret"]
        for lo, hi in WINDOWS:
            lines.append(f".data=0x{lo:04x}")
            words = [f"0x{r.getrandbits(32):08x}" for _ in range((hi - lo) // 4)]
            for j in range(0, len(words), 8):
                lines.append("long " + " ".join(words[j:j + 8]))
        return "\n".join(l if l.endswith(':') or l.startswith('.') else "    " + l for l in lines) + "\n"


# ---------------------------------------------------------------- running and comparing
SIM = os.path.join(ROOT, 'sim_c')
PATS = {d['name']: sem.pattern(d) for d in isa.INSTRUCTIONS}


def mnemonic(hexbytes):
    n = len(hexbytes) // 2; w = int(hexbytes, 16)
    for name, (tpl, pat) in PATS.items():
        if len(tpl) == n * 8 and all(((w >> (n * 8 - 1 - p)) & 1) == b for p, b in pat.items()):
            return name
    return '?'


def run_sim(asm, gen, maxsteps):
    with tempfile.NamedTemporaryFile(suffix='.ret', delete=False) as t:
        ret = t.name
    cmd = [SIM] + (['-gen'] if gen else []) + ['-maxsteps', str(maxsteps), '-retire', ret, asm]
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    trace = open(ret).read().splitlines(); os.unlink(ret)
    final = next((l for l in p.stdout.splitlines() if l.startswith('r0:')), '')
    return p.returncode, trace, final, p.stderr


def run_cpupy(asm, maxsteps):
    p = subprocess.run([sys.executable, 'sim.py', '--maxsteps', str(maxsteps), asm], capture_output=True, text=True, errors="replace",
                       cwd=os.path.join(ROOT, 'cpu4'))
    m = re.search(r'r0:.*?H:\d', p.stdout)
    return m.group(0) if m else ('ERROR ' + (p.stderr.strip().splitlines() or ['?'])[-1])


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
    ap.add_argument('--cpupy', action='store_true', help='also compare final state with cpu4/cpu.py')
    ap.add_argument('--keep', metavar='DIR', help='save each failing program here')
    ap.add_argument('--maxsteps', type=int, default=200000)
    a = ap.parse_args()
    cover = collections.Counter(); fails = collections.Counter(); retired = 0; examples = {}
    for k in range(a.n):
        seed = a.seed * 1000003 + k
        src = Prog(random.Random(seed), a.len).program()
        with tempfile.NamedTemporaryFile('w', suffix='.s', delete=False) as f:
            f.write(src); asm = f.name
        h = run_sim(asm, False, a.maxsteps); g = run_sim(asm, True, a.maxsteps)
        problems = []
        if g[0] != 0:
            problems.append(('gen-error', g[3].strip().splitlines()[-1:] if g[3].strip() else [f'exit {g[0]}']))
        if h[0] != g[0] or h[1] != g[1] or h[2] != g[2]:
            if h[0] < 0 or h[0] > 1:
                problems.append(('hand-crash', [f'hand exit {h[0]} after {len(h[1])} instructions; next: ' + (g[1][len(h[1])] if len(g[1]) > len(h[1]) else '?')]))
            else:
                i, x, y = first_diff(h[1], g[1])
                problems.append(('hand-vs-gen', [f'instruction {i}', f'hand {x}', f'gen  {y}']))
        if a.cpupy and g[0] == 0:
            c = run_cpupy(asm, a.maxsteps)
            if not g[2].startswith(c):
                problems.append(('cpupy-vs-gen', [f'gen   {g[2][:120]}', f'cpupy {c[:120]}']))
        for line in g[1]:
            cover[mnemonic(line.split()[1])] += 1
        retired += len(g[1])
        for kind, detail in problems:
            fails[kind] += 1
            examples.setdefault(kind, (seed, detail))
            if a.keep:
                os.makedirs(a.keep, exist_ok=True)
                open(os.path.join(a.keep, f'{kind}-{seed}.s'), 'w').write(src)
        os.unlink(asm)
    names = [d['name'] for d in isa.INSTRUCTIONS]
    missing = [n for n in names if not cover[n]]
    print(f"{a.n} programs, {retired:,} retired instructions; {len(names) - len(missing)}/{len(names)} instructions executed"
          + (f" (never: {', '.join(missing)})" if missing else ""))
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
