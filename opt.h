#ifndef OPT_H
#define OPT_H

#include "ssa.h"

// ============================================================
// Optimization pass bitmask
// ============================================================
// Each optional pass is gated by a bit in opt_flags.
// Passes not listed here (split_critical_edges, compute_dominators,
// out_of_ssa, legalize Pass A/B, irc_allocate) are correctness
// requirements and always run.

// Post-OOS passes (opt.c)
#define OPT_FOLD_BR        (1u << 0)   // R2A: fold IK_BR(const) → IK_JMP
#define OPT_DEAD_BLOCKS    (1u << 1)   // R2B: remove zero-predecessor blocks
#define OPT_COPY_PROP      (1u << 2)   // R2D: collapse single-def copy chains
#define OPT_CSE            (1u << 3)   // R2E: same-block CSE of pure ops
#define OPT_REDUNDANT_BOOL (1u << 4)   // R2G: eliminate NE(cmp, 0)
#define OPT_NARROW_LOADS   (1u << 5)   // R2H: AND(LOAD, mask) → narrower LOAD
#define OPT_LICM           (1u << 6)   // R2F: LICM — hoist constants + invariant expressions
#define OPT_JUMP_THREAD    (1u << 7)   // R2I: thread jumps through thin blocks
#define OPT_UNROLL         (1u << 8)   // R2J: MVE self-loop unrolling

// Legalization passes E-F (legalize.c)
// Note: Passes C (NEG/NOT lowering) and D (ZEXT/TRUNC lowering) are correctness
// requirements and always run — emit.c has no fallback for un-lowered instructions.
#define OPT_LEG_E          (1u << 9)   // AND-chain constant folding
#define OPT_LEG_F          (1u << 10)  // materialize large VAL_CONST operands

// Presets
#define OPT_ALL            0x7FFu      // all 11 bits
#define OPT_NONE           0u
#define OPT_SAFE           (OPT_FOLD_BR | OPT_DEAD_BLOCKS | OPT_COPY_PROP)

// Global flags — set once at startup from CLI, read by pipeline
extern unsigned opt_flags;

// ============================================================
// Speculative optimization profiles
// ============================================================
// Heuristic parameters that control register-pressure-sensitive
// decisions.  The speculative pipeline tries both profiles and
// picks the one that produces cheaper code after IRC.

typedef struct {
    // opt_licm_const — VAL_CONST hoisting
    int licm_const_budget_small;   // budget when body_insts <= 16
    int licm_const_budget_large;   // budget when body_insts > 16
    int licm_const_hard_cap;       // max nlive for loop-bound const hoist
    int licm_const_max_hoist;      // max constants hoisted per loop
    int licm_const_use_threshold;  // min uses - 1 (best_uses init; >= threshold+1 to hoist)

    // opt_licm — general invariant hoisting
    int licm_gen_reserve;          // registers reserved for in-body temps (K - reserve - nlive = budget)
    int licm_gen_max;              // cap on hoists per loop
    int licm_gen_dense_hi;         // nloop_defs threshold for budget=1
    int licm_gen_dense_lo;         // nloop_defs threshold for budget=min(budget,2)

    // opt_cse — post-OOS cross-block policy
    int cse_xblock_dom;            // 1 = allow any dominator (not just direct pred)
    int cse_xblock_samedelta;      // 1 = allow same loop depth (not just depth 0)

    // opt_copy_prop
    int copyprop_typecoerce;       // 1 = propagate type-coercing copies (vtype mismatch)
} OptProfile;

extern const OptProfile opt_profile_conservative;
extern const OptProfile opt_profile_aggressive;
extern const OptProfile *opt_profile;   // points to active profile

// ============================================================
// Pass declarations
// ============================================================

// R2A: Replace IK_BR with a VAL_CONST condition by IK_JMP to the taken target.
void opt_fold_branches(Function *f);

// R2B: Remove blocks with no predecessors (except entry block).
void opt_remove_dead_blocks(Function *f);

// R2D: Collapse IK_COPY chains; rewrite operands to canonical values; recount use_count.
void opt_copy_prop(Function *f);

// R2E: Hash-based CSE; eliminates duplicate pure computations across dominating blocks.
void opt_cse(Function *f);

// Pre-OOS GVN: run CSE on true SSA form (before out_of_ssa) with relaxed cross-block policy.
void opt_pre_oos_cse(Function *f);

// R2G: Eliminate NE(cmp_result, 0) → cmp_result when input is a comparison.
void opt_redundant_bool(Function *f);

// R2H: Narrow AND(LOAD(addr, sz), mask) → LOAD(addr, smaller_sz) when possible.
void opt_narrow_loads(Function *f);

// R2K: Known-bits simplification: eliminate redundant AND/TRUNC/ZEXT using
// forward known-bits analysis + unwrap_for_mask.
void opt_known_bits(Function *f);

// R2L: Bitwise distribution: OP(AND(a,c),AND(b,c)) → AND(OP(a,b),c).
// Uses unwrap_for_mask to see through casts invisible to the mask.
void opt_bitwise_dist(Function *f);

// R2F: Hoist IK_CONST out of loop bodies into pre-headers.
void opt_licm_const(Function *f);

// R2K: General LICM — hoist loop-invariant pure instructions to pre-headers.
void opt_licm(Function *f);

// R2I: Thread IK_JMP→thin IK_BR blocks when condition is defined in jumping block.
void opt_jump_thread(Function *f);

// R2J: Unroll self-loop blocks with copy-rotation chains (modulo variable expansion).
void opt_unroll_loops(Function *f);

// Loop strength reduction: replace iv*invariant with an incrementing induction variable.
// Runs pre-OOS on true SSA form where phis are explicit.
void opt_lsr(Function *f);

// Scalar promotion: hoist load-modify-store to loop-invariant address into a register
// accumulator phi.  Runs pre-OOS after GVN (so addresses are CSE'd).
void opt_scalar_promote(Function *f);

// Address induction variables: replace address recomputations inside loops with
// pointer IVs that increment each iteration.  Runs pre-OOS after scalar_promote.
void opt_addr_iv(Function *f);

#endif
