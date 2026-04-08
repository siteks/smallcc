# Backend Documentation

This document describes the **CPU3 stack-machine backend** — the path from annotated AST
to CPU3 assembly emission (`codegen.c`, `optimise.c`, `backend.c`).

> **CPU4 path:** The `-arch cpu4` flag uses a completely different nanopass pipeline
> (`lower.c` → `braun.c` → `dom.c` → `oos.c` → `alloc.c` → `emit.c`). That pipeline
> is documented in `docs/compiler-pipeline.md`.

## Pipeline Overview

```
gen_ir()              [codegen.c]    AST → stack IR list
mark_basic_blocks()   [codegen.c]    insert IR_BB_START sentinels
peephole(opt_level)   [optimise.c]   constant fold, dead branch, adj merge, store/reload elim
backend_emit_asm()    [backend.c]    stack IR → CPU3 assembly
```

---

## Stack IR

### `IRInst` Struct

```c
typedef struct IRInst {
    IROp          op;       // operation kind
    int           operand;  // integer operand (imm, offset, label id, byte count)
    const char   *sym;      // symbolic operand (label name, symbol name); NULL if unused
    int           line;     // source line number (0 = unknown); set by ir_append()
    struct IRInst *next;
} IRInst;
```

`ir_append(op, operand, sym)` allocates a new node, stamps `current_codegen_line`, and appends it to `codegen_ctx.ir_head/ir_tail`.

### `IROp` Enum

| Group | Operations |
|---|---|
| Immediate load | `IR_IMM` — r0 = operand (or &sym when sym != NULL) |
| Stack | `IR_PUSH`, `IR_PUSHW`, `IR_POP`, `IR_POPW` |
| Arithmetic | `IR_ADD`, `IR_SUB`, `IR_MUL`, `IR_DIV`, `IR_MOD` |
| Bitwise | `IR_SHL`, `IR_SHR`, `IR_AND`, `IR_OR`, `IR_XOR` |
| Signed variants | `IR_DIVS`, `IR_MODS`, `IR_SHRS` |
| Comparisons (unsigned) | `IR_EQ`, `IR_NE`, `IR_LT`, `IR_LE`, `IR_GT`, `IR_GE` |
| Comparisons (signed) | `IR_LTS`, `IR_LES`, `IR_GTS`, `IR_GES` |
| Float arithmetic | `IR_FADD`, `IR_FSUB`, `IR_FMUL`, `IR_FDIV` |
| Float comparisons | `IR_FLT`, `IR_FLE`, `IR_FGT`, `IR_FGE` |
| Float/int conversion | `IR_ITOF`, `IR_FTOI` |
| Sign extension | `IR_SXB`, `IR_SXW` |
| Memory load | `IR_LB`, `IR_LW`, `IR_LL` (byte / word / long) |
| Memory store | `IR_SB`, `IR_SW`, `IR_SL` |
| Address | `IR_LEA` — r0 = bp + operand |
| Control flow | `IR_J`, `IR_JZ`, `IR_JNZ`, `IR_JL`, `IR_JLI`, `IR_RET` |
| Frame | `IR_ENTER`, `IR_ADJ` |
| Labels | `IR_LABEL` (numeric, operand = id), `IR_SYMLABEL` (sym = name) |
| Data directives | `IR_WORD`, `IR_BYTE`, `IR_ALIGN` |
| Special | `IR_PUTCHAR`, `IR_COMMENT`, `IR_BB_START`, `IR_NOP` |

Binary ops (`IR_ADD` … `IR_GES`, `IR_FADD` … `IR_FGE`) follow the stack-binary convention: left operand is popped from the stack (`mem[sp]`), right operand is in r0, result written to r0, sp advanced by 4.

---

## IR Construction (`gen_ir`)

`gen_ir(node, tu_index)` sets `current_tu` then makes four passes over the top-level declaration list for the current TU:

1. **Pass 1** — function definitions (text area). Each function body emits `IR_SYMLABEL`, `IR_ENTER`, per-scope `IR_ADJ` pairs, expression IR, and `IR_RET`. Local static variables encountered here are collected for pass 2b.
2. **Pass 2** — global variable declarations (data area). `extern` declarations and bare function prototypes are skipped. Static globals use the mangled label `_s{tu_index}_{name}`.
3. **Pass 2b** — local static variable data (`_ls{id}:` labels). `local_static_counter` is monotonically increasing across TUs; label IDs are assigned at symbol-table build time.
4. **Pass 3** — deferred string literal data (`_l{id}:` labels), each as a sequence of `IR_BYTE` nodes followed by a null-terminator byte. Label IDs are monotonically increasing across TUs.

---

## Basic-Block Markers (`mark_basic_blocks`)

`mark_basic_blocks()` inserts `IR_BB_START` sentinel nodes immediately after `gen_ir` completes. These nodes emit no assembly; they exist solely to delimit basic blocks for the optimiser.

A **basic-block leader** is:
1. The very first instruction.
2. Any instruction immediately following a terminator (`IR_J`, `IR_JZ`, `IR_JNZ`, `IR_RET`).
3. Any `IR_LABEL` or `IR_SYMLABEL` (a potential jump target).

`IR_JL` and `IR_JLI` are calls (not terminators) — execution continues at the next instruction.

---

## Stack-IR Peephole (`optimise.c`)

Enabled by `-O<N>` (`-O` alone implies `-O1`). Called as `peephole(opt_level)` after `mark_basic_blocks`. `IR_BB_START` is a hard barrier — no pattern fires across it. `IR_NOP`/`IR_COMMENT` are transparent within a block. The pass repeats until stable (max 20 iterations), then `compact_ir()` removes all `IR_NOP` nodes.

### `-O1` Rules

| # | Pattern | Replacement | Notes |
|---|---|---|---|
| 1 | `IMM K1; PUSH; IMM K2; binop` | `IMM (K1 op K2)` | Constant-fold arithmetic and comparisons |
| 2 | `IMM K; SXB/SXW` | `IMM sext(K)` | Constant sign-extend |
| 3 | `J/JZ/JNZ Lx` → `BB_START` → `LABEL Lx` | kill branch | Dead jump to immediately following label |
| 4 | `ADJ N; ADJ M` | `ADJ N+M` (or delete if 0) | Merge adjacent stack adjustments |
| 5 | `ADJ 0` | delete | Zero adjustment |
| 9 | `LEA N; PUSH; <expr>; SW/SB/SL` + `LEA N; LW/LB/LL` | kill reload | Store/reload elim; gated on CPU3 only |

Rule 1 covers: `add sub mul and or xor shl shr eq ne lt le gt ge`. Symbolic immediates are never folded. Rules 4+5 together collapse `adj -N`/`adj +N` pairs around empty scopes.

### `-O2` Additional Rules

| # | Pattern | Replacement | Notes |
|---|---|---|---|
| 6 | `PUSH; IMM 1; MUL` | delete | Multiply by 1 |
| 7 | `PUSH; IMM 2^k; MUL` | `PUSH; IMM k; SHL` | Strength-reduce; Rule 1 then folds constant shifts |
| 8 | `PUSH; IMM 0; ADD/SUB` | delete | Add/subtract zero |

Rule 7 is particularly effective for array indexing: stride-scale multiplications (always a power of 2 since `sizeof(int) = 2`) convert to shifts, which constant-fold when the index is compile-time constant, collapsing the entire address computation to a single `IMM`.

Rule 9 scans forward from the address push, tracking expression-stack depth across pushes, pops, and binary ops. When depth reaches 0 at a store, r0 still holds the stored value. If the immediately following instructions reload from the same address, they are killed. Aborts at `JL`/`JLI` (calls may clobber memory) and at BB boundaries. 64-instruction scan limit.

---

## CPU3 Backend (`backend.c`)

`backend_emit_asm(ir_head)` walks the stack IR list and writes CPU3 assembly text. It is the sole difference between the two targets from the perspective of `smallcc.c`.

### Stack Frame Layout

```
  bp+8 + 2*(n-1)   param n   (last param)
       ...
  bp+8             param 0   (first param)  ← sp immediately before jl
  bp+4             return address (lr)
  bp+0             saved bp                 ← bp set here by enter
  bp-2             local 0
  bp-4             local 1
       ...
  bp-2k            local k
```

`enter N`: `mem[sp-4]=lr; mem[sp-8]=bp; bp=sp-8; sp-=N+8`.
`ret`: `sp=bp; bp=mem[sp]; pc=mem[sp+4]; sp+=8`.

Locals within a compound statement are allocated with `adj -size` on entry and reclaimed with `adj +size` on exit. Nested compound statements chain their adj pairs.

### Variable Addressing

`gen_varaddr_from_ident(node, name)` emits an address into r0:

| Variable kind | IR emitted | Result in r0 |
|---|---|---|
| Global (non-static) | `IR_IMM sym=label_name` | symbolic address |
| Global (static) | `IR_IMM sym=_s{tu}_{name}` | mangled label address |
| Local static | `IR_IMM sym=_ls{id}` | persistent data-section label |
| Parameter | `IR_LEA +offset` | bp + positive offset |
| Local | `IR_LEA -offset` | bp - offset |

For non-array, non-pointer variables, `gen_ld(size)` immediately follows with `IR_LB/LW/LL` to load the value. Arrays and pointers leave r0 as the address.

### Expression Idioms

**Binary operation** (`ND_BINOP`):
```asm
; gen_expr(lhs) → r0 = lhs value
push
; gen_expr(rhs) → r0 = rhs value
add/sub/mul/…       ; r0 = mem[sp] op r0; sp += 4
```

`gen_arith_op(op, is_float, is_signed)` selects the mnemonic. Signed integer types (`char`, `short`, `int`, `long`) use signed variants (`lts les gts ges divs mods shrs`); unsigned types and pointers use unsigned variants. Float operands use `f`-prefixed instructions.

**Assignment** (`ND_ASSIGN`):
```asm
; gen_addr(lhs) → r0 = address of lhs
push
; gen_expr(rhs) → r0 = value to store
sb/sw/sl            ; mem[stack[sp]] = r0; sp += 4
```

**Pointer/array dereference** (`ND_UNARYOP "*"`, non-call):
```asm
; gen_expr(child) → r0 = address to load from
lb/lw/ll            ; r0 = mem[r0]  (size from elem_type(node->type)->size)
```

**Struct member access** (`ND_MEMBER`, `is_function=false`):
```asm
; gen_addr(struct_expr) → r0 = struct base address
push
immw    member_offset
add
lb/lw/ll            ; load member value
```

**Direct function call**:
```asm
; push arguments right-to-left (last arg pushed first)
jl      func_name
adj     param_total_bytes   ; caller cleans up parameters
```

**Argument slot widening** (`push_args_list`): when a function's declared parameter type is wider than the argument's natural slot size (e.g. passing an `int` to an `unsigned long` parameter), the caller inserts `IR_SXW` (for signed types) to widen the value to 4 bytes before pushing. Conversely, when the arg is wider than the param (e.g. `long` to `int`), the caller truncates with `push; immw 0xffff; and`. This ensures the callee's `IR_LL`/`IR_LW` loads see correctly-sized values.

**Indirect call through function pointer variable** (`fp(args)`):
```asm
; push arguments right-to-left
lea     fp_offset           ; or immw fp for globals
lw                          ; load function pointer into r0
jli                         ; lr = pc; pc = r0
adj     param_total_bytes
```

**Logical OR** (`||`):
```asm
; gen_expr(lhs)
jnz  L1             ; short-circuit
; gen_expr(rhs)
jnz  L1
immw 0
j    L2
L1: immw 1
L2:
```

**Logical AND** (`&&`) — symmetric with `jz` for false short-circuit.

**Break/continue**: `break_labels[64]` and `cont_labels[64]` stacks (indexed by `loop_depth`) store the target label for the enclosing loop or switch. `gen_breakstmt`/`gen_continuestmt` emit `IR_J break_labels[loop_depth-1]` / `IR_J cont_labels[loop_depth-1]`.

**Switch dispatch**: two-phase — phase 1 re-evaluates the selector for each case and emits `push; eq; jnz Lcase`; phase 2 emits the body, inserting `IR_LABEL` at each case/default node.

**Goto/label**: `collect_labels()` pre-scans the function body AST before codegen, assigning numeric IDs to all `ND_LABELSTMT` nodes. Both forward and backward `goto` then resolve by name.

### Cast Generation (`gen_cast`)

No-op when src and dst sizes match. Otherwise:

| From → To | Instructions |
|---|---|
| char → int (sign-extend) | `sxb` |
| short → int (sign-extend) | `sxw` |
| long/ulong → int (truncate) | `push; immw 0xffff; and` |
| long/ulong → char (truncate) | `push; immw 0xff; and` |
| int → char (truncate) | `push; immw 0xff; and` |
| uchar/ushort → larger | zero-extend (no-op; top bits assumed 0) |
| int/ptr → long (zero-extend) | `push; immw 0; push; …` (builds 32-bit pair) |
| int-like → float/double | sign-extend to 32 bits (`sxb`/`sxw`), then `itof` |
| float/double → int-like | `ftoi`, then `push; immw mask; and` if size < 4 |

### Global Variable Initialization

**Scalars**: emit `word value` (or `long` for 4-byte types) in the data section. Float scalars emit 4 bytes of IEEE 754 representation via `gen_bytes`.

**Arrays** (global): `gen_mem_inits()` fills a byte buffer recursively, then `gen_bytes()` emits one `IR_BYTE` per byte. Exception — pointer arrays with string-literal initializers emit `IR_WORD label` per element (a byte buffer cannot hold symbolic addresses).

**Arrays** (local): `gen_fill(vaddr, size)` zeroes all bytes, then `gen_inits()` emits `lea; push; immw; sw/sl` sequences for non-zero initializers.

**Structs** (local): `gen_struct_inits` writes each field with `lea; push; immw; sw/sl`.

**Structs** (global): `gen_struct_mem_inits` fills a byte buffer, then `gen_bytes` emits one `IR_BYTE` per byte. Exception — when a struct contains pointer fields initialized with address-of-global expressions (e.g. `struct node n2 = {&n3, 20}`), `collect_sym_fields` detects the symbolic fields and emits the struct byte-by-byte with `IR_WORD label` at each symbolic field offset. This is analogous to the pointer-array string-literal handling. Nested structs are handled recursively. Partial initializers zero-fill the remainder.

### Sethi-Ullman Numbering (`label_su`)

Runs as an AST pass (in `parser.c`) between `insert_coercions` and `gen_ir`. Assigns a stack-depth label to every expression node; for commutative binary operators with pure operands, reorders children so the heavier subtree evaluates first. This reduces peak stack depth and moves constant subtrees left where Rule 1 constant-folds them.

| Node kind | `su_label` |
|---|---|
| `ND_LITERAL`, `ND_IDENT` | 0 |
| `ND_UNARYOP`, `ND_MEMBER`, `ND_CAST` | su_label of the single expression child |
| `ND_TERNARY` | max(cond, then, else) |
| `ND_BINOP` commutative (after optional swap) | l==r ? l+1 : max(l,r) |
| `ND_BINOP` non-commutative | max(l, r+1) |
| Comma / `&&` / `\|\|` | 0 (evaluation order is fixed) |

Swap condition: if `su(right) > su(left)` and both children are pure (`is_pure_expr`), swap `ch[0]` ↔ `ch[1]`. Pure = literals, non-call identifiers, non-mutating ops, casts. Impure = assignments, calls, `++`/`--`.

### Annotation Mode (`-ann`)

Every `IRInst` carries a `line` field stamped by `ir_append()` from `current_codegen_line`. `backend_emit_asm` maintains `prev_line` and `bb_idx`; when `flag_annotate` is set it emits a `; source text` comment on the first instruction from each new source line, and `; --- bbN ---` at each `IR_BB_START`. `set_ann_source(src)` builds a line-pointer index into the preprocessed source text.

---

## CPU4 Backend

> **Note:** The `nanopass` branch replaces the old CPU4 stack-IR→IR3→risc_backend pipeline
> with a new pipeline: `lower.c` → `braun.c` → `dom.c` → `oos.c` → `alloc.c` → `emit.c`.
> The old `braun_ssa`, `ir3_opt`, `irc_regalloc`, and `risc_backend_emit` files no longer
> exist. See `docs/compiler-pipeline.md` for the current CPU4 architecture.

