# CPU4 Backend: Braun SSA Construction and Register Allocation

After the shared frontend (parsing, type resolution, IR generation, peephole optimisation),
the CPU4 backend converts the flat stack IR into register-based 3-address code and emits
CPU4 assembly.  The pipeline has four passes:

```
Stack IR (IRInst list)
    │
    ├─ codegen.c: mark_address_taken()    pre-scan: tag &-escaped locals
    │
    ▼
  braun_ssa()          [braun.c]          stack IR → IR3Inst (fresh vregs, phi nodes)
    │
    ├─ build_cfg()     [ir3.c]            construct per-function CFG
    ├─ compute_rpo()                      reverse post-order traversal
    ├─ translate_bb()                     per-block stack IR → IR3 conversion
    ├─ sealBlock()                        complete phi operands when all preds done
    ├─ deconstructPhis()                  convert phis to parallel copies
    │
    ▼
  linscan_regalloc()   [linscan.c]        vregs → physical r0–r7 (in-place)
    │
    ▼
  ir3_lower()          [ir3_lower.c]      IR3Inst → SSAInst (1:1 translation)
    │
    ▼
  risc_backend_emit()  [risc_backend.c]   SSAInst → CPU4 assembly text
```

---

## Source Files

| File | Lines | Role |
|---|---|---|
| `ir3.h` | 123 | IR3 data structures: `IR3Inst`, `IR3Op`, `BB`, vreg encoding constants |
| `ir3.c` | 168 | Infrastructure: fresh-vreg counter, CFG builder (`build_cfg`), `free_ir3` |
| `braun.c` | 1255 | Braun SSA construction: virtual stack model, phi insertion/elimination, phi deconstruction |
| `linscan.c` | ~520 | Poletto/Sarkar linear-scan register allocator with spill support |
| `ir3_lower.c` | 220 | Lower IR3Inst to SSAInst for the existing RISC backend |
| `ssa.h` | 72 | SSAInst/SSAOp definitions consumed by `risc_backend.c` |
| `risc_backend.c` | ~500 | CPU4 assembly emission (simplified: no callee-save infrastructure) |

---

## Preparation: Address-Taken Analysis (`codegen.c`)

Before IR generation, `mark_address_taken(func_body)` walks the function AST and sets
`sym->address_taken = true` on every `SYM_LOCAL` or `SYM_PARAM` symbol that appears as
the operand of a unary `&` (stripping any implicit `ND_CAST` wrappers from
`insert_coercions`).

During IR generation, `gen_varaddr_from_ident` classifies each `IR_LEA` instruction using
the `sym` field as a three-way tag:

| `IR_LEA` sym value | Meaning | braun.c treatment |
|---|---|---|
| `ir_promote_sentinel` (sentinel pointer) | Scalar local/param, address never escapes | Eligible for SSA promotion |
| Variable name string | Address-taken or aggregate (struct/array) variable | Memory-only: IR3_LOAD / IR3_STORE |
| `NULL` | Structural LEA (array fill, copy buffer, non-ident address) | Memory-only |

`ir_promote_sentinel` is a file-scope `const char[]` in `codegen.c`; braun.c tests by
pointer identity (`p->sym == ir_promote_sentinel`), not string comparison.

---

## IR3: The Intermediate Representation

### `IR3Inst` Struct

```c
typedef struct IR3Inst {
    IR3Op        op;
    IROp         alu_op;              /* IR3_ALU / IR3_ALU1: original stack-IR opcode */
    int          rd, rs1, rs2;        /* virtual or physical registers */
    int          imm;                 /* immediate / bp offset / label id */
    int          size;                /* 1/2/4 for LOAD/STORE */
    const char  *sym;                 /* symbolic name */
    int          line;                /* source line for -ann */
    int          phi_ops[BB_MAX_PREDS]; /* phi source vregs (IR3_PHI only) */
    int          n_phi_ops;
    struct IR3Inst *next;
} IR3Inst;
```

### Virtual Register Encoding

| Value | Meaning |
|---|---|
| `IR3_VREG_NONE` (-1) | No destination / unused operand |
| `IR3_VREG_BP` (-2) | bp-relative addressing sentinel (LOAD/STORE only) |
| `IR3_VREG_ACCUM` (100) | Accumulator; always maps to physical r0 |
| 101, 102, … | Fresh scratch vregs allocated by `ir3_new_vreg()` |
| 0–7 | Physical registers (only after `linscan_regalloc()`) |

Every expression result gets a unique vreg ID from the monotonic counter in `ir3.c`.
The counter resets per function (`ir3_reset()` called from `process_function`).

### IR3Op Enum

| Op | Description |
|---|---|
| `IR3_CONST` | `rd = imm` (integer) or `rd = &sym` (symbolic) when `sym != NULL` |
| `IR3_LOAD` | `rd = mem[rs1 + imm]`, `size` bytes; `rs1 = IR3_VREG_BP` for bp-relative |
| `IR3_STORE` | `mem[rd + imm] = rs1`, `size` bytes; `rd = IR3_VREG_BP` for bp-relative |
| `IR3_LEA` | `rd = bp + imm` (address computation, no load) |
| `IR3_ALU` | `rd = rs1 alu_op rs2` (three-register) |
| `IR3_ALU1` | `rd = alu_op(rs1)` (in-place sign-extend or float convert) |
| `IR3_MOV` | `rd = rs1` (register copy) |
| `IR3_CALL` | `jl sym` (direct call; result in ACCUM) |
| `IR3_CALLR` | `jlr` (indirect call through r0) |
| `IR3_RET` | `ret` |
| `IR3_J` | Unconditional jump to label `imm` |
| `IR3_JZ` / `IR3_JNZ` | Conditional jump (r0 == 0 / r0 != 0) |
| `IR3_ENTER` | `enter N` (frame setup) |
| `IR3_ADJ` | `sp += imm` (stack adjustment) |
| `IR3_PHI` | `rd = phi(phi_ops[0..n-1])`; eliminated before register allocation |
| `IR3_SYMLABEL` / `IR3_LABEL` | Named / numeric labels |
| `IR3_WORD` / `IR3_BYTE` / `IR3_ALIGN` | Data directives (pass-through) |
| `IR3_PUTCHAR` / `IR3_COMMENT` | Special pass-throughs |

---

## Pass 1: `braun_ssa()` — SSA Construction (`braun.c`)

Implements the Braun, Buchwald, Hack, Leißa, Mallon, Zwinkau (2013) algorithm for SSA
construction.  The algorithm interleaves CFG construction, instruction translation, and
phi-node placement in a single pass over the IR, then deconstructs phi nodes into parallel
copies.

### Overview

The public entry point `braun_ssa(blocks, n_blocks, ir_head)` scans the flat stack-IR list
and identifies function boundaries (IR_SYMLABEL followed by IR_ENTER).  Each function is
processed independently by `process_function`:

1. Emit the IR3_SYMLABEL for the function.
2. Build a per-function CFG with `build_cfg`.
3. Compute reverse post-order (RPO) traversal.
4. Translate each basic block's stack IR to IR3 in RPO order.
5. Seal blocks (complete phi operands) as predecessors finish.
6. Deconstruct surviving phi nodes into parallel copies.

Non-function IR (data labels, string literals, byte directives) is emitted as pass-through
IR3 nodes without SSA processing.

### CFG Construction (`build_cfg`, `ir3.c`)

`build_cfg(sym_node, &n_blocks)` scans from the function's IR_SYMLABEL to the next
IR_SYMLABEL (or end of list) and creates a `BB` array:

- **BB 0** covers the function entry (SYMLABEL through the first IR_BB_START).
- **BB k** (k > 0) starts at each IR_BB_START sentinel inserted by `mark_basic_blocks()`.

Each BB records:
- `ir_first` / `ir_last`: bounds for `translate_bb` iteration.
- `label_id`: the IR_LABEL operand of the block's first label instruction (-1 if unlabeled).
- `preds[]` / `succs[]`: predecessor/successor edges derived from the block's terminator.

Edge construction examines the last real instruction in each block:
- `IR_J label` → single successor (the labeled block).
- `IR_JZ`/`IR_JNZ label` → two successors (fall-through + branch target).
- `IR_RET` → no successors.
- Anything else → fall-through to the next block.

### RPO Traversal

`compute_rpo(n_blocks)` performs a depth-first search from BB 0 and produces a reverse
post-order sequence in `rpo_order[]`.  Successors are visited in reverse order so that the
fall-through edge (succs[0]) appears first in RPO, immediately after the branching block.

### Virtual Stack Model

Identical in structure to the previous `lift_to_ssa` implementation.  The expression stack
is modelled with file-scope arrays:

```c
static int vs_reg[MAX_VDEPTH];    /* fresh vreg at each depth slot   */
static int vs_size[MAX_VDEPTH];   /* push size (2 or 4 bytes)        */
static int vs_depth;              /* current virtual stack depth      */
static int vsp;                   /* running (sp - bp) byte offset    */
```

`vs_reg[0]` is always `IR3_VREG_ACCUM` (100, physical r0).  Stack IR operations manipulate
depth and assign fresh vregs:

| Stack IR | IR3 Output | Notes |
|---|---|---|
| `IR_IMM K` | `IR3_CONST rd=ACCUM, imm=K` | |
| `IR_IMM S` (symbolic) | `IR3_CONST rd=ACCUM, sym=S` | |
| `IR_PUSH` / `IR_PUSHW` | `IR3_MOV rd=fresh, rs1=ACCUM`; push fresh onto vstack | |
| `IR_POP` / `IR_POPW` | `IR3_MOV rd=ACCUM, rs1=vs_reg[top]`; pop vstack | |
| `IR_ADD` … `IR_FGE` | `IR3_ALU rd=ACCUM, rs1=vs_reg[top], rs2=ACCUM`; pop | Result directly to r0 |
| `IR_SXB`/`IR_SXW`/`IR_ITOF`/`IR_FTOI` | `IR3_ALU1 rd=ACCUM, rs1=ACCUM` | In-place on r0 |

### LEA+LOAD/STORE Collapsing

`translate_bb` peeks one instruction ahead when it encounters `IR_LEA`:

- **LEA + load (LW/LB/LL)**: collapsed to a single `IR3_LOAD rd=ACCUM, rs1=BP, imm=N, size`.
  For promoted variables, the load is replaced by a `readVariable` lookup (see below).
- **LEA (standalone)**: emitted as `IR3_LEA rd=ACCUM, imm=N`.  If the LEA is for a promotable
  variable, `accum_offset` is set so that a subsequent store can detect the promotion
  opportunity.

### Variable Promotion (Braun SSA)

Promotion eliminates memory traffic for scalar locals and parameters whose addresses never
escape.  The mechanism uses the Braun algorithm's `readVariable` / `writeVariable` /
`readVariableRecursive` functions.

**State per function:**

| Structure | Purpose |
|---|---|
| `block_def[bb_id][slot]` | Per-block current SSA definition for each variable slot |
| `phi_node_map[vreg]` | Maps phi vreg → `IR3Inst*` for trivial-phi elimination |
| `phi_insert_point[bb_id]` | Where to insert new phi nodes in a block's IR3 list |
| `pending_phis[bb_id]` | Queued phis for blocks whose insertion point is not yet known |
| `inc_phi_list` | Incomplete phis for unsealed blocks |
| `block_pretail[bb_id]` | Last IR3 node before the block's terminator (for phi deconstruction) |

Variable slots are indexed by `bp_offset + PROMOTE_BASE` (PROMOTE_BASE = 128, giving
256 slots for bp offsets -128..+127, covering all realistic frame sizes).

**Promoted store** (IR_SW/SB/SL where the address vreg tracks a promotable variable):
Instead of emitting `IR3_STORE`, a fresh vreg is created with `IR3_MOV` and registered
via `writeVariable(bb_id, slot, fresh)`.

**Promoted load** (IR_LEA + IR_LW/LB/LL for a promotable variable):
`readVariable(bb_id, slot)` returns the current SSA definition.  If a definition exists
in the current block, it is returned directly.  Otherwise `readVariableRecursive` is called,
which:

- **Single predecessor**: recurses to that predecessor (no phi needed).
- **Multiple predecessors (sealed block)**: creates a phi node, writes it as the block's
  definition (to break cycles), fills phi operands by recursing into each predecessor,
  then calls `tryRemoveTrivialPhi`.
- **Unsealed block**: creates an incomplete phi and queues it.  When the block is later
  sealed, the phi's operands are filled and trivial-phi elimination runs.

**Trivial phi elimination** (`tryRemoveTrivialPhi`): If all phi operands (excluding self-
references) are the same value `v`, the phi is replaced by `v`.  All uses of the phi vreg
in `block_def`, `phi_node_map`, pending phis, and already-emitted IR3 instructions are
rewritten.  Phis that used the eliminated phi are then recursively checked.

**Current state**: Promotion is implemented but **disabled** (`is_promo` is hardcoded to
`false` in `translate_bb`).  Enabling it requires loop-aware live-range extension in
linscan to handle values that cross loop back-edges.  All the Braun infrastructure
(readVariable, writeVariable, phi creation/elimination/deconstruction) is present and
tested; only the `is_promo` flag prevents activation.

### Phi Deconstruction (`deconstructPhis`)

After all blocks are translated and sealed, surviving (non-trivial) phi nodes are converted
to parallel copies:

1. For each phi `rd = phi(op0, op1, …)` in block B:
   - For each predecessor P_i of B: insert `IR3_MOV rd, op_i` at the tail of P_i
     (just before the terminator, tracked by `block_pretail[pred_id]`).
2. Mark the phi node dead (`rd = IR3_VREG_NONE`).

The owner block of each phi is found by walking the IR3 list from the head and tracking
the current block via SYMLABEL/LABEL nodes.

### Call Flushing (`flush_for_call_n`)

Before every `IR_JL` / `IR_JLI`, the virtual stack must be partitioned into argument slots
(bottom) and outer expression temporaries (above the args).

1. **Detect the arg boundary**: scan ahead for the post-call `IR_ADJ +N`; bottom `n_args`
   vstack slots are arguments.
2. **Spill outer temps** to memory (`IR3_STORE rd=BP, rs1=vreg, imm=vsp`) with a preceding
   `IR3_ADJ -outer_bytes`.
3. **Flush args** to the machine stack (`IR3_STORE` per arg) with `IR3_ADJ -arg_bytes`.
4. **Emit the call** (`IR3_CALL` or `IR3_CALLR`).
5. **Post-call restore** (triggered by the arg-cleanup `IR_ADJ +arg_bytes`): reload each
   spilled temp into a **fresh vreg** via `IR3_LOAD`, update the vstack, then emit
   `IR3_ADJ +outer_bytes`.

Fresh vregs for restored temps ensure no vreg's live range spans a call instruction.  This
means linscan never needs call-clobber awareness — all registers are effectively
caller-saved.

For indirect calls (`IR_JLI`), the function pointer in ACCUM is saved to a fresh vreg
before the flush and restored to ACCUM immediately before `IR3_CALLR`.

### Forward-Branch vsp Fixup

When a branch (`IR_J`/`IR_JZ`/`IR_JNZ`) is emitted, the current `vsp` is recorded in a
table keyed by label id.  When the target `IR_LABEL` is processed, `vsp` is restored from
the table.  This prevents scope-exit `adj` instructions from one branch arm from corrupting
the sp model at merge points.

---

## Pass 2: `linscan_regalloc()` — Register Allocation (`linscan.c`)

Standard Poletto/Sarkar (1999) linear-scan allocator.  Runs independently per function
(between `IR3_SYMLABEL` boundaries).  Rewrites `rd`/`rs1`/`rs2` in the IR3 list in-place.

### Physical Register Pool and ABI

**ABI**: All-caller-save.  r0–r7 are all caller-saved.  `braun_ssa`'s call flushing
guarantees no vreg live range spans a call instruction, so no callee-save/restore
infrastructure is needed.

`r1`–`r6` are available for allocation.  `r0` is permanently reserved for `IR3_VREG_ACCUM`
(handled by a final rewrite pass).  `r7` is reserved as the spill scratch register — used
to shuttle values between spill slots and instructions when a vreg cannot be assigned a
physical register.

The pool is initialized as a stack with r1 allocated first (lowest-numbered registers
preferred).  For expression temporaries (no promotion), Sethi-Ullman labelling keeps depth
≤ 3, so at most r1–r3 are needed.

### Algorithm

**Phase 1 — Live intervals**: Instructions are numbered sequentially within each function.
For each vreg (> `IR3_VREG_ACCUM`), `interval.start` = serial of first definition,
`interval.end` = serial of last use.

**Phase 2 — Sort**: Intervals are sorted by `start` position.

**Phase 3 — Linear scan**:

1. For each interval in start order:
   - Expire all active intervals whose `end < current.start`; return their registers
     to the pool.
   - `pool_alloc()` returns the next available physical register.
   - If the pool is empty: spill the longest-lived active interval.  Steal its register
     for the current interval; record a bp-relative spill slot for the displaced interval.

**Phase 4 — Rewrite**: Walk the function's IR3 list and replace each vreg with its assigned
physical register via `rewrite_reg()`.

**ACCUM rewrite**: A final pass over the entire IR3 list (including data/label nodes)
rewrites `IR3_VREG_ACCUM` (100) → physical register 0.

### Spilling

When the register pool is exhausted, the longest-lived active interval is spilled to a
bp-relative stack slot.  A diagnostic is printed to stderr (`linscan: spilling vreg N to
[bp-M]`).

**Spill slot placement**: Spill slots are bump-allocated below the function's deepest
existing frame usage.  The allocator scans all bp-relative LOAD/STORE/LEA offsets and all
cumulative ADJ adjustments to find the most negative bp offset in use, then places spill
slots strictly below that.

**Phase 5 — Spill insertion**: After register rewriting, the allocator walks the IR3 list
and inserts:
- `IR3_LOAD rd=r7, rs1=BP, imm=spill_off` (reload) before each USE of a spilled vreg
- `IR3_STORE rd=BP, rs1=r7, imm=spill_off` (spill) after each DEF of a spilled vreg

r7 serves as the scratch register for all spill traffic.

**Frame expansion**: The function's `IR3_ENTER` node is expanded to cover the spill area.
Since braun.c's call-arg flush offsets are computed relative to the original ENTER frame,
any bp-relative offset strictly below `-original_enter_N` (i.e., flush positions below the
original frame) is shifted down by the expansion amount.  This preserves the invariant that
flush stores land right at sp at call time.

Without promotion, spilling should never fire (SU guarantees depth ≤ 3, and 6 allocatable
registers are more than sufficient).  With promotion enabled, spilling handles the overflow
from promoted variables whose live ranges exceed register pressure.

---

## Pass 3: `ir3_lower()` — IR3 to SSA Lowering (`ir3_lower.c`)

Near-1:1 translation from `IR3Inst` (physical registers) to `SSAInst` for
`risc_backend_emit`.  One `IR3Inst` → one `SSAInst` in all cases.  Zero-imm `IR3_ADJ`
nodes and dead phi residuals (`IR3_MOV` with `rd == IR3_VREG_NONE`) are dropped.

`free_ssa()` (also in `ir3_lower.c`) releases the SSAInst list after emission.

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
| `IR3_ADJ` (imm != 0) | `SSA_ADJ` | Zero-imm nodes dropped |
| `IR3_SYMLABEL` | `SSA_SYMLABEL` | |
| `IR3_LABEL` | `SSA_LABEL` | |
| `IR3_WORD/BYTE/ALIGN` | `SSA_WORD/BYTE/ALIGN` | |
| `IR3_PUTCHAR` | `SSA_PUTCHAR` | |
| `IR3_COMMENT` | `SSA_COMMENT` | |
| `IR3_PHI` (dead) | skipped | rd == IR3_VREG_NONE after deconstruction |

---

## Pass 4: `risc_backend_emit()` — Assembly Emission (`risc_backend.c`)

Unchanged from the previous pipeline.  Walks the SSAInst list (all registers now physical
0–7) and emits CPU4 assembly text.  See `docs/codegen.md` for full instruction selection
tables, F2 range checks, and `le`/`ge` expansion details.

---

## Design Decisions and Trade-offs

### Why Braun SSA?

The Braun algorithm constructs SSA form during translation rather than as a separate pass
over a pre-built CFG.  This is a good fit because:

1. The stack IR is already a linear instruction stream; building a full CFG + dominance
   tree + iterated dominance frontier just to place phis would be significant infrastructure
   for a small compiler.
2. Braun's on-the-fly phi insertion + trivial-phi elimination produces minimal SSA without
   a separate phi-pruning pass.
3. The algorithm naturally handles the virtual-stack-to-register translation in one pass.

### ABI: All-Caller-Save

The CPU4 ABI is all-caller-save: r0–r7 are all caller-saved with no callee-saved registers.
This is safe because `braun_ssa`'s `flush_for_call_n` ensures every expression temporary is
spilled to memory before a call and reloaded into a **fresh vreg** afterward.  No vreg's
live range ever spans a call instruction, so no register needs to survive a call.

r7 is reserved as the spill scratch register (not available for allocation).  r0 is reserved
for the accumulator (ACCUM).  r1–r6 are available for general allocation.

### Why is Promotion Disabled?

Promoting variables to SSA registers eliminates loads/stores but extends live ranges.
A variable live across a loop back-edge creates a live range that spans the entire loop
body.  With only 6 allocable registers (r1–r6), promotion can cause register pressure to
exceed capacity, triggering spilling.

The spill mechanism is now fully implemented (r7 as scratch, frame expansion with flush
offset adjustment).  However, enabling promotion currently causes 7 test failures in
variadic, coremark, and stdlib tests — likely due to bugs in the promotion logic itself
(va_arg interaction, phi deconstruction across loop back-edges, or modification of
promoted variables inside loops).

The F2 bp-relative load/store instructions (`lw r, [bp+imm7]`: 2 bytes, 1 cycle) make
memory access cheap enough that no-promotion is competitive with promotion for typical
code patterns on this target.

Promotion can be re-enabled by setting `is_promo = (p->sym == ir_promote_sentinel)` in
`translate_bb` in `braun.c` once the promotion bugs are fixed.  Future work should also
add heuristics to promote only profitable variables (loop counters, frequently-read
scalars) rather than all eligible scalars, to avoid unnecessary register pressure.

### Two-Level IR (IR3 → SSA)

The `ir3_lower` pass exists because `risc_backend_emit` consumes `SSAInst` (defined in
`ssa.h`), not `IR3Inst`.  The lowering is trivial 1:1 translation.  A future cleanup could
merge `IR3Inst` and `SSAInst` into a single type and eliminate this pass, but the current
separation keeps the backend completely unchanged and independently testable.
