# Code Generator, IR, and Backends

`codegen.c` walks the annotated AST and builds a flat linked list of `IRInst` (stack IR) nodes.
From there the pipeline splits by target architecture:

```
gen_ir()              [codegen.c]    AST → stack IR list
mark_basic_blocks()   [codegen.c]    insert IR_BB_START sentinels
peephole(opt_level)   [optimise.c]   constant fold, dead branch, adj merge, store/reload elim

── CPU3 ──────────────────────────────────────────────────────────────────────
backend_emit_asm()    [backend.c]    stack IR → CPU3 assembly

── CPU4 ──────────────────────────────────────────────────────────────────────
braun_ssa()           [braun.c]       stack IR → IR3Inst (fresh vregs)
linscan_regalloc()    [linscan.c]     vregs → physical r0–r7 (linear-scan)
ir3_lower()           [ir3_lower.c]   IR3Inst → SSAInst
risc_backend_emit()   [risc_backend.c]  SSAInst → CPU4 assembly
```

The stack IR is identical for both targets; the split happens after `peephole`. `backend.c`
and `risc_backend.c` are independent emitters that consume the same upstream IR.

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

`ir_append(op, operand, sym)` allocates a new node, stamps `current_codegen_line`, and appends
it to `codegen_ctx.ir_head/ir_tail`.

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

Binary ops (`IR_ADD` … `IR_GES`, `IR_FADD` … `IR_FGE`) follow the stack-binary convention:
left operand is popped from the stack (`mem[sp]`), right operand is in r0, result written to r0,
sp advanced by 4.

---

## IR Construction (`gen_ir`)

`gen_ir(node, tu_index)` sets `current_tu` then makes four passes over the top-level declaration
list for the current TU:

1. **Pass 1** — function definitions (text area). Each function body emits `IR_SYMLABEL`,
   `IR_ENTER`, per-scope `IR_ADJ` pairs, expression IR, and `IR_RET`. Local static variables
   encountered here are collected for pass 2b.
2. **Pass 2** — global variable declarations (data area). `extern` declarations and bare
   function prototypes are skipped. Static globals use the mangled label `_s{tu_index}_{name}`.
3. **Pass 2b** — local static variable data (`_ls{id}:` labels). `local_static_counter` is
   monotonically increasing across TUs; label IDs are assigned at symbol-table build time.
4. **Pass 3** — deferred string literal data (`_l{id}:` labels), each as a sequence of
   `IR_BYTE` nodes followed by a null-terminator byte. Label IDs are monotonically increasing
   across TUs.

---

## Basic-Block Markers (`mark_basic_blocks`)

`mark_basic_blocks()` inserts `IR_BB_START` sentinel nodes immediately after `gen_ir` completes.
These nodes emit no assembly; they exist solely to delimit basic blocks for the optimiser.

A **basic-block leader** is:
1. The very first instruction.
2. Any instruction immediately following a terminator (`IR_J`, `IR_JZ`, `IR_JNZ`, `IR_RET`).
3. Any `IR_LABEL` or `IR_SYMLABEL` (a potential jump target).

`IR_JL` and `IR_JLI` are calls (not terminators) — execution continues at the next instruction.

---

## Stack-IR Peephole (`optimise.c`)

Enabled by `-O<N>` (`-O` alone implies `-O1`). Called as `peephole(opt_level)` after
`mark_basic_blocks`. `IR_BB_START` is a hard barrier — no pattern fires across it.
`IR_NOP`/`IR_COMMENT` are transparent within a block. The pass repeats until stable (max 20
iterations), then `compact_ir()` removes all `IR_NOP` nodes.

### `-O1` Rules

| # | Pattern | Replacement | Notes |
|---|---|---|---|
| 1 | `IMM K1; PUSH; IMM K2; binop` | `IMM (K1 op K2)` | Constant-fold arithmetic and comparisons |
| 2 | `IMM K; SXB/SXW` | `IMM sext(K)` | Constant sign-extend |
| 3 | `J/JZ/JNZ Lx` → `BB_START` → `LABEL Lx` | kill branch | Dead jump to immediately following label |
| 4 | `ADJ N; ADJ M` | `ADJ N+M` (or delete if 0) | Merge adjacent stack adjustments |
| 5 | `ADJ 0` | delete | Zero adjustment |
| 9 | `LEA N; PUSH; <expr>; SW/SB/SL` + `LEA N; LW/LB/LL` | kill reload | Store/reload elim; gated on CPU3 only |

Rule 1 covers: `add sub mul and or xor shl shr eq ne lt le gt ge`. Symbolic immediates are
never folded. Rules 4+5 together collapse `adj -N`/`adj +N` pairs around empty scopes.

### `-O2` Additional Rules

| # | Pattern | Replacement | Notes |
|---|---|---|---|
| 6 | `PUSH; IMM 1; MUL` | delete | Multiply by 1 |
| 7 | `PUSH; IMM 2^k; MUL` | `PUSH; IMM k; SHL` | Strength-reduce; Rule 1 then folds constant shifts |
| 8 | `PUSH; IMM 0; ADD/SUB` | delete | Add/subtract zero |

Rule 7 is particularly effective for array indexing: stride-scale multiplications (always a
power of 2 since `sizeof(int) = 2`) convert to shifts, which constant-fold when the index is
compile-time constant, collapsing the entire address computation to a single `IMM`.

Rule 9 scans forward from the address push, tracking expression-stack depth across pushes, pops,
and binary ops. When depth reaches 0 at a store, r0 still holds the stored value. If the
immediately following instructions reload from the same address, they are killed. Aborts at
`JL`/`JLI` (calls may clobber memory) and at BB boundaries. 64-instruction scan limit.

---

## CPU3 Backend (`backend.c`)

`backend_emit_asm(ir_head)` walks the stack IR list and writes CPU3 assembly text. It is the
sole difference between the two targets from the perspective of `smallcc.c`.

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

Locals within a compound statement are allocated with `adj -size` on entry and reclaimed with
`adj +size` on exit. Nested compound statements chain their adj pairs.

### Variable Addressing

`gen_varaddr_from_ident(node, name)` emits an address into r0:

| Variable kind | IR emitted | Result in r0 |
|---|---|---|
| Global (non-static) | `IR_IMM sym=label_name` | symbolic address |
| Global (static) | `IR_IMM sym=_s{tu}_{name}` | mangled label address |
| Local static | `IR_IMM sym=_ls{id}` | persistent data-section label |
| Parameter | `IR_LEA +offset` | bp + positive offset |
| Local | `IR_LEA -offset` | bp - offset |

For non-array, non-pointer variables, `gen_ld(size)` immediately follows with `IR_LB/LW/LL`
to load the value. Arrays and pointers leave r0 as the address.

### Expression Idioms

**Binary operation** (`ND_BINOP`):
```asm
; gen_expr(lhs) → r0 = lhs value
push
; gen_expr(rhs) → r0 = rhs value
add/sub/mul/…       ; r0 = mem[sp] op r0; sp += 4
```

`gen_arith_op(op, is_float, is_signed)` selects the mnemonic. Signed integer types (`char`,
`short`, `int`, `long`) use signed variants (`lts les gts ges divs mods shrs`); unsigned types
and pointers use unsigned variants. Float operands use `f`-prefixed instructions.

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

**Break/continue**: `break_labels[64]` and `cont_labels[64]` stacks (indexed by `loop_depth`)
store the target label for the enclosing loop or switch. `gen_breakstmt`/`gen_continuestmt`
emit `IR_J break_labels[loop_depth-1]` / `IR_J cont_labels[loop_depth-1]`.

**Switch dispatch**: two-phase — phase 1 re-evaluates the selector for each case and emits
`push; eq; jnz Lcase`; phase 2 emits the body, inserting `IR_LABEL` at each case/default node.

**Goto/label**: `collect_labels()` pre-scans the function body AST before codegen, assigning
numeric IDs to all `ND_LABELSTMT` nodes. Both forward and backward `goto` then resolve by name.

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

**Scalars**: emit `word value` (or `long` for 4-byte types) in the data section. Float scalars
emit 4 bytes of IEEE 754 representation via `gen_bytes`.

**Arrays** (global): `gen_mem_inits()` fills a byte buffer recursively, then `gen_bytes()` emits
one `IR_BYTE` per byte. Exception — pointer arrays with string-literal initializers emit
`IR_WORD label` per element (a byte buffer cannot hold symbolic addresses).

**Arrays** (local): `gen_fill(vaddr, size)` zeroes all bytes, then `gen_inits()` emits
`lea; push; immw; sw/sl` sequences for non-zero initializers.

**Structs**: `gen_struct_inits` (local) and `gen_struct_mem_inits` (global) write each field;
nested structs recurse. Partial initializers zero-fill the remainder.

### Sethi-Ullman Numbering (`label_su`)

Runs as an AST pass (in `parser.c`) between `insert_coercions` and `gen_ir`. Assigns a
stack-depth label to every expression node; for commutative binary operators with pure operands,
reorders children so the heavier subtree evaluates first. This reduces peak stack depth and moves
constant subtrees left where Rule 1 constant-folds them.

| Node kind | `su_label` |
|---|---|
| `ND_LITERAL`, `ND_IDENT` | 0 |
| `ND_UNARYOP`, `ND_MEMBER`, `ND_CAST` | su_label of the single expression child |
| `ND_TERNARY` | max(cond, then, else) |
| `ND_BINOP` commutative (after optional swap) | l==r ? l+1 : max(l,r) |
| `ND_BINOP` non-commutative | max(l, r+1) |
| Comma / `&&` / `\|\|` | 0 (evaluation order is fixed) |

Swap condition: if `su(right) > su(left)` and both children are pure (`is_pure_expr`), swap
`ch[0]` ↔ `ch[1]`. Pure = literals, non-call identifiers, non-mutating ops, casts. Impure =
assignments, calls, `++`/`--`.

### Annotation Mode (`-ann`)

Every `IRInst` carries a `line` field stamped by `ir_append()` from `current_codegen_line`.
`backend_emit_asm` maintains `prev_line` and `bb_idx`; when `flag_annotate` is set it emits a
`; source text` comment on the first instruction from each new source line, and `; --- bbN ---`
at each `IR_BB_START`. `set_ann_source(src)` builds a line-pointer index into the preprocessed
source text.

---

## CPU4 Backend

After `peephole`, the CPU4 path runs three additional passes (braun_ssa, linscan_regalloc,
ir3_lower) before reaching the unchanged `risc_backend_emit`.

```
Stack IR (codegen.c, unchanged)
    ↓  peephole(opt_level)      [optimise.c]    — unchanged
    ↓  braun_ssa()              [braun.c]        stack IR → IR3Inst (fresh vregs)
    ↓  linscan_regalloc()       [linscan.c]      vregs → physical r0–r6 (r7 = spill scratch)
    ↓  ir3_lower()              [ir3_lower.c]    IR3Inst → SSAInst
    ↓  risc_backend_emit()      [risc_backend.c] SSAInst → CPU4 assembly (unchanged)
```

**Removed from old pipeline:** `lift_to_ssa` (`ssa.c`), `ssa_peephole` (`ssa_opt.c`),
`rb_prescan` (callee-save pre-scan), depth-indexed `regalloc` (`regalloc.c`).

**ABI:** All-caller-save.  r0–r7 are all caller-saved.  No callee-save infrastructure.
r7 is reserved as the spill scratch register; r0 is the accumulator; r1–r6 are available
for allocation.

### IR3: the intermediate representation

#### `IR3Inst` struct (`ir3.h`)

```c
typedef struct IR3Inst {
    IR3Op        op;      /* operation kind                                    */
    IROp         alu_op;  /* IR3_ALU / IR3_ALU1: original stack-IR opcode      */
    int          rd;      /* dest vreg  (IR3_VREG_NONE = no destination)       */
    int          rs1;     /* source vreg 1                                     */
    int          rs2;     /* source vreg 2                                     */
    int          imm;     /* integer immediate, bp-relative byte offset, or
                             numeric label id                                  */
    int          size;    /* 1 / 2 / 4 bytes (IR3_LOAD / IR3_STORE)            */
    const char  *sym;     /* call target name, symbolic label, or NULL         */
    int          line;    /* source line for -ann annotation                   */
    struct IR3Inst *next;
} IR3Inst;
```

#### `IR3Op` enum

| Op | Description |
|---|---|
| `IR3_CONST` | `rd = imm` (integer); or `rd = &sym` (symbolic) when `sym != NULL` |
| `IR3_LOAD` | `rd = mem[rs1 + imm]`, `size` bytes; `rs1 = IR3_VREG_BP` → bp-relative |
| `IR3_STORE` | `mem[rd + imm] = rs1`, `size` bytes; `rd = IR3_VREG_BP` → bp-relative |
| `IR3_LEA` | `rd = bp + imm` — non-promoted local address, no load |
| `IR3_ALU` | `rd = rs1 alu_op rs2` |
| `IR3_ALU1` | `rd = alu_op(rs1)` — in-place sign-extend or float convert |
| `IR3_MOV` | `rd = rs1` — register copy |
| `IR3_CALL` | `jl sym` — direct call; result in ACCUM (r0) |
| `IR3_CALLR` | `jlr` — indirect call via r0 |
| `IR3_RET` | `ret` |
| `IR3_J` | unconditional jump to numeric label `imm` |
| `IR3_JZ` | jump if r0 == 0 |
| `IR3_JNZ` | jump if r0 != 0 |
| `IR3_ENTER` | `enter N` |
| `IR3_ADJ` | `sp += imm` |
| `IR3_SYMLABEL` | named label (function entry or data label) |
| `IR3_LABEL` | numeric label |
| `IR3_WORD / IR3_BYTE / IR3_ALIGN` | data directives (pass-through) |
| `IR3_PUTCHAR / IR3_COMMENT` | special pass-throughs |

#### Virtual register encoding

| Value | Meaning |
|---|---|
| `-2` (`IR3_VREG_BP`) | bp-relative addressing sentinel (not a real register) |
| `-1` (`IR3_VREG_NONE`) | no destination / unused operand |
| `0–7` | physical registers (only after `linscan_regalloc`) |
| `100` (`IR3_VREG_ACCUM`) | accumulator — always maps to physical r0; never allocated |
| `101, 102, …` | fresh scratch vregs, allocated by `ir3_new_vreg()` |

The fresh-vreg scheme replaces the old depth-indexed scheme (`VREG_START + depth`). Every
expression result gets a unique ID, so the linear-scan allocator can assign physical registers
based on actual live ranges rather than expression depth.

---

### Pass 1: `braun_ssa()` (`braun.c`)

Converts the flat stack-IR list to IR3Inst with fresh virtual registers. No phi nodes and no
local promotion — all locals and parameters go through `IR3_LEA + IR3_LOAD / IR3_STORE`.
Promotion of scalar variables to SSA registers (local to a basic block or across phi copies)
is disabled: promoting variables across loop back-edges would require loop-aware live-range
extension in linscan, and with 7 available caller-saved registers, deeply-nested switch/state
machine code would trigger register spilling.  The F2 bp-relative load/store instructions
(`lw r, [bp+imm7]`, 2 bytes, 1 cycle) make memory access cheap enough that no-promotion is
competitive with promotion for typical code patterns.

#### Virtual stack model

Identical to the old `lift_to_ssa`, except virtual register IDs are fresh instead of
depth-indexed:

```c
static int vs_reg[MAX_VDEPTH];   // fresh vreg at each depth slot
static int vs_size[MAX_VDEPTH];  // push size (2 or 4 bytes) per slot
static int vs_depth;             // current virtual stack depth
static int vsp;                  // running (sp - bp) byte offset for scope-adj tracking
```

`vs_reg[0]` is always `IR3_VREG_ACCUM` (100, physical r0). `IR_PUSH` saves the accumulator
into a fresh vreg; `IR_POP` restores a previously saved vreg to the accumulator.

#### Stack IR → IR3 mapping

| Stack IR | IR3 output | Notes |
|---|---|---|
| `IR_IMM K` (integer) | `IR3_CONST rd=ACCUM, imm=K` | |
| `IR_IMM S` (symbolic) | `IR3_CONST rd=ACCUM, sym=S` | |
| `IR_LEA N; IR_LW/LB/LL` | `IR3_LOAD rd=ACCUM, rs1=BP, imm=N, size` | peek-ahead collapses pair |
| `IR_LEA N` (standalone) | `IR3_LEA rd=ACCUM, imm=N` | non-promotable local address |
| `IR_LW/LB/LL` | `IR3_LOAD rd=ACCUM, rs1=ACCUM, imm=0, size` | reg-relative deref |
| `IR_SW/SB/SL` | `IR3_STORE rd=vs_reg[top], rs1=ACCUM, imm=0, size` | addr from vstack |
| `IR_PUSH/PUSHW` | `IR3_MOV rd=fresh, rs1=ACCUM`; push fresh onto vstack | |
| `IR_POP/POPW` | `IR3_MOV rd=ACCUM, rs1=vs_reg[top]`; pop vstack | |
| `IR_ADD` … `IR_FGE` | `IR3_ALU rd=ACCUM, rs1=vs_reg[top], rs2=ACCUM`; pop vstack | result directly to r0 |
| `IR_SXB/SXW/ITOF/FTOI` | `IR3_ALU1 rd=ACCUM, rs1=ACCUM, alu_op` | in-place on r0 |
| `IR_JL sym` | flush args + `IR3_CALL sym` | see call flushing below |
| `IR_JLI` | save fp → fresh, flush args, restore → ACCUM, `IR3_CALLR` | |
| `IR_JZ L` | `IR3_JZ imm=L` | condition implicit in r0 |
| `IR_JNZ L` | `IR3_JNZ imm=L` | |
| `IR_J L` | `IR3_J imm=L` | |
| `IR_ENTER N` | `IR3_ENTER imm=N`; reset vsp | |
| `IR_ADJ N` | `IR3_ADJ imm=N`; update vsp; trigger post-call restore if pending | |
| `IR_RET` | `IR3_RET` | |
| `IR_SYMLABEL S` | `IR3_SYMLABEL sym=S`; reset all vstack / vsp / spill state | function boundary |
| `IR_LABEL L` | `IR3_LABEL imm=L`; apply any saved vsp from forward-branch table | |
| data ops | pass-through | |

**ALU result directly to ACCUM**: Binary ALU ops write their result directly to
`IR3_VREG_ACCUM` (`rd = ACCUM, rs1 = vs_reg[top], rs2 = ACCUM`). CPU4's 3-address
F1a instructions allow `rd == rs2` (both sources are read before the destination is written),
so `add r0, r1, r0` is architecturally valid. This eliminates the extra `IR3_MOV` that a
fresh-vreg-then-copy approach would require.

**ALU1 in-place on r0**: `sxb`, `sxw`, `itof`, `ftoi` are encoded as F1b / F0 instructions
that operate in-place on a single register. `braun_ssa` models this as
`IR3_ALU1 rd=ACCUM, rs1=ACCUM`; `risc_backend_emit` emits the in-place instruction using
only `rd`.

#### Call flushing (`flush_for_call_n`)

Before every `IR_JL` / `IR_JLI`, `braun_ssa` must partition the virtual stack into argument
slots (bottom of the stack, pushed by the caller) and outer expression temporaries (above
the args, live across the call). The key difference from the old `ssa.c` implementation: after
the call, outer temps are restored into **fresh vregs** (not the same vreg IDs). This ensures
no vreg's live range spans a call instruction, so `linscan_regalloc` can assign physical
registers without any call-clobber awareness.

1. **Detect the arg boundary**: scan ahead for the post-call `IR_ADJ +N` to determine
   `arg_bytes`; bottom `n_args` vstack slots are args.
2. **Spill outer temps** to memory (`IR3_STORE rd=BP, rs1=vs_reg[k], imm=vsp+k*sz`),
   preceded by `IR3_ADJ -outer_bytes` to allocate the spill area.
3. **Flush args** to the machine stack (`IR3_STORE` per arg) with `IR3_ADJ -arg_bytes`.
4. **Emit the call** (`IR3_CALL` or `IR3_CALLR`).
5. **Post-call restore** (triggered by the arg-cleanup `IR3_ADJ +arg_bytes`): reload each
   spilled temp into a **fresh vreg** via `IR3_LOAD`, update the vstack, then emit
   `IR3_ADJ +outer_bytes` to free the spill area.

For indirect calls, the function pointer lives in ACCUM and must survive `flush_for_call_n`.
It is saved to a fresh vreg before the flush and moved back to ACCUM immediately before
`IR3_CALLR`.

#### Forward-branch vsp fixup

Same mechanism as the old `lift_to_ssa`: when a branch (`IR_J/JZ/JNZ`) is emitted, the
current `vsp` is recorded in a table keyed by label id. When the target `IR_LABEL` is
processed, `vsp` is restored from the table. This prevents scope-exit `adj` instructions
from one branch arm from corrupting the sp model seen by the merge point.

---

### Pass 2: `linscan_regalloc()` (`linscan.c`)

Standard Poletto/Sarkar (1999) linear-scan allocator. Runs per function (between
`IR3_SYMLABEL` nodes). Rewrites `rd`/`rs1`/`rs2` in-place.

#### Physical register pool

`r1`–`r6` are available for allocation. `r0` is permanently reserved for `IR3_VREG_ACCUM`.
`r7` is reserved as the spill scratch register. Both are handled by a final rewrite pass.

#### Live intervals

1. **Number instructions** sequentially (0, 1, 2, …) within the function.
2. **Scan each `IR3Inst`**: `rd` records a definition; `rs1`/`rs2` record uses.
   Guard: `vreg <= IR3_VREG_ACCUM` (i.e., ≤ 100) skips ACCUM, BP, NONE, and physical regs.
3. For each vreg, `interval.start = serial of first definition`, `interval.end = serial of
   last use`.

#### Scan

1. Sort intervals by `start`.
2. For each interval, expire all active intervals whose `end < current.start` and return
   their registers to the pool.
3. `pool_alloc()` returns the next available physical register (r1 first, then r2, …, r6).
4. If the pool is empty, spill the longest-lived active interval: steal its register for the
   current interval; record a bp-relative spill slot for the displaced interval.

#### Spill insertion (Phase 5)

After register rewriting, if any vregs were spilled:
- Insert `IR3_LOAD rd=r7, rs1=BP, imm=spill_off` before each USE of a spilled vreg.
- Insert `IR3_STORE rd=BP, rs1=r7, imm=spill_off` after each DEF of a spilled vreg.
- Expand the function's `IR3_ENTER` to cover the spill area.
- Shift call-arg flush offsets (bp-relative offsets below the original ENTER) down by
  the expansion amount to preserve correct argument placement.

#### ACCUM rewrite

After the per-function scan, a final pass over the entire IR3 list rewrites every occurrence
of `IR3_VREG_ACCUM` (100) to physical register 0. This handles ACCUM in SYMLABEL/LABEL/data
nodes as well as inside function bodies.

---

### Pass 3: `ir3_lower()` (`ir3_lower.c`)

Near-1:1 translation from `IR3Inst` (physical registers in `rd`/`rs1`/`rs2`) to `SSAInst`
for `risc_backend_emit`. One `IR3Inst` → one `SSAInst` in all cases. Zero-imm `IR3_ADJ`
nodes are dropped.

| IR3Op | SSAOp | Notes |
|---|---|---|
| `IR3_CONST` (sym==NULL) | `SSA_MOVI` | |
| `IR3_CONST` (sym!=NULL) | `SSA_MOVSYM` | |
| `IR3_LOAD` | `SSA_LOAD` | rs1==-2 triggers bp-relative path in backend |
| `IR3_STORE` | `SSA_STORE` | rd==-2 triggers bp-relative path |
| `IR3_LEA` | `SSA_LEA` | |
| `IR3_ALU` | `SSA_ALU` | alu_op forwarded |
| `IR3_ALU1` | `SSA_ALU1` | alu_op forwarded |
| `IR3_MOV` | `SSA_MOV` | |
| `IR3_CALL` | `SSA_CALL` | |
| `IR3_CALLR` | `SSA_CALLR` | |
| `IR3_RET` | `SSA_RET` | |
| `IR3_J` | `SSA_J` | |
| `IR3_JZ` | `SSA_JZ` | |
| `IR3_JNZ` | `SSA_JNZ` | |
| `IR3_ENTER` | `SSA_ENTER` | |
| `IR3_ADJ` (imm != 0) | `SSA_ADJ` | zero-imm nodes dropped |
| `IR3_SYMLABEL` | `SSA_SYMLABEL` | |
| `IR3_LABEL` | `SSA_LABEL` | |
| `IR3_WORD/BYTE/ALIGN` | `SSA_WORD/BYTE/ALIGN` | |
| `IR3_PUTCHAR` | `SSA_PUTCHAR` | |
| `IR3_COMMENT` | `SSA_COMMENT` | |

---

### Pass 4: `risc_backend_emit()` (`risc_backend.c`) — unchanged

Walks the post-lower `SSAInst` list and emits CPU4 assembly text.

#### SSAInst struct

```c
typedef struct SSAInst {
    SSAOp       op;
    IROp        alu_op;   // SSA_ALU/SSA_ALU1: original IR opcode
    int         rd;       // -2 = bp-relative, -1 = none, 0–7 = physical
    int         rs1;
    int         rs2;
    int         imm;      // immediate / bp offset / label id
    int         size;     // 1/2/4 for LOAD/STORE
    const char *sym;
    int         line;
    struct SSAInst *next;
} SSAInst;
```

#### Instruction selection

| SSAOp | Condition | CPU4 emission |
|---|---|---|
| `SSA_MOVI` | imm fits in 16 bits | `immw rd, imm` |
| `SSA_MOVI` | imm > 0xffff | `immw rd, lo16; immwh rd, hi16` |
| `SSA_MOVSYM` | — | `immw rd, sym` |
| `SSA_LEA` | — | `lea rd, imm` (F3c) |
| `SSA_MOV` | rd != rs1 | `or rd, rs1, rs1` (F1a pseudo-mov) |
| `SSA_LOAD` | rs1 == -2, in F2 range | `lw/lb/ll rd, [bp+imm7]` (F2, 2 bytes) |
| `SSA_LOAD` | rs1 == -2, out of F2 range | `lea scratch, imm; llw/llb/lll rd, [scratch+0]` |
| `SSA_LOAD` | rs1 >= 0 | `llw/llb/lll rd, [rs1+imm]` (F3b, 3 bytes) |
| `SSA_STORE` | rd == -2, in F2 range | `sw/sb/sl rs1, [bp+imm7]` (F2, 2 bytes) |
| `SSA_STORE` | rd == -2, out of F2 range | `lea scratch, imm; slw/slb/sll rs1, [scratch+0]` |
| `SSA_STORE` | rd >= 0 | `slw/slb/sll rs1, [rd+imm]` (F3b, 3 bytes) |
| `SSA_ALU` | most ops | `add/sub/mul/… rd, rs1, rs2` (F1a, 2 bytes) |
| `SSA_ALU` | `le`/`ge` | `gt/lt rd, rs2, rs1; immw tmp, 0; eq rd, rd, tmp` (3 insns) |
| `SSA_ALU` | `les`/`ges` | `gts/lts rd, rs2, rs1; immw tmp, 0; eq rd, rd, tmp` |
| `SSA_ALU` | `fle`/`fge` | `fgt/flt rd, rs2, rs1; immw tmp, 0; eq rd, rd, tmp` |
| `SSA_ALU1` | `sxb`/`sxw` | `sxb/sxw rd` (F1b, 2 bytes; in-place) |
| `SSA_ALU1` | `itof`/`ftoi` | `itof`/`ftoi` (F0; r0 implicit) |
| `SSA_ENTER` | — | `enter N` (F3a) |
| `SSA_ADJ` | imm != 0 | `adjw imm` (F3a) |
| `SSA_RET` | — | `ret` (F0) |
| `SSA_CALL` | — | `jl sym` (F3a) |
| `SSA_CALLR` | — | `jlr` (F0; r0 holds target) |
| `SSA_J/JZ/JNZ` | — | `j/jz/jnz _lN` (F3a) |
| `SSA_LABEL` | — | `_lN:` |
| `SSA_SYMLABEL` | — | `sym:` |
| `SSA_WORD/BYTE/ALIGN` | — | `word`/`byte`/`align` directives |
| `SSA_PUTCHAR` | — | `putchar` (F0) |

#### F2 range checks

F2 bp-relative instructions use a scaled 7-bit signed immediate:

| Access size | Scale | F2 byte-offset range |
|---|---|---|
| byte (`lb`/`sb`) | ×1 | −64 … +63 bytes from bp |
| word (`lw`/`sw`) | ×2 | −128 … +126 bytes from bp (even offsets) |
| long (`ll`/`sl`) | ×4 | −256 … +252 bytes from bp (multiples of 4) |

Outside F2 range: `lea rd, imm` (F3c, 3 bytes) + `llw/slw rd, [rd+0]` (F3b, 3 bytes) = 6 bytes.

#### `le`/`ge` expansion

`le`/`ge` and their signed and float variants have no direct F1a encoding. When a 0/1
comparison result is needed in a register, the backend expands `le rd, rs1, rs2` as:

```asm
gt   rd, rs1, rs2    ; rd = (rs1 > rs2) ? 1 : 0
immw tmp, 0
eq   rd, rd, tmp     ; rd = (rd == 0) ? 1 : 0  ≡  !(rs1 > rs2)
```

#### Annotation (`-ann`)

`rb_emit_src_comment(line)` emits a `; source text` comment before the first instruction
from each new source line, using the `ann_lines[]` index built by `set_ann_source()` in
`smallcc.c`. Comments are suppressed at label nodes.
