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
./smallcc t.c > out.s && ./cpu3/sim.py out.s          # output to stdout
./smallcc -o out.s t.c && ./cpu3/sim.py out.s          # output to file
./smallcc -o out.s file1.c file2.c && ./cpu3/sim.py out.s  # multi-TU
```

Tests use `cpu3/sim.py` to execute generated assembly and check the value left in register `r0`. On test failure, verbose output is written to `error.log`. The test harness is in `test.sh`; individual suites are in `tests/`.

---

## Compiler Architecture

C89 subset compiler. Input is one or more `.c` files; assembly is written to stdout or a file specified with `-o`. Usage: `smallcc [-o outfile] <source.c> [source2.c ...]`.

### Compilation Pipeline

```
Preamble (ssp / jl main / halt) is emitted once before the per-TU loop.

Per-TU loop [smallcc.c]:
  reset_codegen()           [codegen.c]     Clear per-TU codegen state
  reset_parser()            [parser.c]      Clear per-TU parser state
  reset_types_state()       [types.c]       Fresh symbol/scope tables (type list preserved)
  make_basic_types()        [types.c]       Populate/reuse global type table
  prepopulate_extern_syms() [smallcc.c]        Inject globals from previous TUs
  tokenise()                [tokeniser.c]   Token linked list
  program()                 [parser.c]      AST (Node tree)
  resolve_symbols(root)     [parser.c]      Set ND_IDENT types via symbol table lookup
  derive_types(root)        [parser.c]      Propagate types bottom-up through the AST
  insert_coercions(root)    [parser.c]      Insert ND_CAST / stride-scale nodes
  finalize_local_offsets()  [types.c]       Compute bp-relative offsets for all locals
  gen_ir(node, tu_index)    [codegen.c]     Walk AST; build flat IR instruction list
  backend_emit_asm(ir_head) [backend.c]     Walk IR list; emit assembly text
  harvest_globals()         [smallcc.c]        Collect non-static globals for next TU
```

Every phase prints debug information to stderr.

### Source Files

| File | Role |
|---|---|
| `smallcc.h` | All shared structs, enums, and function prototypes |
| `smallcc.c` | Entry point; `-o` arg parsing; per-TU loop; `ExternSym` table; `harvest_globals`/`prepopulate_extern_syms` |
| `tokeniser.c` | Lexer — produces a `Token` linked list |
| `parser.c` | Recursive-descent parser — builds AST; `resolve_symbols`, `derive_types`, `insert_coercions` |
| `types.c` | Type table, symbol table, struct layout, `add_types_and_symbols`, `reset_types_state`, `insert_extern_sym` |
| `codegen.c` | AST walk; builds flat IR instruction list (`gen_ir`); `reset_codegen`; static label mangling |
| `backend.c` | IR → assembly emission (`backend_emit_asm`); `set_asm_out`; retargeting point |

### Key Target Facts

- 16-bit address space: `int` and pointers are **2 bytes**, `long`/`float`/`double` are **4 bytes**
- `new_node()` initializes `node->type = t_void` (not NULL) — type-propagation guards use `== t_void`
- Type singletons (`t_int`, `t_void`, etc.) are interned — use pointer equality for comparison
- Stack starts at `sp = 0x1000`; grows downward; `enter N` saves lr+bp and allocates N bytes

---

## Detailed Reference

- @docs/architecture.md — tokeniser, parser (grammar, AST nodes, node struct), type system, per-TU compilation
- @docs/codegen.md — stack frame, variable addressing, expression idioms, cast generation, initialization, three-pass structure
- @docs/isa.md — CPU3 registers, full instruction set, assembly syntax
- @docs/c89-status.md — compliance tables, deliberate deviations, what's implemented/missing
