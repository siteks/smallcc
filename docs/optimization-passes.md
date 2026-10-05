# Optimization Passes

Complete inventory of optimization passes, their dependencies, the bitmask
system for selective enabling/disabling, and the LICM/CSE tuning constants.

## The cost model: dynamic instruction count is the metric

The CPU4 hardware is an 8-context barrel processor — one instruction per
context per rotation, so no context ever sees a branch penalty, and (for
BRAM-resident code) `sim_c`'s instruction count equals the aggregate
hardware cycle count exactly. Consequences for every pass and peephole:

- A transform pays off iff it reduces the **dynamic instruction count**
  (or, secondarily, code size at equal count). There is no speculative or
  pipeline dimension to reason about.
- Branch-reduction tricks that add instructions (if-conversion, branchless
  selects, shared epilogues) are **losses** here. Branchy-but-shorter
  always wins; taken branches cost 1 cycle like everything else.
- Fused operations (`dbnz`, `bitex`, `cbeq`, compare+branch) are worth
  exactly the instructions they remove — no more, no less.

---

## Pipeline Order

```
braun_function()             R1B, R2C  (construction-time, always on)
split_critical_edges()       structural prerequisite

── Pre-OOS cleanup (CFG-structural, no phis/copies needed) ────────

opt_fold_branches()          R2A   OPT_FOLD_BR
opt_remove_dead_blocks()     R2B   OPT_DEAD_BLOCKS
compute_dominators()         structural prerequisite

── Pre-OOS passes (true SSA form, phis present) ───────────────────

opt_frame_promote()                (frame_promote)
opt_redundant_bool()         R2G   OPT_REDUNDANT_BOOL
opt_narrow_loads()           R2H   OPT_NARROW_LOADS
opt_known_bits()             R2K   (always on)
opt_bitwise_dist()           R2L   (always on)
opt_range_check()                  (always on)
opt_fold_branches()          R2A   re-run: R2K's phi-select fold exposes
opt_remove_dead_blocks()     R2B   convergent branches; dominators recomputed
opt_pre_oos_cse()            GVN   OPT_CSE
opt_load_cse()                     OPT_CSE (dominating-load reuse)
opt_narrow_wrap_range()             (always on)
opt_scalar_promote()                (always on)
opt_addr_iv()                       (always on)
opt_lsr()                           (always on)
opt_downcount()                     (always on)

── Phi elimination ────────────────────────────────────────────────

out_of_ssa()                 phi elimination (Boissinot 2009)

── Post-OOS passes (run_post_oos_pipeline, profile-parameterised) ─

legalize_materialize_consts() LEG_F OPT_LEG_F   (early: large VAL_CONSTs become IK_CONST so
                                                  CSE dedupes and LICM hoists them)
opt_copy_prop()              R2D   OPT_COPY_PROP          (1st call)
compute_dominators()                                       (for CSE)
opt_cse()                    R2E   OPT_CSE
opt_licm_const()             R2F   OPT_LICM
opt_licm()                   R2F   OPT_LICM
opt_jump_thread()            R2I   OPT_JUMP_THREAD
opt_unroll_loops()           R2J   OPT_UNROLL
opt_copy_prop()              R2D   OPT_COPY_PROP          (2nd call, cleanup)
compute_dominators()         recompute after CFG changes

── Legalization ───────────────────────────────────────────────────

legalize_function()
  Pass A: pre-color params              always on (ABI correctness)
  Pass B: pre-color call args           always on (ABI correctness)
  Pass B2: IK_MEMCPY scratch def        always on (correctness: emit.c copy-loop scratch)
  Pass C: lower NEG/NOT                 always on (emit.c has no fallback)
  Pass D: lower ZEXT/TRUNC             always on (emit.c has no fallback)
  Pass E: AND-chain fold       LEG_E   OPT_LEG_E
  Pass F: materialize consts   LEG_F   OPT_LEG_F   (second run; catches anything created since)
  Pass G: frame-slot access    LEG_G   OPT_LEG_G   (IK_ADDR[+k] + load/store → bp-relative F2 form;
                                                   p+k + load/store → register-relative offset)
  Pass G2: absolute globals    LEG_G   OPT_LEG_G   (4-byte access to sym+k → ldl/stl, if the ISA has them)
  Pass H: slot forwarding      LEG_H   OPT_LEG_H   (store→load forwarding on private frame slots,
                                                   dead private stores removed)

── Register allocation + emission ─────────────────────────────────

irc_allocate()               register allocation + DCE

emit_function()
  mark_dead_consts()          P7/P8/P9/P11/P13 pre-pass
  remap_single_use_values()   register assignment refinement
  detect_bitex_fusions()      P16 detection (SHR+AND → bitex)
  detect_branch_fusions()     P5/P5+/P6/P17 detection
  detect_dbnz()               P20 detection (down-count latch → dbnz)
  emit_inst()                 P2/P3/P4/P7/P8/P9/P10/P11/P13/P14/P15/P16/P18/P19
  emit_rotated_branch()       P12
  inline IK_BR dispatch       P1/P15 (branch inversion)/P17 (cbeq/cbne)
```

---

## Pass Catalog

### Construction-Time (braun.c) — Always On

| ID | What | Example |
|----|------|---------|
| R1B | Dead branch elimination | `if(0) { ... }` → skip body |
| R2C | Algebraic simplification | `x * 4` → `x << 2`, `x + 0` → `x` |

These fire during IR construction. They cannot be disabled without breaking the
Braun algorithm (R1B avoids creating unreachable blocks; R2C avoids creating
instructions that would have no definition for their constant operands).

### Pre-OOS Passes (opt.c)

These run on true SSA form where phis are still explicit. Pre-OOS GVN uses a
relaxed cross-block policy (any dominating block at same or shallower loop depth,
not just same-block). Loop transforms exploit the explicit phi structure for IV
detection and accumulator promotion.

| ID | Bit | Function | What |
|----|-----|----------|------|
| R2A | `OPT_FOLD_BR` | `opt_fold_branches` | Fold `IK_BR(const)` → `IK_JMP` |
| R2B | `OPT_DEAD_BLOCKS` | `opt_remove_dead_blocks` | Remove zero-predecessor blocks |
| R2G | `OPT_REDUNDANT_BOOL` | `opt_redundant_bool` | Eliminate `NE(cmp, 0)` → `cmp` |
| R2H | `OPT_NARROW_LOADS` | `opt_narrow_loads` | `AND(LOAD(2), 0xFF)` → `LOAD(1)` |
| R2K | — | `opt_known_bits` | Known-bits: eliminate redundant AND/TRUNC/ZEXT |
| R2L | — | `opt_bitwise_dist` | `OP(AND(a,c),AND(b,c))` → `AND(OP(a,b),c)` |
| GVN | `OPT_CSE` | `opt_pre_oos_cse` | Dominator-tree CSE on true SSA form |
| — | `OPT_CSE` | `opt_load_cse` | Reuse identical dominating loads (clobber-free region; const addrs skipped) |
| — | — | `opt_frame_promote` | Scalar replacement of aggregates: each 4-byte field of a local that never escapes becomes an SSA value (stores define it, loads read it, phis built on demand at joins). Escape = a frame address reaching anything but a load/store address or a constant add; the object's extent runs to the next frame base, as in Pass H. Parameters are left in memory. `frame_promote` |
| — | — | `opt_range_check` | `AND(LE(a,x), LE(x,b))` → `ULE(SUB(x,a), b-a)` |
| — | — | `opt_narrow_wrap_range` | Drop braun's wrap of an unsigned char/short `x + k` when a dominating branch bounds `x` (`i < 8` before `i++`), so counted loops stay visible to down-counting |
| — | — | `opt_scalar_promote` | Hoist load-modify-store to register accumulator phi |
| — | — | `opt_addr_iv` | Address induction variables: replace recomputation with pointer IV |
| — | — | `opt_lsr` | Loop strength reduction: `iv*invariant` → ADD chain |
| — | — | `opt_downcount` | Counted loops with trip-count-only IVs → countdown form (enables P20 dbnz) |

R2A+R2B run as a pre-OOS cleanup pass (before `compute_dominators`) to reduce the
block count for all subsequent passes. They are **not** re-run post-OOS: OOS only
inserts `IK_COPY` into existing blocks, so it cannot create new const-condition
branches or unreachable blocks. A corpus measurement (Phase 1 OPT_STATS) confirmed
0 fires for the post-OOS calls and they were removed. `opt_jump_thread` internally
calls `opt_remove_dead_blocks` when it rewrites CFG edges, so any orphans it creates
are still cleaned up.

### Post-OOS Passes (opt.c)

These run after phi elimination via `out_of_ssa()`. The post-OOS pass sequence is
encapsulated in `run_post_oos_pipeline()`.

| ID | Bit | Function | What | Depends on |
|----|-----|----------|------|------------|
| R2D | `OPT_COPY_PROP` | `opt_copy_prop` | Collapse single-def copy chains | — |
| R2E | `OPT_CSE` | `opt_cse` | Dominator-tree scoped GVN; `IK_CONST` is CSE-pure, so constants materialised by the early Pass F run dedupe | dominators (block ordering) |
| R2F | `OPT_LICM` | `opt_licm_const` | Hoist `VAL_CONST` out of loops | dominators, loop depth |
| R2F | `OPT_LICM` | `opt_licm` | Hoist loop-invariant pure instructions | dominators, loop depth |
| R2I | `OPT_JUMP_THREAD` | `opt_jump_thread` | Thread jumps through thin blocks | R2D (copy targets resolved) |
| R2J | `OPT_UNROLL` | `opt_unroll_loops` | MVE self-loop unrolling | — |

`OPT_LICM` (bit 6) gates both `opt_licm_const` and `opt_licm`. These are separate
functions because constant hoisting uses a different budget model (count-based)
than general invariant hoisting (register-pressure-based).

**Dependency graph** (post-OOS):

```
R2D ──→ R2E           (copy prop resolves operands for CSE matching)
R2D ──→ R2I           (copy prop resolves jump thread targets)
dom ──→ R2E           (CSE uses dominator pre-order)
dom ──→ R2F           (LICM needs loop depth)
R2J ──→ R2D(2nd)      (unroll creates copies that need propagation)
```

(Pre-OOS: `R2A ──→ R2B` — R2A creates dead blocks for R2B to remove.)

### Legalization Passes (legalize.c)

| ID | Bit | What | Required? |
|----|-----|------|-----------|
| Pass A | — | Pre-color `IK_PARAM` → r1/r2/r3 | **Correctness** (ABI) |
| Pass B | — | Pre-color call arg copies | **Correctness** (ABI) |
| Pass C | — | `IK_NEG` → `IK_SUB(0,x)`, `IK_NOT` → `IK_EQ(x,0)` | **Correctness** (emit.c has no fallback) |
| Pass D | — | `IK_ZEXT/TRUNC` → `IK_AND(x, mask)` | **Correctness** (emit.c has no fallback) |
| Pass E | `OPT_LEG_E` | `AND(AND(x,c1),c2)` → `AND(x,c1&c2)` | Optimization (reduces register pressure) |
| Pass F | `OPT_LEG_F` | Materialize large `VAL_CONST` operands (ALU ops and float compares); also run at the start of the post-OOS pipeline | Optimization (avoids pushr/popr scratch; lets CSE/LICM see constants) |
| Pass G | `OPT_LEG_G` | `IK_ADDR(slot) [+ k]` as a load/store base → bp-relative form (`lea`+`lll` → one F2 `ll`); `p + k` base → folded into the F3c offset when within ±511 elements | Optimization (one instruction per frame-slot access) |
| Pass G2 | `OPT_LEG_G` | A 4-byte load/store at a global (+ constant) → absolute `ldl`/`stl`, on an ISA that has them (docs/compiler-pipeline.md) | Optimization (one instruction per global access) |
| Pass H | `OPT_LEG_H` | Forward a frame-slot store to later same-sized loads (same block, or single-predecessor chain); delete stores to private slots nothing loads. Escape analysis: a slot reachable from a live `IK_ADDR` (call arg, memcpy, pointer store) is invalidated by calls and pointer stores. Pre-coloured values are forwarded through a working copy | Optimization (out-param vectors and union puns live in registers) |

Passes A–D are correctness requirements — they must always run. emit.c has no
fallback for un-lowered `IK_NEG`/`IK_NOT`/`IK_ZEXT`/`IK_TRUNC` instructions.
Passes E–F are optimizations that reduce register pressure.

**Dependencies:**

```
Pass D ──→ Pass E     (E folds mask constants created by D)
Pass F after E        (F should see folded AND chains from E)
Pass G after F        (G needs the constant operands of address adds)
Pass H after G        (H only sees slots that G exposed as bp offsets)
```

### Emission Peepholes (emit.c)

| ID | What | Encoding | Bytes saved |
|----|------|----------|-------------|
| P1 | Skip `j` to next block | — | 3 per fall-through |
| P2 | `ADD(x,±1)` in-place → `inc`/`dec` | F1b | 3 (5→2) |
| P3 | `ADD(x,±64)` in-place → `addi` | F2 | 3 (5→2) |
| P4 | `ADD(x,±255)` cross-reg → `addli` | F0b | 2 (5→3) |
| P5 | Compare+branch fusion (2 regs) | F3c | 5 (8→3) |
| P5+ | Compare+branch fusion (1 const) | F3e+F3c | 2-3 |
| P6 | `NE/EQ(x,0)` → `jnz`/`jz` | F3e | 5 (8→3) |
| P7 | Fold `OP(const, const)` at emit | — | 3-5 per fold |
| P8 | `AND(x, 0xFF/0xFFFF)` → `zxb`/`zxw` | F1b | 3 (5→2) |
| P9 | `SEXT(const)` → `immw` | F3e | 2 (4→2) |
| P10 | Redundant SEXT after sign-ext load | — | 2 per elim |
| P11 | `SHL(x, k)` → `shli`/`shlli` | F2/F0b | 3/2 (5→2/3) |
| P12 | Loop rotation (duplicate header BR) | — | 3 per loop iter |
| P13 | `SHR/USHR(x, k)` → `shrsi`/`shrsli`/`shrli` | F2/F0b | 3/2 (5→2/3) |
| P14 | `AND(x, 0-255)` → `andi`/`andli` | F2/F0b | 3/2 (5→2/3) |
| P15 | Redundant AND via known-bits | — | 2-5 per elim |
| P16 | `SHR(x,k)+AND(r,mask)` → `bitex` | F0b | 1-3 (4-6→3) |
| P17 | `EQ/NE(x,const7)+BR` → `cbeq`/`cbne` | F0c | 2-3 (5-6→3) |
| P18 | `MUL(x, const)` → `mulli` | F0b | 2 (5→3) |
| P19 | General F0b imm ALU (cmp/div/mod/or/xor) | F0b | 2 (5→3) |
| P20 | Down-count latch `dec + rotated jnz` → `dbnz` | F3d | 2 (5→3) + 1 cycle/iter |

Peepholes are purely local and have no ordering dependencies on each other.
They fire during the single emission walk based on pattern matching.
P2/P3/P4 are checked in priority order (smallest encoding first).
P16 is detected in a pre-pass and takes priority over P13+P14 on matched pairs.
P17 is detected in `detect_branch_fusions` and checked before P5+ (const 1–127
with EQ/NE only; const 0 is handled by P6 with `jz`/`jnz` at unlimited range).

---

## LICM / CSE Heuristics

The loop passes work on each loop's natural body (`find_loops`: the header
and every block that reaches a back edge without passing through it), not on
everything the header dominates.

The register-pressure budgets are per target (`Tune` in `target.h`, values
in `target.c`) and can be overridden with `-Oparam=NAME=VALUE`
(`-Oparam=list` prints them). K is the target's register count; a reserve
is subtracted from K. The values were chosen by `tools/tune.py`, a
coordinate-descent sweep that compiles and runs the corpus, CoreMark, the ray
tracer and the NanoJPEG decode for each setting, checks every result, and
keeps a change only if no workload loses more than 0.5% of its cycles.
Rerun it after a change to the ISA or to these passes.

| Parameter | Pass | Meaning | CPU4 | CPU5 |
|---|---|---|---|---|
| `lc_reserve` | `opt_licm_const` | budget K - this for a small body | 6 | 14 |
| `lc_reserve_large` | `opt_licm_const` | budget K - this when the body exceeds `lc_large_body` | 6 | 14 |
| `lc_large_body` | `opt_licm_const` | instructions | 16 | 64 |
| `lc_cap_reserve` | `opt_licm_const` | hard cap K - this, also for the loop-bound constant | 5 | 10 |
| `lc_max_hoist` | `opt_licm_const` | constants hoisted per loop | 4 | 2 |
| `lc_min_uses` | `opt_licm_const` | hoist a constant used more than this many times | 1 | 3 |
| `licm_reserve` | `opt_licm` | budget K - this - live-ins | 5 | 4 |
| `licm_max` | `opt_licm` | hoists per loop | 4 | 5 |
| `licm_dense_hi` | `opt_licm` | more loop-defined values than this: at most 1 hoist | 30 | 1000 (off) |
| `licm_dense_lo` | `opt_licm` | more than this: at most 2 | 6 | 14 |
| `lsr_reserve` | `opt_lsr` | reductions K - this - live-ins | 7 | 15 (LSR effectively off) |
| `ipra` | allocator, emission | qualifying static functions take their own convention | 1 | 1 |
| `ipra_reserve` | allocator | ... only if they leave this many registers unwritten | 2 | 0 |
| `inline_cf_nodes` | braun inliner | inline functions with control flow up to this many AST nodes (0: only straight-line bodies) | 0 | 160 |
| `frame_promote` | `opt_frame_promote` | promote non-escaping frame slots to SSA values | 1 | 1 |
| `spill_cost` | allocator | spill cost: 0 = use count scaled by the def's loop depth; 1 = every use and def weighted by its own loop depth | 1 | 1 |
| `spill_slots` | allocator | after allocation, merge the slots of copy-related spilled values (and stack parameters) that are never live together; drop the copies | 1 | 1 |
| `jt_min_cases` | braun `switch` | a jump table needs at least this many cases ... | 12 | 8 |
| `jt_density` | braun `switch` | ... covering at least this percentage of the case value range (at most 256 values) | 50 | 10 |

Both targets settle on less aggressive constant hoisting and strength
reduction than the old hand-set values (2, 5, 16, 2, 4, 1, 4, 4, 10, 6, 4):
on CPU4 a hoisted constant or reduced multiply competes for 8 registers,
and on CPU5 most constants fit a 16-bit immediate, so hoisting one replaces a
free operand with a register.

`ipra` and `ipra_reserve` control the per-function convention for
qualifying static functions (docs/abi.md): with `ipra = 1` such a function
saves no callee-saved registers and its callers avoid every register it
writes, provided it leaves at least `ipra_reserve` registers unwritten.
CPU4 uses 1 and 2 (8 registers: a callee that writes almost all of them
pushes spills into its callers), CPU5 1 and 0.

`inline_cf_nodes` lets braun inline functions with control flow (loops,
early returns, switches): a `return` writes a result variable and jumps to a
continuation block, and the callee's locals that live in memory get a frame
slot in each caller. Excluded: goto and labels, static locals, variadic
access and taking a parameter's address. On CPU4 it stays off: 8 registers
do not hold the merged live ranges, and the allocator's spill rewriting can
fail to converge (it then refuses to emit code). Legalize Pass F also turns
a constant stored to memory into an `IK_CONST` so LICM can hoist it: a store
has no immediate form, and inlining a fill loop with a constant value
otherwise materialised it on every iteration.

Struct-returning functions inline through the same path: the result is the
address of the callee's local (a slot in the caller's frame), which every
consumer copies at once, as with a real call (docs/abi.md §4.4). With
`inline_cf_nodes = 0` a straight-line body of up to 80 nodes still takes
this path when a struct local or a struct result is all that kept it from
the plain inliner. A small 4-aligned struct copy is emitted as word loads
and stores rather than `IK_MEMCPY`, so `opt_frame_promote` sees through
struct assignments and arguments, and the copy costs two instructions per
word instead of per halfword. Together these took the by-value ray tracer
from 260M to 87M cycles on CPU5 (283M to 126M on CPU4).

`spill_cost = 1` weights each reload and spill store by the loop depth of
the block it would go in, so a value defined outside a loop but used inside
it is expensive to spill. The older cost (use count scaled by the def's
depth) undervalued exactly those values.

Two policies are fixed rather than tuned:

| Pass | Policy | Why |
|------|--------|-----|
| `opt_cse` post-OOS | cross-block: direct predecessor only | Wider policies defeat P12 loop rotation |
| `opt_copy_prop` | type-coercing copies not propagated | Same P12 interaction |

An earlier version of the compiler carried an `OptProfile` struct and a
`-speculative` mode that tried a conservative + aggressive variant per function
and picked the cheaper result. It produced no CoreMark improvement over the
hand-tuned conservative profile, so the infrastructure was removed in favour of
the inlined constants above.

---

## Bitmask System

### Bit Assignment

```c
// Post-OOS passes (opt.c)
#define OPT_FOLD_BR        (1u << 0)   // R2A
#define OPT_DEAD_BLOCKS    (1u << 1)   // R2B
#define OPT_COPY_PROP      (1u << 2)   // R2D
#define OPT_CSE            (1u << 3)   // R2E + pre-OOS GVN
#define OPT_REDUNDANT_BOOL (1u << 4)   // R2G
#define OPT_NARROW_LOADS   (1u << 5)   // R2H
#define OPT_LICM           (1u << 6)   // R2F (both licm_const and licm)
#define OPT_JUMP_THREAD    (1u << 7)   // R2I
#define OPT_UNROLL         (1u << 8)   // R2J

// Legalization passes E-F (C and D are always-on correctness passes)
#define OPT_LEG_E          (1u << 9)   // AND-chain fold
#define OPT_LEG_F          (1u << 10)  // Materialize large consts
#define OPT_LEG_G          (1u << 11)  // Frame-slot / pointer-offset access folding
#define OPT_LEG_H          (1u << 12)  // Frame-slot store→load forwarding + dead private stores

// Presets
#define OPT_ALL            0x1FFFu     // all 13 bits
#define OPT_NONE           0u
#define OPT_SAFE           (OPT_FOLD_BR | OPT_DEAD_BLOCKS | OPT_COPY_PROP)
```

### CLI Interface

```
-O0                   OPT_NONE — no optional passes
-O1                   OPT_SAFE — fold branches + dead blocks + copy prop
-O2                   OPT_ALL  — all passes (default)
-Opass=NAME           enable single pass by name (additive)
-Ono-pass=NAME        disable single pass by name (subtractive from default)
-Omask=0x1FFF         set exact bitmask (hex)
```

Examples:
```bash
./smallcc -O0 -o out.s t.c                    # no opts
./smallcc -Ono-pass=unroll -o out.s t.c        # all except unroll
./smallcc -O0 -Opass=copy_prop -o out.s t.c    # only copy prop
./smallcc -Omask=0x007 -o out.s t.c            # R2A+R2B+R2D only
```

### Pass Names for CLI

| Name | Bit | Pass |
|------|-----|------|
| `fold_br` | 0 | R2A |
| `dead_blocks` | 1 | R2B |
| `copy_prop` | 2 | R2D |
| `cse` | 3 | R2E + pre-OOS GVN |
| `redundant_bool` | 4 | R2G |
| `narrow_loads` | 5 | R2H |
| `licm` | 6 | R2F (both const + general) |
| `jump_thread` | 7 | R2I |
| `unroll` | 8 | R2J |
| `leg_e` | 9 | Legalize E |
| `leg_f` | 10 | Legalize F |
| `leg_g` | 11 | Legalize G |
| `leg_h` | 12 | Legalize H |

### Exploration Script

To measure pass effectiveness systematically:

```bash
#!/bin/bash
# measure_passes.sh — test each pass's contribution to CoreMark
BASE=0x1FFF  # all on
for bit in $(seq 0 12); do
    mask=$(printf "0x%04x" $((BASE & ~(1 << bit))))
    name=$(echo "fold_br dead_blocks copy_prop cse redundant_bool \
        narrow_loads licm jump_thread unroll \
        leg_e leg_f leg_g leg_h" | awk "{print \$$((bit+1))}")
    ./smallcc -Omask=$mask -arch cpu4 -o bench/coremark/coremark.s bench/coremark/coremark_single.c 2>/dev/null
    cycles=$(./sim_c -arch cpu4 -maxsteps 4000000 bench/coremark/coremark.s 2>&1 | grep -o 'cycles:[0-9]*' | cut -d: -f2)
    echo "without $name (mask=$mask): $cycles cycles"
done
echo "all on (mask=0x1FFF):"
./smallcc -arch cpu4 -o bench/coremark/coremark.s bench/coremark/coremark_single.c 2>/dev/null
./sim_c -arch cpu4 -maxsteps 4000000 bench/coremark/coremark.s 2>&1 | grep -o 'cycles:[0-9]*'
```

---

## Volatile accesses

A load or store through a volatile lvalue carries `Inst.is_volatile` (set by
braun). Every pass that could move, merge, narrow or delete a memory access
must skip it: `opt_licm`, `opt_load_cse`, `opt_narrow_loads`,
`opt_scalar_promote`, IRC's dead-code pass (`is_pure_inst`) and legalize
Pass H do. A new pass that touches loads or stores must do the same.

## Correctness Constraints

Passes that **must** always run (not gated by bitmask):
- `split_critical_edges` — structural prerequisite for OOS
- `compute_dominators` — prerequisite for OOS and several opt passes
- `out_of_ssa` — phi elimination (correctness)
- `opt_known_bits`, `opt_bitwise_dist` — always-on pre-OOS simplification
- `opt_scalar_promote`, `opt_addr_iv`, `opt_lsr` — always-on pre-OOS loop transforms
- Legalize Pass A — ABI param pre-coloring (correctness)
- Legalize Pass B — ABI call arg pre-coloring (correctness)
- Legalize Pass C — NEG/NOT lowering (correctness: emit.c has no fallback)
- Legalize Pass D — ZEXT/TRUNC lowering (correctness: emit.c has no fallback)
- `irc_allocate` — register allocation (correctness)

Passes gated by the bitmask are all **optimizations** — the compiler produces
correct code without them, just slower/larger.

**Safety note on disabling R2D (copy_prop):** Without copy prop, R2E/R2I
will see unresolved copy chains and may miss optimization opportunities but will
not produce incorrect code. The `val_resolve()` function chases alias chains at
each use site regardless of whether copy prop has run.

---

## Emission Peepholes (Not Bitmask-Gated)

The emit.c peepholes (P1–P18) are not included in the bitmask because:

1. They have zero risk of producing incorrect code (pattern-match on final IR)
2. They always reduce code size (never pessimize)
3. They are cheap — single-pass, no dataflow analysis
4. Disabling them individually would require threading a flags parameter
   through `emit_inst`, adding complexity for no practical benefit

If fine-grained peephole control is needed in the future, a separate
`emit_flags` bitmask can be added.
