#!/usr/bin/env python3
"""Two-pass assembler for any ISA defined by an <arch>/isa.py.

Line syntax, labels, sections and data directives are common to every ISA;
instruction encoding comes from the ISA definition through isatool/model.py:
each instruction starts as its fixed bits and each operand is range-checked
and placed into its template field. sim_c's assembler does the same from the
generated C table, so the two produce the same bytes.

  python3 isatool/asm.py [-arch cpu4] file.s [-o image.hex]
"""

import os
import re
import sys
from io import IOBase
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from isatool import model

DIRECTIVES = {'byte': 1, 'word': 2, 'long': 4, 'allocb': 1, 'allocw': 2, 'allocl': 4, 'align': 1}
_ISAS = {}


def isa_for(arch):
    if arch not in _ISAS:
        _ISAS[arch] = model.load(arch)
    return _ISAS[arch]


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


class AsmError(Exception):
    pass


def encode(isa, ins, operands, addr, resolve, final):
    """The instruction's bytes. `resolve(tok)` gives a label's address (0 before
    the final pass); errors are raised only in the final pass."""
    def fail(msg):
        if final:
            raise AsmError(msg)
    if len(operands) != len(ins.ops):
        fail(f"{ins.name} takes {len(ins.ops)} operand(s), got {len(operands)}")
    w = ins.match
    for k, o in enumerate(ins.ops):
        tok = operands[k] if k < len(operands) else '0'
        if o.reg:
            m = re.match(r'^r(\d+)$', tok)
            v = int(m.group(1)) if m else -1
            if not 0 <= v < isa.ngpr:
                fail(f"{ins.name} operand {k + 1}: expected a register r0..r{isa.ngpr - 1}, got {tok!r}")
                v = 0
        else:
            try:
                v = int(tok, 0)
            except ValueError:
                if o.kind in model.LABEL_KINDS:
                    v = resolve(tok)
                else:
                    fail(f"{ins.name} operand {k + 1}: expected a number, got {tok!r}")
                    v = 0
            if o.kind == 'pcrel':
                v = v - (addr + ins.len) if final else 0
            if o.kind == 'bytes':
                if v % o.scale:
                    fail(f"{ins.name} operand {k + 1}: byte offset {v} is not a multiple of {o.scale} at 0x{addr:04x}")
                v = int(v / o.scale)
            lo, hi = o.field_range()
            if not lo <= v <= hi:
                fail(f"{ins.name} operand {k + 1} value {v * o.scale} out of range [{lo * o.scale}..{hi * o.scale}] at 0x{addr:04x}")
        field = v & ((1 << o.bits) - 1)
        left = o.bits
        for shift, width in o.runs(ins.fmt.len):
            left -= width
            w |= ((field >> left) & ((1 << width) - 1)) << shift
    return [(w >> (8 * (ins.len - 1 - k))) & 0xff for k in range(ins.len)]


class Assembler:
    def __init__(self, arch='cpu4'):
        self.isa = isa_for(arch)
        self.clearmem_range = None  # (start_addr, end_addr) or None

    def assemble(self, code, showsymbols=False):
        self.code = code
        self.symbols = {}
        self.assembly = []
        self.clearmem_range = None

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

                # Expand pseudo-ops (operand permutations from the ISA definition)
                if mnemonic in self.isa.pseudos:
                    real_ins, perm = self.isa.pseudos[mnemonic]
                    operands = [operands[p] if p < len(operands) else '' for p in perm]
                    mnemonic = real_ins

                # clearmem pseudo-instruction: clearmem start_label, end_label
                # Zero-fills the image from start_label up to (but not including)
                # end_label at load time. Emits no machine code.
                if mnemonic == 'clearmem':
                    i.length = 0
                    i.ins = []
                    if passes == 'assemble':
                        if len(operands) != 2:
                            print('clearmem requires 2 operands (start, end): %s' % i.line)
                            sys.exit(1)
                        if operands[0] not in self.symbols or operands[1] not in self.symbols:
                            print('clearmem: undefined symbol in %s' % i.line)
                            sys.exit(1)
                        start = self.symbols[operands[0]].addr
                        end   = self.symbols[operands[1]].addr
                        self.clearmem_range = (start, end)
                        self.assembly.append(i)
                    continue

                # Data/space directives
                if mnemonic in DIRECTIVES:
                    size = DIRECTIVES[mnemonic]
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
                        # data values are separated by spaces (sim_c, docs/abi.md) or commas
                        operands = [t for op in operands for t in op.split()]
                        for tok in operands:
                            v = res_tok(tok)
                            for b in range(size):
                                i.ins.append((v >> (8 * b)) & 0xff)
                        i.length = size * len(operands)
                        adr(adr() + i.length)
                        if passes == 'assemble':
                            self.assembly.append(i)
                        continue

                # Real instruction: encode from the ISA definition
                ins = self.isa.by_name.get(mnemonic)
                if ins is None:
                    print('Error line %d: unrecognised instruction %r' % (lineno, mnemonic), file=sys.stderr)
                    sys.exit(1)
                try:
                    i.ins = encode(self.isa, ins, operands, adr(), resolve, passes == 'assemble')
                except AsmError as e:
                    print('Error line %d: %s' % (lineno, e), file=sys.stderr)
                    sys.exit(1)
                i.length = ins.len

                adr(adr() + i.length)
                if passes == 'assemble':
                    self.assembly.append(i)

            if passes == 'assemble' and showsymbols:
                for k, v in sorted(self.symbols.items()):
                    print('%-15s%4s %04x' % (k, v.section, v.addr))

    def makeimage(self, m):
        if self.clearmem_range is not None:
            start, end = self.clearmem_range
            m.mem[start:end] = 0
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
    argparser = argparse.ArgumentParser('Assembler')
    argparser.add_argument('filename', help='input assembly file')
    argparser.add_argument('-arch', default='cpu4', help='ISA (a directory holding isa.py)')
    argparser.add_argument('-o', help='output memory image file')
    args = argparser.parse_args()
    a = Assembler(args.arch)
    with open(args.filename, 'r') as fh:
        data = fh.read()
    a.assemble(data, showsymbols=True)
    from isatool.pysim import Mem
    m = Mem()
    a.makeimage(m)
    a.dumpasm(sys.stdout)
    if args.o:
        m.dumpmemf(open(args.o, 'w'), 0, 512, format='verilog')
