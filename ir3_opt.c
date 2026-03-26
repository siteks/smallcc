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

static int  copy_of[VMAP_SIZE];     /* root vreg, or -1 */
static bool cval_valid[VMAP_SIZE];  /* true if constant value known */
static int  cval[VMAP_SIZE];        /* known constant */
static int  use_count[VMAP_SIZE];   /* reference count for DCE */

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
    memset(copy_of, -1, sizeof(copy_of));
    int accum_copy = -1;     /* fresh vreg that ACCUM is a copy of, or -1 */

    for (IR3Inst *p = start; p && p != end; p = p->next) {
        /* Reset at BB boundaries */
        if (is_bb_leader_or_term(p->op)) {
            memset(copy_of, -1, sizeof(copy_of));
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
            memset(copy_of, -1, sizeof(copy_of));
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
    memset(cval_valid, 0, sizeof(cval_valid));
    bool accum_cval_valid = false;
    int  accum_cval = 0;

    for (IR3Inst *p = start; p && p != end; p = p->next) {
        /* Reset at BB boundaries */
        if (is_bb_leader_or_term(p->op)) {
            memset(cval_valid, 0, sizeof(cval_valid));
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

        if (is_fresh(p->rd) && vidx_ok(p->rd) &&
            use_count[vidx(p->rd)] == 0 &&
            is_side_effect_free(p->op))
        {
            /* Decrement use counts of operands (may cascade) */
            if (is_fresh(p->rs1) && vidx_ok(p->rs1))
                use_count[vidx(p->rs1)]--;
            if (is_fresh(p->rs2) && vidx_ok(p->rs2))
                use_count[vidx(p->rs2)]--;

            /* Unlink and free */
            if (prev)
                prev->next = next;
            free(p);
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
            free(p);
        } else {
            prev = p;
        }
        p = next;
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
            for (int iter = 0; iter < 4; iter++) {
                bool changed = false;
                changed |= copy_propagate(func_start, func_end);
                changed |= const_prop_fold(func_start, func_end);
                changed |= dce(func_start, func_end);
                if (!changed) break;
            }
        }

        p = func_end;
    }

    compact_ir3(head);
}
