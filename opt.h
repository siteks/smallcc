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
#define OPT_LICM_CONST     (1u << 6)   // R2F: hoist VAL_CONST out of loops
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

// R2G: Eliminate NE(cmp_result, 0) → cmp_result when input is a comparison.
void opt_redundant_bool(Function *f);

// R2H: Narrow AND(LOAD(addr, sz), mask) → LOAD(addr, smaller_sz) when possible.
void opt_narrow_loads(Function *f);

// R2F: Hoist IK_CONST out of loop bodies into pre-headers.
void opt_licm_const(Function *f);

// R2I: Thread IK_JMP→thin IK_BR blocks when condition is defined in jumping block.
void opt_jump_thread(Function *f);

// R2J: Unroll self-loop blocks with copy-rotation chains (modulo variable expansion).
void opt_unroll_loops(Function *f);

#endif
