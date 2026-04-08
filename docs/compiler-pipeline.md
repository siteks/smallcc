# Compiler Pipeline: Nanopass CPU4 Backend

**Status:** Implementation reference — reflects the `nanopass` branch
**Target:** CPU4 RISC-like ISA; K=8 general-purpose registers (r0–r7); sp, bp, lr special
**Driver benchmark:** CoreMark

This document describes the `nanopass` CPU4 pipeline that replaces `codegen.c` /
`backend.c` / `optimise.c` for `-arch cpu4`. The CPU3 stack-machine path is unchanged.

---

## Design Principles

**One representation per regime.** The IR changes form at two hard boundaries:
tree→graph (Braun SSA construction) and graph→virtual-register sequence (out-of-SSA).
Each representation is exactly what its regime needs.

**Type information is spent at the boundary where it affects code shape.** The full
`Type*` from the parse tree is consumed structurally during lowering. A small residual
`ValType` enum survives into the IR, carrying what the backend needs for scalar
decisions. `CallDesc` survives from lowering to emission where ABI decisions are made.
The sexp AST and all post-lowering passes are type-unaware.

**Incremental testability.** Each stage is independently runnable: `-ssa` dumps Braun
output; `DUMP_IR=1` dumps post-OOS and post-IRC IR.

---

## Pipeline Overview

```
Node* parse tree  (output of resolve_symbols / derive_types / insert_coercions)
  │
  ├─ lower_program()        lower.c      Node* → Sexp AST + TypeMap
  │   ├─ alpha-rename variables (Symbol* → "name@N")
  │   ├─ lower ++/-- (in lowering, not a separate pass)
  │   ├─ desugar switch (in lower.c, not a separate Regime 1 pass)
  │   ├─ handle static locals (emit as data-section gvars)
  │   └─ populate TypeMap with ValType + CallDesc
  │
  ├─ emit_globals()         emit.c       data section: gvars + string literals
  │
  └─ per function:
      ├─ braun_function()   braun.c      Sexp → SSA IR (Braun 2013)
      │   └─ handles for/do/while/goto/label/switch natively
      │
      ├─ compute_dominators() dom.c      idom, dom_children, dom_pre/post, loop_depth
      │
      ├─ out_of_ssa()       oos.c        eliminate φ-nodes (Boissinot 2009 parallel copies)
      │
      ├─ irc_allocate()     alloc.c      IRC register allocation (Appel & George 1996)
      │                                  vregs → physical r0–r7; spills if needed
      │
      └─ emit_function()    emit.c       physical-reg IR → CPU4 assembly text
```

**Regime 1 (Sexp tree) optimisation passes are not yet implemented.** The pipeline goes
directly from lowering to Braun SSA construction.

**Regime 2 (SSA graph) optimisation passes are not yet implemented.** The pipeline goes
directly from Braun → dom → OOS → IRC.

---

## Debug Flags

| Mechanism | What it shows |
|-----------|--------------|
| `-ssa` flag | Braun SSA IR for each function, printed to stderr after `braun_function` |
| `DUMP_IR=1` env var | Post-OOS IR and post-IRC IR for each function, printed to stderr |

---

## Type Artifacts

### `Type*` — Parse tree only

The full type system object from the parser. Rich structure: pointer chains, array
dimensions, struct layouts, function signatures. **Consumed entirely during lowering.**
Does not cross into the sexp AST or IR.

Consumed to produce:
- Explicit scale factor integers in pointer arithmetic sexp nodes
- Explicit byte offset integers in struct member access sexp nodes
- `(gaddr "name")` nodes for global variables
- `(load N e)` / `(store N addr val)` nodes (N = byte size)
- Hidden first-parameter pattern for struct-returning functions
- `CallDesc` attached to call nodes in the TypeMap
- `ValType` annotation in the TypeMap

### `ValType` — IR values (Braun through emission)

```c
typedef enum {
    VT_VOID,
    VT_I8, VT_I16, VT_I32,   // signed integers
    VT_U8, VT_U16, VT_U32,   // unsigned integers
    VT_PTR,                    // pointer (16-bit address space)
    VT_F32,                    // single-precision float
} ValType;
```

Used for:
- Signed vs unsigned comparison instruction selection (`lts` vs `lt`)
- Arithmetic vs logical right shift (`shrs` vs `shr`)
- Spill slot sizing (byte / word / long)
- Float vs integer arithmetic path selection

### `CallDesc` — Call nodes (lowering through emission)

```c
typedef struct {
    Type    *return_type;
    int      nparams;
    Type   **param_types;
    bool     is_variadic;
    bool     hidden_sret;  // struct-return: caller allocates buffer + passes pointer
} CallDesc;
```

Survives from lowering through IRC to emission. Opaque to all intermediate passes.

### `TypeMap` — Hash table `uint32_t id → {ValType, CallDesc*}`

Keyed on `Sx.id` (not pointer — Sx nodes may be structurally rebuilt). Populated during
`lower_program`, consumed during `braun_function`, freed afterwards.

---

## Lowering Pass (`lower.c`)

**Entry:** `lower_program(Node *root, TypeMap *tm, int tu_index, int *strlit_id) → Sx*`

`strlit_id` is a persistent counter threaded across TUs to give string literals
monotonically increasing unique IDs (`_l0`, `_l1`, …) without collisions.

**Output:** A `(program ...)` sexp whose children are gvar/strlit/func nodes, plus a
populated TypeMap.

### Alpha-renaming

Every `Symbol*` is assigned a sequential integer id during lowering. Two identifiers
with the same C name but different `Symbol*` pointers get different sexp names:
`"x@N"` where N is the symbol id. Original name prefix is preserved for readability.

### Sexp nodes produced

**Literals:**

| Node | Shape | Notes |
|------|-------|-------|
| `int` | `(int v)` | integer literal |
| `flt` | `(flt bits)` | float literal; bits = IEEE 754 bit pattern as int |

**Variable access:**

| Node | Shape | Notes |
|------|-------|-------|
| `var` | `(var "x@N")` | local/param variable reference |
| `addr` | `(addr "x@N")` | address of local/param variable |
| `gaddr` | `(gaddr "label")` | address of global symbol (function or global variable) |

**Expressions:**

| Node | Shape | Notes |
|------|-------|-------|
| `binop` | `(binop "op" lhs rhs)` | op: `+ - * / % & \| ^ << >> < <= > >= == != u< u<= u> u>=` |
| `unop` | `(unop "op" expr)` | op: `- ~ !` |
| `assign` | `(assign "x@N" rval)` | write to local/param variable |
| `load` | `(load size ptr)` | load N bytes from pointer; size: 1/2/4 |
| `store` | `(store size ptr val)` | store N bytes through pointer; size: 1/2/4 |
| `call` | `(call "fname" args...)` | direct function call |
| `icall` | `(icall fptr args...)` | indirect call through function pointer |
| `sext8` | `(sext8 expr)` | sign-extend from 8 bits |
| `sext16` | `(sext16 expr)` | sign-extend from 16 bits |
| `itof` | `(itof expr)` | int-to-float conversion |
| `ftoi` | `(ftoi expr)` | float-to-int conversion |

**Statements:**

| Node | Shape | Notes |
|------|-------|-------|
| `decl` | `(decl "x@N")` | declare local variable (storage allocated by frame layout) |
| `block` | `(block stmt...)` | sequence of statements |
| `if` | `(if cond then else?)` | else child is optional |
| `while` | `(while cond body)` | |
| `for` | `(for init cond step body)` | NOT desugared; handled natively in Braun |
| `do` | `(do body cond)` | NOT desugared; handled natively in Braun |
| `switch` | `(switch expr (case (int v) stmt...) ... (default stmt...))` | desugared in lower.c |
| `return` | `(return expr?)` | expr omitted for void return |
| `break` | `(break)` | |
| `continue` | `(continue)` | |
| `goto` | `(goto "label")` | |
| `label` | `(label "name" stmt)` | |

**Top-level:**

| Node | Shape | Notes |
|------|-------|-------|
| `func` | `(func "name" frame_size is_variadic (params "p@N"...) body)` | is_variadic: 0 or 1 |
| `gvar` | `(gvar "label" size init_bytes...)` | global variable |
| `strlit` | `(strlit "_lN" bytes...)` | string literal data |

### `++`/`--` handling

Pre/post increment and decrement are handled in the lowering pass with correct semantics:
- Pre-`++x`: compute `x+1`, store back, return new value
- Post-`x++`: save old value, compute `x+1`, store back, return old value
- For pointer operands, the step is scaled by the element size
- LHS address is computed once (no double-evaluation)

### `switch` desugaring

`switch` is desugared during lowering (not as a separate Regime 1 pass) into:
```
(block
  (decl "_sw@N")
  (assign "_sw@N" expr)
  (if (binop "==" (var "_sw@N") (int v1)) (goto "_case1@N") ...)
  (goto "_default@N")
  (label "_case1@N" stmt...)
  (label "_default@N" stmt...)
  (label "_swend@N"))
```
`(break)` inside switch body → `(goto "_swend@N")`.

### Static local variables

`SYM_STATIC_LOCAL` declarations inside functions are lowered as `gvar` data-section
nodes (not stack locals) and appended after the enclosing function in the sexp output.
They are initialized at program start (not at function entry).

### Struct-returning functions

Functions returning struct types use a hidden first parameter (sret pointer):
- Callee receives `"sret@N"` as first param; writes return value through it
- Callsite allocates a stack temp, passes its address as first arg
- `CallDesc.hidden_sret = true` records this for emission

---

## Braun SSA Construction (`braun.c`)

**Entry:** `braun_function(Sx *func_node, TypeMap *tm) → Function*`

**Algorithm:** Braun et al. 2013. `write_var(b, name, val)` / `read_var(b, name)` /
`read_var_recursive` / `seal_block` / `try_remove_trivial_phi`.

### Sexp → IR mapping

| Sexp | IR emitted |
|------|-----------|
| `(int v)` / `(flt bits)` | `IK_CONST dst, v` |
| `(var "x@N")` | `read_var(b, "x@N")` |
| `(addr "x@N")` | `IK_ADDR dst, frame_slot` |
| `(gaddr "label")` | `IK_GADDR dst, "label"` |
| `(binop "+" a b)` | `IK_ADD dst, va, vb` |
| `(load N ptr)` | `IK_LOAD dst, ptr_val, size=N` |
| `(store N ptr val)` | `IK_STORE ptr_val, val_val, size=N` |
| `(assign "x@N" rhs)` | `write_var(b, "x@N", cg_expr(rhs))` |
| `(call "f" args...)` | `IK_CALL landing → IK_COPY working` (landing pre-colored r0) |
| `(icall fp args...)` | `IK_ICALL landing → IK_COPY working` |
| `(if cond then else)` | `IK_BR cond_val → then_blk / else_blk` |
| `(while cond body)` | header (unsealed) → body → seal; `IK_JMP` back-edge |
| `(for init cond step body)` | entry; cond_blk (unsealed); body; step; seal |
| `(do body cond)` | body_blk; cond; `IK_BR` back-edge / exit |
| `(return expr)` | `IK_RET cg_expr(expr)` |
| `(break)` | `IK_JMP brk_target` |
| `(continue)` | `IK_JMP cont_target` |
| `(goto "label")` | `IK_JMP named_block` |
| `(label "name" stmt)` | create/retrieve named block; cg_stmt |

**Call result landing+copy pattern:** To avoid IRC conflicts between call results
(pre-colored r0) and subsequent branch conditions (also using r0), every call emits:
1. `IK_CALL landing` — landing value pre-colored to r0
2. `IK_COPY working ← landing` — working value freely allocated by IRC

Post-lowering code uses `working`, allowing IRC to assign it any register.

**Parameters:** Each parameter is emitted as `IK_PARAM i` in the entry block;
`write_var(entry, "p@N", param_val)` records it for Braun reads.

**Type annotation:** After emitting each value, `braun_function` queries the TypeMap
for the corresponding sexp node's `ValType` and sets `val->vtype`. For calls,
`CallDesc` is transferred from TypeMap to `inst->calldesc`.

---

## Dominator Analysis (`dom.c`)

**Entry:** `compute_dominators(Function *f)`

**Algorithm:** Cooper, Harvey & Kennedy 2001 iterative post-dominator.

**Output:**
- `Block.idom` — immediate dominator
- `Block.dom_children` / `Block.ndom_children` — dominator tree children
- `Block.dom_pre` / `Block.dom_post` — DFS pre/post timestamps for O(1) dominance queries
- `Block.rpo_index` — reverse-postorder index
- `Block.loop_depth` — nesting depth (0 = no loop; 1 = one enclosing loop; etc.)

**Query:** `dominates(a, b)` iff `a.dom_pre <= b.dom_pre && b.dom_post <= a.dom_post`

---

## Out-of-SSA (`oos.c`)

**Entry:** `out_of_ssa(Function *f)`

**Algorithm:** Boissinot et al. 2009 parallel-copy insertion.

For each `IK_PHI v = phi(v1@B1, v2@B2, ...)`:
- Insert `IK_COPY v ← vi` at end of each predecessor `Bi` (before terminator)
- Detect swap cycles (where copies on one edge form a permutation); break with a temp

After insertion: all `IK_PHI` instructions are removed.

---

## SSA IR Data Structures (`ssa.h`)

### `InstKind` opcodes

| Category | Opcodes |
|----------|---------|
| Values | `IK_CONST IK_COPY IK_PHI IK_PARAM` |
| Integer ALU | `IK_ADD IK_SUB IK_MUL IK_DIV IK_UDIV IK_MOD IK_UMOD` |
| Shift | `IK_SHL IK_SHR IK_USHR` |
| Bitwise | `IK_AND IK_OR IK_XOR IK_NEG IK_NOT` |
| Compare (signed) | `IK_LT IK_LE IK_EQ IK_NE` |
| Compare (unsigned) | `IK_ULT IK_ULE` |
| Float ALU | `IK_FADD IK_FSUB IK_FMUL IK_FDIV` |
| Float compare | `IK_FLT IK_FLE IK_FEQ IK_FNE` |
| Conversions | `IK_ITOF IK_FTOI IK_SEXT8 IK_SEXT16 IK_ZEXT IK_TRUNC` |
| Memory | `IK_LOAD IK_STORE IK_ADDR IK_GADDR IK_MEMCPY` |
| Calls | `IK_CALL IK_ICALL IK_PUTCHAR` |
| Control flow | `IK_BR IK_JMP IK_RET` |

`IK_LT` / `IK_LE` are signed; `IK_ULT` / `IK_ULE` are unsigned. Emission chooses
`lts`/`les` vs `lt`/`le` by inspecting `Value.vtype` of the operands.

### Key struct fields

```c
struct Value {
    ValKind  kind;       // VAL_CONST | VAL_INST | VAL_UNDEF
    int      id;         // unique within Function
    int      iconst;     // VAL_CONST: constant value
    Inst    *def;        // VAL_INST: defining instruction
    Value   *alias;      // trivial-φ forwarding (val_resolve() chases)
    ValType  vtype;
    int      phys_reg;   // -1 until IRC assigns (0-7)
    int      spill_slot; // -1 unless spilled
    int      use_count;
};

struct Inst {
    InstKind  kind;
    Value    *dst;       // NULL for void (IK_STORE, IK_RET void, IK_BR, IK_JMP)
    Value   **ops;       int nops;
    int       imm;       // IK_LOAD/IK_STORE offset; IK_CONST value
    int       size;      // IK_LOAD/IK_STORE/IK_MEMCPY byte count
    int       param_idx; // IK_PARAM
    Block    *target;    // IK_BR true / IK_JMP
    Block    *target2;   // IK_BR false
    char     *fname;     // IK_CALL / IK_GADDR
    char     *label;     // IK_JMP to named label (goto)
    CallDesc *calldesc;  // IK_CALL / IK_ICALL
    Inst     *prev, *next;
    Block    *block;
    int       is_dead;
};

struct Block {
    int     id;
    char   *label;          // non-NULL only for named labels (goto targets)
    Inst   *head, *tail;
    Block **preds;  int npreds;
    Block **succs;  int nsuccs;
    int     sealed, filled;
    // Braun maps (fixed-size arrays; valid only during construction):
    char   *def_names[256];  Value *def_vals[256];  int ndef;
    Inst   *iphi_insts[256]; char  *iphi_names[256]; int niphi;
    // Dominator analysis:
    int     loop_depth, rpo_index;
    Block  *idom;
    Block **dom_children; int ndom_children;
    int     dom_pre, dom_post;
    // Liveness (valid during IRC):
    int      nwords;
    uint32_t *live_in, *live_out;
};

struct Function {
    char    *name;
    Block  **blocks;  int nblocks;
    Value  **values;  int nvalues;
    Value  **params;  int nparams;
    int      next_val_id, next_blk_id;
    int      frame_size;
    bool     is_variadic;
};
```

---

## IRC Register Allocation (`alloc.c`)

**Entry:** `irc_allocate(Function *f)`

**Algorithm:** Appel & George 1996, Iterated Register Coalescing.

**K = 8:** r0–r7 are allocatable. r0 is pre-colored for `IK_CALL`/`IK_ICALL` landing
values and `IK_PARAM 0`.

**ABI constraints:**
- r0–r3: caller-saved → interfere with all live values at call sites
- r4–r7: callee-saved → no call-site interference; saves/restores inserted by emission

**Phases:**
```
liveness analysis (backward dataflow, bitvector per block)
    ↓
build interference graph
    ↓
repeat until stable:
    ├─ simplify   (remove non-move-related nodes of degree < K)
    ├─ coalesce   (merge IK_COPY pairs; George criterion)
    ├─ freeze     (give up coalescing for low-degree move-related nodes)
    └─ spill      (select by spill_cost = use_count / (1 + loop_depth))
    ↓
select (assign colors; actual spills insert loads/stores and restart)
```

**Output:** `Value.phys_reg` set for all live values (0–7). Coalesced `IK_COPY`
instructions removed. Spill loads/stores inserted. `Value.spill_slot` set for spilled
values.

---

## Emission (`emit.c`)

**Entries:**
- `emit_globals(Sx *program, FILE *out)` — data section: `gvar` and `strlit` nodes
- `emit_function(Function *f, FILE *out)` — text section: CPU4 assembly for one function

### Data section

String literals: `_lN: byte b0 b1 ... 0`
Global variables: `_label: word/long/byte initial_value` (or `allocb N` for zero-filled)

### Frame layout

```
bp+8+2*(n-1)   param n (last)
    ...
bp+8           param 0 (first)
bp+4           return address
bp+0           saved bp          ← enter sets bp here
bp-K           callee-saved reg save area (r4–r7 as needed)
bp-K-M         spill slots
```

`enter N` is emitted with N = frame_size (from Function.frame_size, expanded by IRC
spill rewriting). `adjw` adjusts sp for scope-allocated locals.

### Instruction selection

| IR | CPU4 |
|----|------|
| `IK_CONST rd, k` (k≤16b) | `immw rd, k` |
| `IK_CONST rd, k` (k>16b) | `immw rd, lo16; immwh rd, hi16` |
| `IK_GADDR rd, "sym"` | `immw rd, sym` |
| `IK_ADDR rd, slot` | `lea rd, offset` |
| `IK_ADD rd, ra, rb` | `add rd, ra, rb` |
| `IK_SUB rd, ra, rb` | `sub rd, ra, rb` |
| `IK_MUL rd, ra, rb` | `mul rd, ra, rb` |
| `IK_DIV rd, ra, rb` | `divs rd, ra, rb` |
| `IK_UDIV rd, ra, rb` | `div rd, ra, rb` |
| `IK_LT rd, ra, rb` | `lts rd, ra, rb` |
| `IK_ULT rd, ra, rb` | `lt rd, ra, rb` |
| `IK_SEXT8 rd, ra` | `sxb rd` |
| `IK_SEXT16 rd, ra` | `sxw rd` |
| `IK_ITOF` | `itof` |
| `IK_FTOI` | `ftoi` |
| `IK_LOAD rd, [ra+0]` (bp-rel, in F2 range) | `lw/lb/ll rd, [bp+imm]` |
| `IK_LOAD rd, [ra+0]` (other) | `llw/llb/lll rd, [ra+0]` |
| `IK_STORE [ra+0], rb` (bp-rel, in F2 range) | `sw/sb/sl rb, [bp+imm]` |
| `IK_STORE [ra+0], rb` (other) | `slw/slb/sll rb, [ra+0]` |
| `IK_COPY rd, rs` | `or rd, rs, rs` (mov pseudo) |
| `IK_MEMCPY dst, src, n` | inlined byte loop or call to memcpy |
| `IK_CALL "fname"` | `jl fname` |
| `IK_ICALL fp` | `jlr` (fp in r0) |
| `IK_BR cond, T, F` | `jnz T` + fall-through to F |
| `IK_JMP target` | `j target` |
| `IK_RET v` | move v to r0 (if not already); `ret` |
| `IK_PUTCHAR v` | move v to r0; `putchar` |

Block labels: `_{funcname}_BN:` (function-scoped to avoid cross-function collisions).

Callee-saved registers (r4–r7): emit stores to frame immediately after `enter`, loads
immediately before each `ret`. Uses bp-relative F2 instructions when in range.

Float operations (`IK_FADD` etc.) are emitted as CPU4 float instructions (`fadd`, etc.)
or lowered to libcalls if needed. The target ISA has native float instructions.

---

## Pass Summary (Implemented)

| Pass | File | Status | Notes |
|------|------|--------|-------|
| **lowering** | `lower.c` | ✅ | Node* → Sexp + TypeMap; handles ++/--, switch, static locals |
| **emit_globals** | `emit.c` | ✅ | Data section emission |
| **Braun SSA** | `braun.c` | ✅ | Braun 2013; for/do/while/goto native; call landing+copy |
| **dominator tree** | `dom.c` | ✅ | Cooper 2001; loop_depth |
| **out-of-SSA** | `oos.c` | ✅ | Boissinot 2009 parallel copies |
| **IRC** | `alloc.c` | ✅ | Appel & George 1996; K=8; spill support |
| **emission** | `emit.c` | ✅ | CPU4 assembly; F2 bp-rel selection; callee-save handling |
| Regime 1 optimisations | — | ❌ | const-fold, CSE etc. — not yet implemented |
| Regime 2 optimisations | — | ❌ | SCCP, DCE, LICM, GVN — not yet implemented |

---

## What Is Deliberately Absent

**Regime 1 passes.** Constant folding, CSE, strength reduction etc. as sexp-tree rewrites.
Will be implemented as plain C recursive tree-walker functions (not a DSL/pattern engine).

**Regime 2 passes.** SCCP, DCE, LICM, GVN, SSA peephole. The pipeline is currently
-O0 only; optimisation passes will be added incrementally.

**`long long` / 64-bit arithmetic.** Not required by C89 on this 32-bit target.

**`setjmp`/`longjmp`.** Requires special stack frame treatment. Not implemented.

**Jump tables for switch.** Switch is currently lowered to comparison chains.
Jump table optimisation is future work.

---

## Implementation Notes

**File map:**

| File | Role |
|------|------|
| `sx.h` / `sx.c` | Sexp AST types + constructors + printer + TypeMap |
| `lower.h` / `lower.c` | Lowering pass: Node* → Sexp |
| `ssa.h` / `ssa.c` | SSA IR types + constructors + printer |
| `braun.h` / `braun.c` | Braun SSA construction |
| `dom.h` / `dom.c` | Dominator tree + loop depth |
| `oos.h` / `oos.c` | Out-of-SSA (Boissinot) |
| `alloc.h` / `alloc.c` | Liveness + IRC |
| `emit.h` / `emit.c` | CPU4 emission (globals + functions) |

**Alpha-renaming:** `Symbol*` → `"name@N"` where N is a sequential integer id assigned
per `Symbol*` during lowering. Original name prefix preserved for IR dump readability.

**String literal IDs:** `int *strlit_id` is threaded through `lower_program` across all
TUs and persists in `smallcc.c` as `cpu4_strlit_id`. This ensures `_l0`, `_l1`, … are
globally unique and never collide with lib TU string literals.

**Block labels in emission:** `_{funcname}_BN` prefix avoids cross-function label
collisions when multiple functions are emitted to the same assembly file.

**Call result pre-coloring:** `IK_CALL`/`IK_ICALL` landing values are pre-colored to r0.
An immediate `IK_COPY working ← landing` gives IRC a freely-allocatable working value,
avoiding conflicts between the call result and the next branch condition (which also
uses r0 as its condition register).
