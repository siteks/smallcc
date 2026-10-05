# isatool — one ISA definition, every tool generated from it

An instruction set is a directory holding an `isa.py` (`cpu4/isa.py`, and any
other `<arch>/isa.py` in this checkout). The file is pure data: encoding,
machine state and one line of register-transfer semantics per instruction
(proposal 0005). Nothing in `isatool/` names an instruction. The assemblers,
the disassembler, both simulators, the encoding docs and the random
instruction generator all work from the definition.

## The definition

| Name | What it is |
|---|---|
| `NAME` | `'cpu4'` — also the `-arch` name |
| `STATE` | `{'gpr': 8, 'gpr_bits': 32, 'special': {'PC': 16, 'SP': 16, 'BP': 16, 'LR': 16}}` |
| `FORMATS` | `{name: template}`. Template characters, most significant bit of the first byte first: `0`/`1` fixed, `o` opcode, `d x y z r` register fields, `i j k` immediate fields, `-` reserved; spaces ignored |
| `INSTRUCTIONS` | `[(name, format, opcode, operands[, fields])]`. `opcode` is the value of the format's `o` bits, most significant first. `operands` are in assembly order: `'rd'`/`'rx'`/`'ry'` or `(name, kind[, scale])`. An operand's width is the width of the field it binds to. Registers bind to the register letters in order and immediates to the immediate letters in order, unless `fields` (`{operand: letter}`) says otherwise |
| `PSEUDOS` | `{name: (real, [source operand for each real operand])}` — operand permutations such as `gt` → `lt` swapped, `mov rd, rx` → `or rd, rx, rx`. A pseudo-op may not share a name with an instruction |
| `INVARIANTS` | `[(expression, message)]` — checked after every instruction; nonzero stops the machine (CPU4: SP and BP word-aligned) |
| `PRIMITIVES`, `PRIMITIVE_PREFIX`, `PRIMITIVE_C`, `PRIMITIVE_PY` | Functions the semantics may call that are too big for one line (CPU4's float ops), implemented as `<prefix><name>` in a C header and a Python file, both paths from the toolchain root. The generated `exec_gen.h` includes the header, so it needs an include guard, and quoted includes inside it resolve from its own directory |
| `SEMANTICS_PREAMBLE`, `SEMANTICS` | The notation, and `{name: line}` |

Immediate kinds decide how an assembly operand becomes a field value and what
range it accepts:

| Kind | Source value | Field | Labels |
|---|---|---|---|
| `simm` | signed | the value | no |
| `uimm` | unsigned | the value | no |
| `index` | signed element index (the semantics scale it) | the value | as `label/N` |
| `bytes` | signed byte offset, a multiple of `scale` | value ÷ `scale` | no |
| `raw` | a bit pattern: the signed or the unsigned range | the value | yes |
| `abs` | an absolute address | the value | yes |
| `pcrel` | a target | target − address of the next instruction | yes |

Where labels are allowed, `label+N` and `label-N` (decimal or hex `N`) are
too, in both assemblers; the compiler uses it for absolute accesses to a
field of a global. An `index` operand takes `label/N`, `label/N+K` or
`label/N-K`: the label's address divided by the element size `N` (it must
divide exactly), plus `K` elements. The compiler uses it to put a jump
table's address in a scaled load's displacement.

An instruction without a `SEMANTICS` line still assembles, disassembles and
decodes; executing it stops the machine with "has no semantics yet", and the
generated encoding doc lists it. That is how a new ISA can be brought up an
instruction at a time.

## The tools

| File | Role |
|---|---|
| `model.py` | Loads a definition and derives bit patterns, match/mask, field positions and runs, operand ranges and decoding. Checks that no two instructions match the same bits |
| `sem.py` | Parses the semantics lines; C and Python back ends (`<arch>/exec_gen.h`, `<arch>/exec_gen.py`); `analyse()` says what an instruction reads and writes, which rig uses |
| `gen.py` | `make isa` / `make isa-check`: writes `<arch>/isa_table_c.h`, `<arch>/exec_gen.h`, `<arch>/exec_gen.py`, `docs/isa/<arch>-encoding.md` and `isatool/arches.h` |
| `isa_types.h`, `exec_common.h` | The C table types and the helpers the generated C executors share |
| `arches.h` | Generated, not checked in: the ISAs `sim_c` is built with, one per `<arch>/isa.py` present |
| `asm.py` | The Python assembler (`Assembler(arch)`); `cpu4/assembler.py` is it bound to CPU4 |
| `pysim.py` | The Python simulator (`CPU(mem, arch=...)`, `--arch`, `--retire`); `cpu4/cpu.py` and `cpu4/sim.py` are it bound to CPU4 |
| `../tools/rig.py` | Random instruction streams on `sim_c -arch A` and `pysim --arch A` in lockstep. Instruction groups come from `sem.analyse` and the operand shapes, not from names |
| `../tools/wb2isa.py` | Turns an encoding-workbench export into a starting `<arch>/isa.py` |

Both assemblers encode the same way: start from the instruction's fixed bits,
check each operand's count, register number and range, convert it by its
kind, and scatter it into its field. `sim_c` does it from the generated C
table, `asm.py` from `model.py`.

## Adding an ISA

1. Write `<arch>/isa.py`, or convert a workbench design:
   `python3 tools/wb2isa.py design.json cpu5`. The converter takes formats
   and format assignment from the design, numbers opcodes in order within
   each format, and takes operands, semantics, pseudo-ops, invariants and
   primitives from the base ISA (`--base`, default `cpu4`) for the same
   mnemonic. Immediates bind to fields by width; an operand named for its
   width (`imm7`) is renamed to the new one. Afterwards the file is the
   definition and is edited by hand.
2. `make isa ARCH=<arch>` (or `make isa`), then `make sim_c`. Both simulators
   now take `-arch <arch>` / `--arch <arch>`.
3. `python3 tools/rig.py --arch <arch> --show` prints how rig groups the
   instructions; `python3 tools/rig.py --arch <arch> -n 100` runs them.
   `make rig-quick` runs every ISA present.

The compiler targets CPU4 only; retargeting it is separate work
(`docs/review-2026-09.md`). The simulators' device models (MMIO, `-cores`,
`-soc`) are CPU4's and are shared by every ISA.

Known limits: the special registers must be a subset of PC, SP, BP, LR (the
simulators' state has a fixed place for each), memory is 64 KiB of BRAM plus
`sim_c`'s SDRAM model, and an instruction's length must be decidable from its
first byte.
