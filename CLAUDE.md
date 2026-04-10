# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Debugging Rule

**Always debug test failures at the earliest point in the pipeline.** For CPU4 pipeline failures, check and fix the issue in the IR (OOS IR via `-oos` dump) before looking at final CPU4 assembly output. The pipeline is: Braun SSA → OOS → legalize → IRC → emission. Fix the problem at the first stage where it appears.

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
./smallcc -arch cpu4 -o out.s t.c && ./sim_c -arch cpu4 out.s  # compile and run
./smallcc -o out.s t.c && ./sim_c -arch cpu4 out.s             # -arch cpu4 is the default
./smallcc -o out.s file1.c file2.c && ./sim_c -arch cpu4 out.s # multi-TU
./smallcc -ann -o out.s t.c                                    # annotate assembly with source comments
./smallcc -arch cpu4 -ssa t.ssa -o out.s t.c                   # dump Braun SSA IR to t.ssa
./smallcc -arch cpu4 -oos t.oos -o out.s t.c                   # dump post-OOS IR to t.oos
./smallcc -arch cpu4 -irc t.irc -o out.s t.c                   # dump post-IRC IR to t.irc
DUMP_IR=1 ./smallcc -arch cpu4 -o out.s t.c                    # dump post-OOS and post-IRC IR to stderr
```

Tests use `sim_c` (the C simulator) to execute generated assembly and check the value left in register `r0`. On test failure, verbose output is written to `error.log`. The test harness is in `test.sh`; individual suites are in `tests/`.

### Simulators

`sim_c` (`sim_c.c`) is the primary simulator — a self-contained C program that assembles and executes CPU4 assembly. Build with `make sim_c`.

**`sim_c` usage:**
```
./sim_c [-v] [-arch cpu4] [-maxsteps N] file.s
```

**`sim_c` debug facilities:**

| Feature | Description |
|---|---|
| Register dump | Always printed on halt: `r0:xxxxxxxx sp:xxxx bp:xxxx lr:xxxx pc:xxxx H:x cycles:N` |
| `-v` (verbose) | Prints every instruction as it executes: `[pc] op=xx r0=xxxxxxxx sp=xxxx bp=xxxx` |
| Write watchpoints | Writes to addresses below `0x5000` print to stderr: `WRITE8/16/32 to addr = val  at pc=... sp=... bp=... r0=...`; useful for catching stray stores into the code/data area |
| Crash trace | On unknown opcode, dumps the last 32 executed instructions (pc, opcode, r0, sp, bp) to help locate the crash |
| MMIO cycle counter | A 32-bit read-only cycle counter at address `0xFF00` incremented once per instruction; used by `core_portme.c` for timing |

### CoreMark Benchmark

CoreMark lives in `../coremark` (relative to this repo). To rebuild `coremark4.s`:

```bash
cd ../coremark
../smallcc/smallcc -Icpu3 -I. -arch cpu4 \
  -DFLAGS_STR=\""-DPERFORMANCE_RUN=1  "\" \
  -DITERATIONS=8 -DPERFORMANCE_RUN=1 \
  -o ./coremark4.s \
  core_list_join.c core_main.c core_matrix.c core_state.c core_util.c cpu3/core_portme.c
../smallcc/sim_c -arch cpu4 -maxsteps 20000000 coremark4.s
```

---

## Compiler Architecture

C89 subset compiler. Input is one or more `.c` files; assembly is written to stdout or a file specified with `-o`. Usage: `smallcc [-o outfile] [-stats] [-ssa] [-arch cpu4] <source.c> [source2.c ...]`.

### Compilation Pipeline

```
Startup [smallcc.c]:
  get_compiler_dir(argv[0])                 Locate compiler binary directory
  set_include_dir("$bindir/include")        Set header search path
  collect_needed_libs()                     Collect required lib/*.c files

Preamble (ssp / jl main / halt) emitted once before per-TU loop.

Per-TU loop [smallcc.c] (lib TUs first, then user TUs):
  read_file()               [smallcc.c]     Read source from disk
  preprocess()              [preprocess.c]  Macro expansion, #include, conditionals
  reset_parse/types/preprocessor()          Reset per-TU state
  make_basic_types()        [types.c]       Populate type table
  tokenise()                [tokeniser.c]   Token linked list
  program()                 [parser.c]      AST (Node tree)
  resolve_symbols()         [parser.c]      Symbol table lookup
  derive_types()            [parser.c]      Bottom-up type propagation
  insert_coercions()        [parser.c]      Insert casts and stride-scaling
  finalize_local_offsets()  [types.c]       Compute bp-relative offsets

  ── CPU4 nanopass pipeline ─────────────────────────────────────────────────────
  lower_globals()           [lower.c]       Node* → Sexp AST (data section only)
  emit_globals()            [emit.c]        Data section (globals, string literals)
  per function:
    braun_function()        [braun.c]       Node* → SSA IR directly (Braun 2013)
    compute_dominators()    [dom.c]         Dominator tree + loop depth
    out_of_ssa()            [oos.c]         φ elimination (Boissinot 2009)
    legalize_function()     [legalize.c]    ABI pre-coloring; NEG/NOT/ZEXT/TRUNC lowering
    irc_allocate()          [alloc.c]       IRC register allocation (Appel & George)
    emit_function()         [emit.c]        Physical-reg IR → CPU4 assembly

  harvest_globals()         [smallcc.c]     Carry globals to next TU
```

### Source Files

| File | Role |
|---|---|
| `smallcc.h` | All shared structs, enums, and function prototypes |
| `smallcc.c` | Entry point; flag parsing; lib scanning; include-dir setup; per-TU loop; `harvest_globals`; nanopass dispatch |
| `preprocess.c` | Preprocessor — `#define`/`#undef`, `#ifdef`/`#ifndef`/`#else`/`#endif`, `#include "f"`/`#include <f>`; `set_include_dir()` |
| `tokeniser.c` | Lexer — produces a `Token` linked list |
| `parser.c` | Recursive-descent parser — builds AST; `resolve_symbols`, `derive_types`, `insert_coercions` |
| `types.c` | Type table, symbol table, struct layout, `add_types_and_symbols`, `reset_types_state`, `insert_extern_sym` |
| `sx.h` / `sx.c` | Sexp AST: `Sx` cons-cell tree with `SX_PAIR/SX_SYM/SX_STR/SX_INT` kinds; constructors; printer (used for data-section globals only) |
| `lower.h` / `lower.c` | Global lowering: Node* → Sexp `gvar`/`strlit` nodes for the data section; function bodies compiled directly by braun.c |
| `ssa.h` / `ssa.c` | SSA IR types (`Value`, `Inst`, `Block`, `Function`); `InstKind` opcodes; constructors; IR printer (`print_function`) |
| `braun.h` / `braun.c` | Braun SSA construction directly from Node* AST; Symbol*-keyed variable maps; derives ValType/CallDesc from Node*.type; handles all statement/expression kinds; accumulates function-body string literals and static locals |
| `dom.h` / `dom.c` | Dominator tree (Cooper 2001); loop depth; `compute_dominators`; `dominates` query |
| `oos.h` / `oos.c` | Out-of-SSA: Boissinot 2009 parallel-copy insertion; swap cycle detection |
| `legalize.h` / `legalize.c` | ISA/ABI legalization: Pass A (param pre-color); Pass B (call-arg IK_COPY); Pass C (NEG/NOT lowering); Pass D (ZEXT/TRUNC lowering) |
| `alloc.h` / `alloc.c` | Liveness analysis + IRC register allocation (Appel & George 1996); K=8; phantom-node ABI; George coalescing; spill support |
| `emit.h` / `emit.c` | CPU4 emission: `emit_globals` (data section) + `emit_function` (text section); F2 bp-rel selection; callee-save prologue/epilogue |
| `sim_c.c` | CPU4 assembler + simulator (self-contained C program); `make sim_c` |
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

- @docs/architecture.md — tokeniser, parser (grammar, AST nodes), type system, per-TU compilation
- @docs/backend.md — stack IR, CPU3 backend, CPU3 peephole optimisations
- @docs/compiler-pipeline.md — CPU4 nanopass pipeline: lowering, Braun SSA, IRC, emission
- @docs/isa/cpu3.md — CPU3 registers, instruction set, assembly syntax
- @docs/isa/cpu4.md — CPU4 registers, instruction set, assembly syntax
- @docs/c89-status.md — compliance tables, deliberate deviations, what's implemented/missing
- @docs/testing.md — test systems and debugging tips
