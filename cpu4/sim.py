#!/usr/bin/env python3

import argparse
import sys
import assembler
import cpu


def runfile(sourcefile, maxsteps=1000, verbose=False):
    # Get text of C program
    text = open(sourcefile).read()
    return runasm(text, sourcefile, maxsteps, verbose)


def runasm(text, filename, maxsteps=1000, verbose=True):

    # Create memory
    m = cpu.Mem()
    # Assemble the code and output result
    a = assembler.Assembler()
    #print(text)
    a.assemble(text, showsymbols=verbose)

    # Put machine code image in memory
    a.makeimage(m)

    if verbose:
        a.dumpasm('%s.lst' % filename)
        a.dumpasm(sys.stdout)
        m.dumpmem(0, 128)
        m.dumpmem(0x2000, 0x20)

    # Create the CPU and step until halted or run out of steps
    c = cpu.CPU(m)
    c.reset()
    for i in range(maxsteps):
        s = c.step(trace=verbose)
        if s.H:
            break

    return s


if __name__ == "__main__":
    argparser = argparse.ArgumentParser('Dump AST')
    argparser.add_argument('filename', help='name of file to parse')
    argparser.add_argument('-v', '--verbose', action='store_true', help='verbose')
    argparser.add_argument('--maxsteps', type=int, default=1000, help='max simulation steps')
    args = argparser.parse_args()

    s = runfile(args.filename, maxsteps=args.maxsteps, verbose=args.verbose)
    print(s)

