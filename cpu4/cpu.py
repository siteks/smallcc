"""CPU4 Python model.

Execution is generated: cpu4/exec_gen.py is written by `make isa` from the
format templates and SEMANTICS lines in cpu4/isa.py (cpu4/sem.py, Python back
end), the same description sim_c's executor is generated from. Float
arithmetic comes from cpu4/fpu_model.py, the Python port of cpu4/fpu_model.h.
This file holds the machine state, a 64 KiB memory with the cycle counter at
0xFF00 (no SDRAM, no other devices), the assembler's tables (class G) and the
retirement trace. See "Where an instruction is defined" in docs/isa/cpu4.md.
"""
import sys
import numpy as np
from isa_table import PTABLE
from fpu_model import *        # cpu4_fadd ... cpu4_frsqrt, f2b, b2f (re-exported for callers)
import exec_gen


class G:

    directive = {
        'byte'  :   1,
        'word'  :   2,
        'long'  :   4,
        'allocb':   1,
        'allocw':   2,
        'allocl':   4,
        'align':    1,
    }

    # Encoding table generated from cpu4/isa.py (see isa_table.py); edit isa.py, run `make isa`.
    ptable = PTABLE

    # The key becomes (first byte, subop). the value is (instr, length, subfmt, subopcode)
    rptable = {(v[0], v[3]) : (k, v[1], v[2]) for k, v in ptable.items()}


def sexb(b):
    return b if b < 128 else b - 256
def sexw(b):
    return b if b < 0x8000 else b - 0x10000
def sext(b, w):
    return b if b < (1 << (w - 1)) else b - (1 << w)

MMIO_BASE = 0xFF00

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
    def __init__(self):
        self.reset()
    def reset(self):
        self.r  = [0, 0, 0, 0, 0, 0, 0, 0]
        self.sp = self.bp = self.lr = self.pc = 0
        self.H  = 0
    def __repr__(self):
        return 'r0:%08x r1:%08x r2:%08x r3:%08x r4:%08x r5:%08x r6:%08x r7:%08x sp:%04x bp:%04x lr:%04x pc:%04x H:%x' % (
            self.r[0], self.r[1], self.r[2], self.r[3], self.r[4], self.r[5], self.r[6], self.r[7], self.sp, self.bp, self.lr, self.pc, self.H)


class CPU:
    def __init__(self, m, retire=None):
        self.state  = State()
        self.mem    = m
        self.cycles = 0
        self.retire = retire          # a file: one line per retired instruction, sim_c -retire format
        m.get_cycles = lambda: self.cycles

    def reset(self):
        self.state.reset()

    def step(self, trace=False):
        s, m = self.state, self.mem
        pc0 = s.pc
        if self.retire is not None:
            before = (list(s.r), s.sp, s.bp, s.lr)
            exec_gen.MW = []
        n = exec_gen.step(s, m)
        self.cycles += 1               # the cycle counter (MMIO 0xFF00) counts retired instructions, as in sim_c
        # SP and BP must be multiples of 4 after every instruction
        if s.sp & 3:
            sys.stderr.write("CPU4 alignment error: SP misaligned 0x%04x at pc=0x%04x\n" % (s.sp, pc0)); s.H = 1
        if s.bp & 3:
            sys.stderr.write("CPU4 alignment error: BP misaligned 0x%04x at pc=0x%04x\n" % (s.bp, pc0)); s.H = 1
        if self.retire is not None:
            r0, sp0, bp0, lr0 = before
            line = '%04x ' % pc0 + ''.join('%02x' % m.read8((pc0 + i) & 0xffff) for i in range(n))
            line += ''.join(' r%d=%08x' % (i, s.r[i]) for i in range(8) if s.r[i] != r0[i])
            if s.sp != sp0: line += ' sp=%04x' % s.sp
            if s.bp != bp0: line += ' bp=%04x' % s.bp
            if s.lr != lr0: line += ' lr=%04x' % s.lr
            line += ''.join(' m%d[%08x]=%0*x' % (k, a, 2 * k, v) for a, v, k in exec_gen.MW)
            if s.H: line += ' halt'
            self.retire.write(line + ' >%04x\n' % s.pc)
        if trace:
            print('%04x %s' % (pc0, s))
        return s
