#!/usr/bin/env python3
"""The Python simulator for any ISA defined by an <arch>/isa.py.

Execution is generated: <arch>/exec_gen.py is written by `make isa` from the
ISA definition's formats and SEMANTICS lines (isatool/sem.py, Python back
end), the same description sim_c's executor is generated from. The ISA's
primitives (float arithmetic for cpu4) come from its PRIMITIVE_PY file and are
bound into the executor's namespace here. This file holds the machine state
(STATE in the definition), a 64 KiB memory with the cycle counter at 0xFF00
(no SDRAM, no other devices) and the retirement trace.

  python3 isatool/pysim.py [--arch cpu4] [--maxsteps N] [--retire FILE] file.s
"""
import argparse
import importlib.util
import os
import sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from isatool import model

MMIO_BASE = 0xFF00
_EXEC = {}


def _load_file(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def primitives(arch):
    """The ISA's primitive implementations as {prefixed name: function}."""
    isa = model.load(arch)
    if not isa.primitives:
        return {}
    mod = _load_file(f"{arch}_primitives", os.path.join(model.ROOT, isa.prim_py))
    return {isa.prim_prefix + n: getattr(mod, isa.prim_prefix + n) for n in isa.primitives}


def executor(arch):
    """<arch>/exec_gen.py, loaded once, with the primitives bound into it."""
    if arch not in _EXEC:
        isa = model.load(arch)
        ex = _load_file(f"{arch}_exec_gen", os.path.join(isa.dir, 'exec_gen.py'))
        for k, f in primitives(arch).items():
            setattr(ex, k, f)
        _EXEC[arch] = (isa, ex)
    return _EXEC[arch]


class Mem:
    """64 KiB, zero at reset (the specification: memory the image does not cover reads as 0)."""
    def __init__(self):
        self.mem = np.zeros(65536, np.ubyte)
        self.get_cycles = lambda: 0

    def _mmio_read8(self, addr):
        off = (addr - MMIO_BASE) & 0xffff
        return (self.get_cycles() >> (off * 8)) & 0xff if off < 4 else 0

    def read8(self, addr, trace=False):
        addr &= 0xffff
        return self._mmio_read8(addr) if addr >= MMIO_BASE else int(self.mem[addr])

    def read16(self, addr, trace=False):
        return self.read8(addr) | (self.read8(addr + 1) << 8)

    def read32(self, addr, trace=False):
        return self.read16(addr) | (self.read16(addr + 2) << 16)

    def write(self, addr, data):
        addr &= 0xffff
        for d in data:
            self.mem[addr] = d
            addr += 1

    def write8(self, addr, data):
        addr &= 0xffff
        if addr < MMIO_BASE:
            self.mem[addr] = data & 0xff

    def write16(self, addr, data):
        self.write8(addr, data); self.write8(addr + 1, data >> 8)

    def write32(self, addr, data):
        self.write16(addr, data); self.write16(addr + 2, data >> 16)

    def dumpmem(self, start=0, length=65536):
        for i in range(start, start + length, 32):
            print('%04x  ' % i + ''.join('%02x' % self.read8(i + j) for j in range(32)))

    def dumpmemf(self, f, start=0, length=65536, format='default'):
        for i in range(start, start + length, 32):
            if format != 'verilog':
                f.write('%04x  ' % i)
            f.write((' ' if format == 'verilog' else '').join('%02x' % self.read8(i + j) for j in range(32)))
            f.write(' \n' if format == 'verilog' else '\n')


class State:
    """General registers r[0..ngpr-1], the special registers (lower-case attributes) and H."""
    def __init__(self, ngpr=8, special=('PC', 'SP', 'BP', 'LR')):
        self.ngpr = ngpr
        self.special = [s.lower() for s in special]
        self.reset()

    def reset(self):
        self.r = [0] * self.ngpr
        for s in self.special:
            setattr(self, s, 0)
        self.H = 0

    def __repr__(self):
        return ' '.join('r%d:%08x' % (i, v) for i, v in enumerate(self.r)) + \
            ' sp:%04x bp:%04x lr:%04x pc:%04x H:%x' % (self.sp, self.bp, self.lr, self.pc, self.H)


class CPU:
    def __init__(self, m, retire=None, arch='cpu4'):
        self.isa, self.ex = executor(arch)
        self.state = State(self.isa.ngpr, self.isa.special)
        self.mem = m
        self.cycles = 0
        self.retire = retire          # a file: one line per retired instruction, sim_c -retire format
        m.get_cycles = lambda: self.cycles

    def reset(self):
        self.state.reset()

    def step(self, trace=False):
        s, m, ex = self.state, self.mem, self.ex
        pc0 = s.pc
        if self.retire is not None:
            before = (list(s.r), s.sp, s.bp, s.lr)
            ex.MW = []
        n = ex.step(s, m)             # includes the ISA's invariant checks
        self.cycles += 1               # the cycle counter (MMIO 0xFF00) counts retired instructions, as in sim_c
        if self.retire is not None:
            r0, sp0, bp0, lr0 = before
            line = '%04x ' % pc0 + ''.join('%02x' % m.read8((pc0 + i) & 0xffff) for i in range(n))
            line += ''.join(' r%d=%08x' % (i, s.r[i]) for i in range(s.ngpr) if s.r[i] != r0[i])
            if s.sp != sp0: line += ' sp=%04x' % s.sp
            if s.bp != bp0: line += ' bp=%04x' % s.bp
            if s.lr != lr0: line += ' lr=%04x' % s.lr
            line += ''.join(' m%d[%08x]=%0*x' % (k, a, 2 * k, v) for a, v, k in ex.MW)
            if s.H: line += ' halt'
            self.retire.write(line + ' >%04x\n' % s.pc)
        if trace:
            print('%04x %s' % (pc0, s))
        return s


def runasm(text, filename, maxsteps=1000, verbose=True, retire=None, arch='cpu4'):
    from isatool.asm import Assembler
    m = Mem()
    a = Assembler(arch)
    a.assemble(text, showsymbols=verbose)
    a.makeimage(m)
    if verbose:
        a.dumpasm('%s.lst' % filename)
        a.dumpasm(sys.stdout)
        m.dumpmem(0, 128)
    c = CPU(m, retire=retire, arch=arch)
    c.reset()
    s = c.state
    for _ in range(maxsteps):
        s = c.step(trace=verbose)
        if s.H:
            break
    return s


def runfile(sourcefile, maxsteps=1000, verbose=False, retire=None, arch='cpu4'):
    return runasm(open(sourcefile).read(), sourcefile, maxsteps, verbose, retire, arch)


def main(default_arch='cpu4'):
    ap = argparse.ArgumentParser('pysim')
    ap.add_argument('filename', help='assembly file')
    ap.add_argument('--arch', default=default_arch, help='ISA (a directory holding isa.py)')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--maxsteps', type=int, default=1000, help='max simulation steps')
    ap.add_argument('--retire', metavar='FILE', help='write one line per retired instruction (sim_c -retire format)')
    args = ap.parse_args()
    ret = open(args.retire, 'w') if args.retire else None
    s = runfile(args.filename, maxsteps=args.maxsteps, verbose=args.verbose, retire=ret, arch=args.arch)
    if ret:
        ret.close()
    print(s)


if __name__ == '__main__':
    main()
