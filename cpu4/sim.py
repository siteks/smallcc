#!/usr/bin/env python3
"""Run a CPU4 assembly file on the Python model: isatool/pysim.py with --arch cpu4.

  python3 cpu4/sim.py [--maxsteps N] [--retire FILE] file.s
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from isatool import pysim


def runfile(sourcefile, maxsteps=1000, verbose=False, retire=None):
    return pysim.runfile(sourcefile, maxsteps, verbose, retire, 'cpu4')


def runasm(text, filename, maxsteps=1000, verbose=True, retire=None):
    return pysim.runasm(text, filename, maxsteps, verbose, retire, 'cpu4')


if __name__ == "__main__":
    pysim.main('cpu4')
