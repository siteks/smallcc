"""CPU4 instruction set: the definition.

Pure data, in the schema isatool/model.py describes: the machine state, the
bit template of every format, every instruction's opcode and operands,
assembler pseudo-ops, machine invariants, the float primitives, and one line
of register-transfer semantics per instruction (notation defined by
SEMANTICS_PREAMBLE). `make isa` (isatool/gen.py) generates from it:

  cpu4/isa_table_c.h          sim_c's assembler and disassembler table
  cpu4/exec_gen.h             sim_c's executor
  cpu4/exec_gen.py            cpu4/cpu.py's executor
  docs/isa/cpu4-encoding.md   the encoding tables with semantics

and the hardware's `make isa-check` compares rtl/decode.v with it. Prose in
docs/isa/cpu4.md explains; this file defines.
"""

NAME = 'cpu4'

STATE = {
    'gpr': 8,                    # r0..r7
    'gpr_bits': 32,
    'special': {'PC': 16, 'SP': 16, 'BP': 16, 'LR': 16},
}

FORMATS = {
    'F0a': '0000oooo',
    'F0b': '0001oooo odddxxxi iiiiiiii',
    'F0c': '001odddi iiiiiijj jjjjjjjj',
    'F1a': '01oooood ddxxxyyy',
    'F1b': '0111111d ddoooooo',
    'F2': '10ooooxx xiiiiiii',
    'F3a': '110000oo iiiiiiii iiiiiiii',
    'F3b': '110001od ddiiiiii iiiiiiii',
    'F3c': '1101oooo xxxyyyii iiiiiiii',
    'F3d': '11011111 xxxoooii iiiiiiii',
    'F3e': '111ooxxx iiiiiiii iiiiiiii',
}

# (name, format, opcode = value of the format's o bits, operands in assembly order)
INSTRUCTIONS = [
    # F0a  0000oooo
    ('halt',    'F0a',  0b0000,     ()),
    ('ret',     'F0a',  0b0001,     ()),
    ('zero0',   'F0a',  0b0010,     ()),
    ('zero1',   'F0a',  0b0011,     ()),
    ('zero2',   'F0a',  0b0100,     ()),
    ('zero3',   'F0a',  0b0101,     ()),
    ('zero4',   'F0a',  0b0110,     ()),
    ('zero5',   'F0a',  0b0111,     ()),
    ('zero6',   'F0a',  0b1000,     ()),
    ('zero7',   'F0a',  0b1001,     ()),
    # F0b  0001oooo odddxxxi iiiiiiii
    ('addli',   'F0b',  0b00000,    ('rd', 'rx', ('imm', 'simm'))),
    ('subli',   'F0b',  0b00001,    ('rd', 'rx', ('imm', 'simm'))),
    ('mulli',   'F0b',  0b00010,    ('rd', 'rx', ('imm', 'simm'))),
    ('divli',   'F0b',  0b00011,    ('rd', 'rx', ('imm', 'simm'))),
    ('modli',   'F0b',  0b00100,    ('rd', 'rx', ('imm', 'simm'))),
    ('shlli',   'F0b',  0b00101,    ('rd', 'rx', ('imm', 'simm'))),
    ('shrli',   'F0b',  0b00110,    ('rd', 'rx', ('imm', 'simm'))),
    ('leli',    'F0b',  0b00111,    ('rd', 'rx', ('imm', 'simm'))),
    ('gtli',    'F0b',  0b01000,    ('rd', 'rx', ('imm', 'simm'))),
    ('eqli',    'F0b',  0b01001,    ('rd', 'rx', ('imm', 'simm'))),
    ('neli',    'F0b',  0b01010,    ('rd', 'rx', ('imm', 'simm'))),
    ('andli',   'F0b',  0b01011,    ('rd', 'rx', ('imm', 'simm'))),
    ('orli',    'F0b',  0b01100,    ('rd', 'rx', ('imm', 'simm'))),
    ('xorli',   'F0b',  0b01101,    ('rd', 'rx', ('imm', 'simm'))),
    ('lesli',   'F0b',  0b01110,    ('rd', 'rx', ('imm', 'simm'))),
    ('gtsli',   'F0b',  0b01111,    ('rd', 'rx', ('imm', 'simm'))),
    ('divsli',  'F0b',  0b10000,    ('rd', 'rx', ('imm', 'simm'))),
    ('modsli',  'F0b',  0b10001,    ('rd', 'rx', ('imm', 'simm'))),
    ('shrsli',  'F0b',  0b10010,    ('rd', 'rx', ('imm', 'simm'))),
    ('bitex',   'F0b',  0b10011,    ('rd', 'rx', ('imm', 'uimm'))),
    ('rsubli',  'F0b',  0b10100,    ('rd', 'rx', ('imm', 'simm'))),
    ('rdivli',  'F0b',  0b10101,    ('rd', 'rx', ('imm', 'simm'))),
    ('rmodli',  'F0b',  0b10110,    ('rd', 'rx', ('imm', 'simm'))),
    ('rdivsli', 'F0b',  0b10111,    ('rd', 'rx', ('imm', 'simm'))),
    # F0c  001odddi iiiiiijj jjjjjjjj
    ('cbeq',    'F0c',  0b0,        ('rx', ('imm7', 'uimm'), ('disp', 'pcrel'))),
    ('cbne',    'F0c',  0b1,        ('rx', ('imm7', 'uimm'), ('disp', 'pcrel'))),
    # F1a  01oooood ddxxxyyy
    ('add',     'F1a',  0b00000,    ('rd', 'rx', 'ry')),
    ('sub',     'F1a',  0b00001,    ('rd', 'rx', 'ry')),
    ('mul',     'F1a',  0b00010,    ('rd', 'rx', 'ry')),
    ('div',     'F1a',  0b00011,    ('rd', 'rx', 'ry')),
    ('mod',     'F1a',  0b00100,    ('rd', 'rx', 'ry')),
    ('shl',     'F1a',  0b00101,    ('rd', 'rx', 'ry')),
    ('shr',     'F1a',  0b00110,    ('rd', 'rx', 'ry')),
    ('lt',      'F1a',  0b00111,    ('rd', 'rx', 'ry')),
    ('le',      'F1a',  0b01000,    ('rd', 'rx', 'ry')),
    ('eq',      'F1a',  0b01001,    ('rd', 'rx', 'ry')),
    ('ne',      'F1a',  0b01010,    ('rd', 'rx', 'ry')),
    ('and',     'F1a',  0b01011,    ('rd', 'rx', 'ry')),
    ('or',      'F1a',  0b01100,    ('rd', 'rx', 'ry')),
    ('xor',     'F1a',  0b01101,    ('rd', 'rx', 'ry')),
    ('lts',     'F1a',  0b01110,    ('rd', 'rx', 'ry')),
    ('les',     'F1a',  0b01111,    ('rd', 'rx', 'ry')),
    ('divs',    'F1a',  0b10000,    ('rd', 'rx', 'ry')),
    ('mods',    'F1a',  0b10001,    ('rd', 'rx', 'ry')),
    ('shrs',    'F1a',  0b10010,    ('rd', 'rx', 'ry')),
    ('fadd',    'F1a',  0b10011,    ('rd', 'rx', 'ry')),
    ('fsub',    'F1a',  0b10100,    ('rd', 'rx', 'ry')),
    ('fmul',    'F1a',  0b10101,    ('rd', 'rx', 'ry')),
    ('fdiv',    'F1a',  0b10110,    ('rd', 'rx', 'ry')),
    ('flt',     'F1a',  0b10111,    ('rd', 'rx', 'ry')),
    ('fle',     'F1a',  0b11000,    ('rd', 'rx', 'ry')),
    ('zxwor',   'F1a',  0b11001,    ('rd', 'rx', 'ry')),
    ('sxwor',   'F1a',  0b11010,    ('rd', 'rx', 'ry')),
    # F1b  0111111d ddoooooo
    ('sxb',     'F1b',  0b000000,   ('rd',)),
    ('sxw',     'F1b',  0b000001,   ('rd',)),
    ('inc',     'F1b',  0b000010,   ('rd',)),
    ('dec',     'F1b',  0b000011,   ('rd',)),
    ('pushr',   'F1b',  0b000100,   ('rd',)),
    ('popr',    'F1b',  0b000101,   ('rd',)),
    ('zxb',     'F1b',  0b000110,   ('rd',)),
    ('zxw',     'F1b',  0b000111,   ('rd',)),
    ('itof',    'F1b',  0b001000,   ('rd',)),
    ('ftoi',    'F1b',  0b001001,   ('rd',)),
    ('jlr',     'F1b',  0b001010,   ('rd',)),
    ('jr',      'F1b',  0b001011,   ('rd',)),
    ('ssp',     'F1b',  0b001100,   ('rd',)),
    ('neg',     'F1b',  0b001101,   ('rd',)),
    ('frecip',  'F1b',  0b001110,   ('rd',)),
    ('frsqrt',  'F1b',  0b001111,   ('rd',)),
    ('putchar', 'F1b',  0b111111,   ('rd',)),
    # F2  10ooooxx xiiiiiii
    ('lb',      'F2',   0b0000,     ('rx', ('imm', 'index'))),
    ('lw',      'F2',   0b0001,     ('rx', ('imm', 'index'))),
    ('ll',      'F2',   0b0010,     ('rx', ('imm', 'index'))),
    ('sb',      'F2',   0b0011,     ('rx', ('imm', 'index'))),
    ('sw',      'F2',   0b0100,     ('rx', ('imm', 'index'))),
    ('sl',      'F2',   0b0101,     ('rx', ('imm', 'index'))),
    ('lbx',     'F2',   0b0110,     ('rx', ('imm', 'index'))),
    ('lwx',     'F2',   0b0111,     ('rx', ('imm', 'index'))),
    ('addi',    'F2',   0b1000,     ('rx', ('imm', 'simm'))),
    ('shli',    'F2',   0b1001,     ('rx', ('imm', 'simm'))),
    ('andi',    'F2',   0b1010,     ('rx', ('imm', 'uimm'))),
    ('shrsi',   'F2',   0b1011,     ('rx', ('imm', 'simm'))),
    ('imms',    'F2',   0b1100,     ('rx', ('imm', 'simm'))),
    # F3a  110000oo iiiiiiii iiiiiiii
    ('j',       'F3a',  0b00,       (('imm', 'abs'),)),
    ('jl',      'F3a',  0b01,       (('imm', 'abs'),)),
    ('enter',   'F3a',  0b10,       (('imm', 'uimm'),)),
    # F3b  110001od ddiiiiii iiiiiiii
    ('adjw',    'F3b',  0b0,        (('imm', 'bytes', 4),)),
    ('lea',     'F3b',  0b1,        ('rd', ('imm', 'bytes', 4))),
    # F3c  1101oooo xxxyyyii iiiiiiii
    ('llb',     'F3c',  0b0000,     ('rx', 'ry', ('imm', 'index'))),
    ('llw',     'F3c',  0b0001,     ('rx', 'ry', ('imm', 'index'))),
    ('lll',     'F3c',  0b0010,     ('rx', 'ry', ('imm', 'index'))),
    ('slb',     'F3c',  0b0011,     ('rx', 'ry', ('imm', 'index'))),
    ('slw',     'F3c',  0b0100,     ('rx', 'ry', ('imm', 'index'))),
    ('sll',     'F3c',  0b0101,     ('rx', 'ry', ('imm', 'index'))),
    ('llbx',    'F3c',  0b0110,     ('rx', 'ry', ('imm', 'index'))),
    ('llwx',    'F3c',  0b0111,     ('rx', 'ry', ('imm', 'index'))),
    ('beq',     'F3c',  0b1000,     ('rx', 'ry', ('disp', 'pcrel'))),
    ('bne',     'F3c',  0b1001,     ('rx', 'ry', ('disp', 'pcrel'))),
    ('blt',     'F3c',  0b1010,     ('rx', 'ry', ('disp', 'pcrel'))),
    ('ble',     'F3c',  0b1011,     ('rx', 'ry', ('disp', 'pcrel'))),
    ('blts',    'F3c',  0b1100,     ('rx', 'ry', ('disp', 'pcrel'))),
    ('bles',    'F3c',  0b1101,     ('rx', 'ry', ('disp', 'pcrel'))),
    # F3d  11011111 xxxoooii iiiiiiii
    ('beqz',    'F3d',  0b000,      ('rx', ('disp', 'pcrel'))),
    ('bnez',    'F3d',  0b001,      ('rx', ('disp', 'pcrel'))),
    ('dbnz',    'F3d',  0b010,      ('rx', ('disp', 'pcrel'))),
    ('bltz',    'F3d',  0b011,      ('rx', ('disp', 'pcrel'))),
    ('bgez',    'F3d',  0b100,      ('rx', ('disp', 'pcrel'))),
    ('bgtz',    'F3d',  0b101,      ('rx', ('disp', 'pcrel'))),
    ('blez',    'F3d',  0b110,      ('rx', ('disp', 'pcrel'))),
    # F3e  111ooxxx iiiiiiii iiiiiiii
    ('immw',    'F3e',  0b00,       ('rx', ('imm', 'raw'))),
    ('immwh',   'F3e',  0b01,       ('rx', ('imm', 'raw'))),
    ('jz',      'F3e',  0b10,       ('rx', ('imm', 'abs'))),
    ('jnz',     'F3e',  0b11,       ('rx', ('imm', 'abs'))),
]

# pseudo-op -> (real instruction, source operand for each real operand)
PSEUDOS = {
    'mov': ('or', (0, 1, 1)),
    'gt': ('lt', (0, 2, 1)),
    'ge': ('le', (0, 2, 1)),
    'gts': ('lts', (0, 2, 1)),
    'ges': ('les', (0, 2, 1)),
    'fgt': ('flt', (0, 2, 1)),
    'fge': ('fle', (0, 2, 1)),
    'bgt': ('blt', (1, 0, 2)),
    'bge': ('ble', (1, 0, 2)),
    'bgts': ('blts', (1, 0, 2)),
    'bges': ('bles', (1, 0, 2)),
}

# After every instruction each expression must be 0, or the machine stops.
INVARIANTS = [
    ('SP & 3', 'alignment error: SP misaligned'),
    ('BP & 3', 'alignment error: BP misaligned'),
]

# Float primitives used by SEMANTICS: name -> arity. Bit-exact definition in
# cpu4/fpu_model.h (C, the specification) and its Python port cpu4/fpu_model.py.
PRIMITIVES = {'fadd': 2, 'fsub': 2, 'fmul': 2, 'fdiv': 2, 'flt': 2, 'fle': 2,
              'itof': 1, 'ftoi': 1, 'frecip': 1, 'frsqrt': 1}
PRIMITIVE_PREFIX = 'cpu4_'
PRIMITIVE_C = 'cpu4/fpu_model.h'
PRIMITIVE_PY = 'cpu4/fpu_model.py'


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
After every instruction each INVARIANTS expression must be 0, or the machine
stops: SP and BP must be multiples of 4.
"""

SEMANTICS = {
    # F0a
    'halt': 'H = 1',
    'zero0': 'R[0] = 0',
    'zero1': 'R[1] = 0',
    'zero2': 'R[2] = 0',
    'zero3': 'R[3] = 0',
    'zero4': 'R[4] = 0',
    'zero5': 'R[5] = 0',
    'zero6': 'R[6] = 0',
    'zero7': 'R[7] = 0',
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
