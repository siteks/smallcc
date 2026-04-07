/* ir3_opt.c — IR3-level optimizations for the CPU4 backend.
 *
 * Runs after braun_ssa() and before irc_regalloc().
 * Three passes per function, iterated until stable:
 *   1. Copy propagation   — eliminates MOV chains between fresh vregs
 *   2. Constant prop/fold — folds ALU ops with known-constant operands
 *   3. Dead code elimination — removes defs with zero uses
 *
 * ACCUM (vreg 100) tracking: copy_propagate tracks a shadow "accum_copy"
 * so that copies flowing through ACCUM can propagate to fresh vregs.
 * const_prop_fold tracks "accum_cval" similarly for constants.
 *
 * CALL/CALLR invalidate ACCUM tracking (return value overwrites r0).
 * copy_propagate also resets fresh vreg copy maps at calls (propagating a
 * pre-call vreg into a post-call use extends its live range across the call).
 * const_prop_fold does NOT reset cval maps at calls: constants are immediates
 * and can be rematerialized at any post-call use with a fresh IR3_CONST,
 * so keeping the knowledge avoids live-range extension rather than causing it.
 */

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include "ir3.h"

/* ----------------------------------------------------------------
 * Vreg tracking arrays (indexed by vreg - IR3_VREG_BASE)
 * ---------------------------------------------------------------- */
#define VMAP_SIZE 4096

static int       copy_of[VMAP_SIZE];     /* root vreg, or -1 */
static bool      cval_valid[VMAP_SIZE];  /* true if constant value known */
static int       cval[VMAP_SIZE];        /* known constant */
static int       use_count[VMAP_SIZE];   /* reference count for DCE and fold pass */
static IR3Inst  *def_inst[VMAP_SIZE];    /* defining instruction for each fresh vreg */
static bool      multi_def[VMAP_SIZE];   /* true if vreg has >1 definition (phi copies) */

static inline bool is_fresh(int v) { return v > IR3_VREG_ACCUM; }
static inline int  vidx(int v)     { return v - IR3_VREG_BASE; }
static inline bool vidx_ok(int v)  { int i = vidx(v); return i >= 0 && i < VMAP_SIZE; }

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

static bool is_side_effect_free(IR3Op op)
{
    switch (op) {
    case IR3_CONST: case IR3_LOAD: case IR3_LEA:
    case IR3_ALU:   case IR3_ALU1: case IR3_MOV:
        return true;
    default:
        return false;
    }
}

/* Is this instruction a basic-block boundary for local propagation?
 * Labels and terminators reset per-BB tracking.  CALL/CALLR are handled
 * inline in each pass (reset ACCUM + fresh vreg maps). */
static bool is_bb_leader_or_term(IR3Op op)
{
    switch (op) {
    case IR3_LABEL: case IR3_SYMLABEL:
    case IR3_J:     case IR3_JZ:  case IR3_JNZ:
    case IR3_RET:
        return true;
    default:
        return false;
    }
}

/* Evaluate a binary ALU operation on two constants.
 * Duplicate of fold_op() in optimise.c (static there). */
static uint32_t ir3_fold(IROp op, uint32_t lhs, uint32_t rhs)
{
    int32_t sl = (int32_t)lhs, sr = (int32_t)rhs;
    switch (op) {
    case IR_ADD:  return lhs + rhs;
    case IR_SUB:  return lhs - rhs;
    case IR_MUL:  return (uint32_t)(sl * sr);
    case IR_DIV:  return rhs ? lhs / rhs : 0;
    case IR_MOD:  return rhs ? lhs % rhs : 0;
    case IR_AND:  return lhs & rhs;
    case IR_OR:   return lhs | rhs;
    case IR_XOR:  return lhs ^ rhs;
    case IR_SHL:  return lhs << (rhs & 31);
    case IR_SHR:  return lhs >> (rhs & 31);
    case IR_SHRS: return (uint32_t)(sl >> (rhs & 31));
    case IR_EQ:   return (lhs == rhs) ? 1 : 0;
    case IR_NE:   return (lhs != rhs) ? 1 : 0;
    case IR_LT:   return (lhs < rhs)  ? 1 : 0;
    case IR_LE:   return (lhs <= rhs) ? 1 : 0;
    case IR_GT:   return (lhs > rhs)  ? 1 : 0;
    case IR_GE:   return (lhs >= rhs) ? 1 : 0;
    case IR_LTS:  return (sl < sr)    ? 1 : 0;
    case IR_LES:  return (sl <= sr)   ? 1 : 0;
    case IR_GTS:  return (sl > sr)    ? 1 : 0;
    case IR_GES:  return (sl >= sr)   ? 1 : 0;
    case IR_DIVS: return (uint32_t)(sr ? sl / sr : 0);
    case IR_MODS: return (uint32_t)(sr ? sl % sr : 0);
    default:      return 0;
    }
}

/* Can this ALU opcode be constant-folded by ir3_fold? */
static bool is_foldable_alu(IROp op)
{
    switch (op) {
    case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
    case IR_AND: case IR_OR:  case IR_XOR: case IR_SHL: case IR_SHR:
    case IR_SHRS:
    case IR_EQ:  case IR_NE:  case IR_LT:  case IR_LE:
    case IR_GT:  case IR_GE:
    case IR_LTS: case IR_LES: case IR_GTS: case IR_GES:
    case IR_DIVS: case IR_MODS:
        return true;
    default:
        return false;
    }
}

/* Resolve a vreg through the copy chain to its root. */
static int resolve_copy(int v)
{
    if (!is_fresh(v) || !vidx_ok(v)) return v;
    int idx = vidx(v);
    int root = copy_of[idx];
    if (root < 0) return v;
    /* One level of indirection is enough — chains are flattened on construction. */
    return root;
}

/* ----------------------------------------------------------------
 * Pass 1: Copy Propagation (per basic block)
 *
 * Tracks a shadow "accum_copy" for ACCUM so copies flowing through
 * ACCUM (the dominant pattern from braun_ssa) can propagate to fresh
 * vregs.  Example:
 *   MOV ACCUM, v101    → accum_copy = v101
 *   MOV v102, ACCUM    → copy_of[v102] = v101
 * ---------------------------------------------------------------- */
static bool copy_propagate(IR3Inst *start, IR3Inst *end)
{
    bool changed = false;
    for (int i = 0; i < VMAP_SIZE; i++) { if (multi_def[i]) copy_of[i] = -1; }
    int accum_copy = -1;     /* fresh vreg that ACCUM is a copy of, or -1 */

    for (IR3Inst *p = start; p && p != end; p = p->next) {
        /* Reset at BB boundaries */
        if (is_bb_leader_or_term(p->op)) {
            for (int i = 0; i < VMAP_SIZE; i++) { if (multi_def[i]) copy_of[i] = -1; }
            accum_copy = -1;
            continue;
        }

        if (p->op == IR3_MOV) {
            /* MOV fresh ← fresh: record copy chain */
            if (is_fresh(p->rd) && is_fresh(p->rs1)) {
                int root = resolve_copy(p->rs1);
                if (vidx_ok(p->rd))
                    copy_of[vidx(p->rd)] = root;
                continue;
            }
            /* MOV ACCUM ← fresh: track ACCUM's copy source */
            if (p->rd == IR3_VREG_ACCUM && is_fresh(p->rs1)) {
                accum_copy = resolve_copy(p->rs1);
                continue;
            }
            /* MOV fresh ← ACCUM: propagate through ACCUM */
            if (is_fresh(p->rd) && p->rs1 == IR3_VREG_ACCUM) {
                if (accum_copy >= 0 && vidx_ok(p->rd))
                    copy_of[vidx(p->rd)] = accum_copy;
                continue;
            }
        }

        /* Propagate into rs1 */
        if (is_fresh(p->rs1) && vidx_ok(p->rs1)) {
            int root = resolve_copy(p->rs1);
            if (root != p->rs1) {
                p->rs1 = root;
                changed = true;
            }
        }

        /* Propagate into rs2 */
        if (is_fresh(p->rs2) && vidx_ok(p->rs2)) {
            int root = resolve_copy(p->rs2);
            if (root != p->rs2) {
                p->rs2 = root;
                changed = true;
            }
        }

        /* Propagate into STORE rd (it's the address, a read not a write) */
        if (p->op == IR3_STORE && is_fresh(p->rd) && vidx_ok(p->rd)) {
            int root = resolve_copy(p->rd);
            if (root != p->rd) {
                p->rd = root;
                changed = true;
            }
        }

        /* Invalidate ACCUM tracking on any non-MOV write to ACCUM */
        if (p->rd == IR3_VREG_ACCUM)
            accum_copy = -1;

        /* CALL/CALLR clobber ACCUM (return value overwrites r0).
         * Also reset fresh vreg copy maps to prevent extending live ranges
         * across calls unnecessarily; irc_regalloc will spill as needed. */
        if (p->op == IR3_CALL || p->op == IR3_CALLR) {
            accum_copy = -1;
            for (int i = 0; i < VMAP_SIZE; i++) { if (multi_def[i]) copy_of[i] = -1; }
        }

        /* Invalidate fresh vreg if redefined (STORE reads rd, doesn't define it) */
        if (p->op != IR3_STORE && is_fresh(p->rd) && vidx_ok(p->rd))
            copy_of[vidx(p->rd)] = -1;
    }

    return changed;
}

/* ----------------------------------------------------------------
 * Pass 2: Constant Propagation and Folding (per basic block)
 * ---------------------------------------------------------------- */
static bool const_prop_fold(IR3Inst *start, IR3Inst *end)
{
    bool changed = false;
    for (int i = 0; i < VMAP_SIZE; i++) { if (multi_def[i]) cval_valid[i] = false; }
    bool accum_cval_valid = false;
    int  accum_cval = 0;

    for (IR3Inst *p = start; p && p != end; p = p->next) {
        /* Reset at BB boundaries */
        if (is_bb_leader_or_term(p->op)) {
            for (int i = 0; i < VMAP_SIZE; i++) { if (multi_def[i]) cval_valid[i] = false; }
            accum_cval_valid = false;
            continue;
        }

        /* Track constants: IR3_CONST with no symbolic operand */
        if (p->op == IR3_CONST && !p->sym) {
            if (is_fresh(p->rd) && vidx_ok(p->rd)) {
                cval_valid[vidx(p->rd)] = true;
                cval[vidx(p->rd)] = p->imm;
            }
            if (p->rd == IR3_VREG_ACCUM) {
                accum_cval_valid = true;
                accum_cval = p->imm;
            }
            continue;
        }

        /* Track constants through MOV */
        if (p->op == IR3_MOV) {
            /* fresh ← fresh with known constant: rewrite to CONST */
            if (is_fresh(p->rd) && vidx_ok(p->rd) &&
                is_fresh(p->rs1) && vidx_ok(p->rs1) && cval_valid[vidx(p->rs1)]) {
                int val = cval[vidx(p->rs1)];
                p->op  = IR3_CONST;
                p->imm = val;
                p->rs1 = IR3_VREG_NONE;
                p->sym = NULL;
                cval_valid[vidx(p->rd)] = true;
                cval[vidx(p->rd)] = val;
                changed = true;
                continue;
            }
            /* ACCUM ← fresh with known constant: rewrite to CONST */
            if (p->rd == IR3_VREG_ACCUM &&
                is_fresh(p->rs1) && vidx_ok(p->rs1) && cval_valid[vidx(p->rs1)]) {
                int val = cval[vidx(p->rs1)];
                p->op  = IR3_CONST;
                p->imm = val;
                p->rs1 = IR3_VREG_NONE;
                p->sym = NULL;
                accum_cval_valid = true;
                accum_cval = val;
                changed = true;
                continue;
            }
            /* fresh ← ACCUM with known constant: rewrite to CONST */
            if (is_fresh(p->rd) && vidx_ok(p->rd) &&
                p->rs1 == IR3_VREG_ACCUM && accum_cval_valid) {
                int val = accum_cval;
                p->op  = IR3_CONST;
                p->imm = val;
                p->rs1 = IR3_VREG_NONE;
                p->sym = NULL;
                cval_valid[vidx(p->rd)] = true;
                cval[vidx(p->rd)] = val;
                changed = true;
                continue;
            }
        }

        /* Fold ALU1: sign-extend of known constant */
        if (p->op == IR3_ALU1) {
            bool src_const = false;
            int  src_val = 0;
            if (is_fresh(p->rs1) && vidx_ok(p->rs1) && cval_valid[vidx(p->rs1)]) {
                src_const = true;
                src_val = cval[vidx(p->rs1)];
            } else if (p->rs1 == IR3_VREG_ACCUM && accum_cval_valid) {
                src_const = true;
                src_val = accum_cval;
            }
            if (src_const) {
                uint32_t v = (uint32_t)src_val;
                uint32_t result;
                if (p->alu_op == IR_SXB)
                    result = (uint32_t)(int32_t)(int8_t)(v & 0xff);
                else if (p->alu_op == IR_SXW)
                    result = (uint32_t)(int32_t)(int16_t)(v & 0xffff);
                else
                    goto skip_alu1;
                p->op  = IR3_CONST;
                p->imm = (int)result;
                p->rs1 = IR3_VREG_NONE;
                p->rs2 = IR3_VREG_NONE;
                p->sym = NULL;
                p->alu_op = 0;
                if (is_fresh(p->rd) && vidx_ok(p->rd)) {
                    cval_valid[vidx(p->rd)] = true;
                    cval[vidx(p->rd)] = p->imm;
                }
                if (p->rd == IR3_VREG_ACCUM) {
                    accum_cval_valid = true;
                    accum_cval = p->imm;
                }
                changed = true;
                continue;
            }
        }
skip_alu1:

        /* Fold ALU: binary operation */
        if (p->op == IR3_ALU && is_foldable_alu(p->alu_op)) {
            /* Helper: get constant value for a vreg (fresh or ACCUM) */
            #define GET_CONST(reg, out_valid, out_val) do {                 \
                if (is_fresh(reg) && vidx_ok(reg) && cval_valid[vidx(reg)]) \
                    { out_valid = true; out_val = cval[vidx(reg)]; }        \
                else if ((reg) == IR3_VREG_ACCUM && accum_cval_valid)       \
                    { out_valid = true; out_val = accum_cval; }             \
                else { out_valid = false; }                                 \
            } while(0)

            bool rs1_const; int rs1_val;
            bool rs2_const; int rs2_val;
            GET_CONST(p->rs1, rs1_const, rs1_val);
            GET_CONST(p->rs2, rs2_const, rs2_val);

            /* Helper: record a known constant result in rd */
            #define RECORD_CONST_RD(val) do {                        \
                if (is_fresh(p->rd) && vidx_ok(p->rd)) {            \
                    cval_valid[vidx(p->rd)] = true;                  \
                    cval[vidx(p->rd)] = (val);                       \
                }                                                    \
                if (p->rd == IR3_VREG_ACCUM) {                       \
                    accum_cval_valid = true;                          \
                    accum_cval = (val);                               \
                }                                                    \
            } while(0)

            if (rs1_const && rs2_const) {
                /* Both constant — fold completely */
                uint32_t result = ir3_fold(p->alu_op,
                    (uint32_t)rs1_val, (uint32_t)rs2_val);
                p->op  = IR3_CONST;
                p->imm = (int)result;
                p->rs1 = IR3_VREG_NONE;
                p->rs2 = IR3_VREG_NONE;
                p->sym = NULL;
                p->alu_op = 0;
                RECORD_CONST_RD(p->imm);
                changed = true;
                continue;
            }

            /* One constant — identity simplification */
            if (rs2_const) {
                uint32_t k = (uint32_t)rs2_val;
                IROp op = p->alu_op;
                /* x + 0, x - 0, x | 0, x ^ 0, x << 0, x >> 0 → MOV rd, rs1 */
                if (k == 0 && (op == IR_ADD || op == IR_SUB || op == IR_OR ||
                               op == IR_XOR || op == IR_SHL || op == IR_SHR ||
                               op == IR_SHRS)) {
                    p->op = IR3_MOV;
                    p->rs2 = IR3_VREG_NONE;
                    p->alu_op = 0;
                    changed = true;
                    continue;
                }
                /* x * 0, x & 0 → CONST rd, 0 */
                if (k == 0 && (op == IR_MUL || op == IR_AND)) {
                    p->op  = IR3_CONST;
                    p->imm = 0;
                    p->rs1 = IR3_VREG_NONE;
                    p->rs2 = IR3_VREG_NONE;
                    p->sym = NULL;
                    p->alu_op = 0;
                    RECORD_CONST_RD(0);
                    changed = true;
                    continue;
                }
                /* x * 1 → MOV rd, rs1 */
                if (k == 1 && op == IR_MUL) {
                    p->op = IR3_MOV;
                    p->rs2 = IR3_VREG_NONE;
                    p->alu_op = 0;
                    changed = true;
                    continue;
                }
                /* x & 0xff → zxb rd  (in-place only: rd must already equal rs1) */
                if (!flag_no_newinsns && op == IR_AND && k == 0xff && p->rd == p->rs1) {
                    p->op = IR3_ALU1; p->alu_op = IR_ZXB;
                    p->rs2 = IR3_VREG_NONE;
                    changed = true; continue;
                }
                /* x & 0xffff → zxw rd  (in-place only) */
                if (!flag_no_newinsns && op == IR_AND && k == 0xffff && p->rd == p->rs1) {
                    p->op = IR3_ALU1; p->alu_op = IR_ZXW;
                    p->rs2 = IR3_VREG_NONE;
                    changed = true; continue;
                }
            }

            if (rs1_const) {
                uint32_t k = (uint32_t)rs1_val;
                IROp op = p->alu_op;
                /* 0 + x → MOV rd, rs2 */
                if (k == 0 && op == IR_ADD) {
                    p->op  = IR3_MOV;
                    p->rs1 = p->rs2;
                    p->rs2 = IR3_VREG_NONE;
                    p->alu_op = 0;
                    changed = true;
                    continue;
                }
                /* 0 * x, 0 & x → CONST rd, 0 */
                if (k == 0 && (op == IR_MUL || op == IR_AND)) {
                    p->op  = IR3_CONST;
                    p->imm = 0;
                    p->rs1 = IR3_VREG_NONE;
                    p->rs2 = IR3_VREG_NONE;
                    p->sym = NULL;
                    p->alu_op = 0;
                    RECORD_CONST_RD(0);
                    changed = true;
                    continue;
                }
                /* 1 * x → MOV rd, rs2 */
                if (k == 1 && op == IR_MUL) {
                    p->op  = IR3_MOV;
                    p->rs1 = p->rs2;
                    p->rs2 = IR3_VREG_NONE;
                    p->alu_op = 0;
                    changed = true;
                    continue;
                }
                /* 0xff & x → zxb rd  (in-place only: rd must equal rs2) */
                if (!flag_no_newinsns && op == IR_AND && k == 0xff && p->rd == p->rs2) {
                    p->op = IR3_ALU1; p->alu_op = IR_ZXB;
                    p->rs1 = p->rd; p->rs2 = IR3_VREG_NONE;
                    changed = true; continue;
                }
                /* 0xffff & x → zxw rd  (in-place only) */
                if (!flag_no_newinsns && op == IR_AND && k == 0xffff && p->rd == p->rs2) {
                    p->op = IR3_ALU1; p->alu_op = IR_ZXW;
                    p->rs1 = p->rd; p->rs2 = IR3_VREG_NONE;
                    changed = true; continue;
                }
            }
            #undef GET_CONST
            #undef RECORD_CONST_RD
        }

        /* Invalidate rd if defined (STORE reads rd, doesn't define it) */
        if (p->op != IR3_STORE && is_fresh(p->rd) && vidx_ok(p->rd))
            cval_valid[vidx(p->rd)] = false;
        if (p->op != IR3_STORE && p->rd == IR3_VREG_ACCUM)
            accum_cval_valid = false;

        /* CALL/CALLR: r0 receives the return value, so its constant is unknown.
         * Do NOT wipe cval_valid for fresh vregs: constants are immediates and
         * can be rematerialized with a fresh IR3_CONST at any post-call use.
         * Wiping would cause post-call uses to retain references to the original
         * pre-call vreg, extending its live range across the call and forcing a
         * spill instead of a trivial immw rematerialization. */
        if (p->op == IR3_CALL || p->op == IR3_CALLR)
            accum_cval_valid = false;
    }

    return changed;
}

/* ----------------------------------------------------------------
 * Pass 3: Dead Code Elimination (per function, global use count)
 * ---------------------------------------------------------------- */
/* Does this instruction read ACCUM (as rs1, rs2, STORE base, or implicitly)? */
static bool reads_accum(IR3Inst *p)
{
    if (p->rs1 == IR3_VREG_ACCUM || p->rs2 == IR3_VREG_ACCUM)
        return true;
    /* IR3_STORE: rd is the base address register (a read, not a def) */
    if (p->op == IR3_STORE && p->rd == IR3_VREG_ACCUM)
        return true;
    /* JZ/JNZ/CALLR/RET/PUTCHAR implicitly read r0 (ACCUM) */
    switch (p->op) {
    case IR3_JZ: case IR3_JNZ: case IR3_CALLR: case IR3_RET: case IR3_PUTCHAR:
        return true;
    default:
        return false;
    }
}

static void compute_multi_def(IR3Inst *start, IR3Inst *end)
{
    memset(multi_def, 0, sizeof(multi_def));
    memset(def_inst, 0, sizeof(def_inst));
    for (IR3Inst *p = start; p && p != end; p = p->next) {
        if (p->op != IR3_STORE && is_fresh(p->rd) && vidx_ok(p->rd)) {
            int idx = vidx(p->rd);
            if (def_inst[idx] != NULL)
                multi_def[idx] = true;
            else
                def_inst[idx] = p;
        }
    }
}

static bool dce(IR3Inst *start, IR3Inst *end)
{
    memset(use_count, 0, sizeof(use_count));

    /* Scan 1: count uses */
    for (IR3Inst *p = start; p && p != end; p = p->next) {
        if (is_fresh(p->rs1) && vidx_ok(p->rs1))
            use_count[vidx(p->rs1)]++;
        if (is_fresh(p->rs2) && vidx_ok(p->rs2))
            use_count[vidx(p->rs2)]++;
        /* IR3_STORE reads rd as the base address (not a definition) */
        if (p->op == IR3_STORE && is_fresh(p->rd) && vidx_ok(p->rd))
            use_count[vidx(p->rd)]++;
    }

    /* Scan 1b: dead ACCUM store elimination.
     * If ACCUM is written by a side-effect-free op and the next non-comment
     * instruction also writes ACCUM without reading it first, the first
     * write is dead. */
    for (IR3Inst *p = start; p && p != end; p = p->next) {
        if (p->rd != IR3_VREG_ACCUM || !is_side_effect_free(p->op))
            continue;
        /* Look ahead for next non-comment instruction */
        IR3Inst *q = p->next;
        while (q && q != end && q->op == IR3_COMMENT) q = q->next;
        if (!q || q == end) continue;
        /* If next reads ACCUM or is a BB boundary, this write is live */
        if (reads_accum(q) || is_bb_leader_or_term(q->op)) continue;
        /* If next also writes ACCUM, this write is dead */
        if (q->rd == IR3_VREG_ACCUM) {
            /* Decrement use counts of operands */
            if (is_fresh(p->rs1) && vidx_ok(p->rs1))
                use_count[vidx(p->rs1)]--;
            if (is_fresh(p->rs2) && vidx_ok(p->rs2))
                use_count[vidx(p->rs2)]--;
            p->op = IR3_COMMENT;
            p->rd = IR3_VREG_NONE;
            p->rs1 = IR3_VREG_NONE;
            p->rs2 = IR3_VREG_NONE;
        }
    }

    /* Scan 2: kill dead defs */
    bool changed = false;
    IR3Inst *prev = NULL;
    for (IR3Inst *p = start; p && p != end; ) {
        IR3Inst *next = p->next;

        /* Dead phi remnants: tryRemoveTrivialPhi sets rd=NONE, rs1=unique_vreg.
         * rd=NONE is never "fresh" so the normal dead-def check below misses
         * these nodes entirely, leaving rs1's use count inflated from Scan 1.
         * That keeps unique_vreg artificially live through the allocator,
         * causing a spill and a dead load before every occurrence.  Kill them
         * here and correct the use count. */
        if (p->op == IR3_MOV && p->rd == IR3_VREG_NONE) {
            if (is_fresh(p->rs1) && vidx_ok(p->rs1))
                use_count[vidx(p->rs1)]--;
            if (prev) prev->next = next;
            changed = true;
            p = next;
            continue;
        }

        if (is_fresh(p->rd) && vidx_ok(p->rd) &&
            use_count[vidx(p->rd)] == 0 &&
            is_side_effect_free(p->op))
        {
            /* Decrement use counts of operands (may cascade) */
            if (is_fresh(p->rs1) && vidx_ok(p->rs1))
                use_count[vidx(p->rs1)]--;
            if (is_fresh(p->rs2) && vidx_ok(p->rs2))
                use_count[vidx(p->rs2)]--;

            /* Unlink (node abandoned in scratch arena until scratch_reset) */
            if (prev)
                prev->next = next;
            changed = true;
        } else {
            prev = p;
        }

        p = next;
    }

    return changed;
}

/* Remove IR3_COMMENT nodes with no sym (dead instruction stubs). */
static void compact_ir3(IR3Inst *head)
{
    IR3Inst *prev = NULL;
    for (IR3Inst *p = head; p; ) {
        IR3Inst *next = p->next;
        if (p->op == IR3_COMMENT && !p->sym) {
            if (prev) prev->next = next;
            /* node abandoned in scratch arena until scratch_reset */
        } else {
            prev = p;
        }
        p = next;
    }
}

/* ----------------------------------------------------------------
 * Helper: find a single-use CONST operand on an ADD/SUB node.
 * Returns true and sets out_cst, out_base, out_C on success.
 * For ADD: either operand may be the constant (commutative).
 * For SUB: only rs2 may be the constant (v_R = v_B - v_C).
 * ---------------------------------------------------------------- */
static bool find_add_const_operand(IR3Inst *add,
                                   IR3Inst **out_cst, int *out_base, int *out_C)
{
    if (!add || add->op != IR3_ALU) return false;
    if (add->alu_op != IR_ADD && add->alu_op != IR_SUB) return false;

    /* Try rs2 as the constant (works for both ADD and SUB) */
    if (is_fresh(add->rs2) && vidx_ok(add->rs2) &&
        use_count[vidx(add->rs2)] == 1 && !multi_def[vidx(add->rs2)]) {
        IR3Inst *d = def_inst[vidx(add->rs2)];
        if (d && d->op == IR3_CONST && !d->sym) {
            int sign = (add->alu_op == IR_SUB) ? -1 : 1;
            *out_cst  = d;
            *out_base = add->rs1;
            *out_C    = sign * d->imm;
            return true;
        }
    }
    /* For ADD only: also try rs1 as the constant (commutative) */
    if (add->alu_op == IR_ADD &&
        is_fresh(add->rs1) && vidx_ok(add->rs1) &&
        use_count[vidx(add->rs1)] == 1 && !multi_def[vidx(add->rs1)]) {
        IR3Inst *d = def_inst[vidx(add->rs1)];
        if (d && d->op == IR3_CONST && !d->sym) {
            *out_cst  = d;
            *out_base = add->rs2;
            *out_C    = d->imm;
            return true;
        }
    }
    return false;
}

/* ----------------------------------------------------------------
 * Pass 4: Fold CONST+ADD patterns (function-global, one shared
 *          def_inst/use_count build).
 *
 * Sub-pass A: LOAD/STORE offset folding
 *   CONST v_C, imm  (single-use)
 *   ALU   v_R = v_B + v_C  (v_R single-use, ADD or SUB)
 *   LOAD  [v_R + 0]   =>   LOAD [v_B + imm]  (similarly for STORE)
 *
 * Sub-pass B: arithmetic immediate (rs2=NONE sentinel for backend)
 *   CONST v_C, imm  (single-use)
 *   ALU   v_R = v_B + v_C   =>   ALU v_R = v_B + imm  (rs2=NONE)
 *   risc_backend emits addli/addi/inc/dec for these.
 *
 * Sub-pass C: compare-with-zero
 *   CONST v_Z, 0  (single-use)
 *   ALU   ACCUM = v_A eq/ne v_Z
 *   JNZ/JZ L   =>   MOV ACCUM, v_A + adjusted JZ/JNZ L
 *   Eliminates the immw 0 and the eq/ne; reduces to one branch.
 *
 * Runs once per function after the copy/const/DCE iteration loop.
 * Killed nodes are marked IR3_COMMENT; compact_ir3 removes them.
 * ---------------------------------------------------------------- */
static void fold_const_patterns(IR3Inst *start, IR3Inst *end)
{
    /* Phase 1: build def_inst[], use_count[], multi_def[] */
    memset(def_inst,   0, sizeof(def_inst));
    memset(use_count,  0, sizeof(use_count));
    memset(multi_def,  0, sizeof(multi_def));

    for (IR3Inst *p = start; p && p != end; p = p->next) {
        /* Record the unique definition of each fresh vreg.
         * IR3_STORE: rd is a read (base address), not a def.
         * If a vreg is defined more than once (phi-deconstruction copies from
         * multiple predecessor blocks), mark it multi_def so the fold passes
         * do not treat one branch's constant as the definitive value. */
        if (p->op != IR3_STORE && is_fresh(p->rd) && vidx_ok(p->rd)) {
            int idx = vidx(p->rd);
            if (def_inst[idx] != NULL)
                multi_def[idx] = true;
            else
                def_inst[idx] = p;
        }

        /* Count uses */
        if (is_fresh(p->rs1) && vidx_ok(p->rs1))
            use_count[vidx(p->rs1)]++;
        if (is_fresh(p->rs2) && vidx_ok(p->rs2))
            use_count[vidx(p->rs2)]++;
        if (p->op == IR3_STORE && is_fresh(p->rd) && vidx_ok(p->rd))
            use_count[vidx(p->rd)]++;
    }

    /* Sub-pass A: fold CONST+ADD into LOAD/STORE offset */
    for (IR3Inst *p = start; p && p != end; p = p->next) {
        int base_v;
        bool is_load = false, is_store = false;

        if (p->op == IR3_LOAD  && p->rs1 != IR3_VREG_BP &&
            is_fresh(p->rs1)   && vidx_ok(p->rs1)) {
            base_v = p->rs1;  is_load = true;
        } else if (p->op == IR3_STORE && p->rd != IR3_VREG_BP &&
                   is_fresh(p->rd)    && vidx_ok(p->rd)) {
            base_v = p->rd;  is_store = true;
        } else {
            continue;
        }

        if (use_count[vidx(base_v)] != 1) continue;
        IR3Inst *add = def_inst[vidx(base_v)];

        IR3Inst *cst; int real_base, C;
        if (!find_add_const_operand(add, &cst, &real_base, &C)) continue;

        int new_imm = p->imm + C;

        /* F3b imm10 range check (scaled by access size) */
        int sz = p->size;
        bool in_range = false;
        if      (sz == 1) in_range = (new_imm >= -512  && new_imm <= 511);
        else if (sz == 2) in_range = (new_imm >= -1024 && new_imm <= 1022 && (new_imm & 1) == 0);
        else if (sz == 4) in_range = (new_imm >= -2048 && new_imm <= 2044 && (new_imm & 3) == 0);
        if (!in_range) continue;

        if (is_load)  p->rs1 = real_base;
        if (is_store) p->rd  = real_base;
        p->imm = new_imm;

        add->op  = IR3_COMMENT;
        add->rd  = add->rs1 = add->rs2 = IR3_VREG_NONE;
        cst->op  = IR3_COMMENT;
        cst->rd  = IR3_VREG_NONE;
    }

    /* Sub-pass B: fold CONST+ADD into arithmetic immediate.
     * Uses rs2=IR3_VREG_NONE as a sentinel; risc_backend emits addli/addi/inc/dec.
     * Only fold when the CONST is single-use (consumed entirely by this ADD). */
    for (IR3Inst *p = start; p && p != end; p = p->next) {
        if (p->op != IR3_ALU) continue;
        if (p->alu_op != IR_ADD && p->alu_op != IR_SUB) continue;
        if (p->rs2 == IR3_VREG_NONE) continue;  /* already folded */

        IR3Inst *cst; int base, C;
        if (!find_add_const_operand(p, &cst, &base, &C)) continue;

        /* addli imm10 range: [-512, 511] */
        if (C < -512 || C > 511) continue;

        /* Rewrite to immediate form: alu_op=IR_ADD, rs1=base, rs2=NONE, imm=C */
        p->alu_op = IR_ADD;
        p->rs1    = base;
        p->rs2    = IR3_VREG_NONE;
        p->imm    = C;

        cst->op  = IR3_COMMENT;
        cst->rd  = IR3_VREG_NONE;
    }

    /* Sub-pass B2: fold CONST+AND/SHL/SHR/SHRS into immediate form.
     * AND:  andi  rx, sext(imm7)  — range [-64, 63]; rs2=NONE sentinel.
     * SHL:  shli  rx, imm7        — range [0, 31];   rs2=NONE sentinel.
     * SHR:  shri  rx, imm7        — range [0, 31];   rs2=NONE sentinel.
     * SHRS: shrsi rx, imm7        — range [0, 31];   rs2=NONE sentinel.
     * risc_backend emits andi/shli in-place (F2) when rd==rs1, else F3b 0xdf.
     * shri/shrsi always use F3b 0xdf (no F2 in-place form exists).
     * Disabled by -no-newinsns. */
    if (flag_no_newinsns) goto skip_b2;
    for (IR3Inst *p = start; p && p != end; p = p->next) {
        if (p->op != IR3_ALU) continue;
        if (p->rs2 == IR3_VREG_NONE) continue;  /* already folded */

        if (p->alu_op == IR_AND) {
            /* Commutative: constant may be in rs2 or rs1 */
            IR3Inst *cst = NULL;
            int base = IR3_VREG_NONE, C = 0;
            if (is_fresh(p->rs2) && vidx_ok(p->rs2) && use_count[vidx(p->rs2)] == 1 &&
                !multi_def[vidx(p->rs2)]) {
                IR3Inst *d = def_inst[vidx(p->rs2)];
                if (d && d->op == IR3_CONST && !d->sym) { cst=d; base=p->rs1; C=d->imm; }
            }
            if (!cst && is_fresh(p->rs1) && vidx_ok(p->rs1) && use_count[vidx(p->rs1)] == 1 &&
                !multi_def[vidx(p->rs1)]) {
                IR3Inst *d = def_inst[vidx(p->rs1)];
                if (d && d->op == IR3_CONST && !d->sym) { cst=d; base=p->rs2; C=d->imm; }
            }
            if (!cst) continue;
            if (C < -64 || C > 63) continue;   /* andi sext(imm7) range */
            p->rs1 = base; p->rs2 = IR3_VREG_NONE; p->imm = C;
            cst->op = IR3_COMMENT; cst->rd = IR3_VREG_NONE;

        } else if (p->alu_op == IR_SHL || p->alu_op == IR_SHR || p->alu_op == IR_SHRS) {
            /* Non-commutative: only rs2 can be the shift count */
            if (!is_fresh(p->rs2) || !vidx_ok(p->rs2) || use_count[vidx(p->rs2)] != 1 ||
                multi_def[vidx(p->rs2)]) continue;
            IR3Inst *d = def_inst[vidx(p->rs2)];
            if (!d || d->op != IR3_CONST || d->sym) continue;
            int C = d->imm;
            if (C < 0 || C > 31) continue;     /* shift counts 0-31 */
            p->rs2 = IR3_VREG_NONE; p->imm = C;
            d->op = IR3_COMMENT; d->rd = IR3_VREG_NONE;
        }
    }
    skip_b2:;

    /* Sub-pass B3: AND-chain constant folding.
     * Folds (rx AND C1) AND C2  →  rx AND (C1 & C2)  when C1 is a constant.
     * Catches the common case where C1 is out of B2's imm7 range (e.g. 0xff)
     * so the inner AND was left unfused, but the outer andi has been B2-folded.
     *
     * Requires:
     *   - outer AND in B2 sentinel form: alu_op=IR_AND, rs2=NONE, imm=C2, rs1=inner_vreg
     *   - inner_vreg is single-use (use_count==1, so this outer AND is the sole user)
     *   - inner_vreg is defined by ALU alu_op=IR_AND with a known CONST operand C1
     *     (handled both for un-B2-folded inner and already-B2-folded inner)
     * Kills the intermediate AND and CONST when they become dead. */
    if (flag_no_newinsns) goto skip_b3;
    for (IR3Inst *p = start; p && p != end; p = p->next) {
        if ((int)p->op < 0 || p->op != IR3_ALU || p->alu_op != IR_AND) continue;
        if (p->rs2 != IR3_VREG_NONE) continue;   /* must be B2-folded (sentinel form) */
        int C2 = p->imm;
        int inner = p->rs1;
        if (!is_fresh(inner) || !vidx_ok(inner) || multi_def[vidx(inner)]) continue;
        if (use_count[vidx(inner)] != 1) continue;  /* inner must have sole use here */
        IR3Inst *d_inner = def_inst[vidx(inner)];
        if (!d_inner || d_inner->op != IR3_ALU || d_inner->alu_op != IR_AND) continue;

        int C1 = 0, rx = IR3_VREG_NONE;
        IR3Inst *d_const = NULL;  /* CONST node to kill, or NULL if inner was B2-folded */
        int      const_vreg = IR3_VREG_NONE; /* the vreg holding C1, for use_count check */

        if (d_inner->rs2 == IR3_VREG_NONE) {
            /* Inner was already B2-folded: andi(rx, C1) */
            C1 = d_inner->imm;
            rx = d_inner->rs1;
        } else {
            /* Inner not B2-folded: find CONST operand in rs2 or rs1 (commutative) */
            if (is_fresh(d_inner->rs2) && vidx_ok(d_inner->rs2) &&
                !multi_def[vidx(d_inner->rs2)]) {
                IR3Inst *dc = def_inst[vidx(d_inner->rs2)];
                if (dc && dc->op == IR3_CONST && !dc->sym) {
                    d_const = dc; C1 = dc->imm;
                    rx = d_inner->rs1; const_vreg = d_inner->rs2;
                }
            }
            if (!d_const && is_fresh(d_inner->rs1) && vidx_ok(d_inner->rs1) &&
                !multi_def[vidx(d_inner->rs1)]) {
                IR3Inst *dc = def_inst[vidx(d_inner->rs1)];
                if (dc && dc->op == IR3_CONST && !dc->sym) {
                    d_const = dc; C1 = dc->imm;
                    rx = d_inner->rs2; const_vreg = d_inner->rs1;
                }
            }
            if (!d_const) continue;
        }

        uint32_t new_mask = (uint32_t)C1 & (uint32_t)C2;
        /* Always fold: eliminates the inner AND (and its CONST) regardless of
         * whether new_mask equals C1 or C2.  Even if the mask value is unchanged,
         * removing the intermediate AND + CONST reduces instruction count. */

        /* Fold: outer andi now reads rx directly with the combined mask */
        p->rs1 = rx;
        p->imm = (int)new_mask;

        /* Kill the intermediate AND (sole-use confirmed above) */
        d_inner->op = IR3_COMMENT;
        d_inner->rd = IR3_VREG_NONE;

        /* Kill the CONST if it was the sole user of that vreg */
        if (d_const && vidx_ok(const_vreg) && use_count[vidx(const_vreg)] == 1) {
            d_const->op = IR3_COMMENT;
            d_const->rd = IR3_VREG_NONE;
        }
    }
    skip_b3:;

    /* Sub-pass C: fold CONST(0) + EQ/NE → direct branch test.
     * Pattern: CONST v_Z, 0 (single-use) + ALU ACCUM = v_A eq/ne v_Z
     *           + (JNZ or JZ) L
     * → MOV ACCUM, v_A  (or eliminated if v_A==ACCUM)
     *   + adjusted JZ/JNZ L
     * Eliminates the immw 0 and eq/ne; after copy/DCE only a jump remains. */
    for (IR3Inst *p = start; p && p != end; p = p->next) {
        if (p->op != IR3_ALU) continue;
        if (p->rd  != IR3_VREG_ACCUM) continue;
        if (p->alu_op != IR_EQ && p->alu_op != IR_NE) continue;

        /* One operand must be a single-use CONST 0, other is the value to test */
        int val_v = IR3_VREG_NONE;
        IR3Inst *cst = NULL;

        /* Check rs2 */
        if (is_fresh(p->rs2) && vidx_ok(p->rs2) && use_count[vidx(p->rs2)] == 1 &&
            !multi_def[vidx(p->rs2)]) {
            IR3Inst *d = def_inst[vidx(p->rs2)];
            if (d && d->op == IR3_CONST && !d->sym && d->imm == 0) {
                cst = d; val_v = p->rs1;
            }
        }
        /* Check rs1 (commutative for EQ/NE) */
        if (!cst && is_fresh(p->rs1) && vidx_ok(p->rs1) && use_count[vidx(p->rs1)] == 1 &&
            !multi_def[vidx(p->rs1)]) {
            IR3Inst *d = def_inst[vidx(p->rs1)];
            if (d && d->op == IR3_CONST && !d->sym && d->imm == 0) {
                cst = d; val_v = p->rs2;
            }
        }
        if (!cst) continue;

        /* Find the immediately following JNZ or JZ */
        IR3Inst *br = p->next;
        while (br && br != end && br->op == IR3_COMMENT) br = br->next;
        if (!br || br == end) continue;
        if (br->op != IR3_JNZ && br->op != IR3_JZ) continue;

        bool is_eq  = (p->alu_op == IR_EQ);
        bool is_jnz = (br->op   == IR3_JNZ);

        /* Kill CONST and replace ALU with MOV ACCUM ← val_v
         * (or delete it if val_v is already ACCUM). */
        cst->op = IR3_COMMENT;
        cst->rd = IR3_VREG_NONE;

        if (val_v == IR3_VREG_ACCUM) {
            /* ACCUM already holds the value — just kill the ALU */
            p->op  = IR3_COMMENT;
            p->rd  = p->rs1 = p->rs2 = IR3_VREG_NONE;
        } else {
            p->op   = IR3_MOV;
            p->rs1  = val_v;
            p->rs2  = IR3_VREG_NONE;
            p->alu_op = 0;
        }

        /* Adjust branch direction for EQ (the condition is inverted by the MOV):
         * eq(v_A, 0) is true when v_A==0; after MOV, ACCUM = v_A (nonzero = true).
         * So JNZ (jump when eq=true=ACCUM_old≠0) → JZ (jump when v_A==0). */
        if (is_eq) {
            br->op = is_jnz ? IR3_JZ : IR3_JNZ;
        }
        /* NE: ne(v_A,0) is true when v_A!=0; after MOV ACCUM=v_A: JNZ keeps JNZ. */
    }
}

/* ----------------------------------------------------------------
 * Helper for Sub-pass D: check if a vreg's value is provably byte-bounded.
 * Returns true when bits [31:8] are known zero — meaning a ZXB on this vreg
 * would be a no-op.  Uses def_inst[] populated by fold_const_patterns Phase 1.
 * ---------------------------------------------------------------- */
static bool fits_in_byte(int vreg)
{
    if (!is_fresh(vreg) || !vidx_ok(vreg) || multi_def[vidx(vreg)]) return false;
    IR3Inst *d = def_inst[vidx(vreg)];
    if (!d) return false;
    if (d->op == IR3_CONST && !d->sym)
        return (uint32_t)(unsigned)d->imm <= 0xff;
    if (d->op == IR3_ALU && d->alu_op == IR_AND) {
        /* B2/B3-folded form: rs2=NONE, imm holds the mask */
        if (d->rs2 == IR3_VREG_NONE)
            return (uint32_t)(unsigned)d->imm <= 0xff;
        /* Unfused: constant may be in rs2 */
        if (is_fresh(d->rs2) && vidx_ok(d->rs2) && !multi_def[vidx(d->rs2)]) {
            IR3Inst *c = def_inst[vidx(d->rs2)];
            if (c && c->op == IR3_CONST && !c->sym)
                return (uint32_t)(unsigned)c->imm <= 0xff;
        }
        /* Commutative AND: constant may be in rs1 */
        if (is_fresh(d->rs1) && vidx_ok(d->rs1) && !multi_def[vidx(d->rs1)]) {
            IR3Inst *c = def_inst[vidx(d->rs1)];
            if (c && c->op == IR3_CONST && !c->sym)
                return (uint32_t)(unsigned)c->imm <= 0xff;
        }
    }
    if (d->op == IR3_ALU1 && d->alu_op == IR_ZXB) return true;
    return false;
}

/* ----------------------------------------------------------------
 * Sub-pass D: redundant sign-extend elimination.
 *
 * Removes SXW/SXB that is applied to a value that is already
 * sign-extended by a prior SXW/SXB:
 *   SXW(SXW(x)) == SXW(x)
 *   SXW(SXB(x)) == SXB(x)  (SXB extends further; SXW is then a no-op)
 *   SXB(SXB(x)) == SXB(x)
 *   SXB(SXW(x)) ≠ SXW(x)   (may narrow; do NOT eliminate)
 *
 * Handles two cases:
 *   Fresh vregs: use def_inst[] (built by fold_const_patterns Phase 1).
 *   ACCUM (multi-def): forward scan tracking running sign-extend state.
 *
 * Redundant ALU1 nodes are replaced by IR3_MOV rd, rs1.
 * A copy_propagate + dce pass after this function cleans them up.
 * ---------------------------------------------------------------- */
static void elim_redundant_sxt(IR3Inst *start, IR3Inst *end)
{
    /* def_inst[] / multi_def[] are already valid from fold_const_patterns */

    bool   accum_sxt  = false;   /* ACCUM currently holds a sign-extended value */
    IROp   accum_sop  = (IROp)0; /* the SXT op that produced the current ACCUM */

    for (IR3Inst *p = start; p && p != end; p = p->next) {
        /* Reset ACCUM sign-ext state at any BB boundary */
        if (is_bb_leader_or_term(p->op)) {
            accum_sxt = false;
            continue;
        }

        if (p->op != IR3_ALU1) {
            /* Any write to ACCUM (except SXT itself) resets its state */
            if (p->rd == IR3_VREG_ACCUM)
                accum_sxt = false;
            continue;
        }

        bool is_sxw = (p->alu_op == IR_SXW);
        bool is_sxb = (p->alu_op == IR_SXB);
        bool is_zxb = (p->alu_op == IR_ZXB);
        if (!is_sxw && !is_sxb && !is_zxb) {
            if (p->rd == IR3_VREG_ACCUM) accum_sxt = false;
            continue;
        }

        /* ZXB elimination: redundant when input is provably byte-bounded. */
        if (is_zxb) {
            bool elim = fits_in_byte(p->rs1);
            /* Also catch ZXB(XOR(a, b)) where both a and b are byte-bounded:
             * (0|1) XOR (0|1) == (0|1), so ZXB is a no-op. */
            if (!elim && is_fresh(p->rs1) && vidx_ok(p->rs1) &&
                !multi_def[vidx(p->rs1)]) {
                IR3Inst *d = def_inst[vidx(p->rs1)];
                if (d && d->op == IR3_ALU && d->alu_op == IR_XOR &&
                    fits_in_byte(d->rs1) && fits_in_byte(d->rs2))
                    elim = true;
            }
            if (elim) {
                p->op     = IR3_MOV;
                p->alu_op = (IROp)0;
                /* ZXB is always in-place (rd==rs1) → identity MOV → copy_prop removes it */
            }
            /* ZXB never contributes to sign-extend state */
            if (p->rd == IR3_VREG_ACCUM) accum_sxt = false;
            continue;
        }

        /* Determine if rs1 is already sign-extended */
        bool redundant = false;

        if (p->rs1 == IR3_VREG_ACCUM) {
            /* ACCUM path: use running state */
            if (accum_sxt) {
                /* SXW after any SXT: redundant.
                 * SXB after SXB: redundant.
                 * SXB after SXW: not redundant (may narrow). */
                if (is_sxw || (is_sxb && accum_sop == IR_SXB))
                    redundant = true;
            }
        } else if (is_fresh(p->rs1) && vidx_ok(p->rs1) && !multi_def[vidx(p->rs1)]) {
            /* Fresh-vreg path: check the unique definition */
            IR3Inst *d = def_inst[vidx(p->rs1)];
            if (d && d->op == IR3_ALU1 &&
                (d->alu_op == IR_SXW || d->alu_op == IR_SXB)) {
                if (is_sxw || (is_sxb && d->alu_op == IR_SXB))
                    redundant = true;
            }
        }

        if (redundant) {
            /* Convert to identity MOV; copy_propagate + DCE will remove it */
            p->op     = IR3_MOV;
            p->alu_op = (IROp)0;
            /* rd == rs1 for ACCUM case → identity MOV, killed by DCE */
        } else {
            /* Update ACCUM state */
            if (p->rd == IR3_VREG_ACCUM) {
                accum_sxt = true;
                accum_sop = p->alu_op;
            }
        }
    }
}

/* ----------------------------------------------------------------
 * Helper: detect a STORE to a known bp-relative slot.
 *
 * braun_ssa emits two kinds of bp-relative stores:
 *   1. rd == IR3_VREG_BP (-2) — only used for call-argument flushing.
 *   2. rd == fresh vreg whose defining instruction is IR3_LEA — the
 *      normal case for all local-variable writes.
 *
 * Returns true and sets *out_imm to the effective bp offset
 * (lea_imm + p->imm) when either form is detected.
 * Requires def_inst[] to be populated (call only after fold_const_patterns
 * has run for the current function).
 * ---------------------------------------------------------------- */
static bool store_get_bp_imm(const IR3Inst *p, int *out_imm)
{
    if (!p || p->op != IR3_STORE) return false;
    if (p->rd == IR3_VREG_BP) { *out_imm = p->imm; return true; }
    if (is_fresh(p->rd) && vidx_ok(p->rd) && !multi_def[vidx(p->rd)]) {
        IR3Inst *d = def_inst[vidx(p->rd)];
        if (d && d->op == IR3_LEA) { *out_imm = d->imm + p->imm; return true; }
    }
    return false;
}

/* ----------------------------------------------------------------
 * Sub-pass E: within-BB local load CSE for bp-relative loads.
 *
 * Within each basic block, if [BP+imm] of size S was already loaded into
 * vreg V and there has been no intervening store to [BP+imm], a second
 * load of the same slot is replaced with IR3_MOV rd, V.  The resulting
 * MOV is then eliminated by copy_propagate + dce in the caller.
 *
 * Reset at every BB leader/terminator (LABEL, J, JZ, JNZ, RET, SYMLABEL)
 * and at CALL/CALLR (callee may clobber stack through pointers).
 * Invalidate the matching entry on STORE to the same bp slot.
 *
 * Note: does not catch cross-BB repeated loads (e.g. loop-invariant loads
 * that appear in a check BB and again in the body BB); that requires LICM.
 * ---------------------------------------------------------------- */
static void local_load_cse(IR3Inst *start, IR3Inst *end)
{
#define LCSE_SLOTS 16
    struct lcse_entry { int imm; int size; int vreg; };
    struct lcse_entry cse[LCSE_SLOTS];
    int n = 0;

    for (IR3Inst *p = start; p && p != end; p = p->next) {
        if ((int)p->op < 0) continue;

        /* Reset at BB boundaries and calls */
        if (is_bb_leader_or_term(p->op) ||
                p->op == IR3_CALL || p->op == IR3_CALLR) {
            n = 0;
            continue;
        }

        if (p->op == IR3_LOAD && p->rs1 == IR3_VREG_BP && is_fresh(p->rd)) {
            /* Search for a previous load of the same slot */
            bool hit = false;
            for (int i = 0; i < n; i++) {
                if (cse[i].imm == p->imm && cse[i].size == p->size) {
                    p->op  = IR3_MOV;
                    p->rs1 = cse[i].vreg;
                    hit    = true;
                    break;
                }
            }
            /* No hit: record this load as the canonical value for this slot */
            if (!hit && n < LCSE_SLOTS) {
                cse[n].imm  = p->imm;
                cse[n].size = p->size;
                cse[n].vreg = p->rd;
                n++;
            }
        } else if (p->op == IR3_STORE) {
            int bp_imm;
            if (store_get_bp_imm(p, &bp_imm)) {
                /* Invalidate any CSE entry for this bp slot */
                for (int i = 0; i < n; ) {
                    if (cse[i].imm == bp_imm)
                        cse[i] = cse[--n];
                    else
                        i++;
                }
            }
        }
    }
}

/* ----------------------------------------------------------------
 * Sub-pass F: Loop-Invariant Code Motion for bp-relative loads.
 *
 * Algorithm:
 *   1. Scan the function to build a label-ID → IR3Inst* table.
 *   2. Find backward unconditional jumps (IR3_J whose target label was
 *      already seen) — each is a loop back edge.
 *   3. For each loop [header..backedge]:
 *      a. Collect all bp-relative store offsets and detect calls.
 *      b. Find bp-relative loads whose slot is never stored in the loop.
 *         The first occurrence of each (imm,size) pair is spliced out and
 *         inserted immediately before the loop header.  Subsequent identical
 *         loads become IR3_MOV from the hoisted vreg (cleaned up by the
 *         copy_propagate + dce that follows).
 *
 * Limitations: bp-relative loads only (phase 1); any call in the loop
 * disables all load hoisting for that loop (conservative); conditional
 * stores block hoisting for their slot.
 * ---------------------------------------------------------------- */

#define LICM_LABEL_MAX 2048
#define LICM_LOOP_MAX   16
#define LICM_SLOT_MAX   64   /* max distinct bp-store slots tracked per loop */
#define LICM_HOIST_MAX  16   /* max hoisted (imm,size) pairs per loop */

static void licm_bp_loads(IR3Inst *start, IR3Inst *end)
{
    /* Pre-check: skip functions that contain any direct or indirect calls.
     * LICM is only safe in call-free functions (e.g. matrix_sum inner loops)
     * because complex control flow from call-containing functions may place
     * loop-body stores or calls outside the linear [hdr, backedge] range. */
    for (IR3Inst *p = start; p && p != end; p = p->next)
        if (p->op == IR3_CALL || p->op == IR3_CALLR) return;

    /* Pass 1: record position of every IR3_LABEL in the function. */
    static IR3Inst *lpos[LICM_LABEL_MAX];
    memset(lpos, 0, sizeof(lpos));
    for (IR3Inst *p = start; p && p != end; p = p->next) {
        if (p->op == IR3_LABEL && (unsigned)p->imm < LICM_LABEL_MAX)
            lpos[p->imm] = p;
    }

    /* Pass 2: find backward jumps → loop descriptors. */
    struct { int hdr_id; IR3Inst *hdr; IR3Inst *backedge; } loops[LICM_LOOP_MAX];
    int n_loops = 0;

    /* Track which labels have been "passed" as we scan forward. */
    static bool label_seen[LICM_LABEL_MAX];
    memset(label_seen, 0, sizeof(label_seen));

    for (IR3Inst *p = start; p && p != end; p = p->next) {
        if (p->op == IR3_LABEL && (unsigned)p->imm < LICM_LABEL_MAX)
            label_seen[p->imm] = true;
        if (p->op == IR3_J && (unsigned)p->imm < LICM_LABEL_MAX
                && label_seen[p->imm] && lpos[p->imm] != NULL) {
            if (n_loops < LICM_LOOP_MAX) {
                loops[n_loops].hdr_id   = p->imm;
                loops[n_loops].hdr      = lpos[p->imm];
                loops[n_loops].backedge = p;
                n_loops++;
            }
        }
    }

    /* Pass 3: hoist invariant bp-loads for each loop.
     * Process innermost first (later loops in discovery order = inner). */
    for (int li = n_loops - 1; li >= 0; li--) {
        IR3Inst *hdr      = loops[li].hdr;
        IR3Inst *backedge = loops[li].backedge;

        /* Step A: collect bp-store offsets and detect calls in the loop. */
        struct { int imm; int size; } bpstore[LICM_SLOT_MAX];
        int n_store = 0;
        bool has_call = false;

        /* Scan from hdr to function end for store collection.
         * We already know has_call=false for the whole function (checked above).
         * Scanning to end (not just backedge) catches stores in out-of-line
         * loop-body code that appears after the backedge due to loop rotation. */
        for (IR3Inst *p = hdr; p && p != end; p = p->next) {
            if ((int)p->op < 0) continue;
            if (p->op == IR3_CALL || p->op == IR3_CALLR) { has_call = true; break; }
            if (p->op == IR3_STORE) {
                int bp_imm;
                if (store_get_bp_imm(p, &bp_imm)) {
                    bool dup = false;
                    for (int i = 0; i < n_store; i++)
                        if (bpstore[i].imm == bp_imm)
                            { dup = true; break; }
                    if (!dup && n_store < LICM_SLOT_MAX) {
                        bpstore[n_store].imm  = bp_imm;
                        bpstore[n_store].size = p->size;
                        n_store++;
                    }
                }
            }
        }
        if (has_call) continue;

        /* Step B+C: find invariant loads and hoist them.
         * hoisted[]: (imm,size) pairs already moved to preheader → vreg. */
        struct { int imm; int size; int vreg; } hoisted[LICM_HOIST_MAX];
        int n_hoisted = 0;

        /* Find the insertion point for hoisted instructions.
         *
         * For typical for/while loops there is a forward unconditional jump
         * to the loop header (e.g. "j _l3" from the loop initialisation
         * block) that appears in the IR3 list BEFORE the header label.
         * Inserting hoisted code between that jump and the label creates
         * unreachable dead code (the jump skips straight to the label).
         * Instead we must insert BEFORE that forward entry jump.
         *
         * Strategy: scan forward from func_start up to hdr; record the
         * node *preceding* the last IR3_J that targets hdr_id — that is
         * our insertion anchor.  Fall back to the node just before hdr if
         * no such forward entry jump exists.
         */
        IR3Inst *insert_after = NULL;   /* splice hoisted nodes after this */
        {
            IR3Inst *prev = start;
            for (IR3Inst *q = start->next; q && q != hdr; prev = q, q = q->next) {
                /* Track fallback: node immediately before hdr */
                if (q->next == hdr)
                    insert_after = q;   /* may be overwritten below */
                /* Forward entry jump: insert before it (after its predecessor) */
                if (q->op == IR3_J && (unsigned)q->imm < LICM_LABEL_MAX
                        && q->imm == loops[li].hdr_id)
                    insert_after = prev;
            }
        }
        if (!insert_after) continue;

        /* Scan the loop body for invariant bp-loads.
         * We walk with a prev pointer so we can splice. */
        IR3Inst *prev = hdr;
        IR3Inst *p    = hdr->next;
        while (p && p != backedge->next && p != end) {
            IR3Inst *next = p->next;
            if (p->op == IR3_LOAD && p->rs1 == IR3_VREG_BP && is_fresh(p->rd)) {
                /* Check slot is not in store set */
                bool stored = false;
                for (int i = 0; i < n_store; i++)
                    if (bpstore[i].imm == p->imm && bpstore[i].size == p->size)
                        { stored = true; break; }

                if (!stored) {
                    /* Check if this (imm,size) was already hoisted */
                    int hv = -1;
                    for (int i = 0; i < n_hoisted; i++)
                        if (hoisted[i].imm == p->imm && hoisted[i].size == p->size)
                            { hv = hoisted[i].vreg; break; }

                    if (hv >= 0) {
                        /* Duplicate: convert to MOV; copy_propagate removes it */
                        p->op  = IR3_MOV;
                        p->rs1 = hv;
                    } else if (n_hoisted < LICM_HOIST_MAX) {
                        /* First occurrence: splice out of loop body */
                        prev->next = next;   /* unlink p */

                        /* Insert into preheader (before the forward entry jump) */
                        p->next          = insert_after->next;
                        insert_after->next = p;
                        insert_after     = p;  /* chain subsequent hoists here */

                        hoisted[n_hoisted].imm  = p->imm;
                        hoisted[n_hoisted].size = p->size;
                        hoisted[n_hoisted].vreg = p->rd;
                        n_hoisted++;

                        /* Don't advance prev (prev->next already = next) */
                        p = next;
                        continue;
                    }
                }
            }
            prev = p;
            p    = next;
        }
    }
}

/* ----------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------- */
void ir3_optimize(IR3Inst *head, int opt_level)
{
    if (opt_level < 1) return;

    for (IR3Inst *p = head; p; ) {
        if (p->op != IR3_SYMLABEL) { p = p->next; continue; }

        IR3Inst *func_start = p;

        /* Find function end (next SYMLABEL or end of list) */
        IR3Inst *func_end = p->next;
        while (func_end && func_end->op != IR3_SYMLABEL)
            func_end = func_end->next;

        /* Only optimize functions (have ENTER), not data labels */
        bool is_func = false;
        for (IR3Inst *q = func_start->next; q && q != func_end; q = q->next) {
            if (q->op == IR3_ENTER) { is_func = true; break; }
            if (q->op == IR3_WORD || q->op == IR3_BYTE) break;
        }

        if (is_func) {
            compute_multi_def(func_start, func_end);
            for (int iter = 0; iter < 4; iter++) {
                bool changed = false;
                changed |= copy_propagate(func_start, func_end);
                changed |= const_prop_fold(func_start, func_end);
                changed |= dce(func_start, func_end);
                if (!changed) break;
            }
            /* Sub-pass G: redirect LOAD(rd=ACCUM) → MOV(v_fresh, ACCUM) pairs to
             * LOAD(rd=v_fresh) directly.  braun_ssa emits all loads through ACCUM;
             * the following MOV (from IR_PUSH) is normally eliminated by local copy_prop
             * within a BB.  This sub-pass handles cross-BB survivors where the LOAD ends
             * one BB and the MOV starts the next. */
            for (IR3Inst *q = func_start; q && q != func_end; q = q->next) {
                if (q->op != IR3_LOAD || q->rd != IR3_VREG_ACCUM) continue;
                /* Skip dead/comment nodes to find the immediate successor */
                IR3Inst *nxt = q->next;
                while (nxt && nxt != func_end &&
                       ((int)nxt->op < 0 || nxt->op == IR3_COMMENT ||
                        nxt->op == IR3_LABEL)) {
                    nxt = nxt->next;
                }
                if (!nxt || nxt == func_end) continue;
                if (nxt->op != IR3_MOV || nxt->rs1 != IR3_VREG_ACCUM) continue;
                if (!IR3_VREG_IS_VIRT(nxt->rd)) continue;
                q->rd      = nxt->rd;
                nxt->op    = IR3_COMMENT;
                nxt->rd    = IR3_VREG_NONE;
            }
            fold_const_patterns(func_start, func_end);
            /* Sub-pass D: eliminate redundant SXW/SXB chains.
             * fold_const_patterns Phase 1 built def_inst[] so this can run
             * immediately after; the resulting MOV nodes are cleaned up
             * by one more copy_propagate + dce round. */
            elim_redundant_sxt(func_start, func_end);
            /* Sub-pass E: within-BB bp-relative load CSE. */
            local_load_cse(func_start, func_end);
            /* Sub-pass F: LICM for bp-relative loads. */
            licm_bp_loads(func_start, func_end);
            copy_propagate(func_start, func_end);
            dce(func_start, func_end);
        }

        p = func_end;
    }

    compact_ir3(head);
}
