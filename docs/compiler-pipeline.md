# Compiler Pipeline: Nanopass CPU4 Backend

**Status:** Implementation reference — reflects the `nanopass` branch
**Target:** CPU4 RISC-like ISA; K=8 general-purpose registers (r0–r7); sp, bp, lr special
**Driver benchmark:** CoreMark

This document describes the `nanopass` CPU4 pipeline. It is the compiler's sole backend;
the CPU3 stack-machine backend (`codegen.c` / `backend.c` / `optimise.c`) has been removed.

---

## Design Principles

**One representation per regime.** The IR changes form at two hard boundaries:
tree→graph (Braun SSA construction) and graph→virtual-register sequence (out-of-SSA).
Each representation is exactly what its regime needs.

**Type information is spent at the boundary where it affects code shape.** `Type*` from
the parse tree flows into Braun SSA construction, where `type_to_valtype()` extracts the
`ValType` for each value and `make_calldesc()` builds `CallDesc` for each call site.
A small residual `ValType` enum survives into the IR, carrying what the backend needs for
scalar decisions. `CallDesc` survives through IRC to emission where ABI decisions are made.
All post-Braun passes are type-unaware.

**Incremental testability.** Each stage is independently runnable: `-ssa` dumps Braun
output; `DUMP_IR=1` dumps post-OOS and post-IRC IR.

---

## Pipeline Overview

```
Node* parse tree  (output of resolve_symbols / derive_types / insert_coercions)
  │
  ├─ lower_globals()        lower.c      Node* → Sexp AST (data section only)
  │   └─ global variables and pre-function string literals → gvar/strlit sexp nodes
  │
  ├─ emit_globals()         emit.c       data section: gvars + string literals
  │
  └─ per function:
      ├─ braun_function()   braun.c      Node* → SSA IR directly (Braun 2013)
      │   ├─ walks Node* AST; variable maps keyed on Symbol* (no alpha-renaming)
      │   ├─ derives ValType via type_to_valtype(); builds CallDesc via make_calldesc()
      │   ├─ handles all statement kinds natively (if/for/while/do/switch/goto/label)
      │   ├─ handles ++/-- with correct pre/post semantics and pointer stride scaling
      │   └─ accumulates function-body string literals and static locals
      │       (flushed to data section via braun_emit_strlits() after each function)
      │
      ├─ compute_dominators() dom.c      idom, dom_children, dom_pre/post, loop_depth
      │
      │  ── Pre-OOS passes (true SSA form, phis still present) ──────────────────
      │
      ├─ opt_redundant_bool() opt.c      R2G: eliminate NE(cmp, 0) → cmp
      ├─ opt_narrow_loads()   opt.c      R2H: AND(LOAD, mask) → narrower LOAD
      ├─ opt_known_bits()     opt.c      R2K: known-bits simplification
      ├─ opt_bitwise_dist()   opt.c      R2L: bitwise distribution
      ├─ opt_pre_oos_cse()    opt.c      GVN on true SSA form (relaxed cross-block policy)
      ├─ opt_scalar_promote() opt.c      hoist load-modify-store to register accumulator phi
      ├─ opt_addr_iv()        opt.c      address induction variables
      ├─ opt_lsr()            opt.c      loop strength reduction (iv*invariant → ADD chain)
      │
      ├─ out_of_ssa()       oos.c        eliminate φ-nodes (Boissinot 2009 parallel copies)
      │
      │  ── Post-OOS passes (run_post_oos_pipeline, profile-parameterised) ──────
      │
      ├─ opt_fold_branches()  opt.c      R2A: fold IK_BR with VAL_CONST condition → IK_JMP
      ├─ opt_remove_dead_blocks() opt.c  R2B: remove blocks with npreds==0 (iterates to fixpoint)
      ├─ opt_copy_prop()      opt.c      R2D: collapse IK_COPY chains; alias single-def copies
      ├─ opt_cse()            opt.c      R2E: hash-based CSE (dominator-tree scoped GVN)
      ├─ opt_licm_const()     opt.c      R2F: hoist IK_CONST out of loop bodies
      ├─ opt_licm()           opt.c      R2F: hoist loop-invariant pure instructions
      ├─ opt_jump_thread()    opt.c      R2I: thread jumps through thin blocks
      ├─ opt_unroll_loops()   opt.c      R2J: MVE self-loop unrolling
      ├─ opt_copy_prop()      opt.c      R2D: second pass (clean up after LICM/unroll)
      │
      ├─ compute_dominators() dom.c      rebuild for legalize/IRC
      ├─ legalize_function() legalize.c  ISA/ABI lowering + post-OOS folding (see Legalize section)
      │   ├─ Pass A: pre-color IK_PARAM landing values (r1/r2/r3)
      │   ├─ Pass B: insert pre-colored IK_COPY for call args (r1/r2/r3) and IK_ICALL fp (r0)
      │   ├─ Pass C: lower IK_NEG/IK_NOT → IK_SUB(0,src)/IK_EQ(src,0) + explicit IK_CONST(0)
      │   ├─ Pass D: lower IK_ZEXT/IK_TRUNC → IK_CONST(mask) + IK_AND
      │   │           fast-path: TRUNC(VAL_CONST(k)) → IK_CONST(k & 0xff) for 8-bit truncation
      │   └─ Pass E: AND-chain constant folding — AND(AND(x,c1),c2) → AND(x,c1&c2);
      │               also looks through type-coercing IK_COPY chains
      │
      ├─ irc_allocate()     alloc.c      IRC register allocation (Appel & George 1996)
      │                                  vregs → physical r0–r7; spills if needed
      │
      └─ emit_function()    emit.c       physical-reg IR → CPU4 assembly text
                                         P1: branch-to-next elimination
                                         P2–P4: inc/dec, addi, addli compact encodings
                                         P5: compare+branch fusion (F3d)
                                         P16: SHR+AND → bitex fusion (F3e)
                                         P17: EQ/NE(x, const8) → cbeq/cbne (F0b)
```

**Regime 1 (Sexp tree) optimisation passes are not implemented.** The pipeline goes
directly from lowering to Braun SSA construction. Constant folding, strength reduction,
and algebraic simplification run during Braun SSA construction in `emit_binop` (R2C/R1B).

**Pre-OOS passes** run on true SSA form (phis still present) between `compute_dominators`
and `out_of_ssa`. Pattern simplification (R2G/R2H/R2K/R2L) feeds cleaner IR into GVN.
Pre-OOS GVN uses a relaxed cross-block policy (any dominating block, not just same-block).
Loop transforms (scalar promotion, address IVs, strength reduction) exploit the explicit
phi structure.

**Post-OOS passes** (R2A–R2J) run after `out_of_ssa`, before legalize. These are
encapsulated in `run_post_oos_pipeline()` and parameterised by the active `OptProfile`
(see Speculative Optimisation). The pass ordering is fixed:
fold branches → dead blocks → copy prop → CSE → LICM const → LICM general →
jump threading → unroll → copy prop (cleanup) → dominators → legalize → IRC.

---

## Debug Flags

| Mechanism | What it shows |
|-----------|--------------|
| `-ssa file` | Braun SSA IR for each function, written to `file` after `braun_function` |
| `-oos file` | Post-OOS IR for each function, written to `file` after `out_of_ssa` |
| `-irc file` | Post-IRC IR for each function, written to `file` after `irc_allocate` |
| `-speculative` | Try both optimisation profiles per function, pick the cheaper result (see Speculative Optimisation) |
| `DUMP_IR=1` env var | Post-OOS IR and post-IRC IR for each function, printed to stderr. With `-speculative`, also prints which profile was chosen per function |
| `DEBUG_SCORE=1` env var | Per-block scoring breakdown during speculative compilation |

---

## Type Artifacts

### `Type*` — Parse tree through Braun SSA construction

The full type system object from the parser. Rich structure: pointer chains, array
dimensions, struct layouts, function signatures. Flows from the parse tree into `braun.c`
where it is consumed to produce:
- `ValType` for each SSA value via `type_to_valtype(node->type)`
- `CallDesc` for each call site via `make_calldesc(fn_type)`
- Load/store sizes from `type->size`
- Pointer arithmetic stride from `elem_type->size`
- Struct member offsets from field layout
- Hidden first-parameter pattern for struct-returning functions

Does not cross into the post-Braun passes (OOS, legalize, IRC, emission).

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

### `CallDesc` — Call nodes (Braun through emission)

```c
typedef struct {
    Type    *return_type;
    int      nparams;
    Type   **param_types;
    bool     is_variadic;
    bool     hidden_sret;  // struct-return: caller allocates buffer + passes pointer
} CallDesc;
```

Built inline in `braun.c` via `make_calldesc(fn_type)` at each call site. Attached to
`Inst.calldesc` and survives through IRC to emission. Opaque to all intermediate passes.

---

## Global Lowering Pass (`lower.c`)

**Entry:** `lower_globals(Node *root, int tu_index, int *strlit_id) → Sx*`

`strlit_id` is a persistent counter threaded across TUs to give string literals
monotonically increasing unique IDs (`_l0`, `_l1`, …) without collisions.

**Output:** A `(program ...)` sexp whose children are `gvar` and `strlit` nodes for the
data section only. Function bodies are compiled directly by `braun_function()`.

### Sexp nodes produced

| Node | Shape | Notes |
|------|-------|-------|
| `gvar` | `(gvar "label" size init_bytes...)` | global variable |
| `strlit` | `(strlit "_lN" bytes...)` | top-level string literal (in global initializers) |

String literals that appear inside function bodies are accumulated separately by
`braun.c` and flushed via `braun_emit_strlits()` after each function is compiled.

### Struct-returning functions

Functions returning struct types use a hidden first parameter (sret pointer):
- Callee receives the sret param as its first argument; writes the return value through it
- Callsite allocates a stack temp, passes its address as the first arg
- `CallDesc.hidden_sret = true` records this for emission

---

## Braun SSA Construction (`braun.c`)

**Entry:** `braun_function(Node *func_decl, int tu_index, int *strlit_id) → Function*`

**Algorithm:** Braun et al. 2013. `write_var(b, sym, val)` / `read_var(b, sym)` /
`read_var_recursive` / `seal_block` / `try_remove_trivial_phi`.

Variable maps are keyed on `Symbol*` directly — no alpha-renaming is needed since each
`Symbol*` is already unique. `ValType` is derived per-value via
`type_to_valtype(node->type)`. `CallDesc` is built inline via `make_calldesc(fn_type)`
and attached to `Inst.calldesc` at each call site.

### Node* → IR mapping

**Expressions:**

| Node kind | IR emitted |
|-----------|-----------|
| `ND_LITERAL` (int/float) | `IK_CONST dst, v` |
| `ND_LITERAL` (string) | `IK_GADDR dst, "_lN"` + strlit accumulated |
| `ND_IDENT` (local/param) | `read_var(b, sym)` |
| `ND_IDENT` (addr-taken local) | `IK_ADDR + IK_LOAD` |
| `ND_IDENT` (global) | `IK_GADDR + IK_LOAD` |
| `ND_IDENT` (function call) | `IK_CALL landing → IK_COPY working` |
| `ND_IDENT` (indirect call via fn-ptr) | `IK_ICALL landing → IK_COPY working` |
| `ND_BINOP` | `IK_ADD/IK_SUB/IK_MUL/IK_DIV/IK_LT/IK_EQ/…` |
| `ND_UNARYOP "-"` | `IK_NEG` |
| `ND_UNARYOP "~"` | `IK_NOT` |
| `ND_UNARYOP "!"` | `IK_EQ(v, IK_CONST(0))` |
| `ND_UNARYOP "++" / "--"` (pre) | read-modify-write via `IK_ADD`/`IK_SUB` |
| `ND_UNARYOP "++" / "--"` (post) | save old; read-modify-write; return old |
| `ND_UNARYOP "*"` | `IK_LOAD` |
| `ND_UNARYOP "&"` | `IK_ADDR` / `IK_GADDR` |
| `ND_ASSIGN` (scalar) | `write_var` or `IK_STORE` (for addr-taken / global) |
| `ND_ASSIGN` (struct) | `IK_MEMCPY` |
| `ND_COMPOUND_ASSIGN` | read-modify-write via `cg_rmw` |
| `ND_CAST` | `IK_SEXT8` / `IK_SEXT16` / `IK_ZEXT` / `IK_TRUNC` / `IK_ITOF` / `IK_FTOI` |
| `ND_MEMBER "." / "->"` | `IK_LOAD` at computed byte offset |
| `ND_TERNARY` | `IK_BR` + merge block |
| `ND_VA_START` | `IK_ADDR` + `IK_ADD` → write ap |
| `ND_VA_ARG` | `IK_LOAD`; advance ap |

**Statements:**

| Node kind | IR emitted |
|-----------|-----------|
| `ND_IFSTMT` | `IK_BR cond → then_blk / else_blk` |
| `ND_WHILESTMT` | header (unsealed) → body → seal; `IK_JMP` back-edge |
| `ND_FORSTMT` | entry; cond_blk (unsealed); body; step; seal |
| `ND_DOWHILESTMT` | body_blk; cond; `IK_BR` back-edge / exit |
| `ND_SWITCHSTMT` | comparison chain with `IK_BR`/`IK_JMP` (native, no desugaring) |
| `ND_RETURNSTMT` | `IK_RET cg_expr(expr)` |
| `ND_BREAKSTMT` | `IK_JMP brk_target` |
| `ND_CONTINUESTMT` | `IK_JMP cont_target` |
| `ND_GOTOSTMT` | `IK_JMP named_block` |
| `ND_LABELSTMT` | create/retrieve named block; `cg_stmt` |
| `ND_DECLARATION` (static local) | data-section entry accumulated; frame slot allocated |

**Call result landing+copy pattern:** Every call emits:
1. `IK_CALL/IK_ICALL landing` — landing value pre-colored to r0
2. `IK_COPY working ← landing` — working value freely allocated by IRC

Post-call code uses `working`, allowing IRC to assign it any register.

**Parameters:** Register params (first 3) are emitted as `IK_PARAM i`; legalize Pass A
pre-colors them to r1–r3. Stack params (4th and beyond) are emitted as `IK_LOAD`s from
bp-relative frame slots. Address-taken register params are additionally homed to a frame
slot via `IK_ADDR + IK_STORE`; subsequent reads come from memory, not an SSA value.

**String literals and static locals** in function bodies are accumulated into internal
lists and flushed to the data section via `braun_emit_strlits(out)` after each function.

### R2C — Algebraic simplification (in `emit_binop`)

`emit_binop` folds both const+const and one-const cases during SSA construction:

- **Const-const folding:** `IK_ADD/SUB/MUL/DIV/…(VAL_CONST, VAL_CONST)` → `VAL_CONST`
  via `fold_binop`. No instruction is emitted.
- **Identity/zero rules (rhs-const):**
  - `x + 0`, `x - 0`, `x << 0`, `x >> 0`, `x | 0`, `x ^ 0` → `x`
  - `x * 0` → `0`; `x * 1` → `x`
  - `x & 0` → `0`; `x & -1` (all bits set) → `x`
- **Strength reduction (rhs-const power-of-2):**
  - `x * 2^n` → `x << n`
  - `x / 2^n` (unsigned) → `x >> n` (logical)
  - `x % 2^n` (unsigned) → `x & (2^n - 1)`

These rules fire at Braun time, before any IR is created for the folded operands.

### R1B — Dead branch elimination (in statement handlers)

When a branch condition is a `VAL_CONST` at SSA construction time, the dead branch is
eliminated immediately rather than emitting `IK_BR` and relying on R2A to clean it up:
- `ND_IFSTMT` / `ND_WHILESTMT`: if condition resolves to `VAL_CONST`, emit `IK_JMP` to
  the taken target only; the dead block is never created.
- `while(0)`: the loop body is never entered; emit only the exit `IK_JMP`.

This fires for constants that are literal or that `emit_binop` folds at construction time.
R2A catches the remaining cases (conditions that become constant after phi resolution).

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

## Optimisation Passes (`opt.c`)

See **@docs/optimization-passes.md** for the complete pass catalog, dependency graph,
bitmask system, CLI interface, and speculative optimisation framework.

Brief summary of the post-OOS pass order (encapsulated in `run_post_oos_pipeline`):

1. **R2A** `opt_fold_branches` — fold `IK_BR(const)` → `IK_JMP`
2. **R2B** `opt_remove_dead_blocks` — remove zero-predecessor blocks
3. **R2D** `opt_copy_prop` — collapse single-def `IK_COPY` chains
4. **R2E** `opt_cse` — dominator-tree scoped GVN (hash-based CSE)
5. **R2F** `opt_licm_const` + `opt_licm` — hoist constants and invariants out of loops
6. **R2I** `opt_jump_thread` — thread jumps through thin blocks
7. **R2J** `opt_unroll_loops` — MVE self-loop unrolling
8. **R2D** `opt_copy_prop` — cleanup pass after LICM/unroll

Pre-OOS passes (R2G, R2H, R2K, R2L, GVN, scalar promotion, address IVs, LSR) run
between `compute_dominators` and `out_of_ssa` on true SSA form. See the pipeline
overview above and `optimization-passes.md` for details.

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
    Symbol *def_syms[BRAUN_MAP_MAX];  Value *def_vals[BRAUN_MAP_MAX];  int ndef;
    Inst   *iphi_insts[BRAUN_MAP_MAX]; Symbol *iphi_syms[BRAUN_MAP_MAX]; int niphi;
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

## Legalize Pass (`legalize.c`)

**Entry:** `legalize_function(Function *f)`

Runs after `opt_cse` and before `irc_allocate`. Consolidates all ISA- and ABI-specific
lowering so that IRC and emission stay target-agnostic.

### Pass A — Pre-color IK_PARAM landing values

Assigns `Value.phys_reg` for each `IK_PARAM` in the entry block:
`param_idx` 1 → r1, 2 → r2, 3 → r3 (1-based). This is the only place that knows the
ABI register assignments for incoming parameters.

### Pass B — Insert pre-colored IK_COPY for call arguments

For each non-variadic `IK_CALL` or `IK_ICALL`:
- For the first 1–3 register arguments: insert `IK_COPY vR ← arg` (vR pre-colored r1/r2/r3)
  immediately before the call; replace the call's operand with vR.
- For `IK_ICALL`: also insert a `IK_COPY v_fp ← fp` (v_fp pre-colored r0) for the function
  pointer, emitted **after** the argument copies so the fp value stays live through them.

This replaces the `emit_reg_arg_copies` helper that used to live in `braun.c`.

### Pass C — Lower IK_NEG / IK_NOT

Lowers unary ops that need an implicit zero register into explicit two-operand forms:
- `IK_NEG src` → `IK_CONST(0)` + `IK_SUB(0, src)` (integer) or `IK_FSUB(0, src)` (float)
- `IK_NOT src` → `IK_CONST(0)` + `IK_EQ(src, 0)`

The explicit `IK_CONST(0)` gives IRC a proper virtual register for the zero value instead of
relying on emit.c to borrow a scratch register.

### Pass D — Lower IK_ZEXT / IK_TRUNC

Replaces zero-extension and truncation with an explicit mask-and:
`IK_ZEXT`/`IK_TRUNC src` → `IK_CONST(mask)` + `IK_AND(src, mask)`.
`mask` is `0xff` for 8-bit and `0xffff` for 16-bit. 32-bit cases (mask_size ≥ 4) are
skipped — no masking is needed. The explicit `IK_CONST(mask)` lets IRC allocate a physical
register for the mask rather than emitting pushr/popr scratch sequences in emit.c.

**Fast path (8-bit truncation):** When the source of `IK_TRUNC` is a `VAL_CONST` and the
destination is 8-bit, the instruction is folded directly to `IK_CONST(k & 0xff)` without
creating a mask value. This avoids adding a register to the interference graph for a
constant that is known at compile time.

Note: The 16-bit constant-source fast-path is intentionally deferred. Applying it changes
the IRC interference graph in a way that exposes a latent coalescing bug where an OOS
phi-copy for a loop variable is dropped even though source and destination have different
physical registers (manifests in `crcu8`: the shifted partial-CRC ends up in the loop
phi slot instead of the full post-update CRC). Fix requires hardening IRC phi-copy
handling under changed graph topology.

### Pass E — AND-chain constant folding

Folds `AND(AND(x, c1), c2) → AND(x, c1 & c2)` when both c1 and c2 are compile-time
constants (either `VAL_CONST` or the result of `IK_CONST`).

**Motivation:** Pass D lowers `(unsigned char)expr` (TRUNC to u8) to `AND(expr, 0xff)`.
The result is often type-coercion-copied to i16 (a copy that `opt_copy_prop` does not
alias, since `src->vtype != dst->vtype`). When code then does `& 1`, the pattern becomes
`AND(copy(AND(expr, 255)), 1)`. Without Pass E, both mask constants (255 and 1) occupy
IRC registers; in tight loops this can force a pushr/popr spill. Pass E folds to
`AND(expr, 1)`, making the inner AND and its `IK_CONST(255)` dead.

**IK_COPY chain traversal:** Before checking whether the inner operand is an AND, Pass E
chases through any type-coercing `IK_COPY` chains that `val_resolve` stops at (since
those copies are not aliased by `opt_copy_prop`).

**Correctness:** `AND(AND(x, c1), c2) == AND(x, c1 & c2)` for all integers. The inner
AND may have other uses; Pass E only bypasses it in the outer AND — the inner AND's other
uses remain valid and it becomes dead only if `use_count` drops to 0 after IRC DCE.
No same-block restriction: the inner AND dominates the outer by SSA construction.

---

## IRC Register Allocation (`alloc.c`)

**Entry:** `irc_allocate(Function *f)`

**Algorithm:** Appel & George 1996, Iterated Register Coalescing.

**K = 8:** r0–r7 are allocatable. r0 is pre-colored for `IK_CALL`/`IK_ICALL` landing
values. Parameters 1–3 are pre-colored to r1–r3 by the legalize pass (Pass A).

**ABI constraints:**
- r0–r3: caller-saved → interfere with all live values at call sites
- r4–r7: callee-saved → no call-site interference; saves/restores inserted by emission

**Phases:**
```
DCE (remove pure instructions with use_count == 0)
liveness analysis (backward dataflow, bitvector per block)
    ↓
repeat until no spills (max 20 iterations):
    ├─ reset IK_COPY is_dead flags (re-evaluate after each spill rewrite)
    ├─ build interference graph
    ├─ coalesce  (George's conservative test on all IK_COPY pairs; fixpoint)
    ├─ simplify/spill  (remove degree < K nodes; select spill candidates)
    ├─ select  (assign colors from stack; propagate color to coalescing aliases)
    └─ if spills: rewrite_spills → recompute liveness → retry
```

**Call-site ABI constraints** are encoded directly in the interference graph via phantom
nodes (indices `nvalues..nvalues+K-1`) that are permanently pre-colored to r0–r7. At each
`IK_CALL`/`IK_ICALL`, every value live through the call receives an interference edge to the
phantom nodes for r0–r3, forcing such values to callee-saved registers or spill.

**Coalescing:** `coalesce_copies` runs to fixpoint over all `IK_COPY` instructions.
For each copy `dst ← src`, after resolving canonical nodes via `ig_find`:
- Both precolored to same register → trivially dead (mark `is_dead`)
- Both precolored to different registers → cannot coalesce
- Already interfere → cannot coalesce
- Precolored node is always the "stay" side (u); free value is merged away (v)
- George's test: for every neighbor t of v not already adjacent to u,
  either `degree[t] < K` (safe) **and** t is not precolored to the same register as u
  (a non-obvious extra guard: two same-color precolored nodes cannot both be
  "safely below K" for a new neighbor, since that would assign the same register
  to two interfering values).

**Freeze** is not implemented: move-related nodes that fail coalescing are treated
as ordinary nodes during simplify. This foregoes a few additional coalescing
opportunities but keeps the implementation simple.

**IGraph internals:**
- `alias[]` — union-find array; `alias[v] = v` initially; `ig_merge` sets `alias[v] = u`
- `ig_find(g, v)` — path-compressed canonical lookup
- `ig_merge(g, u, v)` — redirects all of v's edges to u, clears v's row
- Aliased nodes are marked `removed` in `assign_colors` and skipped during simplify/spill;
  after stack-coloring, their color is propagated from their canonical

**Spill rewriting:** `rewrite_spills` assigns a frame slot for each spilled value
(`frame_size` grows), then walks all instructions inserting:
- A spill load (`IK_ADDR` + `IK_LOAD`) before each use of a spilled value
- A spill store (`IK_ADDR` + `IK_STORE`) after each def of a spilled value

For spill slots within F2 range (±127 words/±63 longs from bp), the `IK_ADDR` is omitted
and the load/store uses a NULL base with the bp-relative `imm` field directly.
For out-of-F2-range slots, `IK_ADDR` is emitted so IRC allocates the address register.

**Key invariant in `rewrite_spills`:** after inserting a spill store for a def, the loop
advances past *both* the `IK_ADDR` (if present) and the `IK_STORE`, ensuring the store's
src operand is never treated as a new use requiring another spill load. The out-of-range
case requires two advances (`inst = inst->next` twice, or once plus a check for `IK_ADDR`)
because `IK_ADDR` is inserted before the store, making `def->next` point to the ADDR
rather than the STORE.

**Output:** `Value.phys_reg` set for all live values (0–7). Coalesced `IK_COPY`
instructions removed (marked `is_dead`). Spill loads/stores inserted.
`Value.spill_slot` set for spilled values.

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
               spill slots       (frame_size bytes; aligned to 4-byte boundary)
bp-(frame+4)   callee-saved r4 (if used)
    ...        callee-saved r5/r6/r7 (if used)
sp             (sp = bp − (frame + callee_frame))
```

`enter N` is emitted with N = `frame + callee_frame`, where `frame` is
`Function.frame_size` rounded up to 4-byte alignment and `callee_frame` is 4 bytes per
used callee-saved register (r4–r7). Callee saves are stored **below** the spill area.
`adjw` is used only to pop extra stack arguments after calls with more than 3 parameters.

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
| `IK_SEXT8 rd, ra` | `or rd, ra, ra` (if ra≠rd); `sxb rd` |
| `IK_SEXT16 rd, ra` | `or rd, ra, ra` (if ra≠rd); `sxw rd` |
| `IK_ITOF` | `itof` |
| `IK_FTOI` | `ftoi` |
| `IK_LOAD rd, [ra+0]` (bp-rel, in F2 range) | `lw/lb/ll rd, [bp+imm]` |
| `IK_LOAD rd, [ra+0]` (other) | `llw/llb/lll rd, [ra+0]` |
| `IK_STORE [ra+0], rb` (bp-rel, in F2 range) | `sw/sb/sl rb, [bp+imm]` |
| `IK_STORE [ra+0], rb` (other) | `slw/slb/sll rb, [ra+0]` |
| `IK_COPY rd, rs` | `or rd, rs, rs` (mov pseudo) |
| `IK_MEMCPY dst, src, n` | inlined word/byte moves (always; no libcall) |
| `IK_CALL "fname"` | `jl fname` |
| `IK_ICALL fp` | `jlr` (fp in r0) |
| `IK_BR cond, T, F` | `jnz rx, T_label`; `j F_label` (both branches explicit) |
| `IK_JMP target` | `j target` |
| `IK_RET v` | move v to r0 (if not already); `ret` |
| `IK_PUTCHAR v` | move v to r0; `putchar` |

Block labels: `_{funcname}_BN:` (function-scoped to avoid cross-function collisions).

Callee-saved registers (r4–r7): emit stores to frame immediately after `enter`, loads
immediately before each `ret`. Uses bp-relative F2 instructions when in range.

Float operations (`IK_FADD` etc.) are emitted as CPU4 float instructions (`fadd`, etc.)
or lowered to libcalls if needed. The target ISA has native float instructions.

### Peephole passes (P1–P5)

These run inside `emit_function` on the physical-register IR. They fire during emission
with no IR mutations — each fires only when the exact structural condition is met.

**P1 — Branch-to-next elimination**

When `IK_BR`'s false target is the immediately following block in `f->blocks[]`, the
`j false_target` is omitted. Saves 3 bytes per conditional branch where the fall-through
is the false path (the common case after Braun's natural block ordering).

```
Before: jnz rx, T_label; j F_label   (6 bytes)
After:  jnz rx, T_label              (3 bytes, when F == next block)
```

**P2 — Inc/dec for ±1 in-place adds**

When `IK_ADD`/`IK_SUB` has one constant operand `±1` and `rd == preg(other_operand)`:
emits `inc rd` or `dec rd` (F1b, 2 bytes) instead of `immw + add` (5 bytes).

**P3 — Addi for in-place small constants**

When `IK_ADD` has one constant operand `k ∈ [−64, 63]` and `rd == preg(other_operand)`:
emits `addi rd, k` (F2, 2 bytes) instead of `immw + add` (5 bytes).

**P4 — Addli for cross-register medium constants**

When `IK_ADD` has one constant operand `k` with `|k| ≤ 255` and `rd ≠ preg(other_operand)`:
emits `addli rd, src, k` (F3f, 3 bytes) instead of `immw rd, k; add rd, src, rd` (5 bytes).
P2/P3/P4 are checked in order: P2 first (smallest), then P3 (in-place), then P4 (cross-reg).
Negative-constant subtraction is canonicalized: `sub rd, ra, k` → `add rd, ra, -k` before
the P2–P4 range checks.

**P5 — Compare+branch fusion**

Replaces a comparison instruction immediately followed by `IK_BR` using that comparison's
result (with `use_count == 1`) by a single F3b branch instruction.

A pre-pass over all blocks marks fuseable pairs by recording `(mnemonic, op0, op1)` in
a per-block side table. During emission, the comparison is suppressed and the `IK_BR` emits
`bcc ra, rb, T_label` (3 bytes) instead of the unfused `cmp_instr; jnz rd, T; j F` (8 bytes).
P1 (branch-to-next) still applies to the `j F` part after fusion.

```
Before: lts rd, ra, rb; jnz rd, T; j F   (8 bytes)
After:  blts ra, rb, T_label             (3 bytes, when F == next block)
After:  blts ra, rb, T_label; j F        (6 bytes, otherwise)
```

Supported fused comparisons: `IK_LT/LE/EQ/NE` (signed: `blts/bles/beq/bne`) and
`IK_ULT/ULE` (unsigned: `blt/ble`). Float comparisons are not fused (no F3b float-branch).

Fusion is skipped when the estimated byte distance to the true target exceeds the F3b
signed 10-bit offset range (±511 bytes from the instruction end).

**P16 — Bitex fusion (SHR + AND → bitex)**

Replaces `SHR(x, k_shift)` followed by `AND(result, k_mask)` — where `k_mask` is
`(1 << width) - 1` for `width` in 1..16 and `k_shift` in 0..31 — with a single
`bitex rd, src, imm9` instruction (F3f, 3 bytes).

A pre-pass (`detect_bitex_fusions`) scans for the pattern: the AND has one constant
mask operand that is a valid bitex mask, the other operand is defined by `IK_SHR`/`IK_USHR`
with a constant shift in the same block, the SHR result has `use_count == 1`, and the
SHR source register is not clobbered between the two instructions. When matched, the SHR
is marked dead and the AND emits `bitex` at its position.

```
Before: or rd, src, src; shrsi rd, 2; andi rd, 15   (6 bytes, 3 instructions)
After:  bitex rd, src, 98                            (3 bytes, 1 instruction)
```

For `IK_SHR` (arithmetic), an additional safety check ensures `shift + width <= 32` so
that sign-extended bits do not leak into the extracted field.

**P17 — cbeq/cbne fusion (EQ/NE with 8-bit constant)**

Replaces `IK_EQ(x, const)` or `IK_NE(x, const)` followed by `IK_BR` — where `const` is
an unsigned 8-bit value (0–255) and the branch target is within the F0b 9-bit signed
displacement range (±255 bytes) — with a single `cbeq`/`cbne` instruction (F0b, 3 bytes).

This replaces the P5+ pattern (`immw scratch, const; beq/bne rx, scratch, target`,
5 bytes) with a single 3-byte instruction. Checked after P6 (which handles the `const == 0`
case with `jz`/`jnz` at unlimited range) and before P5+ (which handles out-of-range or
non-EQ/NE comparisons).

```
Before: immw r0, 44; beq r6, r0, _f_B7     (6 bytes, 2 instructions)
After:  cbeq r6, 44, _f_B7                  (3 bytes, 1 instruction)
```

Supports branch inversion: when the true target is the fall-through block, emits the
inverted instruction (`cbeq` ↔ `cbne`) branching to the false target, eliminating the
trailing `j` instruction.

---

## Pass Summary (Implemented)

See **@docs/optimization-passes.md** for the full catalog with dependencies and bitmask details.

| Pass | File | Notes |
|------|------|-------|
| lowering | `lower.c` | Node* → Sexp; globals + string literals for data section |
| emit_globals | `emit.c` | Data section emission |
| Braun SSA | `braun.c` | Braun 2013; for/do/while/goto native; call landing+copy |
| R2C algebraic simplification | `braun.c` | In `emit_binop`: const-fold, identity/zero rules, strength reduction |
| R1B dead branch in braun | `braun.c` | Const-condition if/while/for short-circuits at construction time |
| dominator tree | `dom.c` | Cooper 2001; loop_depth |
| R2G redundant bool (pre-OOS) | `opt.c` | Eliminate NE(cmp, 0) → cmp |
| R2H narrow loads (pre-OOS) | `opt.c` | AND(LOAD(2), 0xFF) → LOAD(1) |
| R2K known bits (pre-OOS) | `opt.c` | Eliminate redundant AND/TRUNC/ZEXT via forward analysis |
| R2L bitwise dist (pre-OOS) | `opt.c` | OP(AND(a,c),AND(b,c)) → AND(OP(a,b),c) |
| pre-OOS GVN | `opt.c` | Dominator-tree CSE on true SSA form (relaxed cross-block) |
| scalar promotion (pre-OOS) | `opt.c` | Hoist load-modify-store to register accumulator phi |
| address IV (pre-OOS) | `opt.c` | Replace address recomputation with pointer IV |
| LSR (pre-OOS) | `opt.c` | Loop strength reduction: iv*invariant → ADD chain |
| out-of-SSA | `oos.c` | Boissinot 2009 parallel copies |
| R2A branch folding | `opt.c` | Fold IK_BR(VAL_CONST) → IK_JMP |
| R2B dead block elimination | `opt.c` | Remove zero-predecessor blocks; iterates to fixpoint |
| R2D copy propagation | `opt.c` | Collapse single-def IK_COPY chains; recount use_count |
| R2E CSE / GVN | `opt.c` | Dominator-tree scoped hash-based CSE |
| R2F LICM const | `opt.c` | Hoist VAL_CONST out of loop bodies |
| R2F LICM general | `opt.c` | Hoist loop-invariant pure instructions |
| R2I jump threading | `opt.c` | Thread jumps through thin IK_BR blocks |
| R2J loop unrolling | `opt.c` | MVE self-loop unrolling |
| legalize Pass A | `legalize.c` | Pre-color IK_PARAM → r1/r2/r3 |
| legalize Pass B | `legalize.c` | Insert pre-colored IK_COPY for call args and IK_ICALL fp |
| legalize Pass C | `legalize.c` | Lower IK_NEG/IK_NOT → explicit IK_CONST(0) + IK_SUB/IK_EQ |
| legalize Pass D | `legalize.c` | Lower IK_ZEXT/IK_TRUNC → IK_CONST(mask) + IK_AND |
| legalize Pass E | `legalize.c` | AND-chain constant folding |
| legalize Pass F | `legalize.c` | Materialize large VAL_CONST operands |
| IRC | `alloc.c` | Appel & George 1996; K=8; phantom-node ABI; George coalescing; spill support |
| emission P1–P17 | `emit.c` | CPU4 assembly; branch-to-next; compact encodings; compare+branch fusion; loop rotation; bitex fusion; cbeq/cbne fusion |
| speculative pipeline | `smallcc.c` | Try conservative + aggressive profiles, pick cheaper (`-speculative`) |

---

## What Is Deliberately Absent

**Regime 1 (sexp-tree) passes.** Constant folding, CSE, strength reduction as tree
rewrites. The current regime-1 optimisations (R2C, R1B) run during Braun construction
rather than as a separate tree pass; no sexp-level pass infrastructure exists.

**SCCP.** Sparse conditional constant propagation. The current approach uses
construction-time folding (R2C) and post-OOS branch folding (R2A) which covers the
most impactful cases.

**`long long` / 64-bit arithmetic.** Not required by C89 on this 32-bit target.

**`setjmp`/`longjmp`.** Requires special stack frame treatment. Not implemented.

**Jump tables for switch.** Switch is currently lowered to comparison chains.
Jump table optimisation is future work.

---

## Implementation Notes

**File map:**

| File | Role |
|------|------|
| `sx.h` / `sx.c` | Sexp AST types + constructors + printer (data section only) |
| `lower.h` / `lower.c` | Global lowering: Node* → `gvar`/`strlit` sexp for data section |
| `ssa.h` / `ssa.c` | SSA IR types + constructors + printer |
| `braun.h` / `braun.c` | Braun SSA construction directly from Node* AST |
| `dom.h` / `dom.c` | Dominator tree + loop depth |
| `oos.h` / `oos.c` | Out-of-SSA (Boissinot) |
| `opt.h` / `opt.c` | Post-OOS + pre-OOS optimisation passes; `OptProfile`; speculative profiles |
| `legalize.h` / `legalize.c` | ISA/ABI legalization: Passes A–F |
| `alloc.h` / `alloc.c` | Liveness + IRC |
| `emit.h` / `emit.c` | CPU4 emission (globals + functions); peepholes P1–P15 |

**Variable identity:** Braun variable maps are keyed on `Symbol*` directly. No
alpha-renaming is needed — each `Symbol*` is already unique within the compilation.

**String literal IDs:** `int *strlit_id` is threaded through `lower_globals` and
`braun_function` across all TUs and persists in `smallcc.c` as `cpu4_strlit_id`. This
ensures `_l0`, `_l1`, … are globally unique across all TUs and lib files. Top-level
string literals (in global initializers) are handled by `lower_globals`; function-body
string literals are accumulated by `braun.c` and flushed via `braun_emit_strlits(out)`
after each function is compiled.

**Block labels in emission:** `_{funcname}_BN` prefix avoids cross-function label
collisions when multiple functions are emitted to the same assembly file.

**Call result pre-coloring:** `IK_CALL`/`IK_ICALL` landing values are pre-colored to r0.
An immediate `IK_COPY working ← landing` gives IRC a freely-allocatable working value.
This avoids pinning the call result in r0 for the rest of its live range — IRC can
coalesce the copy away or assign `working` to any register.
