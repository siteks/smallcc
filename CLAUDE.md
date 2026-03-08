# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

```bash
make mycc           # Build the compiler
make test_all       # Run all test suites
make test_struct    # Run a specific suite (also: test_init, test_ops, test_logops, test_func,
                    # test_longs, test_array, test_loops, test_goto, test_struct_init,
                    # test_floats, test_compound, test_remaining, test_typedef, test_strings,
                    # test_enum, test_variadic, test_funcptr)
make clean          # Remove binaries and temp files
```

Run the compiler directly (output is assembly to stdout):
```bash
./mycc "int main(){return 5+3;}"
./mycc file1.c file2.c > out.s && ./cpu3/sim.py out.s
./mycc "int main(){return 5+3;}" > tmp.s && ./cpu3/sim.py tmp.s
```

Tests use `cpu3/sim.py` to execute generated assembly and check the value left in register `r0`. On test failure, verbose output is written to `error.log`. The test harness is in `test.sh`; individual suites are in `tests/`.

---

## Compiler Architecture

Single-pass C89 subset compiler. Input is a C source string or one or more `.c` files. Assembly is written to stdout.

### Compilation Pipeline

```
Per-TU loop [mycc.c]:
  reset_codegen()           [codegen.c]     Clear per-TU codegen state
  reset_parser()            [parser.c]      Clear per-TU parser state
  reset_types_state()       [types.c]       Fresh symbol/scope tables (type list preserved)
  make_basic_types()        [types.c]       Populate/reuse global type table
  prepopulate_extern_syms() [mycc.c]        Inject globals from previous TUs
  tokenise()                [tokeniser.c]   Token linked list
  program()                 [parser.c]      AST (Node tree)
  propagate_types()         [parser.c]      Annotate AST nodes with resolved Type*
  gen_code(node, tu_index)  [codegen.c]     Emit assembly to stdout
  harvest_globals()         [mycc.c]        Collect non-static globals for next TU

Preamble (ssp / jl main / halt) is emitted once before the loop.
```

Every phase prints debug information to stderr.

### Source Files

| File | Role |
|---|---|
| `mycc.h` | All shared structs, enums, and function prototypes |
| `mycc.c` | Entry point; per-TU loop; `ExternSym` table; `harvest_globals`/`prepopulate_extern_syms` |
| `tokeniser.c` | Lexer — produces a `Token` linked list |
| `parser.c` | Recursive-descent parser — builds AST; also contains `propagate_types` and `check_operands` |
| `types.c` | Type table, symbol table, struct layout, `add_types_and_symbols`, `reset_types_state`, `insert_extern_sym` |
| `codegen.c` | AST walk; emits assembly pseudoinstructions; `reset_codegen`; static label mangling |

### Key Target Facts

- 16-bit address space: `int` and pointers are **2 bytes**, `long`/`float`/`double` are **4 bytes**
- `new_node()` initializes `node->type = t_void` (not NULL) — propagate_types guards use `== t_void`
- Type singletons (`t_int`, `t_void`, etc.) are interned — use pointer equality for comparison
- Stack starts at `sp = 0x1000`; grows downward; `enter N` saves lr+bp and allocates N bytes

---

## Detailed Reference

- @docs/architecture.md — tokeniser, parser (grammar, AST nodes, node struct), type system, per-TU compilation
- @docs/codegen.md — stack frame, variable addressing, expression idioms, cast generation, initialization, three-pass structure
- @docs/isa.md — CPU3 registers, full instruction set, assembly syntax
- @docs/c89-status.md — compliance tables, deliberate deviations, what's implemented/missing
