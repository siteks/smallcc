#!/usr/bin/env python3
"""The CPU4 assembler: isatool/asm.py bound to cpu4/isa.py.

Kept so existing callers (cpu4/sim.py, hw/scripts/asm2hex.py, hw/tools/loader.py)
can go on importing `assembler` from this directory.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from isatool.asm import Item, Sym, AsmError, encode, DIRECTIVES  # noqa: F401
from isatool.asm import Assembler as _Assembler


class Assembler(_Assembler):
    def __init__(self, arch='cpu4'):
        super().__init__(arch)


if __name__ == '__main__':
    sys.argv.insert(1, '-arch'); sys.argv.insert(2, 'cpu4')
    import runpy
    runpy.run_path(os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'isatool', 'asm.py'), run_name='__main__')
