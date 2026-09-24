# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Cross-Repo Coordination

smallcc is a self-contained repo: the C compiler, the reference simulators (`sim_c`, `irsim`, `cpu4/cpu.py`), the CPU4 ISA and ABI specs, the test corpus, the fuzzer and the CoreMark bench all live here and work with no hardware. It is also consumed as a git submodule (`toolchain/smallcc`) by the CPU4 sea-of-processors project, whose RTL, boards, runtime libraries and applications live in the parent monorepo when this checkout is that submodule (`../..` from here). Read **@coordination.md** before making changes that touch the ISA, the ABI, or anything visible across that boundary. Short version: the spec and `sim_c` are canonical and live here; an ISA/ABI change is one commit here that updates spec, simulators, compiler and a corpus test together, followed by a downstream commit that updates the RTL and bumps the submodule.

## Debugging Rule

**Always debug test failures at the earliest point in the pipeline.** For CPU4 pipeline failures, check and fix the issue in the IR (OOS IR via `-oos` dump) before looking at final CPU4 assembly output. The pipeline is: Braun SSA → OOS → legalize → IRC → emission. Fix the problem at the first stage where it appears.

## Build & Test

```bash
make smallcc        # Build the compiler
make test           # Run all pytest test cases (quiet)
make test_v         # Run all pytest test cases (verbose)
make test_p         # Run pytest cases in parallel (requires pytest-xdist)
make test_irsim     # Run the corpus through the IR interpreter (-runoos/-runirc)
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
IR_VERIFY=1 ./smallcc -arch cpu4 -o out.s t.c                  # run the IR verifier after every pass group (verify.c)
```

Tests are pytest-collected `.c` files under `tests/cases/` with `EXPECT_R0`/`EXPECT_STDOUT`/`EXPECT_COMPILE_FAIL` magic comments (harness: `tests/conftest.py`). `make test` runs them against `sim_c`; `make test_irsim` runs the same corpus through the in-process IR interpreter (`-runoos` and `-runirc`), giving a three-way differential oracle that localizes bugs to legalize/IRC vs emission. Tests under `tests/cases/hw/` exercise sim_c's MMIO device model and are skipped in irsim mode.

### Simulators

`sim_c` (`sim_c.c`) is the primary simulator — a self-contained C program that assembles and executes CPU4 assembly. Build with `make sim_c`.

**`sim_c` usage:**
```
./sim_c [-trace FILE] [-arch cpu4] [-maxsteps N] file.s
```

**`sim_c` debug facilities:**

| Feature | Description |
|---|---|
| Register dump | Always printed on halt: `r0:xxxxxxxx sp:xxxx bp:xxxx lr:xxxx pc:xxxx H:x cycles:N` |
| `-trace FILE` | Writes every instruction as it executes: `[pc] op=xx r0=xxxxxxxx sp=xxxx bp=xxxx` |
| Write watchpoints | Writes to addresses below `0x5000` print to stderr: `WRITE8/16/32 to addr = val  at pc=... sp=... bp=... r0=...`; useful for catching stray stores into the code/data area |
| Crash trace | On unknown opcode, dumps the last 32 executed instructions (pc, opcode, r0, sp, bp) to help locate the crash |
| MMIO cycle counter | A 32-bit read-only cycle counter at address `0xFF00` incremented once per instruction; used by `core_portme.c` for timing |

### CoreMark Benchmark

`bench/coremark/coremark_single.c` is a single-file CoreMark (1 iteration,
performance-run seeds) suitable for quick validation and profiling:

```bash
./smallcc -arch cpu4 -o bench/coremark/coremark.s bench/coremark/coremark_single.c
./sim_c -arch cpu4 -maxsteps 4000000 bench/coremark/coremark.s
# Expect: "Errors detected" — this is ONLY the 10-second minimum-runtime
# check failing on a 1-iteration run and is immaterial for compiler work.
# The real correctness signal is the CRC set: seedcrc 0xe9f5,
# crclist/crcfinal 0xe714, crcmatrix 0x1fd7, crcstate 0x8e3a.
```

To generate an execution profile:
```bash
./sim_c -arch cpu4 -maxsteps 4000000 -profile bench/coremark/coremark.s > bench/coremark/profile.s
```

See `bench/coremark/README.md` for provenance and licence. The multi-file
EEMBC tree is not vendored; the single-file port is a faithful concatenation.

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
| `sx.h` / `sx.c` | Sexp AST: `Sx` cons-cell tree with `SX_PAIR/SX_SYM/SX_STR/SX_INT` kinds; constructors + accessors (data-section interchange only) |
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

- **ILP32 type model.** `char` is 1 byte, `short` is 2 bytes, `int`/`long`/`float`/`double`/pointer are all **4 bytes**. Stack and code addresses still live in the low 64 KB (`sp`, `bp`, `pc` are 16-bit), but pointers are stored as 4-byte values so user code can address the 32 MB SDRAM behind the pbus through ordinary C pointers. The high half of compiler-emitted addresses is zero; `lea` masks its result to 16 bits to guarantee that.
- `new_node()` initializes `node->type = t_void` (not NULL) — type-propagation guards use `== t_void`
- Type singletons (`t_int`, `t_void`, etc.) are interned — use pointer equality for comparison
- Stack starts at `sp = 0xF000`; grows downward; `enter N` saves `(lr<<16)|bp` into one 4-byte slot and allocates N bytes; the first stack-passed param is at `bp+4` (not `bp+8`).
- `adjw imm14` (F3b, opcode 0xc4) adjusts `sp` by a signed 4-byte-scaled amount — used by the backend to pop stack arguments after calls with more than 3 parameters

---

## Detailed Reference

- @docs/architecture.md — tokeniser, parser (grammar, AST nodes), type system, per-TU compilation
- @docs/compiler-pipeline.md — CPU4 nanopass pipeline: lowering, Braun SSA, IRC, emission
- @docs/optimization-passes.md — pass catalog, bitmask system, LICM/CSE tuning constants, emission peepholes
- @docs/isa/cpu4.md — CPU4 registers, instruction set, assembly syntax (encoding tables generated from `cpu4/isa.py`, the source of truth: edit it, run `make isa`)
- @docs/abi.md — calling convention, frame layout, type sizes, symbol naming, MMIO map (the contract between compiler and hardware)
- @docs/c89-status.md — compliance tables, deliberate deviations, what's implemented/missing
- @docs/testing.md — test systems and debugging tips
