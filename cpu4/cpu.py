"""The CPU4 Python model: isatool/pysim.py bound to cpu4/isa.py.

Execution is generated (cpu4/exec_gen.py, from cpu4/isa.py by `make isa`);
float arithmetic is cpu4/fpu_model.py, the Python port of cpu4/fpu_model.h,
re-exported here for callers such as tests/gen_fpu_vectors.py. See "Where an
instruction is defined" in docs/isa/cpu4.md.
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fpu_model import *        # noqa: F401,F403  cpu4_fadd ... cpu4_frsqrt, f2b, b2f
from isatool.pysim import Mem, State, MMIO_BASE  # noqa: F401
from isatool.pysim import CPU as _CPU


class CPU(_CPU):
    def __init__(self, m, retire=None, arch='cpu4'):
        super().__init__(m, retire=retire, arch=arch)
