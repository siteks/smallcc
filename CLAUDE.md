# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

```bash
make smallcc           # Build the compiler
make test_all       # Run all test suites
make test_struct    # Run a specific suite (also: test_init, test_ops, test_logops, test_func,
                    # test_longs, test_array, test_loops, test_goto, test_struct_init,
                    # test_floats, test_compound, test_remaining, test_typedef, test_strings,
                    # test_enum, test_variadic, test_funcptr)
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

`cpu3/sim.py` (Python) is kept as a reference implementation and to support any legacy uses. It imports `cpu3/cpu.py` (instruction definitions, execution) and `cpu3/assembler.py` (two-pass assembler). The assembler picks up new instructions automatically from `cpu.py`'s `ptable`, so `cpu3/assembler.py` rarely needs changes.

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
| `sim_c.c` | Primary simulator — self-contained C assembler + CPU3 executor; `make sim_c` |
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

- @docs/architecture.md — tokeniser, parser (grammar, AST nodes, node struct), type system, per-TU compilation
- @docs/codegen.md — stack frame, variable addressing, expression idioms, cast generation, initialization, three-pass structure
- @docs/isa.md — CPU3 registers, full instruction set, assembly syntax
- @docs/c89-status.md — compliance tables, deliberate deviations, what's implemented/missing
