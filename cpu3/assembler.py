#!/usr/bin/env python3
# coding=utf-8



from cpu import G
import re
from io import IOBase


def sexb(b):
    # turn twos complement byte to signed int
    return b if b < 128 else b - 256

class Sym:
    def __init__(self, addr = 0, section = 'text'):
        self.addr    = addr
        self.section = section
    def __repr__(self):
        return 'addr:0x%04x section:%s' % (self.addr, self.section)

class Item:
    def __init__(self, addr=0, ins=[], length=1, label='', source='', section='text'):
        self.addr       = addr
        self.ins        = ins
        self.length     = length
        self.label      = label
        self.source     = source
        self.section    = section
        self.precomment = ''

    def __repr__(self):
        s = '%s%04x %s %-12s  ' % (self.precomment, self.addr,
                                   't' if self.section == 'text' else 'd', self.label)
        for i in range(self.length):
            s += ' %02x' % (self.ins[i])
        # adjust spacing to make nice output
        s += '   ' * (4 - self.length)
        s += '   %-20s' % self.source
        return s


class Assembler(object):
    def __init__(self):
        pass

    def assemble(self, code, showsymbols=False):
        # Take assembly in the form:
        # [<label>:][<ins>[<num>|<label>]]
        # .text [= val]
        # .data [= val]
        self.code = code
        self.symbols = {}
        self.assembly = []

        for passes in ['labels', 'assemble']:
            self.textaddr = 0
            self.dataaddr = 0
            self.mode = 'text'
            def adr(x=None):
                if self.mode == 'text':
                    if x: self.textaddr = x
                    return self.textaddr
                else:
                    if x: self.dataaddr = x
                    return self.dataaddr

            new_addr = True
            for l in code.split('\n'):
                fields = l.split(';')
                actual = fields[0].strip()
                if len(fields) > 1:
                    comment = fields[1]
                else:
                    comment = ''

                # Instructions must be in the form:
                # label: ins op1 op2 op3
                # label can be:
                #   .text
                #   .text=val
                #   label:
                #   label:=val
                # ins can be
                #   word
                # op1 can be
                #   0x1234
                #   [-]1234
                #   word
                m = re.search('''
                    (\.\w+\s*=\s*\w+|\w+:\s*=\s*\w+|\w+:|\.\w+|)?
                    \s*
                    (\w+)?
                    \s*
                    (0x[0-9a-fA-F]+|-?\d+|\w+)?
                    ''', actual, flags=re.VERBOSE)
                #print(actual)
                if m:
                    label   = m.group(1)
                    ins     = m.group(2)
                    op1     = m.group(3)
                    val     = op1
                    print('l:%s i:%s o1:%s' % (label,ins,op1))

                    if label and label[0] == '.':
                        # Section directive
                        self.mode = label[1:5]
                        if len(label) > 5:
                            val = int(label.split('=')[1], 0)
                            adr(val)
                            val = None

                    if new_addr:
                        i = Item()
                        new_addr = False
                    i.addr      = adr()
                    i.section   = self.mode

                    if comment and l[0] == ';':
                        # Comment starting in first column, append to precomment section
                        i.precomment += '%s\n' % comment

                    if val:
                        # See if there is a number here and convert type to int if so
                        try:
                            val = int(val, 0)
                        except ValueError:
                            pass

                    if label and label[0] != '.':
                        # Address label
                        name = label.split(':')[0]
                        self.symbols[name] = Sym(adr(), self.mode)
                        i.label = name
                        print("Added label %s %s" % (name, self.symbols[name]))

                    # Create the assembler representation
                    srcfmtstr = '%-6s %-16s %s%-20s'
                    i.source = srcfmtstr % (ins,
                                            ('%d' % val) if isinstance(val, int) else
                                            ('%-8s' % val) if val else '',
                                            ';' if comment else '', comment)

                    if ins:
                        if ins in G.directive:
                            if ins == 'allocb':
                                i.length = val
                                adr(adr() + val)
                            elif ins == 'allocw':
                                i.length = val
                                if adr() & 1:
                                    # ensure that space allocated is word aligned
                                    adr(adr() + 1)
                                    self.symbols[i.label] = Sym(adr(), self.mode)
                                adr(adr() + val * 2)
                            else:
                                i.length = G.directive[ins]
                                if val:
                                    v = val
                                    i.ins = [(v >> (8 * j)) & 0xff for j in range(i.length)]
                                else:
                                    i.ins = [0]*i.length
                            fmt = 99
                        elif ins in G.ptable:
                            instr, fmt = G.ptable[ins]
                        else:
                            print('%s unrecognised!' % ins)
                            print(G.ptable)
                            exit(1)


                        if fmt == 0:
                            i.ins       = [instr]
                            i.length    = 1
                        elif fmt == 1:
                            i.ins       = [instr, val & 0xff]
                            i.length    = 2
                        elif fmt == 2:
                            val = 0
                            try:
                                val = int(op1, 0)
                            except ValueError:
                                # it might be a label
                                if passes == 'assemble':
                                    # Should now have label
                                    try:
                                        val = self.symbols[op1].addr
                                    except KeyError:
                                        print("Missing symbol! %s" % self.symbols)
                                        exit(1)

                            i.ins       = [instr, val & 0xff, (val >> 8) & 0xff]
                            i.length    = 3


                        adr(adr() + i.length)
                        new_addr = True
                        if passes == 'assemble':
                            self.assembly.append(i)

            if passes == 'assemble' and showsymbols:
                for k,v in self.symbols.items():
                    print('%-15s%4s %04x' % (k, v.section, v.addr))


    def makeimage(self, m):
        for i in self.assembly:
            m.write(i.addr, i.ins)

    def dumpasm(self, outfile):
        if isinstance(outfile, IOBase):
            f = outfile
        else:
            f = open(outfile, 'w')
        for i in self.assembly:
            f.write(i.__repr__() + '\n')

import sys
import cpu
import argparse
if __name__ == '__main__':
    argparser = argparse.ArgumentParser('Assemble')
    argparser.add_argument('filename', help='name of file to parse')
    argparser.add_argument('-o', help='output mem file')
    args = argparser.parse_args()
    a = Assembler()
    with open(args.filename,'r') as f:
        data = f.read()
        a.assemble(data, showsymbols=True)
        a.dumpasm(sys.argv[1] + '.lst')
        m = cpu.Mem()
        a.makeimage(m)
        if args.o:
            m.dumpmemf(open(args.o,'w'), 0, 512, format='verilog')
