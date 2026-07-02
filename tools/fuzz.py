#!/usr/bin/env python3
"""
Differential fuzzer for smallcc.

Generates random C89 programs inside the supported subset and runs each
through four oracles:

    1. smallcc -O2  -> sim_c          (the production pipeline)
    2. smallcc -O0  -> sim_c          (optimizations off)
    3. smallcc -O2 -runoos            (post-OOS IR interpreter)
    4. smallcc -O2 -runirc            (post-IRC IR interpreter)

plus an IR_VERIFY=1 compile. Any disagreement in the final r0 value (or a
crash/verifier failure) is a bug; the offending program is saved into
fuzz_failures/ for reduction into a tests/cases/ reproducer.

Divergence localizes the bug: -O0 vs -O2 -> an optimization pass;
runoos vs runirc -> legalize/IRC; runirc vs sim_c -> emission/assembler.

The generator stays inside defined behavior for THIS target so the oracles
must agree: arithmetic is mostly unsigned (wraps identically everywhere),
divisors are forced nonzero with `| 1` (the ISA defines x/0 == 0, but
constant folding on the host would trap), and shift amounts are masked to
0..15.

Usage:
    python3 tools/fuzz.py [-n ITERATIONS] [-seed BASE_SEED] [-keep]
"""

import argparse
import random
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SMALLCC = ROOT / "smallcc"
SIM_C = ROOT / "sim_c"
FAILDIR = ROOT / "fuzz_failures"

BINOPS = ["+", "-", "*", "&", "|", "^"]
CMPOPS = ["<", "<=", ">", ">=", "==", "!="]


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)
        self.nvars = self.r.randint(3, 6)
        self.vars = [f"v{i}" for i in range(self.nvars)]
        self.funcs = []          # (name, nparams)
        self.stmt_budget = self.r.randint(8, 30)

    # ---- expressions ----------------------------------------------------
    def expr(self, depth=0):
        r = self.r
        if depth > 3 or r.random() < 0.3:
            if r.random() < 0.5:
                return r.choice(self.vars)
            return str(r.randint(0, 300))
        k = r.random()
        a = self.expr(depth + 1)
        b = self.expr(depth + 1)
        if k < 0.55:
            return f"({a} {r.choice(BINOPS)} {b})"
        if k < 0.65:
            # division/modulo with a guaranteed-nonzero divisor
            return f"({a} {r.choice(['/', '%'])} (({b}) | 1u))"
        if k < 0.75:
            # shift with masked amount
            return f"({a} {r.choice(['<<', '>>'])} (({b}) & 15))"
        if k < 0.9:
            return f"({a} {r.choice(CMPOPS)} {b})"
        if self.funcs and k < 0.97:
            name, np = r.choice(self.funcs)
            args = ", ".join(self.expr(3) for _ in range(np))
            return f"{name}({args})"
        return f"(~({a}))"

    # ---- statements ------------------------------------------------------
    def stmt(self, indent, depth=0):
        r = self.r
        self.stmt_budget -= 1
        pad = "    " * indent
        v = r.choice(self.vars)
        k = r.random()
        if depth < 2 and self.stmt_budget > 4 and k < 0.18:
            body = self.block(indent + 1, depth + 1)
            els = ""
            if r.random() < 0.4:
                els = f"{pad}else\n" + self.block(indent + 1, depth + 1)
            return f"{pad}if ({self.expr(2)})\n{body}{els}"
        if depth < 2 and self.stmt_budget > 6 and k < 0.30:
            # bounded loop: dedicated counter so it always terminates
            c = f"i{r.randint(0, 999)}"
            n = r.randint(1, 12)
            body = self.block(indent + 1, depth + 1)
            return (f"{pad}{{ int {c};\n"
                    f"{pad}for ({c} = 0; {c} < {n}; {c}++)\n{body}{pad}}}\n")
        if k < 0.55:
            return f"{pad}{v} = {self.expr()};\n"
        op = r.choice(["+=", "-=", "^=", "|=", "&="])
        return f"{pad}{v} {op} {self.expr(1)};\n"

    def block(self, indent, depth):
        n = self.r.randint(1, 3)
        pad = "    " * (indent - 1)
        return (f"{pad}{{\n"
                + "".join(self.stmt(indent, depth) for _ in range(n))
                + f"{pad}}}\n")

    # ---- whole program ----------------------------------------------------
    def helper(self, idx):
        np = self.r.randint(1, 3)
        name = f"f{idx}"
        params = ", ".join(f"unsigned p{i}" for i in range(np))
        save = self.vars
        self.vars = [f"p{i}" for i in range(np)]
        body = "".join(self.stmt(1) for _ in range(self.r.randint(1, 4)))
        ret = f"    return {self.expr()};\n"
        self.vars = save
        self.funcs.append((name, np))
        return f"unsigned {name}({params})\n{{\n{body}{ret}}}\n\n"

    def program(self):
        src = ""
        for i in range(self.r.randint(0, 2)):
            src += self.helper(i)
        decls = "".join(f"    unsigned {v} = {self.r.randint(0, 99)};\n"
                        for v in self.vars)
        body = "".join(self.stmt(1) for _ in range(self.r.randint(4, 10)))
        acc = " ^ ".join(self.vars)
        return (src + "int main(void)\n{\n" + decls + body
                + f"    return (int)((({acc}) & 0x7fff));\n}}\n")


# ---- oracles ---------------------------------------------------------------

class Timeout:
    returncode = -1
    stdout = ""
    stderr = "TIMEOUT"


def run(cmd, env=None, timeout=20):
    try:
        return subprocess.run(cmd, capture_output=True, text=True,
                              timeout=timeout, env=env)
    except subprocess.TimeoutExpired:
        return Timeout()


def r0_of(stdout):
    import re
    m = re.search(r"r0:([0-9a-f]{8})", stdout)
    return m.group(1) if m else None


def check_one(seed, tmpdir, keep):
    src = Gen(seed).program()
    cfile = tmpdir / f"fuzz_{seed}.c"
    cfile.write_text(src)
    results = {}
    fail = None

    for tag, extra in [("O2", ["-O2"]), ("O0", ["-O0"])]:
        asm = tmpdir / f"fuzz_{seed}_{tag}.s"
        p = run([str(SMALLCC), "-arch", "cpu4", *extra, "-o", str(asm), str(cfile)])
        if p.returncode != 0:
            fail = f"compile {tag} failed:\n{p.stderr}"
            break
        s = run([str(SIM_C), "-arch", "cpu4", "-maxsteps", "10000000", str(asm)])
        r0 = r0_of(s.stdout)
        if s.returncode != 0 or r0 is None:
            fail = f"sim_c {tag} failed:\n{s.stdout}{s.stderr}"
            break
        results[f"sim/{tag}"] = r0

    if not fail:
        for tag in ["runoos", "runirc"]:
            p = run([str(SMALLCC), "-arch", "cpu4", f"-{tag}", str(cfile)])
            r0 = r0_of(p.stdout)
            if p.returncode != 0 or r0 is None:
                fail = f"{tag} failed:\n{p.stdout}{p.stderr}"
                break
            results[tag] = r0

    if not fail:
        import os
        env = dict(os.environ, IR_VERIFY="1")
        p = run([str(SMALLCC), "-arch", "cpu4", "-o", "/dev/null", str(cfile)], env=env)
        if p.returncode != 0:
            fail = f"IR_VERIFY failed:\n{p.stderr}"

    if not fail and len(set(results.values())) > 1:
        fail = "oracle divergence: " + ", ".join(f"{k}={v}" for k, v in results.items())

    if fail:
        FAILDIR.mkdir(exist_ok=True)
        dst = FAILDIR / f"fuzz_{seed}.c"
        dst.write_text(f"// FUZZ FAILURE seed={seed}\n// {fail.splitlines()[0]}\n" + src)
        print(f"[seed {seed}] FAIL: {fail.splitlines()[0]}")
        print(f"          saved to {dst}")
        return False

    if not keep:
        cfile.unlink(missing_ok=True)
        for tag in ["O2", "O0"]:
            (tmpdir / f"fuzz_{seed}_{tag}.s").unlink(missing_ok=True)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=100, help="iterations (default 100)")
    ap.add_argument("-seed", type=int, default=1, help="base seed (default 1)")
    ap.add_argument("-keep", action="store_true", help="keep generated files")
    args = ap.parse_args()

    if not SMALLCC.exists() or not SIM_C.exists():
        sys.exit("build smallcc and sim_c first (make smallcc sim_c)")

    import tempfile
    nfail = 0
    with tempfile.TemporaryDirectory() as td:
        tmpdir = Path(td)
        for i in range(args.n):
            seed = args.seed + i
            if not check_one(seed, tmpdir, args.keep):
                nfail += 1
            if (i + 1) % 25 == 0:
                print(f"  ... {i + 1}/{args.n} done, {nfail} failures")
    print(f"fuzz: {args.n} programs, {nfail} failures")
    sys.exit(1 if nfail else 0)


if __name__ == "__main__":
    main()
