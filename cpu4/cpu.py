 


import re
import struct
import numpy as np
from isa_table import PTABLE

import sys



#---------------------------------
# RISC-like, with dense instruction encoding
# all ops 32 bit, pointers 16 bit
#
# State:
#   pc   
#   sp
#   bp
#   lr
#   h
#   r0-r7
#
#   Format  Pattern                      Codes           Ops     Imm
#   F0a     0000oooo                     16              0       0
#   F0b     0001oooo odddxxxiiiiiiiii    32              2       9
#   F0c     001odddi iiiiiiiiiiiiiiii    2               1       17
#   F1a     01oooood ddxxxyyy            31 + escape     3       0
#   F1b     0111111d ddoooooo            64              1       0
#   F2      10ooooxx xiiiiiii            16              1       7
#   F3a     110000oo iiiiiiiiiiiiiiii    4               0       16
#   F3b     110001od ddiiiiiiiiiiiiii    2               1       14
#   F3c     1101oooo xxxyyyiiiiiiiiii    15 + 1 escape   2       10
#   F3d     11011111 xxxoooiiiiiiiiii    8               1       10
#   F3e     111ooxxx iiiiiiiiiiiiiiii    4               1       16
#
#   Format 0a implicit or special hotspot (16 slots)
#   halt
#   ret     sp = bp; bp = [sp]&0xffff; pc = [sp]>>16; sp += 4
#   zero0   r0 = 0
#   zero1   r1 = 0
#   zero2   r2 = 0
#   zero3   r3 = 0
#   zero4   r4 = 0
#   zero5   r5 = 0
#   zero6   r6 = 0
#   zero7   r7 = 0
#
#   Format 0b - two op + imm9 (32 slots)
#   addli   rx = ry + sxt(imm9)
#   subli   rx = ry - sxt(imm9)
#   mulli
#   divli
#   modli
#   shlli
#   shrli
#   leli    le and gt, may revisit??
#   gtli
#   eqli
#   neli
#   andli
#   orli
#   xorli
#   lesli
#   gtsli
#   divsli
#   modsli
#   shrsli
#   bitex   rx = (ry >> (imm9 & 0x1f)) & ~(-2 << (imm9 >> 5))   could use cleverer encoding to get all combos
#   rsubli
#   rdivli
#   rmodli
#   rdivsli
#
#   Format 0c - one op + imm17 (2 slots)
#   cbeq    pc = rx==imm17>>10 ? (pc + sxt(imm17&0x3ff)): pc
#   cbne    pc = rx!=imm17>>10 ? (pc + sxt(imm17&0x3ff)): pc
#
#   Format 1a - three op (31 slots + 1 escape)
#   add     rd = rx op ry
#   sub
#   mul
#   div
#   mod
#   shl
#   shr
#   lt
#   le
#   eq
#   ne
#   and
#   or
#   xor
#   lts
#   les
#   divs
#   mods
#   shrs
#   fadd
#   fsub
#   fmul
#   fdiv
#   flt
#   fle
#   zxwor   rd = (rx | ry) & 0xffff
#   sxwor   rd = sxt((rx | ry) & 0xffff, 16)
#   (4 left)
#
#   Format 1b - single op, (64 slots)
#   sxb     rd
#   sxw     rd
#   inc     rd
#   dec     rd
#   pushr   rd
#   popr    rd
#   zxb     rd
#   zxw     rd
#   itof    rd
#   ftoi    rd
#   jlr     lr = pc; pc = rd & 0xffff
#   jr      pc = rd & 0xffff
#   ssp     sp = rd & 0xffff
#   neg     rd = -rd
#   frecip  rd = 1 / rd
#   frsqrt  rd = 1 / sqrt(rd)
#   __putchar   rd
#   
#   Format 2 - one op + imm7; stack frame access (16 slots)
#   lb      rx = [bp+sxt(imm7)]   
#   lw      rx = [bp+sxt(imm7*2)]   
#   ll      rx = [bp+sxt(imm7*4)]   
#   sb      [bp+sxt(imm7)] = rx
#   sw      [bp+sxt(imm7*2)] = rx
#   sl      [bp+sxt(imm7*4)] = rx
#   lbx     rx = sxt([bp+sxt(imm7)])
#   lwx     rx = sxt([bp+sxt(imm7*2)])
#   addi    rx = rx + sxt(imm7)
#   shli    rx = rx << imm7
#   andi    rx = rx & imm7
#   shrsi   rx = rx >> imm7
#   imms    rx = sxt(imm7)
#
#   Format 3a - zero op + imm16 (4 slots)
#   j       pc = imm16
#   jl      lr = pc; pc = imm16
#   enter   [sp-4] = (lr << 16) | bp; bp = sp - 4; sp -= imm16 + 4
#
#   Format 3b - one op + imm14 (2 slots)
#   adjw    sp += (imm14 << 2)          this could move to F3a if needed
#   lea     rx = bp + (imm14 << 2)
#
#   Format 3c - two op + imm10 (14 slots + 2 escape)
#   llb     rx = [ry+sxt(imm10)]   
#   llw     rx = [ry+sxt(imm10*2)]   
#   lll     rx = [ry+sxt(imm10*4)]   
#   slb     [ry+sxt(imm10)] = rx
#   slw     [ry+sxt(imm10*2)] = rx
#   sll     [ry+sxt(imm10*4)] = rx
#   llbx    rx = sxt([ry+sxt(imm10)])
#   llwx    rx = sxt([ry+sxt(imm10*2)])
#   beq     pc = rx==ry ? (pc + sxt(imm10)) : pc
#   bne     pc = rx!=ry ? (pc + sxt(imm10)) : pc
#   blt     pc = rx<ry ? (pc + sxt(imm10)) : pc
#   ble     pc = rx<=ry ? (pc + sxt(imm10)) : pc
#   blts    pc = rx<ry ? (pc + sxt(imm10)) : pc
#   bles    pc = rx<=ry ? (pc + sxt(imm10)) : pc
#   
#   Format 3d - one op + imm10 (8 slots) (escape from 3d with op=0xf)
#   beqz    pc = rx==0 ? (pc + sxt(imm10)) : pc
#   bnez    pc = rx!=0 ? (pc + sxt(imm10)) : pc
#   dbnz    pc = --rx!=0 ? (pc + sxt(imm10)) : pc
#   bltz    pc = rx<0 ? (pc + sxt(imm10)) : pc
#   bgez    pc = rx>=0 ? (pc + sxt(imm10)) : pc
#   bgtz    pc = rx>0 ? (pc + sxt(imm10)) : pc
#   blez    pc = rx<=0 ? (pc + sxt(imm10)) : pc
#
#   Format 3e - one op + imm16 (4 slots)
#   immw    rd = imm16
#   immwh   rd = (rd & 0xffff) | (imm16 << 16)
#   jz      pc = rd==0 ? imm16 : pc
#   jnz     pc = rd!=0 ? imm16 : pc


_ROM_DIR = __import__('os').path.dirname(__import__('os').path.abspath(__file__))
def _load_rom(name):
    with open(__import__('os').path.join(_ROM_DIR, name)) as fh:
        return [int(l, 16) for l in fh.read().split()]
FRECIP_ROM = _load_rom('frecip_rom.hex')
FRSQRT_ROM = _load_rom('frsqrt_rom.hex')

def _fpack(sign, exp9, mant24):
    if exp9 <= 0: return 0
    if exp9 >= 255: return sign | 0x7f800000
    return sign | (exp9 << 23) | (mant24 & 0x7fffff)

def _rne(mant24, guard, sticky, exp9):
    rnd = guard & (sticky | (mant24 & 1))
    m = mant24 + rnd
    if m & 0x1000000: m >>= 1; exp9 += 1
    return m, exp9

def cpu4_frecip(x):
    x &= 0xffffffff
    sign = x & 0x80000000; e = (x >> 23) & 0xff; i = (x >> 13) & 0x3ff
    if e == 0: return sign
    ee = (254 - e) if i == 0 else (253 - e)
    if ee <= 0: return sign
    return sign | (ee << 23) | (FRECIP_ROM[i] << 7)

def cpu4_frsqrt(x):
    x &= 0xffffffff
    e = (x >> 23) & 0xff; m = x & 0x7fffff
    if e == 0: return 0x7f800000
    if e == 255: return 0
    p = (~e) & 1; i = (p << 9) | (m >> 14)
    ee = (379 - e) // 2 if (e & 1) else (380 - e) // 2
    return ((ee & 0xff) << 23) | (FRSQRT_ROM[i] << 7)

def cpu4_fmul(a, b):
    a &= 0xffffffff; b &= 0xffffffff
    sign = (a ^ b) & 0x80000000
    ea = (a >> 23) & 0xff; eb = (b >> 23) & 0xff
    if ea == 0 or eb == 0: return sign
    p = (0x800000 | (a & 0x7fffff)) * (0x800000 | (b & 0x7fffff))
    norm = (p >> 47) & 1
    if norm: mant = (p >> 24) & 0xffffff; guard = (p >> 23) & 1; sticky = int((p & 0x7fffff) != 0)
    else:    mant = (p >> 23) & 0xffffff; guard = (p >> 22) & 1; sticky = int((p & 0x3fffff) != 0)
    mant, exp9 = _rne(mant, guard, sticky, ea + eb - 127 + norm)
    return _fpack(sign, exp9, mant)

def cpu4_fadd(a, b):
    a &= 0xffffffff; b &= 0xffffffff
    sa = a >> 31; sb = b >> 31
    ea = (a >> 23) & 0xff; eb = (b >> 23) & 0xff
    ma = a & 0x7fffff; mb = b & 0x7fffff
    fa = (0x800000 | ma) if ea else 0; fb = (0x800000 | mb) if eb else 0
    a_bigger = (ea > eb) or (ea == eb and ma >= mb)
    big, small = (fa, fb) if a_bigger else (fb, fa)
    bexp = ea if a_bigger else eb
    d = (ea - eb) if a_bigger else (eb - ea)
    sign = sa if a_bigger else sb
    sub = (sa ^ sb) != 0
    ext_small = small << 3
    if d >= 27: aligned = 0; sticky = int(small != 0)
    else: aligned = ext_small >> d; sticky = int((ext_small & ((1 << d) - 1)) != 0)
    aligned |= sticky
    bigx = big << 3
    sum_ = (bigx - aligned) if sub else (bigx + aligned)
    if sum_ == 0: return 0
    if sum_ & 0x8000000:
        mant = (sum_ >> 4) & 0xffffff; guard = (sum_ >> 3) & 1; st = int((sum_ & 7) != 0); exp9 = bexp + 1
    else:
        lzc = 0; v = sum_ & 0x7ffffff
        while not (v & 0x4000000): v <<= 1; lzc += 1
        mant = (v >> 3) & 0xffffff; guard = (v >> 2) & 1; st = int((v & 3) != 0); exp9 = bexp - lzc
    mant, exp9 = _rne(mant, guard, st, exp9)
    return _fpack(sign << 31, exp9, mant)

def cpu4_fsub(a, b): return cpu4_fadd(a, b ^ 0x80000000)
def cpu4_fdiv(a, b): return cpu4_fmul(a, cpu4_frecip(b))

def cpu4_flt(a, b):
    a &= 0xffffffff; b &= 0xffffffff
    sa = a >> 31; sb = b >> 31; aa = a & 0x7fffffff; ab = b & 0x7fffffff
    if aa == 0 and ab == 0: return 0
    if sa and not sb: return 1
    if not sa and sb: return 0
    return int(aa > ab) if sa else int(aa < ab)

def cpu4_fle(a, b):
    a &= 0xffffffff; b &= 0xffffffff
    sa = a >> 31; sb = b >> 31; aa = a & 0x7fffffff; ab = b & 0x7fffffff
    if aa == 0 and ab == 0: return 1
    if sa and not sb: return 1
    if not sa and sb: return 0
    return int(aa >= ab) if sa else int(aa <= ab)

def cpu4_itof(i):
    i &= 0xffffffff
    if i == 0: return 0
    sign = i & 0x80000000
    ab = ((-i) & 0xffffffff) if sign else i
    pos = ab.bit_length() - 1
    mant = (ab >> (pos - 23)) if pos > 23 else (ab << (23 - pos))
    return sign | ((127 + pos) << 23) | (mant & 0x7fffff)

def cpu4_ftoi(x):
    x &= 0xffffffff
    sign = x >> 31; e = (x >> 23) & 0xff
    if e == 0: return 0
    full = 0x800000 | (x & 0x7fffff)
    if e < 127: ab = 0
    elif e >= 150:
        sh = e - 150; ab = 0 if sh >= 32 else ((full << sh) & 0xffffffff)
    else: ab = full >> (150 - e)
    return ((-ab) & 0xffffffff) if sign else ab

def f2b(f):
    """Convert Python float to 32-bit IEEE 754 bit pattern (unsigned int)."""
    return struct.unpack('<I', struct.pack('<f', float(f)))[0]

def b2f(b):
    """Convert 32-bit IEEE 754 bit pattern to Python float."""
    return struct.unpack('<f', struct.pack('<I', int(b) & 0xffffffff))[0]


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
    # turn twos complement byte to signed int
    return b if b < 128 else b - 256
def sexw(b):
    # turn twos complement word to signed int
    return b if b < 0x8000 else b - 0x10000
def sext(b, w):
    # turn twos complement arb length to signed int
    return b if b < (1 << (w - 1)) else b - (1 << w)

MMIO_BASE = 0xFF00

class Mem:
    def __init__(self):
        self.mem    = np.full(65536, 0xff, np.ubyte)
        self.trace = ''
        self.get_cycles = lambda: 0

    def _mmio_read8(self, addr):
        off = (addr - MMIO_BASE) & 0xffff
        if off < 4:
            return (self.get_cycles() >> (off * 8)) & 0xff
        return 0

    def read8(self, addr, trace=True):
        addr &= 0xffff
        if addr >= MMIO_BASE:
            return self._mmio_read8(addr)
        self.trace += 'r8[%04x]=>%02x   ' % (addr, int(self.mem[addr])) if trace else ''
        return int(self.mem[addr])

    def read16(self, addr, trace=True):
        addr &= 0xffff
        if addr & 1:
            import sys
            sys.stderr.write(f"CPU4 alignment error: 16-bit read from unaligned address 0x{addr:04x}\n")
            sys.exit(1)
        if addr >= MMIO_BASE:
            return self._mmio_read8(addr) | (self._mmio_read8(addr + 1) << 8)
        d = int(self.mem[addr]) | (int(self.mem[addr + 1]) << 8)
        self.trace += 'r16[%04x]=>%04x ' % (addr, d) if trace else ''
        return d

    def read32(self, addr, trace=True):
        addr &= 0xffff
        if addr & 3:
            import sys
            sys.stderr.write(f"CPU4 alignment error: 32-bit read from unaligned address 0x{addr:04x}\n")
            sys.exit(1)
        if addr >= MMIO_BASE:
            return (self._mmio_read8(addr) | (self._mmio_read8(addr + 1) << 8) |
                    (self._mmio_read8(addr + 2) << 16) | (self._mmio_read8(addr + 3) << 24))
        d = int(self.mem[addr]) | (int(self.mem[addr + 1]) << 8) | (int(self.mem[addr + 2]) << 16)| (int(self.mem[addr + 3]) << 24)
        self.trace += 'r32[%04x]=>%08x ' % (addr, d) if trace else ''
        return d

    def write(self, addr, data):
        addr &= 0xffff
        self.trace = 'w[%04x]<=[%s]' % (addr, data)
        for d in data:
            self.mem[addr] = d
            addr += 1

    def write8(self, addr, data):
        addr &= 0xffff
        if addr >= MMIO_BASE:
            return
        self.trace += 'w8[%04x]<=%02x   ' % (addr, data & 0xff)
        self.mem[addr] = data & 0xff

    def write16(self, addr, data):
        addr &= 0xffff
        if addr & 1:
            import sys
            sys.stderr.write(f"CPU4 alignment error: 16-bit write to unaligned address 0x{addr:04x}\n")
            sys.exit(1)
        if addr >= MMIO_BASE:
            return
        self.trace += 'w16[%04x]<=%04x ' % (addr, data & 0xffff)
        self.mem[addr] = data & 0xff
        self.mem[addr + 1] = (data >> 8) & 0xff

    def write32(self, addr, data):
        addr &= 0xffff
        if addr & 3:
            import sys
            sys.stderr.write(f"CPU4 alignment error: 32-bit write to unaligned address 0x{addr:04x}\n")
            sys.exit(1)
        if addr >= MMIO_BASE:
            return
        self.trace += 'w32[%04x]<=%08x ' % (addr, data & 0xffffffff)
        self.mem[addr] = data & 0xff
        self.mem[addr + 1] = (data >> 8) & 0xff
        self.mem[addr + 2] = (data >> 16) & 0xff
        self.mem[addr + 3] = (data >> 24) & 0xff

    def dumpmem(self, start=0, length=65536):
        for i in range(start, start + length, 32):
            print('%04x  ' % i, end='')
            for j in range(32):
                print('%02x' % self.read8(i + j), end='')
            print()

    def dumpmemf(self, f, start=0, length=65536, format='default'):
        #if format == 'verilog':
        #    f.write('@%x\n' % start)
        for i in range(start, start + length, 32):
            if format != 'verilog' : f.write('%04x  ' % i)
            for j in range(32):
                f.write('%02x' % self.read8(i + j))
                if format=='verilog':
                    f.write(' ')
            f.write('\n')

class State:
    def __init__(self):
        self.reset()
    def reset(self):
        self.r  = [0,0,0,0,0,0,0,0]
        self.sp = 0
        self.bp = 0
        self.lr = 0
        self.pc = 0
        self.H  = 0
    def __repr__(self):
        return 'r0:%08x r1:%08x r2:%08x r3:%08x r4:%08x r5:%08x r6:%08x r7:%08x sp:%04x bp:%04x lr:%04x pc:%04x H:%x' % (
            self.r[0], self.r[1], self.r[2], self.r[3], self.r[4], self.r[5], self.r[6], self.r[7], self.sp, self.bp, self.lr, self.pc, self.H)



class CPU:
    def __init__(self, m):
        self.state  = State()
        self.mem    = m
        self.cycles = 0
        m.get_cycles = lambda: self.cycles

    def reset(self):
        self.state.reset()

    def step(self, trace=False):
        self.cycles += 1
        s       = self.state
        # Read until we have an instruction.
        ins     = self.mem.read8(s.pc, trace=False)
        oldpc   = s.pc

        # get format, length, subformat, we need to read more to get subop if it exists
        lookupins = ins & (     0xff if ins & 0xf0 == 0x00  # f0a
                           else 0xff if ins & 0xf0 == 0x10  # f0b
                           else 0xf0 if ins & 0xe0 == 0x20  # f0c
                           else 0xfe if ins & 0xc0 == 0x40  # f1a, f1b
                           else 0xfc if ins & 0xc0 == 0x80  # f2
                           else 0xff if ins & 0xfc == 0xc0  # f3a
                           else 0xfe if ins & 0xfc == 0xc4  # f3b
                           else 0xff if ins & 0xf0 == 0xd0  # f3c
                           else 0xff if ins & 0xff == 0xdf  # f3d
                           else 0xf8 if ins & 0xe0 == 0xe0  # f3e
                           else 0xff)                       # 
        i, ilen, subfmt = G.rptable[(lookupins, 0)]
        fmt     = (ins & 0xc0) >> 6
        # fetch rest of instruction
        if ilen > 0: ins = (ins << 8) | self.mem.read8(s.pc + 1, trace=False)
        if ilen > 1: ins = (ins << 8) | self.mem.read8(s.pc + 2, trace=False)
        s.pc    += ilen + 1

        # Extract the subopcode when it is not in the first byte
        if 0x10 <= lookupins <= 0x1f:
            # This is a f0b instruction
            subop = (ins & 0x8000) >> 15
            i, _, _ = G.rptable[(lookupins, subop)]
        if lookupins == 0x7e:
            # This is a f1b instruction
            subop = ins & 0x3f
            i, _, _ = G.rptable[(lookupins, subop)]
        if lookupins == 0xdf:
            # This is a f3d instruction
            subop = (ins & 0x1c00) >> 10
            i, _, _ = G.rptable[(lookupins, subop)]


        imm     = 0
        dst     = 0
        src0    = 0
        src1    = 0

        # extract the fields
        if fmt == 0 and subfmt == 0:
            # irregular decode for op here
            pass
        elif fmt == 0 and subfmt == 1:
            dst = src0 = (ins & 0x7000) >> 12
            src1 = (ins & 0xe00) >> 9
            imm = ins & 0x1ff
        elif fmt == 0 and subfmt == 2:
            dst = src0 = (ins & 0xe0000) >> 17
            imm = ins & 0x1ffff
        elif fmt == 1 and subfmt == 0:
            dst = (ins & 0x1c0) >> 6
            src0 = (ins & 0x38) >> 3
            src1 = (ins & 0x7)
        elif fmt == 1 and subfmt == 1:
            dst = src0 = (ins & 0x1c0) >> 6
        elif fmt == 2:
            dst = src0 = (ins & 0x380) >> 7
            imm = ins  & 0x7f
        elif fmt == 3 and subfmt == 0:
            imm = ins & 0xffff
        elif fmt == 3 and subfmt == 1:
            dst = src0 = (ins & 0x1c000) >> 14
            imm = ins & 0x3fff
        elif fmt == 3 and subfmt == 2:
            dst = src0 = (ins & 0xe000) >> 13
            src1 = (ins & 0x1c00) >> 10
            imm = ins & 0x3ff
        elif fmt == 3 and subfmt == 3:
            dst = src0 = (ins & 0xe000) >> 13
            imm = ins & 0x3ff
        elif fmt == 3 and subfmt == 4:
            dst = src0 = (ins & 0x70000) >> 16
            imm = ins & 0xffff

        # noinspection PyUnreachableCode
        m = self.mem
        m.trace = ''
        p = ''
        # f0a
        if      i == 'halt':    s.H = 1
        elif    i == 'ret':     s.sp = s.bp; s.bp = m.read32(s.sp) & 0xffff; s.pc = m.read32(s.sp) >> 16; s.sp += 4
        elif    i == 'zero0':   s.r[0] = 0
        elif    i == 'zero1':   s.r[1] = 0
        elif    i == 'zero2':   s.r[2] = 0
        elif    i == 'zero3':   s.r[3] = 0
        elif    i == 'zero4':   s.r[4] = 0
        elif    i == 'zero5':   s.r[5] = 0
        elif    i == 'zero6':   s.r[6] = 0
        elif    i == 'zero7':   s.r[7] = 0
        # f0b
        elif    i == 'addli':   s.r[dst] = s.r[src1] + sext(imm, 9)
        elif    i == 'subli':   s.r[dst] = s.r[src1] - sext(imm, 9)
        elif    i == 'mulli':   s.r[dst] = s.r[src1] * sext(imm, 9)
        elif    i == 'divli':   s.r[dst] = int(s.r[src1] / sext(imm, 9)) if imm != 0 else 0
        elif    i == 'modli':   s.r[dst] = s.r[src1] % sext(imm, 9) if imm != 0 else 0
        elif    i == 'shlli':   s.r[dst] = s.r[src1] << (imm & 31)
        elif    i == 'shrli':   s.r[dst] = s.r[src1] >> (imm & 31)
        elif    i == 'leli':    s.r[dst] = s.r[src1] <= (sext(imm, 9) & 0xffffffff)
        elif    i == 'gtli':    s.r[dst] = s.r[src1] > (sext(imm, 9) & 0xffffffff)
        elif    i == 'eqli':    s.r[dst] = s.r[src1] == (sext(imm, 9) & 0xffffffff)
        elif    i == 'neli':    s.r[dst] = s.r[src1] != (sext(imm, 9) & 0xffffffff)
        elif    i == 'andli':   s.r[dst] = s.r[src1] & sext(imm, 9)
        elif    i == 'orli':    s.r[dst] = s.r[src1] | sext(imm, 9)
        elif    i == 'xorli':   s.r[dst] = s.r[src1] ^ sext(imm, 9)
        elif    i == 'lesli':   s.r[dst] = sext(s.r[src1], 32) <= sext(imm, 9)
        elif    i == 'gtsli':   s.r[dst] = sext(s.r[src1], 32) > sext(imm, 9)
        elif    i == 'divsli':  s.r[dst] = int(sext(s.r[src1], 32) / sext(imm, 9)) if imm != 0 else 0
        elif    i == 'modsli':  s.r[dst] = sext(s.r[src1], 32) % sext(imm, 9) if imm != 0 else 0
        elif    i == 'shrsli':  s.r[dst] = sext(s.r[src1], 32) >> (imm & 31)
        elif    i == 'bitex':   s.r[dst] = (s.r[src1] >> (imm & 0x1f)) & ((2 << ((imm >> 5) & 0x0f)) - 1)
        elif    i == 'rsubli':  s.r[dst] = sext(imm, 9) - s.r[src1]
        elif    i == 'rdivli':  s.r[dst] = int(sext(imm, 9) / s.r[src1]) if s.r[src1] != 0 else 0
        elif    i == 'rmodli':  s.r[dst] = sext(imm, 9) % s.r[src1] if s.r[src1] != 0 else 0
        elif    i == 'rdivsli': s.r[dst] = int(sext(imm, 9) / sext(s.r[src1], 32)) if s.r[src1] != 0 else 0
        # f0c
        elif    i == 'cbeq':    s.pc = s.pc + sext(imm & 0x3ff, 10) if s.r[src0] == imm >> 10 else s.pc
        elif    i == 'cbne':    s.pc = s.pc + sext(imm & 0x3ff, 10) if s.r[src0] != imm >> 10 else s.pc
        # f1a
        elif    i == 'add':     s.r[dst] = s.r[src0] + s.r[src1]
        elif    i == 'sub':     s.r[dst] = s.r[src0] - s.r[src1]
        elif    i == 'mul':     s.r[dst] = s.r[src0] * s.r[src1]
        elif    i == 'div':     s.r[dst] = int(s.r[src0] / s.r[src1])
        elif    i == 'mod':     s.r[dst] = s.r[src0] % s.r[src1] if s.r[src1] != 0 else 0
        elif    i == 'shl':     s.r[dst] = s.r[src0] << s.r[src1]
        elif    i == 'shr':     s.r[dst] = s.r[src0] >> s.r[src1]
        elif    i == 'lt':      s.r[dst] = s.r[src0] < s.r[src1]
        elif    i == 'le':      s.r[dst] = s.r[src0] <= s.r[src1]
        elif    i == 'eq':      s.r[dst] = s.r[src0] == s.r[src1]
        elif    i == 'ne':      s.r[dst] = s.r[src0] != s.r[src1]
        elif    i == 'and':     s.r[dst] = s.r[src0] & s.r[src1]
        elif    i == 'or':      s.r[dst] = s.r[src0] | s.r[src1]
        elif    i == 'xor':     s.r[dst] = s.r[src0] ^ s.r[src1]
        elif    i == 'lts':     s.r[dst] = sext(s.r[src0], 32) < sext(s.r[src1], 32)
        elif    i == 'les':     s.r[dst] = sext(s.r[src0], 32) <= sext(s.r[src1], 32)
        elif    i == 'divs':    s.r[dst] = int(sext(s.r[src0], 32) / sext(s.r[src1], 32)) if s.r[src1] != 0 else 0
        elif    i == 'mods':    s.r[dst] = sext(s.r[src0], 32) % sext(s.r[src1], 32) if s.r[src1] != 0 else 0
        elif    i == 'shrs':    s.r[dst] = sext(s.r[src0], 32) >> (s.r[src1] & 31)
        elif    i == 'fadd':    s.r[dst] = cpu4_fadd(s.r[src0], s.r[src1])
        elif    i == 'fsub':    s.r[dst] = cpu4_fsub(s.r[src0], s.r[src1])
        elif    i == 'fmul':    s.r[dst] = cpu4_fmul(s.r[src0], s.r[src1])
        elif    i == 'fdiv':    s.r[dst] = cpu4_fdiv(s.r[src0], s.r[src1])
        elif    i == 'flt':     s.r[dst] = cpu4_flt(s.r[src0], s.r[src1])
        elif    i == 'fle':     s.r[dst] = cpu4_fle(s.r[src0], s.r[src1])
        elif    i == 'zxwor':   s.r[dst] = (s.r[src0] | s.r[src1]) & 0xffff
        elif    i == 'sxwor':   v = (s.r[src0] | s.r[src1]) & 0xffff; s.r[dst] = 0xffff0000 | v if v & 0x8000 else v
        # f1b
        elif    i == 'sxb':     s.r[dst] = 0xffffff00 | s.r[src0] if s.r[src0] & 0x80 else 0xff & s.r[src0]
        elif    i == 'sxw':     s.r[dst] = 0xffff0000 | s.r[src0] if s.r[src0] & 0x8000 else 0xffff & s.r[src0]
        elif    i == 'inc':     s.r[dst] = s.r[src0] + 1
        elif    i == 'dec':     s.r[dst] = s.r[src0] - 1
        elif    i == 'pushr':   s.sp -= 4; m.write32(s.sp, s.r[src0])
        elif    i == 'popr':    s.r[dst] = m.read32(s.sp); s.sp += 4
        elif    i == 'zxb':     s.r[dst] = s.r[src0] & 0xff
        elif    i == 'zxw':     s.r[dst] = s.r[src0] & 0xffff
        elif    i == 'itof':    s.r[dst] = cpu4_itof(s.r[src0])
        elif    i == 'ftoi':    s.r[dst] = cpu4_ftoi(s.r[src0])
        elif    i == 'jlr':     s.pc, s.lr = s.r[src0], s.pc
        elif    i == 'jr':      s.pc = s.r[src0]
        elif    i == 'ssp':     s.sp = s.r[src0]
        elif    i == 'neg':     s.r[dst] = -sext(s.r[src0], 32)
        elif    i == 'frecip':  s.r[dst] = cpu4_frecip(s.r[src0])
        elif    i == 'frsqrt':  s.r[dst] = cpu4_frsqrt(s.r[src0])
        elif    i == 'putchar': sys.stderr.write(chr(s.r[src0] & 0xff)); sys.stderr.flush()
        # f2
        elif    i == 'lb':      s.r[dst] = m.read8(s.bp + sext(imm, 7))
        elif    i == 'lw':      s.r[dst] = m.read16(s.bp + sext(imm<<1, 8))
        elif    i == 'll':      s.r[dst] = m.read32(s.bp + sext(imm<<2, 9))
        elif    i == 'sb':      m.write8(s.bp + sext(imm, 7), s.r[src0])
        elif    i == 'sw':      m.write16(s.bp + sext(imm<<1, 8), s.r[src0])
        elif    i == 'sl':      m.write32(s.bp + sext(imm<<2, 9), s.r[src0])
        elif    i == 'lbx':     s.r[dst] = sext(m.read8(s.bp + sext(imm, 7)), 8)
        elif    i == 'lwx':     s.r[dst] = sext(m.read16(s.bp + sext(imm<<1, 8)), 16)
        elif    i == 'addi':    s.r[dst] = s.r[src0] + sext(imm, 7)
        elif    i == 'shli':    s.r[dst] = s.r[src0] << (imm & 0x1f)
        elif    i == 'andi':    s.r[dst] = s.r[src0] & imm
        elif    i == 'shrsi':   s.r[dst] = sext(s.r[src0], 32) >> (imm & 0x1f)
        elif    i == 'imms':    s.r[dst] = sext(imm, 7)
        # f3a
        elif    i == 'j':       s.pc = imm
        elif    i == 'jl':      s.pc, s.lr = imm, s.pc
        elif    i == 'enter':   m.write32(s.sp - 4, (s.lr << 16) | s.bp); s.bp = s.sp - 4; s.sp -= imm + 4
        # f3b
        elif    i == 'adjw':    s.sp += sext((imm << 2), 16)
        elif    i == 'lea':     s.r[dst] = (s.bp + sext((imm << 2), 16)) & 0xffff  # ILP32: high half always zero
        # f3c
        elif    i == 'llb':     s.r[dst] = m.read8(s.r[src1] + sext(imm, 10))
        elif    i == 'llw':     s.r[dst] = m.read16(s.r[src1] + sext(imm<<1, 11))
        elif    i == 'lll':     s.r[dst] = m.read32(s.r[src1] + sext(imm<<2, 12))
        elif    i == 'slb':     m.write8(s.r[src1] + sext(imm, 10), s.r[src0])
        elif    i == 'slw':     m.write16(s.r[src1] + sext(imm<<1, 11), s.r[src0])
        elif    i == 'sll':     m.write32(s.r[src1] + sext(imm<<2, 12), s.r[src0])
        elif    i == 'llbx':    s.r[dst] = sext(m.read8(s.r[src1] + sext(imm, 10)), 8)
        elif    i == 'llwx':    s.r[dst] = sext(m.read16(s.r[src1] + sext(imm<<1, 11)), 16)
        elif    i == 'beq':     s.pc = s.pc + sext(imm, 10) if s.r[src0] == s.r[src1] else s.pc
        elif    i == 'bne':     s.pc = s.pc + sext(imm, 10) if s.r[src0] != s.r[src1] else s.pc
        elif    i == 'blt':     s.pc = s.pc + sext(imm, 10) if s.r[src0] < s.r[src1] else s.pc
        elif    i == 'ble':     s.pc = s.pc + sext(imm, 10) if s.r[src0] <= s.r[src1] else s.pc
        elif    i == 'blts':    s.pc = s.pc + sext(imm, 10) if sext(s.r[src0], 32) < sext(s.r[src1], 32) else s.pc
        elif    i == 'bles':    s.pc = s.pc + sext(imm, 10) if sext(s.r[src0], 32) <= sext(s.r[src1], 32) else s.pc
        # f3d
        elif    i == 'beqz':    s.pc = s.pc + sext(imm, 10) if s.r[src0] == 0 else s.pc
        elif    i == 'bnez':    s.pc = s.pc + sext(imm, 10) if s.r[src0] != 0 else s.pc
        elif    i == 'bltz':    s.pc = s.pc + sext(imm, 10) if sext(s.r[src0], 32) <  0 else s.pc
        elif    i == 'bgez':    s.pc = s.pc + sext(imm, 10) if sext(s.r[src0], 32) >= 0 else s.pc
        elif    i == 'bgtz':    s.pc = s.pc + sext(imm, 10) if sext(s.r[src0], 32) >  0 else s.pc
        elif    i == 'blez':    s.pc = s.pc + sext(imm, 10) if sext(s.r[src0], 32) <= 0 else s.pc
        elif    i == 'dbnz':    s.r[src0] -= 1; s.pc = s.pc + sext(imm, 10) if s.r[src0] != 0 else s.pc
        # f3e
        elif    i == 'immw':    s.r[dst] = imm
        elif    i == 'immwh':   s.r[dst] = (s.r[dst] & 0xffff) | (imm << 16)
        elif    i == 'jz':      s.pc = imm if s.r[src0] == 0 else s.pc
        elif    i == 'jnz':     s.pc = imm if s.r[src0] != 0 else s.pc

        # clean up state
        for i in range(8): s.r[i] &= 0xffffffff
        s.sp &= 0xffff
        s.bp &= 0xffff
        s.lr &= 0xffff
        s.pc &= 0xffff

        # Exception in illegal sp, bp state
        if s.sp & 3:
            sys.stderr.write(f"CPU4 alignment error: SP misaligned 0x{s.sp:04x}\n")
            sys.exit(1)
        if s.bp & 3:
            sys.stderr.write(f"CPU4 alignment error: BP misaligned 0x{s.bp:04x}\n")
            sys.exit(1)


        if trace:
            sins = '%02x    ' % ins if ilen == 1 else '%02x%02x  ' % (ins,imm&0xff) if ilen == 2 else '%02x%02x%02x' % (ins,imm&0xff,(imm>>8)&0xff)
            print('%04x %s %-5s %-12s: %s %08x %08x %08x %08x   %s' % (
                oldpc, sins, i, p, s, 
                m.read32(s.sp + 12, False), m.read32(s.sp + 8, False), m.read32(s.sp + 4, False), m.read32(s.sp, False),
                m.trace ))

        return s



