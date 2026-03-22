/* ir3.c — IR3 infrastructure: vreg counter, CFG builder, free_ir3. */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "ir3.h"

/* ----------------------------------------------------------------
 * Fresh virtual register counter.
 * IR3_VREG_ACCUM (100) is reserved for the accumulator.
 * Fresh scratch vregs are allocated from 101 upward.
 * ---------------------------------------------------------------- */
static int next_vreg = IR3_VREG_BASE + 1;  /* 101; 100 = ACCUM, reserved */

int ir3_new_vreg(void)
{
    return next_vreg++;
}

void ir3_reset(void)
{
    next_vreg = IR3_VREG_BASE + 1;
}

/* ----------------------------------------------------------------
 * build_cfg — build a basic-block CFG from a stack-IR list.
 *
 * ir_head must be an IR_SYMLABEL node for a function.
 * Scans from ir_head->next until the next IR_SYMLABEL or NULL.
 *
 * BB layout:
 *   BB 0: ir_first = ir_head (the SYMLABEL node itself), ir_last = last
 *         instruction before the first IR_BB_START (or last instruction
 *         in function if no BB_START exists).
 *   BB k (k>0): ir_first = the kth IR_BB_START; ir_last = last instruction
 *         in that block.
 *
 * translate_bb iterates from ir_first->next to ir_last->next (exclusive),
 * so for BB 0 it starts at the instruction after the SYMLABEL (= ENTER),
 * and for subsequent BBs it starts at the instruction after the IR_BB_START.
 *
 * Returns NULL and *n_blocks_out=0 if no real BBs exist.
 * ---------------------------------------------------------------- */

static int find_bb_by_label(BB *blocks, int n, int label)
{
    for (int i = 0; i < n; i++)
        if (blocks[i].label_id == label) return i;
    return -1;
}

BB *build_cfg(IRInst *ir_head, int *n_blocks_out)
{
    if (!ir_head || !ir_head->next) {
        *n_blocks_out = 0;
        return NULL;
    }

    /* Pass 1: count total BBs.
     * BB 0 always exists (it covers instructions from ir_head->next until first BB_START).
     * Each IR_BB_START starts a new BB. */
    int n_bb = 1;  /* BB 0 always exists */
    for (IRInst *p = ir_head->next; p && p->op != IR_SYMLABEL; p = p->next)
        if (p->op == IR_BB_START) n_bb++;

    BB *blocks = calloc(n_bb, sizeof(BB));
    if (!blocks) { fprintf(stderr, "build_cfg: OOM\n"); exit(1); }

    /* Pass 2: fill BB array.
     * BB 0's ir_first = ir_head (SYMLABEL); subsequent BBs' ir_first = BB_START. */
    blocks[0].id       = 0;
    blocks[0].label_id = -1;
    blocks[0].sealed   = false;
    blocks[0].ir_first = ir_head;   /* translate_bb iterates from ir_first->next */
    blocks[0].ir_last  = ir_head;   /* will be updated */

    int bb_idx = 0;
    for (IRInst *p = ir_head->next; p && p->op != IR_SYMLABEL; p = p->next) {
        if (p->op == IR_BB_START) {
            bb_idx++;
            blocks[bb_idx].id       = bb_idx;
            blocks[bb_idx].label_id = -1;
            blocks[bb_idx].sealed   = false;
            blocks[bb_idx].ir_first = p;  /* translate_bb starts from p->next */
            blocks[bb_idx].ir_last  = p;  /* will be updated */
            continue;
        }

        /* Record label_id for the block (first IR_LABEL seen) */
        if (p->op == IR_LABEL && blocks[bb_idx].label_id < 0)
            blocks[bb_idx].label_id = p->operand;

        /* Update ir_last for every non-NOP instruction */
        if (p->op != IR_NOP)
            blocks[bb_idx].ir_last = p;
    }

    /* Pass 3: build succ/pred edges based on ir_last of each BB. */
    for (int i = 0; i < n_bb; i++) {
        IRInst *last = blocks[i].ir_last;
        if (!last || last->op == IR_BB_START) {
            /* Empty block or only had a BB_START: fall-through to next */
            /* (This is a degenerate case: empty block that falls through) */
            int fall = i + 1;
            if (fall < n_bb && blocks[i].n_succs < BB_MAX_SUCCS) {
                blocks[i].succs[blocks[i].n_succs++] = fall;
                if (blocks[fall].n_preds < BB_MAX_PREDS)
                    blocks[fall].preds[blocks[fall].n_preds++] = i;
            }
            continue;
        }

        if (last->op == IR_J) {
            int tgt = find_bb_by_label(blocks, n_bb, last->operand);
            if (tgt >= 0 && blocks[i].n_succs < BB_MAX_SUCCS) {
                blocks[i].succs[blocks[i].n_succs++] = tgt;
                if (blocks[tgt].n_preds < BB_MAX_PREDS)
                    blocks[tgt].preds[blocks[tgt].n_preds++] = i;
            }
        } else if (last->op == IR_JZ || last->op == IR_JNZ) {
            int tgt  = find_bb_by_label(blocks, n_bb, last->operand);
            int fall = i + 1;
            if (fall < n_bb && blocks[i].n_succs < BB_MAX_SUCCS) {
                blocks[i].succs[blocks[i].n_succs++] = fall;
                if (blocks[fall].n_preds < BB_MAX_PREDS)
                    blocks[fall].preds[blocks[fall].n_preds++] = i;
            }
            if (tgt >= 0 && tgt != fall && blocks[i].n_succs < BB_MAX_SUCCS) {
                blocks[i].succs[blocks[i].n_succs++] = tgt;
                if (blocks[tgt].n_preds < BB_MAX_PREDS)
                    blocks[tgt].preds[blocks[tgt].n_preds++] = i;
            }
        } else if (last->op == IR_RET) {
            /* No successors */
        } else if (last->op == IR_BB_START) {
            /* Shouldn't happen (already handled above) */
        } else {
            /* Fall-through to next BB */
            int fall = i + 1;
            if (fall < n_bb && blocks[i].n_succs < BB_MAX_SUCCS) {
                blocks[i].succs[blocks[i].n_succs++] = fall;
                if (blocks[fall].n_preds < BB_MAX_PREDS)
                    blocks[fall].preds[blocks[fall].n_preds++] = i;
            }
        }
    }

    *n_blocks_out = n_bb;
    return blocks;
}

void free_cfg(BB *blocks, int n_blocks)
{
    (void)n_blocks;
    free(blocks);
}

/* ----------------------------------------------------------------
 * free_ir3 — release all nodes in an IR3Inst list.
 * ---------------------------------------------------------------- */
void free_ir3(IR3Inst *head)
{
    while (head) {
        IR3Inst *next = head->next;
        free(head);
        head = next;
    }
}
