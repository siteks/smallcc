/* ssa.c — Stack IR → 3-address SSA lift for the CPU4 RISC backend.
 *
 * lift_to_ssa() walks the existing stack-based IRInst list and produces a
 * SSAInst list with virtual register assignments (>= VREG_START):
 *
 *   VREG_START+0 = accumulator (physical r0 after regalloc)
 *   VREG_START+{depth+1} = scratch at virtual stack depth (physical r1, r2, …)
 *
 * Key invariants:
 *   • IR_PUSH / IR_PUSHW → SSA_MOV v{depth+1}, v0  then depth++
 *   • IR_ADD etc.        → SSA_ALU v0, v{depth}, v0  then depth--
 *   • IR_LEA N + IR_LW   → SSA_LOAD v0, bp+N (collapsed to one F2 load)
 *   • IR_JL              → flush virtual stack to memory first, then SSA_CALL
 *
 * regalloc() (regalloc.c) maps virtual → physical registers after this pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "smallcc.h"
#include "ssa.h"

/* ----------------------------------------------------------------
 * Virtual stack state (file-scope, reset per function)
 * ---------------------------------------------------------------- */
#define MAX_VDEPTH 8

static int vs_reg[MAX_VDEPTH];   /* virtual register holding each depth slot */
static int vs_size[MAX_VDEPTH];  /* push size in bytes (2 or 4) per slot */
static int vs_depth;             /* current virtual stack depth */
static int vsp;                  /* running sp - bp offset (starts at -frame_size) */

/* ----------------------------------------------------------------
 * Forward-branch vsp fixup table
 *
 * ssa.c processes the IR linearly, but the IR has control flow.
 * When a conditional branch (JZ/JNZ) or unconditional jump (J) is
 * processed, we record the current vsp for the target label.  When
 * that label is later encountered in the linear scan, we restore vsp
 * to the recorded value.  This prevents vsp from drifting due to
 * scope-exit IR_ADJ instructions that belong to one branch arm
 * "leaking" into the other arm.
 *
 * We only need to handle FORWARD branches (label seen after the
 * jump instruction in the linear scan).  Backward branches go to
 * labels already processed; recording their vsp is harmless because
 * those labels won't be processed again.
 *
 * All paths to a well-formed label must reach it at the same scope
 * depth, so the first recorded vsp for a label is used; subsequent
 * recordings for the same label are ignored.
 * ---------------------------------------------------------------- */
#define LABEL_VID_MAX 2048

static int  lvsp_base;                    /* label-ID of first label seen in this function */
static int  lvsp_val[LABEL_VID_MAX];      /* recorded vsp for each label offset */
static bool lvsp_set[LABEL_VID_MAX];      /* true if that entry is valid */

static void lvsp_reset(void)
{
    lvsp_base = -1;
    memset(lvsp_set, 0, sizeof(lvsp_set));
}

/* Record vsp for a jump target (only first recording wins). */
static void lvsp_record(int label_id, int vsp_val)
{
    if (lvsp_base < 0) lvsp_base = label_id;
    int idx = label_id - lvsp_base;
    if (idx >= 0 && idx < LABEL_VID_MAX && !lvsp_set[idx]) {
        lvsp_val[idx] = vsp_val;
        lvsp_set[idx] = true;
    }
}

/* Restore vsp from table when a label is reached (no-op if not recorded). */
static void lvsp_apply(int label_id)
{
    if (lvsp_base < 0) return;
    int idx = label_id - lvsp_base;
    if (idx >= 0 && idx < LABEL_VID_MAX && lvsp_set[idx])
        vsp = lvsp_val[idx];
}

/* ----------------------------------------------------------------
 * Post-call restore state
 *
 * When a call has outer expression temporaries in scratch registers
 * (e.g., n * fact(n-1): n lives in r1 while fact is called),
 * those registers are spilled to memory before the call and must
 * be reloaded after the caller-side cleanup ADJ.
 * ---------------------------------------------------------------- */
static int spill_reg[MAX_VDEPTH];  /* scratch register to restore to */
static int spill_off[MAX_VDEPTH];  /* bp-relative byte offset of the spilled value */
static int spill_sz[MAX_VDEPTH];   /* size in bytes */
static int spill_count;            /* number of spilled outer temps */
static int spill_total_bytes;      /* total bytes of outer-temp spill area */
static int await_post_call_adj;    /* set when post-call restore is pending */

/* Output list */
static SSAInst  *ssa_head;
static SSAInst **ssa_tail;

/* ----------------------------------------------------------------
 * Helper: allocate a new SSAInst (unlinked)
 * ---------------------------------------------------------------- */
static SSAInst *new_ssa(SSAOp op)
{
    SSAInst *s = calloc(1, sizeof(SSAInst));
    if (!s) { fprintf(stderr, "ssa: out of memory\n"); exit(1); }
    s->op  = op;
    s->rd  = -1;
    s->rs1 = -1;
    s->rs2 = -1;
    return s;
}

/* Append a node to the output list */
static SSAInst *emit_s(SSAInst *s)
{
    *ssa_tail = s;
    ssa_tail  = &s->next;
    return s;
}

/* Allocate + append in one step */
static SSAInst *emit_op(SSAOp op)
{
    return emit_s(new_ssa(op));
}

/* ----------------------------------------------------------------
 * flush_for_call_n — spill outer expression temps + argument values
 * to the memory stack before a call.
 *
 * Memory layout produced (stack grows downward, vsp = sp - bp):
 *
 *   [vsp_before]   ← old sp
 *   outer_temp[0]  size=vs_size[0]   ← spilled FIRST (highest addresses)
 *   outer_temp[1]  ...
 *   arg[last]      size=vs_size[first_arg+n_args-1]
 *   ...
 *   arg[0]         size=vs_size[first_arg]   ← new sp (lowest address)
 *
 * This ensures args are at (new_sp, new_sp+sz, …), so after enter N
 * in the callee, arg[0] lands at callee_bp + 8.
 *
 * After the call, the caller's IR_ADJ +arg_bytes reclaims the arg area.
 * We set await_post_call_adj so the ADJ handler then emits loads to
 * restore the outer temps and a second ADJ to reclaim their area.
 * ---------------------------------------------------------------- */
static void flush_for_call_n(int arg_bytes, int line)
{
    /* Reset any stale post-call restore state. */
    spill_count       = 0;
    spill_total_bytes = 0;
    await_post_call_adj = 0;

    if (vs_depth == 0) return;

    /* ---- determine boundary between outer temps and args ---- */
    int n_args   = 0;
    int arg_sum  = 0;
    for (int i = vs_depth - 1; i >= 0 && arg_sum < arg_bytes; i--) {
        arg_sum += vs_size[i];
        n_args++;
    }
    int first_arg = vs_depth - n_args;   /* # of outer expression temps */

    /* ---- Step 1: spill outer expression temps to memory ---- *
     * They go at HIGHER addresses (pushed first, so they're above the args).
     * This lets the args land exactly at the new sp after step 2.         */
    if (first_arg > 0) {
        int outer_bytes = 0;
        for (int k = 0; k < first_arg; k++) outer_bytes += vs_size[k];

        SSAInst *adj = emit_op(SSA_ADJ);
        adj->imm  = -outer_bytes;
        adj->line = line;
        vsp -= outer_bytes;

        /* Store outer temps: outer_temp[0] at lowest address of this area,
         * outer_temp[first_arg-1] at highest. */
        int boff = vsp;
        for (int k = 0; k < first_arg; k++) {
            SSAInst *st = emit_op(SSA_STORE);
            st->rd   = -2;
            st->rs1  = vs_reg[k];
            st->imm  = boff;
            st->size = vs_size[k];
            st->line = line;
            spill_reg[spill_count] = vs_reg[k];
            spill_off[spill_count] = boff;
            spill_sz [spill_count] = vs_size[k];
            spill_count++;
            boff += vs_size[k];
        }
        spill_total_bytes   = outer_bytes;
        await_post_call_adj = 1;
    }

    /* ---- Step 2: flush argument values to memory ---- *
     * Args go at the bottom (lowest addresses = new sp).               */
    if (arg_sum > 0) {
        SSAInst *adj = emit_op(SSA_ADJ);
        adj->imm  = -arg_sum;
        adj->line = line;
        vsp -= arg_sum;

        /* Store args: last-pushed arg (vs_reg[vs_depth-1]) at lowest addr. */
        int boff = vsp;
        for (int k = vs_depth - 1; k >= first_arg; k--) {
            SSAInst *st = emit_op(SSA_STORE);
            st->rd   = -2;
            st->rs1  = vs_reg[k];
            st->imm  = boff;
            st->size = vs_size[k];
            st->line = line;
            boff += vs_size[k];
        }
    }

    vs_depth = 0;   /* all items are now in memory */
}

/* ----------------------------------------------------------------
 * lift_to_ssa — main conversion pass
 * ---------------------------------------------------------------- */
SSAInst *lift_to_ssa(IRInst *ir_head)
{
    ssa_head = NULL;
    ssa_tail = &ssa_head;
    vs_depth = 0;
    vsp      = 0;

    for (IRInst *p = ir_head; p; p = p->next)
    {
        SSAInst *s;

        switch (p->op)
        {
        /* Peephole barriers — skip */
        case IR_NOP:
        case IR_BB_START:
            break;

        /* ---- Immediate loads ---- */
        case IR_IMM:
            s = emit_op(p->sym ? SSA_MOVSYM : SSA_MOVI);
            s->rd   = VREG_START + 0;
            s->imm  = p->operand;
            s->sym  = p->sym;
            s->line = p->line;
            break;

        /* ---- Address computation ---- */
        case IR_LEA:
            /* Peek: if the very next instruction is a load, collapse to
             * a single bp-relative load (F2), consuming the load node.  */
            if (p->next)
            {
                int sz = 0;
                if      (p->next->op == IR_LW) sz = 2;
                else if (p->next->op == IR_LB) sz = 1;
                else if (p->next->op == IR_LL) sz = 4;
                if (sz)
                {
                    s = emit_op(SSA_LOAD);
                    s->rd   = VREG_START + 0;
                    s->rs1  = -2;         /* -2 = bp-relative (F2) */
                    s->imm  = p->operand; /* byte offset from bp   */
                    s->size = sz;
                    s->line = p->line;
                    s->sym  = p->sym;     /* variable name for -ann annotation */
                    p = p->next;          /* consume the LW/LB/LL  */
                    break;
                }
            }
            /* Standalone LEA: compute bp-relative address into accumulator */
            s = emit_op(SSA_LEA);
            s->rd   = VREG_START + 0;
            s->imm  = p->operand;
            s->line = p->line;
            break;

        /* ---- Loads (not collapsed by LEA peek) ---- */
        case IR_LW: case IR_LB: case IR_LL:
        {
            /* Address is already in accumulator (from a preceding LEA + possible other ops) */
            int sz = (p->op == IR_LW) ? 2 : (p->op == IR_LB) ? 1 : 4;
            s = emit_op(SSA_LOAD);
            s->rd   = VREG_START + 0;
            s->rs1  = VREG_START + 0;  /* base = accumulator (register-relative, imm=0) */
            s->imm  = 0;
            s->size = sz;
            s->line = p->line;
            break;
        }

        /* ---- Virtual stack push (save accumulator to scratch) ---- */
        case IR_PUSH:
        case IR_PUSHW:
        {
            int sz  = (p->op == IR_PUSH) ? 4 : 2;
            int reg = VREG_START + vs_depth + 1;
            if (reg > VREG_START + 7)
            {
                fprintf(stderr, "ssa: virtual stack overflow at depth %d\n", vs_depth);
                reg = VREG_START + 7;   /* clobber v15; SU labelling makes depth <= 3 in practice */
            }
            s = emit_op(SSA_MOV);
            s->rd   = reg;
            s->rs1  = VREG_START + 0;  /* save accumulator */
            s->line = p->line;
            vs_reg[vs_depth]  = reg;
            vs_size[vs_depth] = sz;
            vs_depth++;
            break;
        }

        /* ---- Virtual stack pop (restore accumulator from scratch) ---- */
        case IR_POP:
        case IR_POPW:
            if (vs_depth > 0) {
                vs_depth--;
                s = emit_op(SSA_MOV);
                s->rd   = VREG_START + 0;
                s->rs1  = vs_reg[vs_depth];
                s->line = p->line;
            }
            break;

        /* ---- Stores: addr in scratch top, value in accumulator ---- */
        case IR_SW: case IR_SB: case IR_SL:
            if (vs_depth > 0)
            {
                int sz = (p->op == IR_SW) ? 2 : (p->op == IR_SB) ? 1 : 4;
                vs_depth--;
                s = emit_op(SSA_STORE);
                s->rd   = vs_reg[vs_depth];  /* register holding the address */
                s->rs1  = VREG_START + 0;    /* accumulator = value to store */
                s->imm  = 0;                 /* no extra offset */
                s->size = sz;
                s->line = p->line;
            }
            break;

        /* ---- Binary ALU: accum = scratch_top OP accum; pop ---- */
        case IR_ADD: case IR_SUB: case IR_MUL: case IR_DIV: case IR_MOD:
        case IR_AND: case IR_OR:  case IR_XOR: case IR_SHL: case IR_SHR:
        case IR_EQ:  case IR_NE:  case IR_LT:  case IR_LE:  case IR_GT:  case IR_GE:
        case IR_LTS: case IR_LES: case IR_GTS: case IR_GES:
        case IR_DIVS: case IR_MODS: case IR_SHRS:
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV:
        case IR_FLT:  case IR_FLE:  case IR_FGT:  case IR_FGE:
            if (vs_depth > 0)
            {
                vs_depth--;
                s = emit_op(SSA_ALU);
                s->alu_op = p->op;
                s->rd     = VREG_START + 0;
                s->rs1    = vs_reg[vs_depth];  /* left operand (was on vstack) */
                s->rs2    = VREG_START + 0;    /* right operand = accumulator */
                s->line   = p->line;
            }
            break;

        /* ---- Single-register ops: sxb, sxw, itof, ftoi ---- */
        case IR_SXB: case IR_SXW: case IR_ITOF: case IR_FTOI:
            s = emit_op(SSA_ALU1);
            s->alu_op = p->op;
            s->rd     = VREG_START + 0;
            s->rs1    = VREG_START + 0;
            s->line   = p->line;
            break;

        /* ---- Function calls: flush only argument slots ---- */
        case IR_JL:
        case IR_JLI:
        {
            /* Determine how many argument bytes to flush by peeking at
             * the IR_ADJ +N that follows this call (caller cleanup). */
            int arg_bytes = 0;
            for (IRInst *q = p->next; q; q = q->next) {
                if (q->op == IR_NOP || q->op == IR_BB_START) continue;
                if (q->op == IR_ADJ && q->operand > 0) { arg_bytes = q->operand; break; }
                /* Any other terminator: stop searching */
                if (q->op == IR_JL  || q->op == IR_JLI || q->op == IR_RET ||
                    q->op == IR_J   || q->op == IR_JZ  || q->op == IR_JNZ) break;
            }

            /* For indirect calls (JLI), the function pointer lives in r0.
             * flush_for_call_n spills outer expression temps to memory; the
             * risc_backend fallback for out-of-F2-range stores uses 'lea r0, off'
             * as a scratch address register, which clobbers r0.
             * Guard: copy r0 to physical r4 (never assigned by the depth-based
             * allocator) before flushing, then restore r0 from r4 afterward.
             * r4 is caller-saved scratch, so this is safe across the call site. */
            bool save_fp = (p->op == IR_JLI && vs_depth > 0);
            if (save_fp) {
                SSAInst *mv = emit_op(SSA_MOV);
                mv->rd   = 4;              /* physical r4 */
                mv->rs1  = VREG_START + 0; /* accumulator */
                mv->line = p->line;
            }

            flush_for_call_n(arg_bytes, p->line);

            if (save_fp) {
                SSAInst *mv = emit_op(SSA_MOV);
                mv->rd   = VREG_START + 0; /* accumulator */
                mv->rs1  = 4;              /* physical r4 */
                mv->line = p->line;
            }

            if (p->op == IR_JL) {
                s = emit_op(SSA_CALL);
                s->sym  = p->sym;
            } else {
                s = emit_op(SSA_CALLR);
            }
            s->line = p->line;

            /* For 0-arg calls with spilled outer expression temps, restore
             * immediately.  The normal deferred path waits for a post-call
             * IR_ADJ +N (caller cleanup), but peephole deletes IR_ADJ 0
             * before lift_to_ssa() runs, so that ADJ never arrives and
             * vsp drifts permanently.  Detect this case and fix it now. */
            if (arg_bytes == 0 && spill_count > 0) {
                await_post_call_adj = 0;
                for (int k = 0; k < spill_count; k++) {
                    SSAInst *ld = emit_op(SSA_LOAD);
                    ld->rd   = spill_reg[k];
                    ld->rs1  = -2;
                    ld->imm  = spill_off[k];
                    ld->size = spill_sz[k];
                    ld->line = p->line;
                }
                SSAInst *adj2 = emit_op(SSA_ADJ);
                adj2->imm  = spill_total_bytes;
                adj2->line = p->line;
                vsp += spill_total_bytes;
                for (int k = 0; k < spill_count; k++) {
                    vs_reg[k]  = spill_reg[k];
                    vs_size[k] = spill_sz[k];
                }
                vs_depth      = spill_count;
                spill_count   = 0;
                spill_total_bytes = 0;
            }
            break;
        }

        /* ---- Frame management ---- */
        case IR_ENTER:
            vsp = -(int)p->operand;  /* sp = bp - frame_size after enter */
            s = emit_op(SSA_ENTER);
            s->imm  = p->operand;
            s->line = p->line;
            break;

        case IR_ADJ:
            vsp += p->operand;
            s = emit_op(SSA_ADJ);
            s->imm  = p->operand;
            s->line = p->line;

            /* Post-call restore: after the caller's arg-cleanup ADJ (+N),
             * reload spilled outer expression temps from memory, then
             * emit a second ADJ to reclaim the outer-temp spill area.   */
            if (await_post_call_adj && p->operand > 0 && spill_count > 0) {
                await_post_call_adj = 0;
                /* Reload outer temps; they are at (spill_off[k]) from bp,
                 * which is unchanged relative to bp regardless of sp.    */
                for (int k = 0; k < spill_count; k++) {
                    SSAInst *ld = emit_op(SSA_LOAD);
                    ld->rd   = spill_reg[k];
                    ld->rs1  = -2;              /* bp-relative */
                    ld->imm  = spill_off[k];    /* byte offset from bp */
                    ld->size = spill_sz[k];
                    ld->line = p->line;
                }
                /* Free the spill area on the memory stack. */
                SSAInst *adj2 = emit_op(SSA_ADJ);
                adj2->imm  = spill_total_bytes;
                adj2->line = p->line;
                vsp += spill_total_bytes;

                /* Restore virtual stack so subsequent code can use them. */
                for (int k = 0; k < spill_count; k++) {
                    vs_reg[k]  = spill_reg[k];
                    vs_size[k] = spill_sz[k];
                }
                vs_depth = spill_count;

                spill_count       = 0;
                spill_total_bytes = 0;
            }
            break;

        case IR_RET:
            emit_op(SSA_RET)->line = p->line;
            break;

        /* ---- Control flow ---- */
        case IR_J:
            lvsp_record(p->operand, vsp);
            s = emit_op(SSA_J);
            s->imm  = p->operand;
            s->line = p->line;
            break;

        case IR_JZ:
            lvsp_record(p->operand, vsp);
            s = emit_op(SSA_JZ);
            s->imm  = p->operand;
            s->line = p->line;
            break;

        case IR_JNZ:
            lvsp_record(p->operand, vsp);
            s = emit_op(SSA_JNZ);
            s->imm  = p->operand;
            s->line = p->line;
            break;

        /* ---- Labels ---- */
        case IR_LABEL:
            /* Restore vsp to the value recorded when a branch to this
             * label was processed.  This corrects drift caused by the
             * linear scan traversing a scope-exit IR_ADJ that belongs
             * to a sibling branch arm (e.g. the non-'%' arm of an
             * if/else inside a while loop). */
            lvsp_apply(p->operand);
            s = emit_op(SSA_LABEL);
            s->imm  = p->operand;
            s->line = p->line;
            break;

        case IR_SYMLABEL:
            /* Function entry: reset virtual stack, spill state, and vsp table */
            vs_depth            = 0;
            vsp                 = 0;
            spill_count         = 0;
            spill_total_bytes   = 0;
            await_post_call_adj = 0;
            lvsp_reset();
            s = emit_op(SSA_SYMLABEL);
            s->sym  = p->sym;
            s->line = p->line;
            break;

        /* ---- Data section ---- */
        case IR_WORD:
            s = emit_op(SSA_WORD);
            s->imm  = p->operand;
            s->sym  = p->sym;
            s->line = p->line;
            break;

        case IR_BYTE:
            s = emit_op(SSA_BYTE);
            s->imm  = p->operand;
            s->line = p->line;
            break;

        case IR_ALIGN:
            emit_op(SSA_ALIGN)->line = p->line;
            break;

        /* ---- Misc ---- */
        case IR_PUTCHAR:
            emit_op(SSA_PUTCHAR)->line = p->line;
            break;

        case IR_COMMENT:
            s = emit_op(SSA_COMMENT);
            s->sym  = p->sym;
            s->line = p->line;
            break;

        default:
            fprintf(stderr, "ssa: unhandled IR op %d\n", (int)p->op);
            break;
        }
    }

    return ssa_head;
}

/* ----------------------------------------------------------------
 * free_ssa — release all nodes in a SSAInst list
 * ---------------------------------------------------------------- */
void free_ssa(SSAInst *head)
{
    while (head)
    {
        SSAInst *next = head->next;
        free(head);
        head = next;
    }
}
