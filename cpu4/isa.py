"""CPU4 instruction encoding — the single source of truth.

Every other copy of the encoding is generated from, or checked against,
this file by cpu4/gen_isa.py:

  cpu4/isa_table_c.h     sim_c's assembler/disassembler table
  cpu4/isa_table.py      cpu.py's ptable and assembler.py's pseudo-ops
  docs/isa/cpu4-encoding.md   the encoding tables in the ISA document
  hw/scripts/imm_range_check.py   imports this file for immediate ranges
  hw/rtl/decode.v        checked (case labels vs. this table), not generated

It also holds each instruction's semantics as one line of register-transfer
notation (SEMANTICS below; the notation is defined in SEMANTICS_PREAMBLE and
parsed by cpu4/sem.py, which generates the executors: cpu4/exec_gen.h for
sim_c and cpu4/exec_gen.py for cpu4/cpu.py). Float arithmetic is named primitives whose bit-exact definition is
cpu4/fpu_model.h. Prose in docs/isa/cpu4.md explains; this file defines.

Formats (first-byte patterns, see docs/isa/cpu4.md):
  F0a  1 byte   0000 oooo                    no operands
  F0b  3 bytes  0001 oooo | s ddd xxx i | iiiiiiii   rd, rx, imm9 (subop s)
  F0c  3 bytes  001o ddd i ...             rx, imm7, disp10
  F1a  2 bytes  01 ooooo ddd | xxx yyy      rd, rx, ry
  F1b  2 bytes  0111111 ddd | oooooo        rd (subop)
  F2   2 bytes  10 oooo xxx | iiiiiii       rx, imm7
  F3a  3 bytes  110000 oo | imm16           imm16
  F3b  3 bytes  110001 o ddd | imm14        rd, imm14 (x4)
  F3c  3 bytes  1101 oooo | xxx yyy ii | imm10 low   rx, ry, imm10
  F3d  3 bytes  11011111 | xxx ooo ii | imm10 low     rx, imm10 (subop)
  F3e  3 bytes  111 oo xxx | imm16          rx, imm16

Immediate kinds:
  simm   signed value, `bits` wide                    (range checked)
  uimm   unsigned value, `bits` wide                  (range checked)
  index  signed element index, hardware scales by the access width
  bytes4 signed byte offset, encoded /4 in `bits` bits (lea/adjw)
  raw16  16-bit bit pattern; the assembler accepts -32768..65535
  pcrel  PC-relative displacement, always a label in source
  abs16  absolute 16-bit address, usually a label
"""

# (name, fmt, first_byte, subop, operands, imm)
#   operands: tuple of 'rd' | 'rx' | 'ry' | 'imm' | 'imm7' | 'disp'
#   imm: None or (kind, bits[, scale])
I = []
def _i(name, fmt, op, subop=0, ops=(), imm=None):
    I.append(dict(name=name, fmt=fmt, op=op, subop=subop, ops=tuple(ops), imm=imm))

# F0a
for k, n in enumerate(['halt', 'ret', 'zero0', 'zero1', 'zero2', 'zero3', 'zero4', 'zero5', 'zero6', 'zero7']):
    _i(n, 'F0a', k)
# F0b: pairs per first byte, subop 0/1
for fb, (a, b) in zip(range(0x10, 0x1c), [
        ('addli', 'subli'), ('mulli', 'divli'), ('modli', 'shlli'), ('shrli', 'leli'),
        ('gtli', 'eqli'), ('neli', 'andli'), ('orli', 'xorli'), ('lesli', 'gtsli'),
        ('divsli', 'modsli'), ('shrsli', 'bitex'), ('rsubli', 'rdivli'), ('rmodli', 'rdivsli')]):
    for s, n in ((0, a), (1, b)):
        _i(n, 'F0b', fb, s, ('rd', 'rx', 'imm'), ('uimm', 9) if n == 'bitex' else ('simm', 9))
# F0c
_i('cbeq', 'F0c', 0x20, 0, ('rx', 'imm7', 'disp'), ('uimm', 7))
_i('cbne', 'F0c', 0x30, 0, ('rx', 'imm7', 'disp'), ('uimm', 7))
# F1a: opcode field = (first_byte - 0x40) >> 1
for k, n in enumerate(['add', 'sub', 'mul', 'div', 'mod', 'shl', 'shr', 'lt', 'le', 'eq', 'ne', 'and', 'or', 'xor',
                       'lts', 'les', 'divs', 'mods', 'shrs', 'fadd', 'fsub', 'fmul', 'fdiv', 'flt', 'fle', 'zxwor', 'sxwor']):
    _i(n, 'F1a', 0x40 + 2 * k, 0, ('rd', 'rx', 'ry'))
# F1b: subop
for s, n in [(0x00, 'sxb'), (0x01, 'sxw'), (0x02, 'inc'), (0x03, 'dec'), (0x04, 'pushr'), (0x05, 'popr'),
             (0x06, 'zxb'), (0x07, 'zxw'), (0x08, 'itof'), (0x09, 'ftoi'), (0x0a, 'jlr'), (0x0b, 'jr'),
             (0x0c, 'ssp'), (0x0d, 'neg'), (0x0e, 'frecip'), (0x0f, 'frsqrt'), (0x3f, 'putchar')]:
    _i(n, 'F1b', 0x7e, s, ('rd',))
# F2: opcode field = (first_byte >> 2) & 0xf
for fb, n, imm in [(0x80, 'lb', ('index', 7)), (0x84, 'lw', ('index', 7)), (0x88, 'll', ('index', 7)),
                   (0x8c, 'sb', ('index', 7)), (0x90, 'sw', ('index', 7)), (0x94, 'sl', ('index', 7)),
                   (0x98, 'lbx', ('index', 7)), (0x9c, 'lwx', ('index', 7)),
                   (0xa0, 'addi', ('simm', 7)), (0xa4, 'shli', ('simm', 7)), (0xa8, 'andi', ('uimm', 7)),
                   (0xac, 'shrsi', ('simm', 7)), (0xb0, 'imms', ('simm', 7))]:
    _i(n, 'F2', fb, 0, ('rx', 'imm'), imm)
# F3a
_i('j', 'F3a', 0xc0, 0, ('imm',), ('abs16', 16))
_i('jl', 'F3a', 0xc1, 0, ('imm',), ('abs16', 16))
_i('enter', 'F3a', 0xc2, 0, ('imm',), ('uimm', 16))
# F3b
_i('adjw', 'F3b', 0xc4, 0, ('imm',), ('bytes4', 14, 4))
_i('lea', 'F3b', 0xc6, 0, ('rd', 'imm'), ('bytes4', 14, 4))
# F3c: opcode field = first_byte & 0xf
for fb, n in [(0xd0, 'llb'), (0xd1, 'llw'), (0xd2, 'lll'), (0xd3, 'slb'), (0xd4, 'slw'), (0xd5, 'sll'),
              (0xd6, 'llbx'), (0xd7, 'llwx')]:
    _i(n, 'F3c', fb, 0, ('rx', 'ry', 'imm'), ('index', 10))
for fb, n in [(0xd8, 'beq'), (0xd9, 'bne'), (0xda, 'blt'), (0xdb, 'ble'), (0xdc, 'blts'), (0xdd, 'bles')]:
    _i(n, 'F3c', fb, 0, ('rx', 'ry', 'disp'), ('pcrel', 10))
# F3d: subop
for s, n in [(0, 'beqz'), (1, 'bnez'), (2, 'dbnz'), (3, 'bltz'), (4, 'bgez'), (5, 'bgtz'), (6, 'blez')]:
    _i(n, 'F3d', 0xdf, s, ('rx', 'disp'), ('pcrel', 10))
# F3e: opcode field = (first_byte >> 3) & 3
_i('immw', 'F3e', 0xe0, 0, ('rx', 'imm'), ('raw16', 16))
_i('immwh', 'F3e', 0xe8, 0, ('rx', 'imm'), ('raw16', 16))
_i('jz', 'F3e', 0xf0, 0, ('rx', 'imm'), ('abs16', 16))
_i('jnz', 'F3e', 0xf8, 0, ('rx', 'imm'), ('abs16', 16))

INSTRUCTIONS = I
BY_NAME = {d['name']: d for d in I}

# Pseudo-ops: name -> (real mnemonic, operand permutation as source indices)
PSEUDOS = {
    'mov':  ('or',   (0, 1, 1)),
    'gt':   ('lt',   (0, 2, 1)), 'ge':   ('le',   (0, 2, 1)),
    'gts':  ('lts',  (0, 2, 1)), 'ges':  ('les',  (0, 2, 1)),
    'fgt':  ('flt',  (0, 2, 1)), 'fge':  ('fle',  (0, 2, 1)),
    'bgt':  ('blt',  (1, 0, 2)), 'bge':  ('ble',  (1, 0, 2)),
    'bgts': ('blts', (1, 0, 2)), 'bges': ('bles', (1, 0, 2)),
}

# Per-format facts: instruction length, and the (extra_bytes, subfmt) codes
# the two assemblers dispatch on (they differ; both are historical).
# tpl: the format's bit template, first bit = most significant bit of the first
# byte. 0/1 fixed, o opcode (first-byte o bits come from `op`, later ones from
# `subop`), d x y register fields (bound to the register operands in order),
# i j immediate fields (bound to the immediate operands in order).
FORMATS = {
    #        tpl  bytes  sim_c(extra,subfmt)  cpu.py(extra,subfmt)
    'F0a': dict(tpl='0000oooo', bytes=1, c=(0, 0), py=(0, 0)),
    'F0b': dict(tpl='0001oooo odddxxxi iiiiiiii', bytes=3, c=(2, 8), py=(2, 1)),
    'F0c': dict(tpl='001odddi iiiiiijj jjjjjjjj', bytes=3, c=(2, 7), py=(2, 2)),
    'F1a': dict(tpl='01oooood ddxxxyyy', bytes=2, c=(1, 0), py=(1, 0)),
    'F1b': dict(tpl='0111111d ddoooooo', bytes=2, c=(1, 1), py=(1, 1)),
    'F2':  dict(tpl='10ooooxx xiiiiiii', bytes=2, c=(1, 0), py=(1, 0)),
    'F3a': dict(tpl='110000oo iiiiiiii iiiiiiii', bytes=3, c=(2, 0), py=(2, 0)),
    'F3b': dict(tpl='110001od ddiiiiii iiiiiiii', bytes=3, c=(2, 3), py=(2, 1)),
    'F3c': dict(tpl='1101oooo xxxyyyii iiiiiiii', bytes=3, c=(2, 1), py=(2, 2)),
    'F3d': dict(tpl='11011111 xxxoooii iiiiiiii', bytes=3, c=(2, 4), py=(2, 3)),
    'F3e': dict(tpl='111ooxxx iiiiiiii iiiiiiii', bytes=3, c=(2, 2), py=(2, 4)),
}


def imm_range(d):
    """(operand_index, lo, hi) in source units for range checking, or None
    when the immediate is a label (pcrel/abs16) or the op has none."""
    if not d['imm']: return None
    kind, bits = d['imm'][0], d['imm'][1]
    scale = d['imm'][2] if len(d['imm']) > 2 else 1
    if kind in ('pcrel', 'abs16'): return None
    idx = d['ops'].index('imm') if 'imm' in d['ops'] else d['ops'].index('imm7')
    if kind == 'uimm':   return (idx, 0, (1 << bits) - 1)
    if kind == 'raw16':  return (idx, -(1 << (bits - 1)), (1 << bits) - 1)
    lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    return (idx, lo * scale, hi * scale)


def operand_syntax(d):
    """Assembly operand list as written in the ISA document."""
    names = []
    imm = d['imm']
    for o in d['ops']:
        if o in ('rd', 'rx', 'ry'): names.append(o)
        elif o == 'imm7': names.append('imm7')
        elif o == 'disp': names.append('disp%d' % (imm[1] if imm and imm[0] == 'pcrel' else 10))
        elif o == 'imm':
            kind, bits = imm[0], imm[1]
            names.append({'abs16': 'addr16', 'raw16': 'imm16', 'index': 'imm%d' % bits,
                          'bytes4': 'imm%d' % bits}.get(kind, 'imm%d' % bits))
    return ', '.join(names)


# ---------------------------------------------------------------------------
# Semantics
# ---------------------------------------------------------------------------
SEMANTICS_PREAMBLE = """
State: R[0..7] are 32-bit; PC, SP, BP, LR are 16-bit; H is the halt flag.
Memory is byte-addressed, little-endian, 32-bit addresses. Memory the
program image does not cover reads as 0 after reset (a machine guarantee;
C's rules on uninitialised objects still apply to C programs, and loading a
program over the debug port is not a reset). M8[a], M16[a],
M32[a] read or write 1, 2, 4 bytes at a. M16 and M32 accesses must be
naturally aligned: a misaligned access is an alignment fault and stops the
machine (sim_c reports it; the RTL must match).
Operands: rd rx ry name register fields; imm imm7 disp name the raw immediate
field, unsigned. sext(f) is field f sign-extended from its width.
Values: every expression is a 32-bit unsigned value, arithmetic wraps mod 2^32.
Operators, loosest binding first: ?: | ^ & (== !=) (<u <=u >u >=u <s <=s >s >=s)
(<< >> >>s) (+ -) (* /u %u /s %s) unary (- ~ !). Comparisons give 1 or 0; the
suffix says unsigned (u) or signed (s). >> is logical, >>s arithmetic; a shift
by 32 or more gives 0 (>>s: 0 or all ones). /u %u /s %s truncate toward zero;
dividing by zero gives 0; INT_MIN /s -1 gives INT_MIN and INT_MIN %s -1 gives 0.
sx(e,n) sign-extends and zx(e,n) zero-extends the low n bits of e; lo16(e) is
zx(e,16).
Statements are separated by ';' and run in order: each sees the writes of the
ones before it. 'if c then s' runs s when c is non-zero. An assignment keeps
the low bits that fit its destination (PC SP BP LR 16, M8 8, M16 16, H 1).
PC, when read, is the address of the next instruction.
Primitives: fadd fsub fmul fdiv flt fle itof ftoi frecip frsqrt are defined
bit-exactly by cpu4/fpu_model.h; putchar(e) writes the low byte of e to the
console.
After every instruction SP and BP must be multiples of 4, or the machine stops.
"""

SEMANTICS = {
    # F0a
    'halt': 'H = 1',
    'ret': 'SP = BP; BP = M32[SP]; PC = M32[SP] >> 16; SP = SP + 4',
    # F0b (rd, rx, imm9)
    'addli': 'R[rd] = R[rx] + sext(imm)',       'subli': 'R[rd] = R[rx] - sext(imm)',
    'mulli': 'R[rd] = R[rx] * sext(imm)',       'divli': 'R[rd] = R[rx] /u sext(imm)',
    'modli': 'R[rd] = R[rx] %u sext(imm)',      'shlli': 'R[rd] = R[rx] << (imm & 31)',
    'shrli': 'R[rd] = R[rx] >> (imm & 31)',     'leli':  'R[rd] = R[rx] <=u sext(imm)',
    'gtli':  'R[rd] = R[rx] >u sext(imm)',      'eqli':  'R[rd] = R[rx] == sext(imm)',
    'neli':  'R[rd] = R[rx] != sext(imm)',      'andli': 'R[rd] = R[rx] & sext(imm)',
    'orli':  'R[rd] = R[rx] | sext(imm)',       'xorli': 'R[rd] = R[rx] ^ sext(imm)',
    'lesli': 'R[rd] = R[rx] <=s sext(imm)',     'gtsli': 'R[rd] = R[rx] >s sext(imm)',
    'divsli': 'R[rd] = R[rx] /s sext(imm)',     'modsli': 'R[rd] = R[rx] %s sext(imm)',
    'shrsli': 'R[rd] = R[rx] >>s (imm & 31)',
    'bitex': 'R[rd] = (R[rx] >> (imm & 31)) & ((2 << ((imm >> 5) & 15)) - 1)',
    'rsubli': 'R[rd] = sext(imm) - R[rx]',      'rdivli': 'R[rd] = sext(imm) /u R[rx]',
    'rmodli': 'R[rd] = sext(imm) %u R[rx]',     'rdivsli': 'R[rd] = sext(imm) /s R[rx]',
    # F0c (rx, imm7, disp10)
    'cbeq': 'if R[rx] == imm7 then PC = PC + sext(disp)',
    'cbne': 'if R[rx] != imm7 then PC = PC + sext(disp)',
    # F1a (rd, rx, ry)
    'add': 'R[rd] = R[rx] + R[ry]',    'sub': 'R[rd] = R[rx] - R[ry]',    'mul': 'R[rd] = R[rx] * R[ry]',
    'div': 'R[rd] = R[rx] /u R[ry]',   'mod': 'R[rd] = R[rx] %u R[ry]',
    'shl': 'R[rd] = R[rx] << (R[ry] & 31)', 'shr': 'R[rd] = R[rx] >> (R[ry] & 31)',
    'lt': 'R[rd] = R[rx] <u R[ry]',    'le': 'R[rd] = R[rx] <=u R[ry]',
    'eq': 'R[rd] = R[rx] == R[ry]',    'ne': 'R[rd] = R[rx] != R[ry]',
    'and': 'R[rd] = R[rx] & R[ry]',    'or': 'R[rd] = R[rx] | R[ry]',     'xor': 'R[rd] = R[rx] ^ R[ry]',
    'lts': 'R[rd] = R[rx] <s R[ry]',   'les': 'R[rd] = R[rx] <=s R[ry]',
    'divs': 'R[rd] = R[rx] /s R[ry]',  'mods': 'R[rd] = R[rx] %s R[ry]',
    'shrs': 'R[rd] = R[rx] >>s (R[ry] & 31)',
    'fadd': 'R[rd] = fadd(R[rx], R[ry])', 'fsub': 'R[rd] = fsub(R[rx], R[ry])',
    'fmul': 'R[rd] = fmul(R[rx], R[ry])', 'fdiv': 'R[rd] = fdiv(R[rx], R[ry])',
    'flt': 'R[rd] = flt(R[rx], R[ry])',   'fle': 'R[rd] = fle(R[rx], R[ry])',
    'zxwor': 'R[rd] = zx(R[rx] | R[ry], 16)', 'sxwor': 'R[rd] = sx(R[rx] | R[ry], 16)',
    # F1b (rd)
    'sxb': 'R[rd] = sx(R[rd], 8)',   'sxw': 'R[rd] = sx(R[rd], 16)',
    'inc': 'R[rd] = R[rd] + 1',      'dec': 'R[rd] = R[rd] - 1',
    'pushr': 'SP = SP - 4; M32[SP] = R[rd]',
    'popr': 'R[rd] = M32[SP]; SP = SP + 4',
    'zxb': 'R[rd] = zx(R[rd], 8)',   'zxw': 'R[rd] = zx(R[rd], 16)',
    'itof': 'R[rd] = itof(R[rd])',   'ftoi': 'R[rd] = ftoi(R[rd])',
    'jlr': 'LR = PC; PC = R[rd]',    'jr': 'PC = R[rd]',     'ssp': 'SP = R[rd]',
    'neg': 'R[rd] = -R[rd]',
    'frecip': 'R[rd] = frecip(R[rd])', 'frsqrt': 'R[rd] = frsqrt(R[rd])',
    'putchar': 'putchar(R[rd])',
    # F2 (rx, imm7): bp-relative, addresses wrap at 64 KiB
    'lb': 'R[rx] = M8[lo16(BP + sext(imm))]',
    'lw': 'R[rx] = M16[lo16(BP + sext(imm) * 2)]',
    'll': 'R[rx] = M32[lo16(BP + sext(imm) * 4)]',
    'sb': 'M8[lo16(BP + sext(imm))] = R[rx]',
    'sw': 'M16[lo16(BP + sext(imm) * 2)] = R[rx]',
    'sl': 'M32[lo16(BP + sext(imm) * 4)] = R[rx]',
    'lbx': 'R[rx] = sx(M8[lo16(BP + sext(imm))], 8)',
    'lwx': 'R[rx] = sx(M16[lo16(BP + sext(imm) * 2)], 16)',
    'addi': 'R[rx] = R[rx] + sext(imm)',
    'shli': 'R[rx] = R[rx] << (imm & 31)',
    'andi': 'R[rx] = R[rx] & imm',
    'shrsi': 'R[rx] = R[rx] >>s (imm & 31)',
    'imms': 'R[rx] = sext(imm)',
    # F3a (imm16)
    'j': 'PC = imm',
    'jl': 'LR = PC; PC = imm',
    'enter': 'M32[lo16(SP - 4)] = (LR << 16) | BP; BP = SP - 4; SP = SP - imm - 4',
    # F3b (imm14, scaled by 4)
    'adjw': 'SP = SP + sext(imm) * 4',
    'lea': 'R[rd] = lo16(BP + sext(imm) * 4)',
    # F3c (rx, ry, imm10): register-relative, 32-bit addresses
    'llb': 'R[rx] = M8[R[ry] + sext(imm)]',
    'llw': 'R[rx] = M16[R[ry] + sext(imm) * 2]',
    'lll': 'R[rx] = M32[R[ry] + sext(imm) * 4]',
    'slb': 'M8[R[ry] + sext(imm)] = R[rx]',
    'slw': 'M16[R[ry] + sext(imm) * 2] = R[rx]',
    'sll': 'M32[R[ry] + sext(imm) * 4] = R[rx]',
    'llbx': 'R[rx] = sx(M8[R[ry] + sext(imm)], 8)',
    'llwx': 'R[rx] = sx(M16[R[ry] + sext(imm) * 2], 16)',
    'beq': 'if R[rx] == R[ry] then PC = PC + sext(disp)',
    'bne': 'if R[rx] != R[ry] then PC = PC + sext(disp)',
    'blt': 'if R[rx] <u R[ry] then PC = PC + sext(disp)',
    'ble': 'if R[rx] <=u R[ry] then PC = PC + sext(disp)',
    'blts': 'if R[rx] <s R[ry] then PC = PC + sext(disp)',
    'bles': 'if R[rx] <=s R[ry] then PC = PC + sext(disp)',
    # F3d (rx, disp10)
    'beqz': 'if R[rx] == 0 then PC = PC + sext(disp)',
    'bnez': 'if R[rx] != 0 then PC = PC + sext(disp)',
    'dbnz': 'R[rx] = R[rx] - 1; if R[rx] != 0 then PC = PC + sext(disp)',
    'bltz': 'if R[rx] <s 0 then PC = PC + sext(disp)',
    'bgez': 'if R[rx] >=s 0 then PC = PC + sext(disp)',
    'bgtz': 'if R[rx] >s 0 then PC = PC + sext(disp)',
    'blez': 'if R[rx] <=s 0 then PC = PC + sext(disp)',
    # F3e (rx, imm16)
    'immw': 'R[rx] = imm',
    'immwh': 'R[rx] = zx(R[rx], 16) | (imm << 16)',
    'jz': 'if R[rx] == 0 then PC = imm',
    'jnz': 'if R[rx] != 0 then PC = imm',
}
for _k in range(8):
    SEMANTICS['zero%d' % _k] = 'R[%d] = 0' % _k
assert set(SEMANTICS) == set(BY_NAME), sorted(set(SEMANTICS) ^ set(BY_NAME))
