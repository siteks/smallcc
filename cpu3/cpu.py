


import re
import numpy as np

import sys



#---------------------------------
# Stack machine as target for compiler. All instructions 8 bit.
# Data 8, 16, 32 bit. Always signed
# all ops 32 bit, all stack operands 32 bit
# pointers 16 bit
#
# State:
#  pc   
#  sp
#  bp
#  lr
#  r0  
#
#

#   halt
#   ret             sp = bp; bp = *sp++; pc = *sp++
#   push            *--sp = r0
#   pushl           *--sp = r0
#   pop             r0 = *sp++
#   popl            r0 = *sp++
#   lb              r0 = *r0
#   lw              r0 = *r0
#   ll              r0 = *r0
#   sb              **--sp = r0
#   sw              **--sp = r0
#   sl              **--sp = r0

#   op              r0 = *sp++ op r0
#   add
#   sub
#   mul
#   div
#   mod
#   shl
#   shr
#   lt
#   le
#   gt
#   ge
#   eq
#   ne
#   and
#   or
#   xor
#
#   immb    byte    r0 = val
#   adj     byte    sp += byte
#
#   immw    word    r0 = val
#   jl      word    lr = pc; pc = word; 
#   j       word    pc = word
#   jz      word    if !r0: pc = word
#   jnz     word    if r0: pc = word
#   enter   word    *--sp = lr; *--sp = bp; bp = sp; sp -= word;
#   lea     word    r0 = bp + val

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

    ptable = {
        # Instructions with no operands
        'halt'  :   (0x00, 0),
        'ret'   :   (0x01, 0),
        'push'  :   (0x02, 0),
        'pushw' :   (0x03, 0),
        'pop'   :   (0x04, 0),
        'popw'  :   (0x05, 0),
        'lb'    :   (0x06, 0),
        'lw'    :   (0x07, 0),
        'll'    :   (0x08, 0),
        'sb'    :   (0x09, 0),
        'sw'    :   (0x0a, 0),
        'sl'    :   (0x0b, 0),
        'add'   :   (0x0c, 0),
        'sub'   :   (0x0d, 0),
        'mul'   :   (0x0e, 0),
        'div'   :   (0x0f, 0),
        'mod'   :   (0x10, 0),
        'shl'   :   (0x11, 0),
        'shr'   :   (0x12, 0),
        'lt'    :   (0x13, 0),
        'le'    :   (0x14, 0),
        'gt'    :   (0x15, 0),
        'ge'    :   (0x16, 0),
        'eq'    :   (0x17, 0),
        'ne'    :   (0x18, 0),
        'and'   :   (0x19, 0),
        'or'    :   (0x1a, 0),
        'xor'   :   (0x1b, 0),
        'sxb'   :   (0x1c, 0),
        'sxw'   :   (0x1d, 0),
        # Instructions with 8 bit operand
        'immb'  :   (0x40, 1),
        'adj'   :   (0x41, 1),
        # Instructions with 16 bit operand
        'immw'  :   (0x80, 2),
        'immwh' :   (0x81, 2),
        'j'     :   (0x82, 2),
        'jl'    :   (0x83, 2),
        'jz'    :   (0x84, 2),
        'jnz'   :   (0x85, 2),
        'enter' :   (0x86, 2),
        'lea'   :   (0x87, 2),
        'ssp'   :   (0x88, 2),
    }

    rptable = {v : k for k, v in ptable.items()}




def sexb(b):
    # turn twos complement byte to signed int
    return b if b < 128 else b - 256
def sexw(b):
    # turn twos complement word to signed int
    return b if b < 0x8000 else b - 0x10000
def sext(b, w):
    # turn twos complement arb length to signed int
    return b if b < (1 << (w - 1)) else b - (1 << w)

class Mem:
    def __init__(self):
        self.mem    = np.full(65536, 0xff, np.byte)
        self.trace = ''

    def read8(self, addr, trace=True):
        addr &= 0xffff
        self.trace += 'r8[%04x]=>%02x   ' % (addr, self.mem[addr] & 0xff) if trace else ''
        return self.mem[addr] & 0xff

    def read16(self, addr, trace=True):
        addr &= 0xffff
        d = (self.mem[addr] & 0xff) | ((self.mem[addr + 1] & 0xff) << 8)
        self.trace += 'r16[%04x]=>%04x ' % (addr, d) if trace else ''
        return d

    def read32(self, addr, trace=True):
        addr &= 0xffff
        d = (self.mem[addr] & 0xff) | ((self.mem[addr + 1] & 0xff) << 8) | ((self.mem[addr + 2] & 0xff) << 16)| ((self.mem[addr + 3] & 0xff) << 24)
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
        self.trace += 'w8[%04x]<=%02x   ' % (addr, data & 0xff)
        self.mem[addr] = data & 0xff

    def write16(self, addr, data):
        addr &= 0xffff
        self.trace += 'w16[%04x]<=%04x ' % (addr, data & 0xffff)
        self.mem[addr] = data & 0xff
        self.mem[addr + 1] = (data >> 8) & 0xff

    def write32(self, addr, data):
        addr &= 0xffff
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
        self.r0 = 0
        self.sp = 0
        self.bp = 0
        self.lr = 0
        self.pc = 0
        self.H  = 0
    def __repr__(self):
        return 'r0:%08x sp:%04x bp:%04x lr:%04x pc:%04x H:%x' % (
            self.r0, self.sp, self.bp, self.lr, self.pc, self.H)



class CPU:
    def __init__(self, m):
        self.state  = State()
        self.mem    = m

    def reset(self):
        self.state.reset()

    def step(self, trace=False):
        s       = self.state
        # Read until we have an instruction.
        # Top two bits, size of data
        # 0x00 - 0x3f   No data
        # 0x40 - 0x7f   One byte
        # 0x80 - 0xbf   Two bytes
        ins     = self.mem.read8(s.pc, trace=False)
        oldpc   = s.pc
        s.pc    += 1
        ilen    = 1
        imm     = 0
        if ins & 0xc0 == 0x40:
            imm = self.mem.read8(s.pc, trace=False)
            imm = sexb(imm)
            s.pc += 1
            ilen = 2
        elif ins & 0xc0 == 0x80:
            imm = self.mem.read16(s.pc, trace=False)
            imm = sexw(imm)
            s.pc += 2
            ilen = 3

        fmt = ilen - 1
        
        # print('%02x %d %04x' % (ins, fmt, imm))
        i = G.rptable[(ins, fmt)]
        # print('%04x %x %s' % (oldpc, ins, i))

        # noinspection PyUnreachableCode
        m = self.mem
        m.trace = ''
        p = ''
        if      i == 'halt':    s.H = 1
        elif    i == 'ret':     s.sp = s.bp; s.bp = m.read32(s.sp); s.pc = m.read32(s.sp + 4); s.sp += 8
        elif    i == 'push':    s.sp -= 4; m.write32(s.sp, s.r0)
        elif    i == 'pop':     s.r0 = m.read32(s.sp); s.sp += 4
        elif    i == 'pushw':   s.sp -= 2; m.write16(s.sp, s.r0)
        elif    i == 'popw':    s.r0 = m.read16(s.sp); s.sp += 2
        elif    i == 'lb':      s.r0 = m.read8(s.r0)
        elif    i == 'lw':      s.r0 = m.read16(s.r0)
        elif    i == 'll':      s.r0 = m.read32(s.r0)
        elif    i == 'sb':      m.write8(m.read32(s.sp), s.r0); s.sp += 4
        elif    i == 'sw':      m.write16(m.read32(s.sp), s.r0); s.sp += 4
        elif    i == 'sl':      m.write32(m.read32(s.sp), s.r0); s.sp += 4

        elif    i == 'add':     s.r0 = m.read32(s.sp) + s.r0; s.sp += 4
        elif    i == 'sub':     s.r0 = m.read32(s.sp) - s.r0; s.sp += 4
        elif    i == 'mul':     s.r0 = m.read32(s.sp) * s.r0; s.sp += 4
        elif    i == 'div':     s.r0 = m.read32(s.sp) / s.r0; s.sp += 4
        elif    i == 'mod':     s.r0 = m.read32(s.sp) % s.r0; s.sp += 4
        elif    i == 'shl':     s.r0 = m.read32(s.sp) << s.r0; s.sp += 4
        elif    i == 'shr':     s.r0 = m.read32(s.sp) >> s.r0; s.sp += 4
        elif    i == 'lt':      s.r0 = m.read32(s.sp) < s.r0; s.sp += 4
        elif    i == 'le':      s.r0 = m.read32(s.sp) <= s.r0; s.sp += 4
        elif    i == 'gt':      s.r0 = m.read32(s.sp) > s.r0; s.sp += 4
        elif    i == 'ge':      s.r0 = m.read32(s.sp) >= s.r0; s.sp += 4
        elif    i == 'eq':      s.r0 = m.read32(s.sp) == s.r0; s.sp += 4
        elif    i == 'ne':      s.r0 = m.read32(s.sp) != s.r0; s.sp += 4
        elif    i == 'and':     s.r0 = m.read32(s.sp) & s.r0; s.sp += 4
        elif    i == 'or':      s.r0 = m.read32(s.sp) | s.r0; s.sp += 4
        elif    i == 'xor':     s.r0 = m.read32(s.sp) ^ s.r0; s.sp += 4

        elif    i == 'sxb':     s.r0 = 0xffffff00 | s.r0 if s.r0 & 0x80 else 0xff & s.r0
        elif    i == 'sxw':     s.r0 = 0xffff0000 | s.r0 if s.r0 & 0x8000 else 0xffff & s.r0
        elif    i == 'immb':    s.r0 = imm
        elif    i == 'adj':     s.sp += imm

        elif    i == 'immw':    s.r0 = imm & 0xffff
        elif    i == 'immwh':   s.r0 = (s.r0 & 0xffff) | (imm << 16)
        elif    i == 'j':       s.pc = imm
        elif    i == 'jl':      s.pc, s.lr = imm, s.pc
        elif    i == 'jz':      s.pc = imm if s.r0 == 0 else s.pc
        elif    i == 'jnz':     s.pc = imm if s.r0 != 0 else s.pc
        elif    i == 'enter':   m.write32(s.sp - 4, s.lr); m.write32(s.sp - 8, s.bp); s.bp = s.sp - 8; s.sp -= imm + 8
        elif    i == 'lea':     s.r0 = s.bp + imm
        elif    i == 'ssp':     s.sp = imm


        # clean up state
        s.r0 &= 0xffffffff
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



