"""CPU5 instruction set definition (pure data, read by isatool/model.py).

Converted by tools/wb2isa.py from the encoding workbench design
cpu5-draft-proposal-0004.json ("CPU5 draft (proposal 0004)"), with operands,
semantics, pseudo-ops, invariants and primitives taken from cpu4/isa.py for the
same mnemonics. From here on this file is the definition: edit it, then run
`make isa` (isatool/gen.py) to regenerate the tables, both executors and
docs/isa/cpu5-encoding.md.

Instructions without semantics (assemble and decode; executing one stops the
machine with a message): mov, llb_0, llw_0, lll_0, slb_0, slw_0, sll_0, llbx_0, llwx_0.

"""

NAME = 'cpu5'

STATE = {
    'gpr': 16,                   # r0..r15
    'gpr_bits': 32,
    'special': {'PC': 16, 'SP': 16, 'BP': 16, 'LR': 16},
}

FORMATS = {
    'S0': '11110ooo',
    'S1': '11111110 oooooooo',
    'S2': '11111111 oooodddd',
    'S3': '110ooddd diiiiiii',
    'S4': '1110oooo ddddxxxx',
    'S5': '1111110o oooodddd xxxxyyyy',
    'S6': '0ooodddd iiiiiiii iiiiiiii',
    'S7': '10oooooo ddddxxxx iiiiiiii iiiiiiii',
    'S8': '111110oo ddddiiii iiiijjjj jjjjjjjj',
}

# (name, format, opcode = value of the format's o bits, operands in assembly order[, fields])
INSTRUCTIONS = [
    # S0  11110ooo
    ('ret',      'S0', 0b000, ()),
    ('zero0',    'S0', 0b001, ()),
    ('zero1',    'S0', 0b010, ()),
    ('zero2',    'S0', 0b011, ()),
    ('zero3',    'S0', 0b100, ()),
    ('zero4',    'S0', 0b101, ()),
    ('zero5',    'S0', 0b110, ()),
    ('zero6',    'S0', 0b111, ()),
    # S1  11111110 oooooooo
    ('halt',     'S1', 0b00000000, ()),
    ('zero7',    'S1', 0b00000001, ()),
    ('zero8',    'S1', 0b00000010, ()),
    ('zero9',    'S1', 0b00000011, ()),
    ('zeroa',    'S1', 0b00000100, ()),
    ('zerob',    'S1', 0b00000101, ()),
    ('zeroc',    'S1', 0b00000110, ()),
    ('zerod',    'S1', 0b00000111, ()),
    ('zeroe',    'S1', 0b00001000, ()),
    ('zerof',    'S1', 0b00001001, ()),
    # S2  11111111 oooodddd
    ('sxb',      'S2', 0b0000, ('rd',)),
    ('sxw',      'S2', 0b0001, ('rd',)),
    ('inc',      'S2', 0b0010, ('rd',)),
    ('dec',      'S2', 0b0011, ('rd',)),
    ('pushr',    'S2', 0b0100, ('rd',)),
    ('popr',     'S2', 0b0101, ('rd',)),
    ('zxb',      'S2', 0b0110, ('rd',)),
    ('itof',     'S2', 0b0111, ('rd',)),
    ('ftoi',     'S2', 0b1000, ('rd',)),
    ('jlr',      'S2', 0b1001, ('rd',)),
    ('jr',       'S2', 0b1010, ('rd',)),
    ('ssp',      'S2', 0b1011, ('rd',)),
    ('neg',      'S2', 0b1100, ('rd',)),
    ('frecip',   'S2', 0b1101, ('rd',)),
    ('frsqrt',   'S2', 0b1110, ('rd',)),
    ('putchar',  'S2', 0b1111, ('rd',)),
    # S3  110ooddd diiiiiii
    ('ll',       'S3', 0b00, ('rx', ('imm', 'index'))),
    ('sl',       'S3', 0b01, ('rx', ('imm', 'index'))),
    ('shli',     'S3', 0b10, ('rx', ('imm', 'simm'))),
    ('imms',     'S3', 0b11, ('rx', ('imm', 'simm'))),
    # S4  1110oooo ddddxxxx
    ('mov',      'S4', 0b0000, ('rd' ,'rx')),
    ('zxw',      'S4', 0b0001, ('rd',)),
    ('llb_0',    'S4', 0b0010, ('rd', 'rx')),
    ('llw_0',    'S4', 0b0011, ('rd', 'rx')),
    ('lll_0',    'S4', 0b0100, ('rd', 'rx')),
    ('slb_0',    'S4', 0b0101, ('rx', 'ry')),
    ('slw_0',    'S4', 0b0110, ('rx', 'ry')),
    ('sll_0',    'S4', 0b0111, ('rx', 'ry')),
    ('llbx_0',   'S4', 0b1000, ('rd', 'rx')),
    ('llwx_0',   'S4', 0b1001, ('rd', 'rx')),
    # S5  1111110o oooodddd xxxxyyyy
    ('add',      'S5', 0b00000, ('rd', 'rx', 'ry')),
    ('sub',      'S5', 0b00001, ('rd', 'rx', 'ry')),
    ('mul',      'S5', 0b00010, ('rd', 'rx', 'ry')),
    ('div',      'S5', 0b00011, ('rd', 'rx', 'ry')),
    ('mod',      'S5', 0b00100, ('rd', 'rx', 'ry')),
    ('shl',      'S5', 0b00101, ('rd', 'rx', 'ry')),
    ('shr',      'S5', 0b00110, ('rd', 'rx', 'ry')),
    ('lt',       'S5', 0b00111, ('rd', 'rx', 'ry')),
    ('le',       'S5', 0b01000, ('rd', 'rx', 'ry')),
    ('eq',       'S5', 0b01001, ('rd', 'rx', 'ry')),
    ('ne',       'S5', 0b01010, ('rd', 'rx', 'ry')),
    ('and',      'S5', 0b01011, ('rd', 'rx', 'ry')),
    ('or',       'S5', 0b01100, ('rd', 'rx', 'ry')),
    ('xor',      'S5', 0b01101, ('rd', 'rx', 'ry')),
    ('lts',      'S5', 0b01110, ('rd', 'rx', 'ry')),
    ('les',      'S5', 0b01111, ('rd', 'rx', 'ry')),
    ('divs',     'S5', 0b10000, ('rd', 'rx', 'ry')),
    ('mods',     'S5', 0b10001, ('rd', 'rx', 'ry')),
    ('shrs',     'S5', 0b10010, ('rd', 'rx', 'ry')),
    ('fadd',     'S5', 0b10011, ('rd', 'rx', 'ry')),
    ('fsub',     'S5', 0b10100, ('rd', 'rx', 'ry')),
    ('fmul',     'S5', 0b10101, ('rd', 'rx', 'ry')),
    ('fdiv',     'S5', 0b10110, ('rd', 'rx', 'ry')),
    ('flt',      'S5', 0b10111, ('rd', 'rx', 'ry')),
    ('fle',      'S5', 0b11000, ('rd', 'rx', 'ry')),
    ('zxwor',    'S5', 0b11001, ('rd', 'rx', 'ry')),
    ('sxwor',    'S5', 0b11010, ('rd', 'rx', 'ry')),
    # S6  0ooodddd iiiiiiii iiiiiiii
    ('j',        'S6', 0b000, (('imm', 'abs'),)),
    ('jl',       'S6', 0b001, (('imm', 'abs'),)),
    ('enter',    'S6', 0b010, (('imm', 'uimm'),)),
    ('lea',      'S6', 0b011, ('rd', ('imm', 'bytes', 4))),
    ('immw',     'S6', 0b100, ('rx', ('imm', 'raw'))),
    ('immwh',    'S6', 0b101, ('rx', ('imm', 'raw'))),
    ('jz',       'S6', 0b110, ('rx', ('imm', 'abs'))),
    ('jnz',      'S6', 0b111, ('rx', ('imm', 'abs'))),
    # S7  10oooooo ddddxxxx iiiiiiii iiiiiiii
    ('addli',    'S7', 0b000000, ('rd', 'rx', ('imm', 'simm'))),
    ('subli',    'S7', 0b000001, ('rd', 'rx', ('imm', 'simm'))),
    ('mulli',    'S7', 0b000010, ('rd', 'rx', ('imm', 'simm'))),
    ('divli',    'S7', 0b000011, ('rd', 'rx', ('imm', 'simm'))),
    ('modli',    'S7', 0b000100, ('rd', 'rx', ('imm', 'simm'))),
    ('shlli',    'S7', 0b000101, ('rd', 'rx', ('imm', 'simm'))),
    ('shrli',    'S7', 0b000110, ('rd', 'rx', ('imm', 'simm'))),
    ('leli',     'S7', 0b000111, ('rd', 'rx', ('imm', 'simm'))),
    ('gtli',     'S7', 0b001000, ('rd', 'rx', ('imm', 'simm'))),
    ('eqli',     'S7', 0b001001, ('rd', 'rx', ('imm', 'simm'))),
    ('neli',     'S7', 0b001010, ('rd', 'rx', ('imm', 'simm'))),
    ('andli',    'S7', 0b001011, ('rd', 'rx', ('imm', 'simm'))),
    ('orli',     'S7', 0b001100, ('rd', 'rx', ('imm', 'simm'))),
    ('xorli',    'S7', 0b001101, ('rd', 'rx', ('imm', 'simm'))),
    ('lesli',    'S7', 0b001110, ('rd', 'rx', ('imm', 'simm'))),
    ('gtsli',    'S7', 0b001111, ('rd', 'rx', ('imm', 'simm'))),
    ('divsli',   'S7', 0b010000, ('rd', 'rx', ('imm', 'simm'))),
    ('modsli',   'S7', 0b010001, ('rd', 'rx', ('imm', 'simm'))),
    ('shrsli',   'S7', 0b010010, ('rd', 'rx', ('imm', 'simm'))),
    ('bitex',    'S7', 0b010011, ('rd', 'rx', ('imm', 'uimm'))),
    ('rsubli',   'S7', 0b010100, ('rd', 'rx', ('imm', 'simm'))),
    ('rdivli',   'S7', 0b010101, ('rd', 'rx', ('imm', 'simm'))),
    ('rmodli',   'S7', 0b010110, ('rd', 'rx', ('imm', 'simm'))),
    ('rdivsli',  'S7', 0b010111, ('rd', 'rx', ('imm', 'simm'))),
    ('lb',       'S7', 0b011000, ('rx', ('imm', 'index'))),
    ('lw',       'S7', 0b011001, ('rx', ('imm', 'index'))),
    ('sb',       'S7', 0b011010, ('rx', ('imm', 'index'))),
    ('sw',       'S7', 0b011011, ('rx', ('imm', 'index'))),
    ('lbx',      'S7', 0b011100, ('rx', ('imm', 'index'))),
    ('lwx',      'S7', 0b011101, ('rx', ('imm', 'index'))),
    ('addi',     'S7', 0b011110, ('rx', ('imm', 'simm'))),
    ('andi',     'S7', 0b011111, ('rx', ('imm', 'uimm'))),
    ('shrsi',    'S7', 0b100000, ('rx', ('imm', 'simm'))),
    ('adjw',     'S7', 0b100001, (('imm', 'bytes', 4),)),
    ('llb',      'S7', 0b100010, ('rx', 'ry', ('imm', 'index'))),
    ('llw',      'S7', 0b100011, ('rx', 'ry', ('imm', 'index'))),
    ('lll',      'S7', 0b100100, ('rx', 'ry', ('imm', 'index'))),
    ('slb',      'S7', 0b100101, ('rx', 'ry', ('imm', 'index'))),
    ('slw',      'S7', 0b100110, ('rx', 'ry', ('imm', 'index'))),
    ('sll',      'S7', 0b100111, ('rx', 'ry', ('imm', 'index'))),
    ('llbx',     'S7', 0b101000, ('rx', 'ry', ('imm', 'index'))),
    ('llwx',     'S7', 0b101001, ('rx', 'ry', ('imm', 'index'))),
    ('beq',      'S7', 0b101010, ('rx', 'ry', ('disp', 'pcrel'))),
    ('bne',      'S7', 0b101011, ('rx', 'ry', ('disp', 'pcrel'))),
    ('blt',      'S7', 0b101100, ('rx', 'ry', ('disp', 'pcrel'))),
    ('ble',      'S7', 0b101101, ('rx', 'ry', ('disp', 'pcrel'))),
    ('blts',     'S7', 0b101110, ('rx', 'ry', ('disp', 'pcrel'))),
    ('bles',     'S7', 0b101111, ('rx', 'ry', ('disp', 'pcrel'))),
    ('beqz',     'S7', 0b110000, ('rx', ('disp', 'pcrel'))),
    ('bnez',     'S7', 0b110001, ('rx', ('disp', 'pcrel'))),
    ('dbnz',     'S7', 0b110010, ('rx', ('disp', 'pcrel'))),
    ('bltz',     'S7', 0b110011, ('rx', ('disp', 'pcrel'))),
    ('bgez',     'S7', 0b110100, ('rx', ('disp', 'pcrel'))),
    ('bgtz',     'S7', 0b110101, ('rx', ('disp', 'pcrel'))),
    ('blez',     'S7', 0b110110, ('rx', ('disp', 'pcrel'))),
    # S8  111110oo ddddiiii iiiijjjj jjjjjjjj
    ('cbeq',     'S8', 0b00, ('rx', ('imm8', 'uimm'), ('disp', 'pcrel'))),
    ('cbne',     'S8', 0b01, ('rx', ('imm8', 'uimm'), ('disp', 'pcrel'))),
]

PSEUDOS = {
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

INVARIANTS = [('SP & 3', 'alignment error: SP misaligned'), ('BP & 3', 'alignment error: BP misaligned')]

PRIMITIVES = {'fadd': 2, 'fsub': 2, 'fmul': 2, 'fdiv': 2, 'flt': 2, 'fle': 2, 'itof': 1, 'ftoi': 1, 'frecip': 1, 'frsqrt': 1}
PRIMITIVE_PREFIX = 'cpu5_'
PRIMITIVE_C = 'cpu5/fpu_model.h'
PRIMITIVE_PY = 'cpu5/fpu_model.py'

SEMANTICS_PREAMBLE = """
State: R[0..15] are 32-bit; PC, SP, BP, LR are 16-bit; H is the halt flag.
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
    'ret': 'SP = BP; BP = M32[SP]; PC = M32[SP] >> 16; SP = SP + 4',
    'zero0': 'R[0] = 0',
    'zero1': 'R[1] = 0',
    'zero2': 'R[2] = 0',
    'zero3': 'R[3] = 0',
    'zero4': 'R[4] = 0',
    'zero5': 'R[5] = 0',
    'zero6': 'R[6] = 0',
    'halt': 'H = 1',
    'zero7': 'R[7] = 0',
    'zero8': 'R[8] = 0',
    'zero9': 'R[9] = 0',
    'zeroa': 'R[10] = 0',
    'zerob': 'R[11] = 0',
    'zeroc': 'R[12] = 0',
    'zerod': 'R[13] = 0',
    'zeroe': 'R[14] = 0',
    'zerof': 'R[15] = 0',
    'sxb': 'R[rd] = sx(R[rd], 8)',
    'sxw': 'R[rd] = sx(R[rd], 16)',
    'inc': 'R[rd] = R[rd] + 1',
    'dec': 'R[rd] = R[rd] - 1',
    'pushr': 'SP = SP - 4; M32[SP] = R[rd]',
    'popr': 'R[rd] = M32[SP]; SP = SP + 4',
    'zxb': 'R[rd] = zx(R[rd], 8)',
    'itof': 'R[rd] = itof(R[rd])',
    'ftoi': 'R[rd] = ftoi(R[rd])',
    'jlr': 'LR = PC; PC = R[rd]',
    'jr': 'PC = R[rd]',
    'ssp': 'SP = R[rd]',
    'neg': 'R[rd] = -R[rd]',
    'frecip': 'R[rd] = frecip(R[rd])',
    'frsqrt': 'R[rd] = frsqrt(R[rd])',
    'putchar': 'putchar(R[rd])',
    'll': 'R[rx] = M32[lo16(BP + sext(imm) * 4)]',
    'sl': 'M32[lo16(BP + sext(imm) * 4)] = R[rx]',
    'shli': 'R[rx] = R[rx] << (imm & 31)',
    'imms': 'R[rx] = sext(imm)',
    'mov': 'R[rd] = R[rx]',
    'zxw': 'R[rd] = zx(R[rd], 16)',
    'llb_0': 'R[rd] = M8[R[rx]]',
    'llw_0': 'R[rd] = M16[R[rx]]',
    'lll_0': 'R[rd] = M32[R[rx]]',
    'slb_0': 'M8[R[ry]] = R[rx]',
    'slw_0': 'M16[R[ry]] = R[rx]',
    'sll_0': 'M32[R[ry]] = R[rx]',
    'llbx_0': 'R[rd] = sx(M8[R[rx]], 8)',
    'llwx_0': 'R[rd] = sx(M16[R[rx]], 16)',
    'add': 'R[rd] = R[rx] + R[ry]',
    'sub': 'R[rd] = R[rx] - R[ry]',
    'mul': 'R[rd] = R[rx] * R[ry]',
    'div': 'R[rd] = R[rx] /u R[ry]',
    'mod': 'R[rd] = R[rx] %u R[ry]',
    'shl': 'R[rd] = R[rx] << (R[ry] & 31)',
    'shr': 'R[rd] = R[rx] >> (R[ry] & 31)',
    'lt': 'R[rd] = R[rx] <u R[ry]',
    'le': 'R[rd] = R[rx] <=u R[ry]',
    'eq': 'R[rd] = R[rx] == R[ry]',
    'ne': 'R[rd] = R[rx] != R[ry]',
    'and': 'R[rd] = R[rx] & R[ry]',
    'or': 'R[rd] = R[rx] | R[ry]',
    'xor': 'R[rd] = R[rx] ^ R[ry]',
    'lts': 'R[rd] = R[rx] <s R[ry]',
    'les': 'R[rd] = R[rx] <=s R[ry]',
    'divs': 'R[rd] = R[rx] /s R[ry]',
    'mods': 'R[rd] = R[rx] %s R[ry]',
    'shrs': 'R[rd] = R[rx] >>s (R[ry] & 31)',
    'fadd': 'R[rd] = fadd(R[rx], R[ry])',
    'fsub': 'R[rd] = fsub(R[rx], R[ry])',
    'fmul': 'R[rd] = fmul(R[rx], R[ry])',
    'fdiv': 'R[rd] = fdiv(R[rx], R[ry])',
    'flt': 'R[rd] = flt(R[rx], R[ry])',
    'fle': 'R[rd] = fle(R[rx], R[ry])',
    'zxwor': 'R[rd] = zx(R[rx] | R[ry], 16)',
    'sxwor': 'R[rd] = sx(R[rx] | R[ry], 16)',
    'j': 'PC = imm',
    'jl': 'LR = PC; PC = imm',
    'enter': 'M32[lo16(SP - 4)] = (LR << 16) | BP; BP = SP - 4; SP = SP - imm - 4',
    'lea': 'R[rd] = lo16(BP + sext(imm) * 4)',
    'immw': 'R[rx] = imm',
    'immwh': 'R[rx] = zx(R[rx], 16) | (imm << 16)',
    'jz': 'if R[rx] == 0 then PC = imm',
    'jnz': 'if R[rx] != 0 then PC = imm',
    'addli': 'R[rd] = R[rx] + sext(imm)',
    'subli': 'R[rd] = R[rx] - sext(imm)',
    'mulli': 'R[rd] = R[rx] * sext(imm)',
    'divli': 'R[rd] = R[rx] /u sext(imm)',
    'modli': 'R[rd] = R[rx] %u sext(imm)',
    'shlli': 'R[rd] = R[rx] << (imm & 31)',
    'shrli': 'R[rd] = R[rx] >> (imm & 31)',
    'leli': 'R[rd] = R[rx] <=u sext(imm)',
    'gtli': 'R[rd] = R[rx] >u sext(imm)',
    'eqli': 'R[rd] = R[rx] == sext(imm)',
    'neli': 'R[rd] = R[rx] != sext(imm)',
    'andli': 'R[rd] = R[rx] & sext(imm)',
    'orli': 'R[rd] = R[rx] | sext(imm)',
    'xorli': 'R[rd] = R[rx] ^ sext(imm)',
    'lesli': 'R[rd] = R[rx] <=s sext(imm)',
    'gtsli': 'R[rd] = R[rx] >s sext(imm)',
    'divsli': 'R[rd] = R[rx] /s sext(imm)',
    'modsli': 'R[rd] = R[rx] %s sext(imm)',
    'shrsli': 'R[rd] = R[rx] >>s (imm & 31)',
    'bitex': 'R[rd] = (R[rx] >> (imm & 31)) & ((2 << ((imm >> 5) & 15)) - 1)',
    'rsubli': 'R[rd] = sext(imm) - R[rx]',
    'rdivli': 'R[rd] = sext(imm) /u R[rx]',
    'rmodli': 'R[rd] = sext(imm) %u R[rx]',
    'rdivsli': 'R[rd] = sext(imm) /s R[rx]',
    'lb': 'R[rx] = M8[lo16(BP + sext(imm))]',
    'lw': 'R[rx] = M16[lo16(BP + sext(imm) * 2)]',
    'sb': 'M8[lo16(BP + sext(imm))] = R[rx]',
    'sw': 'M16[lo16(BP + sext(imm) * 2)] = R[rx]',
    'lbx': 'R[rx] = sx(M8[lo16(BP + sext(imm))], 8)',
    'lwx': 'R[rx] = sx(M16[lo16(BP + sext(imm) * 2)], 16)',
    'addi': 'R[rx] = R[rx] + sext(imm)',
    'andi': 'R[rx] = R[rx] & imm',
    'shrsi': 'R[rx] = R[rx] >>s (imm & 31)',
    'adjw': 'SP = SP + sext(imm) * 4',
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
    'beqz': 'if R[rx] == 0 then PC = PC + sext(disp)',
    'bnez': 'if R[rx] != 0 then PC = PC + sext(disp)',
    'dbnz': 'R[rx] = R[rx] - 1; if R[rx] != 0 then PC = PC + sext(disp)',
    'bltz': 'if R[rx] <s 0 then PC = PC + sext(disp)',
    'bgez': 'if R[rx] >=s 0 then PC = PC + sext(disp)',
    'bgtz': 'if R[rx] >s 0 then PC = PC + sext(disp)',
    'blez': 'if R[rx] <=s 0 then PC = PC + sext(disp)',
    'cbeq': 'if R[rx] == imm8 then PC = PC + sext(disp)',
    'cbne': 'if R[rx] != imm8 then PC = PC + sext(disp)',
}
