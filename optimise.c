
#include "smallcc.h"

/*
 * Peephole optimiser — operates on the IR list after mark_basic_blocks().
 *
 * IR_BB_START nodes are hard barriers: patterns never span across them.
 * IR_NOP      nodes are soft deletes: next_inbb() skips them transparently.
 *
 * -O1 patterns (algebraic / structural):
 *   1. Constant fold:   IMM K1; PUSH; IMM K2; binop  →  IMM (K1 op K2)
 *   2. Const sign-ext:  IMM K; SXB  →  IMM sext8(K)
 *                       IMM K; SXW  →  IMM sext16(K)
 *   3. Dead jump:       J/JZ/JNZ Lx; [BB_START]; LABEL Lx  →  kill branch
 *   4. Merge adj:       ADJ N; ADJ M  →  ADJ N+M  (when sum fits int8)
 *   5. Kill zero adj:   ADJ 0  →  kill
 *   9. Store/reload:    LEA N; PUSH; <expr>; SW; LEA N; LW  →  drop LEA+LW
 *                       IMM s; PUSH; <expr>; SW; IMM s; LW  →  drop IMM+LW
 *      (after any store r0 holds the stored value; reload from same addr is redundant)
 *
 * -O2 additional patterns:
 *   6. Mul-by-1:        PUSH; IMM 1; MUL  →  kill all three
 *   7. Mul-by-pow2:     PUSH; IMM 2^k; MUL  →  PUSH; IMM k; SHL
 *      (lets constant folding collapse it when the pushed value is also a constant)
 *   8. Zero add/sub:    PUSH; IMM 0; ADD/SUB  →  kill all three
 *
 * The pass repeats (up to 20 times) until stable, then compacts out NOP nodes.
 */

/* Mark an instruction as deleted; emits nothing in the backend. */
static void kill(IRInst *p)
{
    p->op      = IR_NOP;
    p->operand = 0;
    p->sym     = NULL;
}

/*
 * Return the next instruction in the same basic block as p, skipping over
 * IR_NOP (deleted) and IR_COMMENT nodes.  Returns NULL if a BB boundary
 * (IR_BB_START) or the end of the list is reached first.
 */
static IRInst *next_inbb(IRInst *p)
{
    if (!p) return NULL;
    IRInst *q = p->next;
    while (q && (q->op == IR_NOP || q->op == IR_COMMENT))
        q = q->next;
    if (!q || q->op == IR_BB_START) return NULL;
    return q;
}

/* True when p is a non-symbolic (plain integer) immediate. */
static bool is_imm(IRInst *p)
{
    return p && p->op == IR_IMM && !p->sym;
}

/* True when op is a constant-foldable binary operation. */
static bool is_foldable_binop(IROp op)
{
    switch (op)
    {
    case IR_ADD: case IR_SUB: case IR_MUL:
    case IR_AND: case IR_OR:  case IR_XOR:
    case IR_SHL: case IR_SHR: case IR_SHRS:
    case IR_EQ:  case IR_NE:
    case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:
    case IR_LTS: case IR_LES: case IR_GTS: case IR_GES:
    case IR_DIVS: case IR_MODS:
        return true;
    default:
        return false;
    }
}

/* Evaluate a foldable binary op on two 32-bit values. */
static uint32_t fold_op(IROp op, uint32_t lhs, uint32_t rhs)
{
    int32_t sl = (int32_t)lhs, sr = (int32_t)rhs;
    switch (op)
    {
    case IR_ADD:  return lhs + rhs;
    case IR_SUB:  return lhs - rhs;
    case IR_MUL:  return (uint32_t)(sl * sr);
    case IR_AND:  return lhs & rhs;
    case IR_OR:   return lhs | rhs;
    case IR_XOR:  return lhs ^ rhs;
    case IR_SHL:  return lhs << (rhs & 31);
    case IR_SHR:  return lhs >> (rhs & 31);          /* logical right shift */
    case IR_SHRS: return (uint32_t)(sl >> (rhs & 31)); /* arithmetic right shift */
    case IR_EQ:   return (lhs == rhs) ? 1 : 0;
    case IR_NE:   return (lhs != rhs) ? 1 : 0;
    case IR_LT:   return (lhs < rhs)  ? 1 : 0;       /* unsigned */
    case IR_LE:   return (lhs <= rhs) ? 1 : 0;
    case IR_GT:   return (lhs > rhs)  ? 1 : 0;
    case IR_GE:   return (lhs >= rhs) ? 1 : 0;
    case IR_LTS:  return (sl < sr)    ? 1 : 0;       /* signed */
    case IR_LES:  return (sl <= sr)   ? 1 : 0;
    case IR_GTS:  return (sl > sr)    ? 1 : 0;
    case IR_GES:  return (sl >= sr)   ? 1 : 0;
    case IR_DIVS: return (uint32_t)(sr ? sl / sr : 0);
    case IR_MODS: return (uint32_t)(sr ? sl % sr : 0);
    default:      return 0;
    }
}

/* One sweep through the IR list applying all peephole rules at the given level.
   Returns true if at least one instruction was changed. */
static bool peephole_pass(int level)
{
    bool changed = false;

    for (IRInst *p = codegen_ctx.ir_head; p; p = p->next)
    {
        /* Skip structural / deleted nodes. */
        if (p->op == IR_BB_START || p->op == IR_NOP || p->op == IR_COMMENT)
            continue;

        IRInst *n1 = next_inbb(p);
        IRInst *n2 = n1 ? next_inbb(n1) : NULL;
        IRInst *n3 = n2 ? next_inbb(n2) : NULL;

        /* ----------------------------------------------------------------
         * Rule 1: constant fold — IMM K1; PUSH; IMM K2; binop → IMM result
         * ---------------------------------------------------------------- */
        if (is_imm(p) && n1 && n1->op == IR_PUSH &&
            is_imm(n2) && n3 && is_foldable_binop(n3->op))
        {
            uint32_t result = fold_op(n3->op,
                                      (uint32_t)p->operand,
                                      (uint32_t)n2->operand);
            p->operand = (int)result;
            kill(n1); kill(n2); kill(n3);
            changed = true;
            continue;
        }

        /* ----------------------------------------------------------------
         * Rule 2: constant sign-extend
         *   IMM K; SXB → IMM sext8(K)
         *   IMM K; SXW → IMM sext16(K)
         * ---------------------------------------------------------------- */
        if (is_imm(p))
        {
            if (n1 && n1->op == IR_SXB)
            {
                p->operand = (int)(int8_t)(p->operand & 0xff);
                kill(n1);
                changed = true;
                continue;
            }
            if (n1 && n1->op == IR_SXW)
            {
                p->operand = (int)(int16_t)(p->operand & 0xffff);
                kill(n1);
                changed = true;
                continue;
            }
        }

        /* ----------------------------------------------------------------
         * Rule 3: dead jump — J/JZ/JNZ Lx immediately before LABEL Lx
         *
         * A branch that targets the very next label is a no-op regardless
         * of condition.  We look past the BB_START that mark_basic_blocks()
         * inserted after the branch.
         * ---------------------------------------------------------------- */
        if (p->op == IR_J || p->op == IR_JZ || p->op == IR_JNZ)
        {
            /* Skip NOP/comment nodes, then expect BB_START. */
            IRInst *q = p->next;
            while (q && (q->op == IR_NOP || q->op == IR_COMMENT))
                q = q->next;
            if (q && q->op == IR_BB_START)
            {
                /* Skip NOP/comment nodes again to reach the label. */
                IRInst *lbl = q->next;
                while (lbl && (lbl->op == IR_NOP || lbl->op == IR_COMMENT))
                    lbl = lbl->next;
                if (lbl && lbl->op == IR_LABEL && lbl->operand == p->operand)
                {
                    kill(p);
                    changed = true;
                    continue;
                }
            }
        }

        /* ----------------------------------------------------------------
         * Rule 4: merge adjacent adj — ADJ N; ADJ M → ADJ N+M
         * ---------------------------------------------------------------- */
        if (p->op == IR_ADJ && n1 && n1->op == IR_ADJ)
        {
            int sum = p->operand + n1->operand;
            if (sum == 0)
            {
                kill(p); kill(n1);
                changed = true;
                continue;
            }
            if (sum >= -128 && sum <= 127)
            {
                p->operand = sum;
                kill(n1);
                changed = true;
                continue;
            }
        }

        /* ----------------------------------------------------------------
         * Rule 5: kill zero adj — ADJ 0 → delete
         * ---------------------------------------------------------------- */
        if (p->op == IR_ADJ && p->operand == 0)
        {
            kill(p);
            changed = true;
            continue;
        }

        /* ----------------------------------------------------------------
         * Rule 9: store/reload elimination (CPU3 only)
         *
         * This rule exploits the fact that after SW/SB/SL, r0 still holds
         * the stored value (CPU3 stack-machine invariant).  On CPU4, values
         * live in named registers and this invariant does not hold.
         *
         * Pattern: LEA N; PUSH; <expr>; SW;  LEA N; LW  → drop LEA N; LW
         *          IMM s; PUSH; <expr>; SW;  IMM s; LW  → drop IMM s; LW
         *          (and likewise for SB/LB and SL/LL)
         *
         * Correctness guard: scan forward from the PUSH tracking expression
         * stack depth to confirm the store really consumes this address push
         * (depth reaches 0 at SW/SB/SL).  Abort at function calls (JL/JLI)
         * because they may clobber memory, and at BB boundaries.
         * ---------------------------------------------------------------- */
        if (g_target_arch == 3 &&
            (p->op == IR_LEA || (p->op == IR_IMM && p->sym)) &&
            n1 && n1->op == IR_PUSH)
        {
            int depth = 1;
            IRInst *store_inst = NULL;
            IRInst *scan = next_inbb(n1);
            int limit = 64;
            while (scan && depth >= 1 && limit-- > 0)
            {
                switch (scan->op)
                {
                case IR_PUSH: case IR_PUSHW:      depth++; break;
                case IR_POP:  case IR_POPW:       depth--; break;
                /* Binary ops pop their left operand from the stack */
                case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
                case IR_AND: case IR_OR:  case IR_XOR: case IR_SHL: case IR_SHR:
                case IR_EQ:  case IR_NE:  case IR_LT:  case IR_LE:
                case IR_GT:  case IR_GE:
                case IR_LTS: case IR_LES: case IR_GTS: case IR_GES:
                case IR_DIVS: case IR_MODS: case IR_SHRS:
                case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
                case IR_FLT:  case IR_FLE:  case IR_FGT:  case IR_FGE:
                    depth--;
                    break;
                case IR_SW: case IR_SB: case IR_SL:
                    depth--;
                    if (depth == 0) store_inst = scan;
                    break;
                /* Function calls may clobber memory — abort */
                case IR_JL: case IR_JLI:
                    scan = NULL;
                    break;
                default:
                    break;
                }
                if (store_inst || !scan) break;
                scan = next_inbb(scan);
            }

            if (store_inst)
            {
                IROp expect_load = (store_inst->op == IR_SW) ? IR_LW :
                                   (store_inst->op == IR_SB) ? IR_LB : IR_LL;
                IRInst *r1 = next_inbb(store_inst);
                IRInst *r2 = r1 ? next_inbb(r1) : NULL;
                bool addr_match = false;
                if (r1 && r2 && r2->op == expect_load)
                {
                    if (p->op == IR_LEA && r1->op == IR_LEA &&
                        p->operand == r1->operand)
                        addr_match = true;
                    else if (p->op == IR_IMM && p->sym &&
                             r1->op == IR_IMM && r1->sym &&
                             strcmp(p->sym, r1->sym) == 0)
                        addr_match = true;
                }
                if (addr_match)
                {
                    kill(r1); kill(r2);
                    changed = true;
                    continue;
                }
            }
        }

        /* ----------------------------------------------------------------
         * -O2 and above rules
         * ---------------------------------------------------------------- */
        if (level >= 2)
        {
            /* Rule 6: mul-by-1 — PUSH; IMM 1; MUL → no-op (r0 unchanged)
             *
             * Before: r0 = X. PUSH saves X. IMM 1 → r0 = 1. MUL → X*1 = X.
             * Net effect: r0 = X unchanged, stack unchanged. */
            if (p->op == IR_PUSH && is_imm(n1) && (uint32_t)n1->operand == 1 &&
                n2 && n2->op == IR_MUL)
            {
                kill(p); kill(n1); kill(n2);
                changed = true;
                continue;
            }

            /* Rule 7: mul-by-power-of-2 → shift left
             *   PUSH; IMM 2^k; MUL → PUSH; IMM k; SHL
             *
             * The rewritten form lets constant folding fire in the next pass
             * if the pushed value was also a constant. */
            if (p->op == IR_PUSH && is_imm(n1) && n2 && n2->op == IR_MUL)
            {
                uint32_t v = (uint32_t)n1->operand;
                if (v > 1 && (v & (v - 1)) == 0)   /* v is a power of 2 > 1 */
                {
                    int shift = 0;
                    while ((v >> shift) > 1) shift++;
                    n1->operand = shift;
                    n2->op      = IR_SHL;
                    changed = true;
                    continue;
                }
            }

            /* Rule 8: zero add/sub — PUSH; IMM 0; ADD/SUB → no-op
             *
             * Before: r0 = X. PUSH saves X. IMM 0. ADD/SUB → X±0 = X.
             * Net effect: r0 = X unchanged, stack unchanged. */
            if (p->op == IR_PUSH && is_imm(n1) && (uint32_t)n1->operand == 0 &&
                n2 && (n2->op == IR_ADD || n2->op == IR_SUB))
            {
                kill(p); kill(n1); kill(n2);
                changed = true;
                continue;
            }
        }
    }

    return changed;
}

/* Remove all IR_NOP nodes from the list, relinking around them. */
static void compact_ir(void)
{
    /* Find the new real head. */
    IRInst *head = codegen_ctx.ir_head;
    while (head && head->op == IR_NOP)
        head = head->next;
    codegen_ctx.ir_head = head;

    /* Relink, skipping NOP nodes. */
    for (IRInst *p = head; p; p = p->next)
        while (p->next && p->next->op == IR_NOP)
            p->next = p->next->next;

    /* Update ir_tail. */
    IRInst *last = codegen_ctx.ir_head;
    if (last)
        while (last->next) last = last->next;
    codegen_ctx.ir_tail = last;
}

void peephole(int level)
{
    if (level == 0) return;

    /* Iterate until the IR stops changing (with a safety limit). */
    for (int pass = 0; pass < 20; pass++)
        if (!peephole_pass(level)) break;

    compact_ir();
}
