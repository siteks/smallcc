# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

```bash
make smallcc        # Build the compiler
make test           # Run all pytest test cases (quiet)
make test_v         # Run all pytest test cases (verbose)
make test_p         # Run pytest cases in parallel (requires pytest-xdist)
make clean          # Remove binaries and temp files
```

Run the compiler directly:
```bash
echo 'int main(){return 5+3;}' > t.c
./smallcc t.c > out.s && ./sim_c out.s          # output to stdout
./smallcc -o out.s t.c && ./sim_c out.s          # output to file
./smallcc -o out.s file1.c file2.c && ./sim_c out.s  # multi-TU
./smallcc -ann -O1 -o out.s t.c                  # annotated output with source comments
```

Tests use `sim_c` (the C simulator) to execute generated assembly and check the value left in register `r0`. On test failure, verbose output is written to `error.log`. The test harness is in `test.sh`; individual suites are in `tests/`.

### Simulators

`sim_c` (`sim_c.c`) is the primary simulator — a self-contained C program that assembles and executes CPU3 assembly. Build with `make sim_c`. It is faster than the Python simulator and is what the test harness and `make test_all` use.

**`sim_c` usage:**
```
./sim_c [-v] file.s
```

**`sim_c` debug facilities:**

| Feature | Description |
|---|---|
| Register dump | Always printed on halt: `r0:xxxxxxxx sp:xxxx bp:xxxx lr:xxxx pc:xxxx H:x cycles:N` |
| `-v` (verbose) | Prints every instruction as it executes: `[pc] op=xx r0=xxxxxxxx sp=xxxx bp=xxxx` |
| Write watchpoints | Writes to addresses below `0x5000` print to stderr: `WRITE8/16/32 to addr = val  at pc=... sp=... bp=... r0=...`; useful for catching stray stores into the code/data area |
| Crash trace | On unknown opcode, dumps the last 32 executed instructions (pc, opcode, r0, sp, bp) to help locate the crash |
| MMIO cycle counter | A 32-bit read-only cycle counter at address `0xFF00` incremented once per instruction; used by `core_portme.c` for timing |

`cpu3/sim.py` (Python) is kept as a reference implementation and to support any legacy uses. It imports `cpu3/cpu.py` (instruction definitions, execution) and `cpu3/assembler.py` (two-pass assembler). The assembler picks up new instructions automatically from `cpu.py`'s `ptable`, so `cpu3/assembler.py` rarely needs changes.

### CoreMark Benchmark

CoreMark lives in `../coremark` (relative to this repo). To rebuild `coremark4.s` (CPU4):

```bash
cd ../coremark
../smallcc/smallcc -Icpu3 -I. -arch cpu4 -ann \
  -DFLAGS_STR=\""-O2 -DPERFORMANCE_RUN=1  "\" \
  -DITERATIONS=8 -DPERFORMANCE_RUN=1 -O2 \
  -o ./coremark4.s \
  core_list_join.c core_main.c core_matrix.c core_state.c core_util.c cpu3/core_portme.c
python3 ../smallcc/cpu4/sim.py coremark4.s
```

For CPU3: same command without `-arch cpu4`, output to `coremark.s`, run with `../smallcc/sim_c coremark.s`.

---

## Compiler Architecture

C89 subset compiler. Input is one or more `.c` files; assembly is written to stdout or a file specified with `-o`. Usage: `smallcc [-o outfile] [-stats] [-ann] [-O[N]] <source.c> [source2.c ...]`.

### Compilation Pipeline

```
Startup [smallcc.c]:
  get_compiler_dir(argv[0])                 Locate the directory of the compiler binary
  set_include_dir("$bindir/include")        Set system header search path for #include <>
  scan_lib_files("$bindir/lib")             Collect lib/*.c — prepended before user files

Preamble (ssp / jl main / halt) is emitted once before the per-TU loop.

Per-TU loop [smallcc.c]  (lib TUs first, then user TUs):
  read_file()               [smallcc.c]     Read source bytes from disk
  preprocess()              [preprocess.c]  Macro expansion, #include, #ifdef/#endif
  reset_codegen()           [codegen.c]     Clear per-TU codegen state
  reset_parser()            [parser.c]      Clear per-TU parser state
  reset_types_state()       [types.c]       Fresh symbol/scope tables (type list preserved;
                                            SYM_EXTERN symbols carried forward from prev TU)
  reset_preprocessor()      [preprocess.c]  Clear macro table and include-depth counter
  make_basic_types()        [types.c]       Populate/reuse global type table
  tokenise()                [tokeniser.c]   Token linked list
  program()                 [parser.c]      AST (Node tree)
  resolve_symbols(root)     [parser.c]      Set ND_IDENT types via symbol table lookup
  derive_types(root)        [parser.c]      Propagate types bottom-up through the AST
  insert_coercions(root)    [parser.c]      Insert ND_CAST / stride-scale nodes
  label_su(root)            [parser.c]      Sethi-Ullman labelling; reorder commutative children
  finalize_local_offsets()  [types.c]       Compute bp-relative offsets for all locals
  gen_ir(node, tu_index)    [codegen.c]     Walk AST; build flat IR instruction list
  mark_basic_blocks()       [codegen.c]     Insert IR_BB_START sentinels between basic blocks
  peephole(opt_level)       [optimise.c]    Constant folding, dead branches, store/reload elim
  backend_emit_asm(ir_head) [backend.c]     Walk IR list; emit assembly text
  harvest_globals()         [smallcc.c]     Mark non-static globals SYM_EXTERN for next TU
```

Every phase prints debug information to stderr. `-stats` prints per-TU and total arena usage to stderr after compilation.

### Source Files

| File | Role |
|---|---|
| `smallcc.h` | All shared structs, enums, and function prototypes |
| `smallcc.c` | Entry point; flag parsing (`-o`, `-stats`); lib scanning; include-dir setup; per-TU loop; `harvest_globals` |
| `preprocess.c` | Preprocessor — `#define`/`#undef`, `#ifdef`/`#ifndef`/`#else`/`#endif`, `#include "f"`/`#include <f>`; `set_include_dir()` |
| `tokeniser.c` | Lexer — produces a `Token` linked list |
| `parser.c` | Recursive-descent parser — builds AST; `resolve_symbols`, `derive_types`, `insert_coercions` |
| `types.c` | Type table, symbol table, struct layout, `add_types_and_symbols`, `reset_types_state`, `insert_extern_sym` |
| `codegen.c` | AST walk; builds flat IR instruction list (`gen_ir`); `reset_codegen`; static label mangling; `mark_basic_blocks` |
| `optimise.c` | Peephole optimiser (`peephole`); rules 1–9; constant folding, dead branch elim, store/reload elim |
| `backend.c` | IR → assembly emission (`backend_emit_asm`); `set_asm_out`; `-ann` annotation mode; retargeting point |
| `braun.c` | Braun SSA construction — stack IR → `IR3Inst` with fresh vregs; phi placement; SSA promotion (leaf functions) |
| `ir3.c` / `ir3.h` | IR3 infrastructure — `IR3Inst`/`IR3Op` definitions; `build_cfg`; `ir3_new_vreg` |
| `linscan.c` | Linear-scan register allocation (Poletto/Sarkar) — vregs → physical r0–r6; spilling via r7 |
| `ir3_lower.c` | IR3 → SSA lowering — near-1:1 `IR3Inst` → `SSAInst` for `risc_backend_emit` |
| `risc_backend.c` | CPU4 emitter — `SSAInst` → CPU4 assembly; F2 bp-relative selection; `le`/`ge` expansion |
| `sim_c.c` | Primary simulator — self-contained C assembler + CPU3/CPU4 executor; `make sim_c` |
| `cpu3/cpu.py` | Python CPU3 definition: `ptable` (opcode/format map) + execution handler |
| `cpu3/assembler.py` | Python two-pass assembler; reads `ptable` from `cpu.py` — no changes needed for new instructions |
| `cpu3/sim.py` | Python simulator (reference / legacy); imports `cpu.py` and `assembler.py` |
| `lib/stdio.c` | `printf` (`%d %s %c %%`), `puts` — compiled as TU 0 automatically |
| `lib/stdlib.c` | `abs` — compiled as TU 1 automatically |
| `lib/string.c` | `strlen`, `strcmp`, `strcpy`, `strcat` — compiled as TU 2 automatically |
| `lib/math.c` | `modf` — compiled as TU 3 automatically |
| `lib/crt0.s` | Optional C runtime startup (not auto-compiled; available for manual use) |
| `include/stdio.h` | Declarations for `putchar`, `puts`, `printf` |
| `include/stdlib.h` | Declaration for `abs`; `#define NULL 0` |
| `include/string.h` | Declarations for `strlen`, `strcmp`, `strcpy`, `strcat` |
| `include/math.h` | Declaration for `modf` |
| `include/stdarg.h` | `va_list`/`va_start`/`va_arg`/`va_end` — built-in compiler support; header documents the interface |
| `include/stdbool.h` | `bool`, `true`, `false` as macros |
| `include/stddef.h` | `size_t`, `ptrdiff_t`, `NULL`, `offsetof` |
| `include/stdint.h` | Fixed-width integer types (`int8_t`, `uint16_t`, etc.) |

### Key Target Facts

- 16-bit address space: `int` and pointers are **2 bytes**, `long`/`float`/`double` are **4 bytes**
- `new_node()` initializes `node->type = t_void` (not NULL) — type-propagation guards use `== t_void`
- Type singletons (`t_int`, `t_void`, etc.) are interned — use pointer equality for comparison
- Stack starts at `sp = 0x1000`; grows downward; `enter N` saves lr+bp and allocates N bytes
- `adj imm8` (opcode 0x41) adjusts `sp` by a signed 8-bit value (−128..127); `adjw imm16` (opcode 0x89) adjusts by a signed 16-bit value — used by the backend for locals larger than 127 bytes

---

## Detailed Reference

- @docs/architecture.md — tokeniser, parser (grammar, AST nodes, node struct), type system, per-TU compilation, backend file overview
- @docs/codegen.md — stack IR (`IRInst`/`IROp`), CPU3 backend (frame layout, expression idioms, cast gen, peephole), CPU4 backend (Braun SSA, IR3, linear-scan regalloc, RISC emit)
- @docs/cpu3-isa.md — CPU3 registers, full instruction set, assembly syntax
- @docs/cpu4-isa.md — CPU4 registers, full instruction set, assembly syntax (verified against `cpu4/cpu.py`)
- @docs/c89-status.md — compliance tables, deliberate deviations, what's implemented/missing
