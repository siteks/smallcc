"""CPU4 floating-point semantics in Python: a line-for-line port of
cpu4/fpu_model.h, which is the bit-exact specification (see its header and the
"Floating-point semantics" section of docs/isa/cpu4.md). Used by the generated
executor (cpu4/exec_gen.py) and by tests/gen_fpu_vectors.py; tools/rig.py checks
it against the C model on random operands by running cpu.py and sim_c in lockstep.
"""
import struct

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

def cpu5_frecip(x):
    x &= 0xffffffff
    sign = x & 0x80000000; e = (x >> 23) & 0xff; i = (x >> 13) & 0x3ff
    if e == 0: return sign
    ee = (254 - e) if i == 0 else (253 - e)
    if ee <= 0: return sign
    return sign | (ee << 23) | (FRECIP_ROM[i] << 7)

def cpu5_frsqrt(x):
    x &= 0xffffffff
    e = (x >> 23) & 0xff; m = x & 0x7fffff
    if e == 0: return 0x7f800000
    if e == 255: return 0
    p = (~e) & 1; i = (p << 9) | (m >> 14)
    ee = (379 - e) // 2 if (e & 1) else (380 - e) // 2
    return ((ee & 0xff) << 23) | (FRSQRT_ROM[i] << 7)

def cpu5_fmul(a, b):
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

def cpu5_fadd(a, b):
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

def cpu5_fsub(a, b): return cpu5_fadd(a, b ^ 0x80000000)
def cpu5_fdiv(a, b): return cpu5_fmul(a, cpu5_frecip(b))

def cpu5_flt(a, b):
    a &= 0xffffffff; b &= 0xffffffff
    sa = a >> 31; sb = b >> 31; aa = a & 0x7fffffff; ab = b & 0x7fffffff
    if aa == 0 and ab == 0: return 0
    if sa and not sb: return 1
    if not sa and sb: return 0
    return int(aa > ab) if sa else int(aa < ab)

def cpu5_fle(a, b):
    a &= 0xffffffff; b &= 0xffffffff
    sa = a >> 31; sb = b >> 31; aa = a & 0x7fffffff; ab = b & 0x7fffffff
    if aa == 0 and ab == 0: return 1
    if sa and not sb: return 1
    if not sa and sb: return 0
    return int(aa >= ab) if sa else int(aa <= ab)

def cpu5_itof(i):
    i &= 0xffffffff
    if i == 0: return 0
    sign = i & 0x80000000
    ab = ((-i) & 0xffffffff) if sign else i
    pos = ab.bit_length() - 1
    mant = (ab >> (pos - 23)) if pos > 23 else (ab << (23 - pos))
    return sign | ((127 + pos) << 23) | (mant & 0x7fffff)

def cpu5_ftoi(x):
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
