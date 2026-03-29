 


import re
import struct
import numpy as np

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
#   r0-r7, r0 implicit accumulator for some ops
#
#   F0  00oooooo
#   F1a 01ooooodddxxxyyy
#   F1b 0111111dddoooooo
#   F2  10ooooxxxiiiiiii
#   F3a 1100ooooiiiiiiiiiiiiiiii
#   F3b 1101ooooxxxyyyiiiiiiiiii
#   F3c 111ooxxxiiiiiiiiiiiiiiii
#
#   Format 0 - r0 or special registers (64 slots)
#   halt
#   ret     sp = bp; bp = *sp++; pc = *sp++
#   itof
#   ftoi
#   jlr     lr = pc; pc = r0 & 0xffff
#   __putchar
#
#   Format 1a - three op (31 slots)
#   add     rd = rx op ry
#   sub
#   mul
#   div
#   mod
#   shl
#   shr
#   lt
#   gt
#   eq
#   ne
#   and
#   or
#   xor
#   lts
#   gts
#   divs
#   mods
#   shrs
#   fadd
#   fsub
#   fmul
#   fdiv
#   flt
#   fgt
#   (4 left)
#
#   Format 1b - single op, (64 slots)
#   sxb     rd
#   sxw     rd
#   inc     rd
#   dec     rd
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
#
#   Format 3a - zero op + imm16 (16 slots)
#   j       pc = imm16
#   jl      lr = pc; pc = imm16
#   jz      pc = r0==0 ? imm16 : pc
#   jnz     pc = r0!=0 ? imm16 : pc
#   enter   [sp-4] = lr; [sp-8] = bp; bp = sp-8; sp -= imm16+8
#   ssp     sp = imm16
#   adjw    sp += sxt(imm16)

#   Format 3b - two op + imm10 (16 slots)
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
#   bgt     pc = rx>ry ? (pc + sxt(imm10)) : pc
#   blts    pc = rx<ry ? (pc + sxt(imm10)) : pc
#   bgts    pc = rx>ry ? (pc + sxt(imm10)) : pc
#   addli   rx = ry + sxt(imm10)

#   Format 3c - one op + imm16 (4 slots)
#   immw    rd = imm16
#   immwh   rd = (rd & 0xffff) | (imm16 << 16)
#   lea     rx = bp + imm16




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
        'align':   1,
    }

    # Fields are a) first byte b) num extra bytes c) subformat d) subopcode
    ptable = {
        # format 0 - zero op, 8 bits    00oooooo
        # This space will be used for hot code ops after analysis
        'halt'  :   (0x00, 0, 0, 0),
        'ret'   :   (0x01, 0, 0, 0),
        'itof'  :   (0x02, 0, 0, 0),
        'ftoi'  :   (0x03, 0, 0, 0),
        'jlr'   :   (0x04, 0, 0, 0),
        'putchar':  (0x1e, 0, 0, 0),
        # format 1a - three op, 16 bits 01ooooodddxxxyyy
        'add'   :   (0x40, 1, 0, 0),
        'sub'   :   (0x42, 1, 0, 0),
        'mul'   :   (0x44, 1, 0, 0),
        'div'   :   (0x46, 1, 0, 0),
        'mod'   :   (0x48, 1, 0, 0),
        'shl'   :   (0x4a, 1, 0, 0),
        'shr'   :   (0x4c, 1, 0, 0),
        'lt'    :   (0x4e, 1, 0, 0),
        'le'    :   (0x50, 1, 0, 0),
        'eq'    :   (0x52, 1, 0, 0),
        'ne'    :   (0x54, 1, 0, 0),
        'and'   :   (0x56, 1, 0, 0),
        'or'    :   (0x58, 1, 0, 0),
        'xor'   :   (0x5a, 1, 0, 0),
        'lts'   :   (0x5c, 1, 0, 0),
        'les'   :   (0x5e, 1, 0, 0),
        'divs'  :   (0x60, 1, 0, 0),
        'mods'  :   (0x62, 1, 0, 0),
        'shrs'  :   (0x64, 1, 0, 0),
        'fadd'  :   (0x66, 1, 0, 0),
        'fsub'  :   (0x68, 1, 0, 0),
        'fmul'  :   (0x6a, 1, 0, 0),
        'fdiv'  :   (0x6c, 1, 0, 0),
        'flt'   :   (0x6e, 1, 0, 0),
        'fle'   :   (0x70, 1, 0, 0),
        # format 1b - one op, 16 bits   0111111dddoooooo
        # this format escapes to give large space for single op no imm
        'sxb'   :   (0x7e, 1, 1, 0x00),
        'sxw'   :   (0x7e, 1, 1, 0x01),
        'inc'   :   (0x7e, 1, 1, 0x02),
        'dec'   :   (0x7e, 1, 1, 0x03),
        # format 2 - one op + imm7      10ooooxxxiiiiiii
        'lb'    :   (0x80, 1, 0, 0),
        'lw'    :   (0x84, 1, 0, 0),
        'll'    :   (0x88, 1, 0, 0),
        'sb'    :   (0x8c, 1, 0, 0),
        'sw'    :   (0x90, 1, 0, 0),
        'sl'    :   (0x94, 1, 0, 0),
        'lbx'   :   (0x98, 1, 0, 0),
        'lwx'   :   (0x9c, 1, 0, 0),
        'addi'  :   (0xa0, 1, 0, 0),
        # format 3a - zero op + imm16   1100ooooiiiiiiiiiiiiiiii
        'j'     :   (0xc0, 2, 0, 0),
        'jl'    :   (0xc1, 2, 0, 0),
        'jz'    :   (0xc2, 2, 0, 0),
        'jnz'   :   (0xc3, 2, 0, 0),
        'enter' :   (0xc4, 2, 0, 0),
        'ssp'   :   (0xc5, 2, 0, 0),
        'adjw'  :   (0xc6, 2, 0, 0),
        # format 3b - two op + imm10    1101ooooxxxyyyiiiiiiiiii
        'llb'   :   (0xd0, 2, 1, 0),
        'llw'   :   (0xd1, 2, 1, 0),
        'lll'   :   (0xd2, 2, 1, 0),
        'slb'   :   (0xd3, 2, 1, 0),
        'slw'   :   (0xd4, 2, 1, 0),
        'sll'   :   (0xd5, 2, 1, 0),
        'llbx'  :   (0xd6, 2, 1, 0),
        'llwx'  :   (0xd7, 2, 1, 0),
        'beq'   :   (0xd8, 2, 1, 0),
        'bne'   :   (0xd9, 2, 1, 0),
        'blt'   :   (0xda, 2, 1, 0),
        'ble'   :   (0xdb, 2, 1, 0),
        'blts'  :   (0xdc, 2, 1, 0),
        'bles'  :   (0xdd, 2, 1, 0),
        'addli' :   (0xde, 2, 1, 0),
        # format 3c - one op + imm16    111ooxxxiiiiiiiiiiiiiiii
        'immw'  :   (0xe8, 2, 2, 0),
        'immwh' :   (0xf0, 2, 2, 0),
        'lea'   :   (0xf8, 2, 2, 0),
    }

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
        # formats f1a, f1b, f2, f3c need bits masking
        lookupins = ins & (0xfe if ins & 0xc0 == 0x40 else 0xfc if ins & 0xc0 == 0x80 else 0xf8 if ins & 0xe0 == 0xe0 else 0xff)
        i, ilen, subfmt = G.rptable[(lookupins, 0)]
        insbyte0 = ins
        fmt     = (ins & 0xc0) >> 6
        # fetch rest of instruction
        if ilen > 0: ins = (ins << 8) | self.mem.read8(s.pc + 1, trace=False)
        if ilen > 1: ins = (ins << 8) | self.mem.read8(s.pc + 2, trace=False)
        s.pc    += ilen + 1

        if lookupins == 0x7e:
            # This is a f1b instruction
            subop = ins & 0x3f
            i, _, _ = G.rptable[(lookupins, subop)]


        imm     = 0
        dst     = 0
        src0    = 0
        src1    = 0

        # extract the fields
        if fmt == 0:
            # irregular decode for op here
            pass
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
            dst = src0 = (ins & 0xe000) >> 13
            src1 = (ins & 0x1c00) >> 10
            imm = ins & 0x3ff
        elif fmt == 3 and subfmt == 2:
            dst = src0 = (ins & 0x70000) >> 16
            imm = ins & 0xffff

        # noinspection PyUnreachableCode
        m = self.mem
        m.trace = ''
        p = ''
        # f0
        if      i == 'halt':    s.H = 1
        elif    i == 'ret':     s.sp = s.bp; s.bp = m.read32(s.sp); s.pc = m.read32(s.sp + 4); s.sp += 8
        elif    i == 'itof':    iv = s.r[0] if s.r[0] < 0x80000000 else s.r[0] - 0x100000000; s.r[0] = f2b(float(iv))
        elif    i == 'ftoi':    s.r[0] = int(b2f(s.r[0])) & 0xffffffff
        elif    i == 'jlr':     s.pc, s.lr = s.r[0], s.pc
        elif    i == 'putchar': sys.stderr.write(chr(s.r[0] & 0xff)); sys.stderr.flush()
        # f1a
        elif    i == 'add':     s.r[dst] = s.r[src0] + s.r[src1]
        elif    i == 'sub':     s.r[dst] = s.r[src0] - s.r[src1]
        elif    i == 'mul':     s.r[dst] = s.r[src0] * s.r[src1]
        elif    i == 'div':     s.r[dst] = int(s.r[src0] / s.r[src1])
        elif    i == 'mod':     s.r[dst] = s.r[src0] % s.r[src1]
        elif    i == 'shl':     s.r[dst] = s.r[src0] << s.r[src1]
        elif    i == 'shr':     s.r[dst] = s.r[src0] >> s.r[src1]
        elif    i == 'lt':      s.r[dst] = s.r[src0] < s.r[src1]
        elif    i == 'le':      s.r[dst] = s.r[src0] <= s.r[src1]
        elif    i == 'eq':      s.r[dst] = s.r[src0] == s.r[src1]
        elif    i == 'ne':      s.r[dst] = s.r[src0] != s.r[src1]
        elif    i == 'and':     s.r[dst] = s.r[src0] & s.r[src1]
        elif    i == 'or':      s.r[dst] = s.r[src0] | s.r[src1]
        elif    i == 'xor':     s.r[dst] = s.r[src0] ^ s.r[src1]
        elif    i == 'lts':     s.r[dst] = 1 if sext(s.r[src0], 32) < sext(s.r[src1], 32) else 0
        elif    i == 'les':     s.r[dst] = 1 if sext(s.r[src0], 32) <= sext(s.r[src1], 32) else 0
        elif    i == 'divs':    s.r[dst] = int(sext(s.r[src0], 32) / sext(s.r[src1], 32)) if s.r[src1] != 0 else 0
        elif    i == 'mods':    s.r[dst] = sext(s.r[src0], 32) % sext(s.r[src1], 32) if s.r[src1] != 0 else 0
        elif    i == 'shrs':    s.r[dst] = sext(s.r[src0], 32) >> (s.r[src1] & 31)
        elif    i == 'fadd':    s.r[dst] = f2b(b2f(s.r[src0]) + b2f(s.r[src1]))
        elif    i == 'fsub':    s.r[dst] = f2b(b2f(s.r[src0]) - b2f(s.r[src1]))
        elif    i == 'fmul':    s.r[dst] = f2b(b2f(s.r[src0]) * b2f(s.r[src1]))
        elif    i == 'fdiv':    s.r[dst] = f2b(b2f(s.r[src0]) / b2f(s.r[src1]))
        elif    i == 'flt':     s.r[dst] = 1 if b2f(s.r[src0]) < b2f(s.r[src1]) else 0
        elif    i == 'fle':     s.r[dst] = 1 if b2f(s.r[src0]) <= b2f(s.r[src1]) else 0
        # f1b
        elif    i == 'sxb':     s.r[dst] = 0xffffff00 | s.r[src0] if s.r[src0] & 0x80 else 0xff & s.r[src0]
        elif    i == 'sxw':     s.r[dst] = 0xffff0000 | s.r[src0] if s.r[src0] & 0x8000 else 0xffff & s.r[src0]
        elif    i == 'inc':     s.r[dst] = s.r[src0] + 1
        elif    i == 'dec':     s.r[dst] = s.r[src0] - 1
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
        # f3a
        elif    i == 'j':       s.pc = imm
        elif    i == 'jl':      s.pc, s.lr = imm, s.pc
        elif    i == 'jz':      s.pc = imm if s.r[0] == 0 else s.pc
        elif    i == 'jnz':     s.pc = imm if s.r[0] != 0 else s.pc
        elif    i == 'enter':   m.write32(s.sp - 4, s.lr); m.write32(s.sp - 8, s.bp); s.bp = s.sp - 8; s.sp -= imm + 8
        elif    i == 'ssp':     s.sp = imm
        elif    i == 'adjw':    s.sp += sext(imm, 16)
        # f3b
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
        elif    i == 'addli':   s.r[dst] = s.r[src1] + sext(imm, 10)
        # f3c
        elif    i == 'immw':    s.r[dst] = imm
        elif    i == 'immwh':   s.r[dst] = (s.r[dst] & 0xffff) | (imm << 16)
        elif    i == 'lea':     s.r[dst] = s.bp + sext(imm, 16)


        # clean up state
        for i in range(8): s.r[i] &= 0xffffffff
        s.sp &= 0xffff
        s.bp &= 0xffff
        s.lr &= 0xffff
        s.pc &= 0xffff

        if trace:
            sins = '%02x    ' % ins if ilen == 1 else '%02x%02x  ' % (ins,imm&0xff) if ilen == 2 else '%02x%02x%02x' % (ins,imm&0xff,(imm>>8)&0xff)
            print('%04x %s %-5s %-12s: %s %08x %08x %08x %08x   %s' % (
                oldpc, sins, i, p, s, 
                m.read32(s.sp + 12, False), m.read32(s.sp + 8, False), m.read32(s.sp + 4, False), m.read32(s.sp, False),
                m.trace ))

        return s



