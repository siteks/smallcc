#!/usr/bin/env python3
"""CPU4 two-pass assembler for the 7-format RISC ISA."""

import re
import sys
from io import IOBase
from cpu import G


class Sym:
    def __init__(self, addr=0, section='text'):
        self.addr = addr
        self.section = section

    def __repr__(self):
        return 'addr:0x%04x section:%s' % (self.addr, self.section)


class Item:
    def __init__(self, addr=0, ins=None, length=0, label='', source='', section='text'):
        self.addr = addr
        self.ins = ins if ins is not None else []
        self.length = length
        self.label = label
        self.source = source
        self.section = section
        self.precomment = ''

    def __repr__(self):
        s = '%s%04x %s %-12s  ' % (
            self.precomment, self.addr,
            't' if self.section == 'text' else 'd', self.label)
        display = min(self.length, 4)
        for i in range(display):
            s += ' %02x' % (self.ins[i] if i < len(self.ins) else 0)
        s += '   ' * (4 - display)
        s += '   %-20s' % self.source
        return s


# Pseudo-ops: name -> (real_mnemonic, lambda(operands) -> new_operands)
PSEUDOS = {
    'mov':  ('or',   lambda ops: [ops[0], ops[1], ops[1]]),
    'gt':   ('lt',   lambda ops: [ops[0], ops[2], ops[1]]),
    'ge':   ('le',   lambda ops: [ops[0], ops[2], ops[1]]),
    'gts':  ('lts',  lambda ops: [ops[0], ops[2], ops[1]]),
    'ges':  ('les',  lambda ops: [ops[0], ops[2], ops[1]]),
    'fgt':  ('flt',  lambda ops: [ops[0], ops[2], ops[1]]),
    'fge':  ('fle',  lambda ops: [ops[0], ops[2], ops[1]]),
    'bgt':  ('blt',  lambda ops: [ops[1], ops[0], ops[2]]),
    'bge':  ('ble',  lambda ops: [ops[1], ops[0], ops[2]]),
    'bgts': ('blts', lambda ops: [ops[1], ops[0], ops[2]]),
    'bges': ('bles', lambda ops: [ops[1], ops[0], ops[2]]),
}

# F3b branch mnemonics use PC-relative imm10
BRANCHES = {'beq', 'bne', 'blt', 'ble', 'blts', 'bles'}


def is_reg(s):
    return bool(re.match(r'^r[0-7]$', s))


def reg_num(s):
    return int(s[1])


class Assembler:
    def __init__(self):
        self.clearmem_addr = None

    def assemble(self, code, showsymbols=False):
        self.code = code
        self.symbols = {}
        self.assembly = []
        self.clearmem_addr = None

        for passes in ['labels', 'assemble']:
            self.textaddr = 0
            self.dataaddr = 0
            self.mode = 'text'

            def adr(x=None):
                if self.mode == 'text':
                    if x is not None:
                        self.textaddr = x
                    return self.textaddr
                else:
                    if x is not None:
                        self.dataaddr = x
                    return self.dataaddr

            def resolve(tok, instr_addr=None, instr_len=None, pc_rel=False):
                """Parse a token as an integer literal or label reference."""
                try:
                    return int(tok, 0)
                except (ValueError, TypeError):
                    if passes == 'assemble' and tok in self.symbols:
                        v = self.symbols[tok].addr
                        if pc_rel and instr_addr is not None:
                            return v - (instr_addr + instr_len)
                        return v
                    return 0  # placeholder in 'labels' pass

            if passes == 'assemble':
                self.assembly = []

            prev_comment = ''
            for lineno, rawline in enumerate(code.split('\n'), 1):
                # Strip inline comment
                if ';' in rawline:
                    idx = rawline.index(';')
                    cmt = rawline[idx + 1:].strip()
                    rawline = rawline[:idx]
                    if not rawline.strip() and cmt:
                        prev_comment += '; ' + cmt + '\n'
                        continue
                rawline = rawline.strip()
                if not rawline:
                    continue

                # Parse optional label (word followed by colon)
                label = ''
                rest = rawline
                m = re.match(r'^(\w+)\s*:(.*)', rest)
                if m:
                    label = m.group(1)
                    rest = m.group(2).strip()

                # Section directive: .text[=N] or .data[=N]
                m2 = re.match(r'^(\.\w+)\s*(?:=\s*(0x[0-9a-fA-F]+|\d+))?\s*$', rest)
                if m2:
                    dname = m2.group(1).lower()
                    if dname in ('.text', '.data'):
                        self.mode = dname[1:]
                        if m2.group(2) is not None:
                            adr(int(m2.group(2), 0))
                        if label and passes == 'labels':
                            self.symbols[label] = Sym(adr(), self.mode)
                        prev_comment = ''
                        continue

                # Parse mnemonic and comma-separated operands
                mnemonic = ''
                operands = []
                if rest:
                    parts = rest.split(None, 1)
                    mnemonic = parts[0].lower()
                    if len(parts) > 1:
                        operands = [op.strip() for op in parts[1].split(',') if op.strip()]

                # Build Item for this line
                i = Item(addr=adr(), section=self.mode, label=label)
                i.precomment = prev_comment
                prev_comment = ''
                src_ops = ', '.join(operands) if operands else ''
                i.source = ('%-8s %s' % (mnemonic, src_ops)).strip() if mnemonic else ''

                # Record label in symbol table (labels pass only)
                if label and passes == 'labels':
                    self.symbols[label] = Sym(adr(), self.mode)
                i.label = label

                if not mnemonic:
                    # Label-only line
                    if passes == 'assemble' and label:
                        i.length = 0
                        i.ins = []
                        self.assembly.append(i)
                    continue

                # Expand pseudo-ops
                if mnemonic in PSEUDOS:
                    real_ins, transform = PSEUDOS[mnemonic]
                    operands = transform(operands)
                    mnemonic = real_ins

                # clearmem pseudo-instruction
                if mnemonic == 'clearmem':
                    i.length = 0
                    i.ins = []
                    if passes == 'assemble':
                        if operands and operands[0] in self.symbols:
                            self.clearmem_addr = self.symbols[operands[0]].addr
                        self.assembly.append(i)
                    continue

                # Data/space directives
                if mnemonic in G.directive:
                    size = G.directive[mnemonic]
                    if mnemonic == 'align':
                        # Align to 4 bytes for CPU4 (required for 32-bit accesses)
                        while adr() & 3:
                            adr(adr() + 1)
                        i.length = 0
                        i.ins = []
                        if passes == 'assemble':
                            self.assembly.append(i)
                        continue
                    elif mnemonic.startswith('alloc'):
                        n = int(operands[0], 0) if operands else 0
                        if mnemonic == 'allocw' and (adr() & 1):
                            adr(adr() + 1)
                            if label and label in self.symbols:
                                self.symbols[label].addr = adr()
                            i.addr = adr()
                        byte_count = n * size
                        i.length = byte_count
                        i.ins = [0] * byte_count
                        adr(adr() + byte_count)
                        if passes == 'assemble':
                            self.assembly.append(i)
                        continue
                    else:
                        # byte / word / long: emit values
                        def res_tok(tok):
                            try:
                                return int(tok, 0)
                            except (ValueError, TypeError):
                                if passes == 'assemble' and tok in self.symbols:
                                    return self.symbols[tok].addr
                                return 0

                        i.ins = []
                        for tok in operands:
                            v = res_tok(tok)
                            for b in range(size):
                                i.ins.append((v >> (8 * b)) & 0xff)
                        i.length = size * len(operands)
                        adr(adr() + i.length)
                        if passes == 'assemble':
                            self.assembly.append(i)
                        continue

                # Real instruction: look up ptable
                if mnemonic not in G.ptable:
                    print('Error line %d: unrecognised instruction %r' % (lineno, mnemonic),
                          file=sys.stderr)
                    sys.exit(1)

                first_byte, extra_bytes, subfmt, subop_val = G.ptable[mnemonic]
                instr_addr = adr()
                instr_len = 1 + extra_bytes

                # --- Encode by format ---

                if extra_bytes == 0:
                    # F0: opcode only, no operands
                    i.ins = [first_byte]
                    i.length = 1

                elif extra_bytes == 1 and subfmt == 1:
                    # F1b: single register operand, subopcode in byte1
                    # Encoding: byte0 = first_byte|(rd>>2); byte1 = ((rd&3)<<6)|subop
                    rd = reg_num(operands[0])
                    byte0 = first_byte | (rd >> 2)
                    byte1 = ((rd & 3) << 6) | (subop_val & 0x3f)
                    i.ins = [byte0, byte1]
                    i.length = 2

                elif extra_bytes == 1 and (first_byte & 0xc0) == 0x40:
                    # F1a: three register operands rd, rx, ry
                    # Encoding: byte0 = first_byte|(rd>>2); byte1 = ((rd&3)<<6)|(rx<<3)|ry
                    rd = reg_num(operands[0])
                    rx = reg_num(operands[1])
                    ry = reg_num(operands[2])
                    byte0 = first_byte | (rd >> 2)
                    byte1 = ((rd & 3) << 6) | (rx << 3) | ry
                    i.ins = [byte0, byte1]
                    i.length = 2

                elif extra_bytes == 1 and (first_byte & 0xc0) == 0x80:
                    # F2: register + 7-bit immediate (bp-relative memory access)
                    # Encoding: byte0 = first_byte|(rx>>1); byte1 = ((rx&1)<<7)|(imm7&0x7f)
                    rx = reg_num(operands[0])
                    imm7 = resolve(operands[1]) & 0x7f
                    byte0 = first_byte | (rx >> 1)
                    byte1 = ((rx & 1) << 7) | imm7
                    i.ins = [byte0, byte1]
                    i.length = 2

                elif extra_bytes == 2 and subfmt == 0:
                    # F3a: 16-bit immediate only (absolute address or value)
                    imm16 = resolve(operands[0]) & 0xffff
                    i.ins = [first_byte, (imm16 >> 8) & 0xff, imm16 & 0xff]
                    i.length = 3

                elif extra_bytes == 2 and subfmt == 1:
                    # F3b: two registers + 10-bit immediate
                    # Branches: imm10 is PC-relative (target - PC_after_instruction)
                    # Memory: imm10 is byte/word/long offset
                    # Encoding: byte1 = (rx<<5)|(ry<<2)|((imm10>>8)&3); byte2 = imm10&0xff
                    rx = reg_num(operands[0])
                    ry = reg_num(operands[1])
                    if mnemonic in BRANCHES:
                        imm10 = resolve(operands[2], instr_addr, instr_len, pc_rel=True) & 0x3ff
                    else:
                        imm10 = resolve(operands[2]) & 0x3ff
                    byte0 = first_byte
                    byte1 = (rx << 5) | (ry << 2) | ((imm10 >> 8) & 0x3)
                    byte2 = imm10 & 0xff
                    i.ins = [byte0, byte1, byte2]
                    i.length = 3

                elif extra_bytes == 2 and subfmt == 2:
                    # F3c: single register + 16-bit immediate
                    # Encoding: byte0 = first_byte|(rd&7); byte1,2 = imm16 big-endian
                    rd = reg_num(operands[0])
                    imm16 = resolve(operands[1]) & 0xffff
                    byte0 = first_byte | (rd & 7)
                    byte1 = (imm16 >> 8) & 0xff
                    byte2 = imm16 & 0xff
                    i.ins = [byte0, byte1, byte2]
                    i.length = 3

                else:
                    print('Error line %d: unknown encoding for %s (extra=%d subfmt=%d)' % (
                        lineno, mnemonic, extra_bytes, subfmt), file=sys.stderr)
                    sys.exit(1)

                adr(adr() + i.length)
                if passes == 'assemble':
                    self.assembly.append(i)

            if passes == 'assemble' and showsymbols:
                for k, v in sorted(self.symbols.items()):
                    print('%-15s%4s %04x' % (k, v.section, v.addr))

    def makeimage(self, m):
        if self.clearmem_addr is not None:
            max_addr = max(
                (item.addr + item.length for item in self.assembly if item.length > 0),
                default=self.clearmem_addr)
            m.mem[self.clearmem_addr:max_addr] = 0
        for item in self.assembly:
            if item.ins:
                m.write(item.addr, item.ins)

    def dumpasm(self, outfile):
        if isinstance(outfile, IOBase):
            f = outfile
        else:
            f = open(outfile, 'w')
        for item in self.assembly:
            f.write(item.__repr__() + '\n')


import argparse
if __name__ == '__main__':
    argparser = argparse.ArgumentParser('CPU4 Assembler')
    argparser.add_argument('filename', help='input assembly file')
    argparser.add_argument('-o', help='output memory image file')
    args = argparser.parse_args()
    a = Assembler()
    with open(args.filename, 'r') as fh:
        data = fh.read()
    a.assemble(data, showsymbols=True)
    import cpu
    m = cpu.Mem()
    a.makeimage(m)
    a.dumpasm(sys.stdout)
    if args.o:
        m.dumpmemf(open(args.o, 'w'), 0, 512, format='verilog')
