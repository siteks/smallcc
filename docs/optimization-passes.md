# Optimization Passes

Complete inventory of optimization passes, their dependencies, the bitmask
system for selective enabling/disabling, and the LICM/CSE tuning constants.

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

opt_redundant_bool()         R2G   OPT_REDUNDANT_BOOL
opt_narrow_loads()           R2H   OPT_NARROW_LOADS
opt_known_bits()             R2K   (always on)
opt_bitwise_dist()           R2L   (always on)
opt_pre_oos_cse()            GVN   OPT_CSE
opt_scalar_promote()                (always on)
opt_addr_iv()                       (always on)
opt_lsr()                           (always on)

── Phi elimination ────────────────────────────────────────────────

out_of_ssa()                 phi elimination (Boissinot 2009)

── Post-OOS passes (run_post_oos_pipeline, profile-parameterised) ─

opt_fold_branches()          R2A   OPT_FOLD_BR            (2nd call)
opt_remove_dead_blocks()     R2B   OPT_DEAD_BLOCKS        (2nd call)
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
  Pass C: lower NEG/NOT                 always on (emit.c has no fallback)
  Pass D: lower ZEXT/TRUNC             always on (emit.c has no fallback)
  Pass E: AND-chain fold       LEG_E   OPT_LEG_E
  Pass F: materialize consts   LEG_F   OPT_LEG_F

── Register allocation + emission ─────────────────────────────────

irc_allocate()               register allocation + DCE

emit_function()
  mark_dead_consts()          P7/P8/P9/P11/P13 pre-pass
  remap_single_use_values()   register assignment refinement
  detect_bitex_fusions()      P16 detection (SHR+AND → bitex)
  detect_branch_fusions()     P5/P5+/P6/P17 detection
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
| — | — | `opt_scalar_promote` | Hoist load-modify-store to register accumulator phi |
| — | — | `opt_addr_iv` | Address induction variables: replace recomputation with pointer IV |
| — | — | `opt_lsr` | Loop strength reduction: `iv*invariant` → ADD chain |

R2A+R2B run as a pre-OOS cleanup pass (before `compute_dominators`) to reduce the
block count for all subsequent passes. They run again post-OOS because OOS may
introduce new constant-condition branches.

### Post-OOS Passes (opt.c)

These run after phi elimination via `out_of_ssa()`. The post-OOS pass sequence is
encapsulated in `run_post_oos_pipeline()`.

| ID | Bit | Function | What | Depends on |
|----|-----|----------|------|------------|
| R2A | `OPT_FOLD_BR` | `opt_fold_branches` | Fold `IK_BR(const)` → `IK_JMP` | — |
| R2B | `OPT_DEAD_BLOCKS` | `opt_remove_dead_blocks` | Remove zero-predecessor blocks | R2A (creates dead blocks) |
| R2D | `OPT_COPY_PROP` | `opt_copy_prop` | Collapse single-def copy chains | — |
| R2E | `OPT_CSE` | `opt_cse` | Dominator-tree scoped GVN | dominators (block ordering) |
| R2F | `OPT_LICM` | `opt_licm_const` | Hoist `VAL_CONST` out of loops | dominators, loop depth |
| R2F | `OPT_LICM` | `opt_licm` | Hoist loop-invariant pure instructions | dominators, loop depth |
| R2I | `OPT_JUMP_THREAD` | `opt_jump_thread` | Thread jumps through thin blocks | R2D (copy targets resolved) |
| R2J | `OPT_UNROLL` | `opt_unroll_loops` | MVE self-loop unrolling | — |

`OPT_LICM` (bit 6) gates both `opt_licm_const` and `opt_licm`. These are separate
functions because constant hoisting uses a different budget model (count-based)
than general invariant hoisting (register-pressure-based).

**Dependency graph:**

```
R2A ──→ R2B           (R2A creates dead blocks for R2B to remove)
R2D ──→ R2E           (copy prop resolves operands for CSE matching)
R2D ──→ R2I           (copy prop resolves jump thread targets)
dom ──→ R2E           (CSE uses dominator pre-order)
dom ──→ R2F           (LICM needs loop depth)
R2J ──→ R2D(2nd)      (unroll creates copies that need propagation)
```

### Legalization Passes (legalize.c)

| ID | Bit | What | Required? |
|----|-----|------|-----------|
| Pass A | — | Pre-color `IK_PARAM` → r1/r2/r3 | **Correctness** (ABI) |
| Pass B | — | Pre-color call arg copies | **Correctness** (ABI) |
| Pass C | — | `IK_NEG` → `IK_SUB(0,x)`, `IK_NOT` → `IK_EQ(x,0)` | **Correctness** (emit.c has no fallback) |
| Pass D | — | `IK_ZEXT/TRUNC` → `IK_AND(x, mask)` | **Correctness** (emit.c has no fallback) |
| Pass E | `OPT_LEG_E` | `AND(AND(x,c1),c2)` → `AND(x,c1&c2)` | Optimization (reduces register pressure) |
| Pass F | `OPT_LEG_F` | Materialize large `VAL_CONST` operands | Optimization (avoids pushr/popr scratch) |

Passes A–D are correctness requirements — they must always run. emit.c has no
fallback for un-lowered `IK_NEG`/`IK_NOT`/`IK_ZEXT`/`IK_TRUNC` instructions.
Passes E–F are optimizations that reduce register pressure.

**Dependencies:**

```
Pass D ──→ Pass E     (E folds mask constants created by D)
Pass F after E        (F should see folded AND chains from E)
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
| P17 | `EQ/NE(x,const8)+BR` → `cbeq`/`cbne` | F0c | 2-3 (5-6→3) |
| P18 | `MUL(x, const)` → `mulli` | F0b | 2 (5→3) |
| P19 | General F0b imm ALU (cmp/div/mod/or/xor) | F0b | 2 (5→3) |

Peepholes are purely local and have no ordering dependencies on each other.
They fire during the single emission walk based on pattern matching.
P2/P3/P4 are checked in priority order (smallest encoding first).
P16 is detected in a pre-pass and takes priority over P13+P14 on matched pairs.
P17 is detected in `detect_branch_fusions` and checked before P5+ (const 1–255
with EQ/NE only; const 0 is handled by P6 with `jz`/`jnz` at unlimited range).

---

## LICM / CSE Heuristics

The register-pressure-sensitive LICM and CSE passes use hardcoded tuning constants
(inlined at each site in `opt.c`):

| Pass | Parameter | Value | Meaning |
|------|-----------|-------|---------|
| `opt_licm_const` | body budget (small/large) | 6 / 3 | Hoist budget when body_insts ≤ 16 / > 16 |
| `opt_licm_const` | hard cap | 6 | Max `nlive+1` for loop-bound const hoist |
| `opt_licm_const` | max hoists | 4 | Cap on constants hoisted per loop |
| `opt_licm_const` | use threshold | 2 | Hoist only when `uses > 2` (i.e. ≥ 3) |
| `opt_licm` | reserve | 4 | Registers reserved for in-body temps (`8 − 4 − nlive = budget`) |
| `opt_licm` | max hoists | 4 | Cap on invariant hoists per loop |
| `opt_licm` | dense-hi | 10 | `nloop_defs` threshold that forces budget ≤ 1 |
| `opt_licm` | dense-lo | 6 | `nloop_defs` threshold that caps budget at 2 |
| `opt_cse` post-OOS | cross-block | direct pred only | Wider policies defeat P12 loop rotation |
| `opt_copy_prop` | type-coercing copies | not propagated | Same P12 interaction |

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

// Presets
#define OPT_ALL            0x7FFu      // all 11 bits
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
-Omask=0x7FF          set exact bitmask (hex)
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

### Exploration Script

To measure pass effectiveness systematically:

```bash
#!/bin/bash
# measure_passes.sh — test each pass's contribution to CoreMark
BASE=0x7FF   # all on
for bit in $(seq 0 10); do
    mask=$(printf "0x%04x" $((BASE & ~(1 << bit))))
    name=$(echo "fold_br dead_blocks copy_prop cse redundant_bool \
        narrow_loads licm jump_thread unroll \
        leg_e leg_f" | awk "{print \$$((bit+1))}")
    cd ../coremark_single_file
    ../smallcc/smallcc -Omask=$mask -arch cpu4 -o coremark.s coremark_single.c 2>/dev/null
    cycles=$(../smallcc/sim_c -arch cpu4 -maxsteps 4000000 coremark.s 2>&1 | grep -o 'cycles:[0-9]*' | cut -d: -f2)
    echo "without $name (mask=$mask): $cycles cycles"
    cd ../smallcc
done
echo "all on (mask=0x7FF):"
cd ../coremark_single_file
../smallcc/smallcc -arch cpu4 -o coremark.s coremark_single.c 2>/dev/null
../smallcc/sim_c -arch cpu4 -maxsteps 4000000 coremark.s 2>&1 | grep -o 'cycles:[0-9]*'
```

---

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
