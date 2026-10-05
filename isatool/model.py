"""Load an ISA definition (<arch>/isa.py, pure data) and derive everything the
tools need from it: bit patterns, field positions, operand bindings, lengths,
immediate ranges, operand syntax, encoding and decoding.

An ISA definition module provides:

  NAME        'cpu4'
  STATE       {'gpr': 8, 'gpr_bits': 32, 'special': {'PC': 16, 'SP': 16, 'BP': 16, 'LR': 16}}
  FORMATS     {format name: bit template}. 0/1 fixed, o opcode, d x y z r register
              fields, i j k immediate fields, - reserved; spaces ignored; the
              first character is the most significant bit of the first byte.
  INSTRUCTIONS  [(name, format, opcode, operands[, fields])]
              opcode   the value of the format's o bits, most significant first
              operands in assembly order: 'rd' | 'rx' | 'ry' for registers, or
                       (name, kind[, scale]) for an immediate; kinds:
                         simm  signed value        uimm  unsigned value
                         index signed element index (the semantics scale it); label/N[+-K] too
                         bytes signed byte offset, encoded divided by `scale`
                         raw   bit pattern: accepts the signed and unsigned range, and labels
                         abs   absolute address: a label or a number
                         pcrel PC-relative: a label; the field holds target - next pc
                       The width is the width of the bound template field.
              fields   optional {operand name: template letter}; by default
                       register operands bind to the register letters and
                       immediates to the immediate letters, in order.
  PSEUDOS     {name: (real name, source operand for each real operand)}
  INVARIANTS  [(expression, message)]: after every instruction the expression
              (semantics notation) must be 0, or the machine stops.
  PRIMITIVES  {name: arity}; implemented as `<PRIMITIVE_PREFIX><name>` in
              PRIMITIVE_C (a header) and PRIMITIVE_PY (a Python file).
  SEMANTICS_PREAMBLE, SEMANTICS {name: line}

Nothing here is specific to one ISA.
"""
import importlib.util, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REG_OPS = ('rd', 'rx', 'ry', 'rz')
REG_LETTERS = 'dxyzr'
IMM_LETTERS = 'ijk'
LABEL_KINDS = ('raw', 'abs', 'pcrel')


class ISAError(Exception):
    pass


class Format:
    def __init__(self, name, tpl):
        self.name = name
        self.tpl = tpl.replace(' ', '').replace('_', '')
        self.len = len(self.tpl)
        if self.len % 8:
            raise ISAError(f"format {name}: {self.len} bits is not a whole number of bytes")
        bad = set(self.tpl) - set('01o-' + REG_LETTERS + IMM_LETTERS)
        if bad:
            raise ISAError(f"format {name}: unknown template characters {sorted(bad)}")
        self.letters = []
        for c in self.tpl:
            if c not in '01o-' and c not in self.letters:
                self.letters.append(c)
        self.pos = {c: [p for p, ch in enumerate(self.tpl) if ch == c] for c in self.letters}
        self.opos = [p for p, c in enumerate(self.tpl) if c == 'o']
        self.reg_letters = [c for c in self.letters if c in REG_LETTERS]
        self.imm_letters = [c for c in self.letters if c in IMM_LETTERS]

    @property
    def bytes(self):
        return self.len // 8


class Operand:
    def __init__(self, spec, positions, letter):
        if isinstance(spec, str):
            if spec not in REG_OPS:
                raise ISAError(f"register operand {spec!r} must be one of {REG_OPS}")
            self.name, self.reg, self.kind, self.scale = spec, True, 'reg', 1
        else:
            self.name, self.kind = spec[0], spec[1]
            self.scale = spec[2] if len(spec) > 2 else 1
            self.reg = False
            if self.kind not in ('simm', 'uimm', 'index', 'bytes', 'raw', 'abs', 'pcrel'):
                raise ISAError(f"operand {self.name}: unknown immediate kind {self.kind!r}")
        self.letter = letter
        self.positions = positions
        self.bits = len(positions)

    def runs(self, total):
        """Contiguous runs of the field, most significant first, as (lsb shift, width)."""
        out, cur = [], [self.positions[0]]
        for p in self.positions[1:]:
            if p == cur[-1] + 1:
                cur.append(p)
            else:
                out.append(cur); cur = [p]
        out.append(cur)
        return [(total - 1 - r[-1], len(r)) for r in out]

    def field_range(self):
        """Allowed values of the encoded field value (before scaling)."""
        b = self.bits
        if self.kind in ('simm', 'index', 'bytes', 'pcrel'):
            return -(1 << (b - 1)), (1 << (b - 1)) - 1
        if self.kind == 'uimm':
            return 0, (1 << b) - 1
        return -(1 << (b - 1)), (1 << b) - 1          # raw, abs: the union of both readings

    def source_range(self):
        lo, hi = self.field_range()
        return lo * self.scale, hi * self.scale


class Instr:
    def __init__(self, isa, row):
        name, fmt, opcode, ops = row[:4]
        binding = row[4] if len(row) > 4 else {}
        if fmt not in isa.formats:
            raise ISAError(f"{name}: unknown format {fmt}")
        self.name, self.fmt, self.opcode = name, isa.formats[fmt], opcode
        f = self.fmt
        if opcode >= (1 << len(f.opos)) or opcode < 0:
            raise ISAError(f"{name}: opcode {opcode} does not fit {len(f.opos)} opcode bits of {fmt}")
        regs = [o for o in ops if isinstance(o, str)]
        imms = [o for o in ops if not isinstance(o, str)]
        rl, il = list(f.reg_letters), list(f.imm_letters)
        if len(regs) > len(rl) or len(imms) > len(il):
            raise ISAError(f"{name}: operands {ops} do not fit template {f.tpl}")
        auto = dict(zip(regs, rl)); auto.update(zip([o[0] for o in imms], il))
        auto.update(binding)
        self.ops = []
        for o in ops:
            nm = o if isinstance(o, str) else o[0]
            letter = auto[nm]
            if letter not in f.pos:
                raise ISAError(f"{name}: operand {nm} bound to letter {letter!r}, not in {f.tpl}")
            self.ops.append(Operand(o, f.pos[letter], letter))
        # full fixed pattern
        self.pattern = {}
        for p, c in enumerate(f.tpl):
            if c in '01':
                self.pattern[p] = int(c)
        for k, p in enumerate(f.opos):
            self.pattern[p] = (opcode >> (len(f.opos) - 1 - k)) & 1
        L = f.len
        self.len = f.bytes
        self.match = sum(b << (L - 1 - p) for p, b in self.pattern.items())
        self.mask = sum(1 << (L - 1 - p) for p in self.pattern)

    @property
    def op_names(self):
        return [o.name for o in self.ops]

    def is_pcrel(self):
        return any(o.kind == 'pcrel' for o in self.ops)

    def syntax(self):
        return ", ".join(o.name for o in self.ops)

    def imm_range(self):
        """(operand index, lo, hi) in source units for the first numeric immediate; None if none."""
        for k, o in enumerate(self.ops):
            if not o.reg and o.kind not in ('pcrel', 'abs'):
                lo, hi = o.source_range()
                return (k, lo, hi)
        return None


class ISA:
    def __init__(self, mod, path):
        self.mod, self.path = mod, path
        self.name = mod.NAME
        self.dir = os.path.dirname(path)
        st = mod.STATE
        self.ngpr, self.gpr_bits, self.special = st['gpr'], st.get('gpr_bits', 32), dict(st['special'])
        self.formats = {n: Format(n, t) for n, t in mod.FORMATS.items()}
        self.instrs = [Instr(self, row) for row in mod.INSTRUCTIONS]
        self.by_name = {i.name: i for i in self.instrs}
        if len(self.by_name) != len(self.instrs):
            raise ISAError("duplicate instruction names")
        self.pseudos = dict(getattr(mod, 'PSEUDOS', {}))
        self.invariants = list(getattr(mod, 'INVARIANTS', []))
        self.primitives = dict(getattr(mod, 'PRIMITIVES', {}))
        self.prim_prefix = getattr(mod, 'PRIMITIVE_PREFIX', '')
        self.prim_c = getattr(mod, 'PRIMITIVE_C', None)
        self.prim_py = getattr(mod, 'PRIMITIVE_PY', None)
        self.semantics = dict(getattr(mod, 'SEMANTICS', {}))
        self.preamble = getattr(mod, 'SEMANTICS_PREAMBLE', '')
        for n in self.semantics:
            if n not in self.by_name:
                raise ISAError(f"SEMANTICS names unknown instruction {n}")
        for n, (real, perm) in self.pseudos.items():
            if real not in self.by_name:
                raise ISAError(f"pseudo-op {n} expands to unknown {real}")
            if n in self.by_name:
                raise ISAError(f"pseudo-op {n} has the name of an instruction, which it would hide from the assemblers")
        self.check_overlaps()

    @property
    def missing_semantics(self):
        return [i.name for i in self.instrs if i.name not in self.semantics]

    def check_overlaps(self):
        """No two instructions may match the same bits (decode must be unambiguous)."""
        for a_i, a in enumerate(self.instrs):
            for b in self.instrs[a_i + 1:]:
                n = min(a.fmt.len, b.fmt.len)
                clash = False
                for p in range(n):
                    if p in a.pattern and p in b.pattern and a.pattern[p] != b.pattern[p]:
                        clash = True; break
                if not clash:
                    raise ISAError(f"{a.name} ({a.fmt.name}) and {b.name} ({b.fmt.name}) match the same bits")

    def decode_bytes(self, get8, addr):
        """Return (instr, word) for the instruction at addr, or (None, first byte)."""
        cache = {}
        for i in self.instrs:
            n = i.len
            if n not in cache:
                w = 0
                for k in range(n):
                    w = (w << 8) | get8((addr + k) & 0xffff)
                cache[n] = w
            if (cache[n] & i.mask) == i.match:
                return i, cache[n]
        return None, get8(addr & 0xffff)

    def field(self, op, w, total):
        v = 0
        for shift, width in op.runs(total):
            v = (v << width) | ((w >> shift) & ((1 << width) - 1))
        return v

    def legacy_op_subop(self, i):
        """CPU4's historical (first byte, sub-opcode) view, for checks against decode.v."""
        first = 0
        for p in range(8):
            if p in i.pattern:
                first |= i.pattern[p] << (7 - p)
        later = [p for p in i.fmt.opos if p >= 8]
        sub = 0
        for p in later:
            sub = (sub << 1) | i.pattern[p]
        return first, sub


def load(arch):
    path = os.path.join(ROOT, arch, 'isa.py')
    if not os.path.exists(path):
        raise ISAError(f"no ISA definition at {path}")
    spec = importlib.util.spec_from_file_location(f"isa_{arch}", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return ISA(mod, path)


def arches():
    """Every directory under the toolchain root that holds an isa.py."""
    return sorted(d for d in os.listdir(ROOT) if os.path.exists(os.path.join(ROOT, d, 'isa.py')))
