#include "opt.h"
#include "ssa.h"

/*
 * opt.c — Post-OOS SSA-level optimisation passes
 *
 * R2A  opt_fold_branches():    Replace IK_BR with a compile-time-constant
 *      condition by IK_JMP to the taken target.  Removes the dead predecessor
 *      edge from the dropped successor.
 *
 * R2B  opt_remove_dead_blocks(): Remove blocks with npreds == 0 that are not
 *      the entry block.  Strips their outgoing edges and iterates until stable.
 *
 * R2D  opt_copy_prop():        Collapse IK_COPY chains post-OOS, before IRC.
 *      Sets Value.alias for each copy destination so val_resolve() chains
 *      through them, then rewrites all operand references and recounts
 *      use_count.  IRC DCE then removes copies whose dst use_count == 0.
 *      Copies whose resolved source is a call landing (IK_CALL/IK_ICALL),
 *      param landing (IK_PARAM), or another IK_COPY are left intact —
 *      collapsing them would extend constrained live ranges or mishandle
 *      OOS swap-cycle temporaries.  Note: phys_reg is assigned by legalize
 *      and irc_allocate which run after this pass, so phys_reg cannot be
 *      used as a guard here.
 */

// ── R2A: Constant-condition branch folding ────────────────────────────────

void opt_fold_branches(Function *f) {
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b    = f->blocks[bi];
        Inst  *term = b->tail;
        if (!term || term->kind != IK_BR || term->nops < 1) continue;

        Value *cond = val_resolve(term->ops[0]);
        if (!cond || cond->kind != VAL_CONST) continue;

        Block *taken   = cond->iconst ? term->target  : term->target2;
        Block *dropped = cond->iconst ? term->target2 : term->target;

        // Remove b from dropped->preds
        for (int k = 0; k < dropped->npreds; k++) {
            if (dropped->preds[k] == b) {
                dropped->preds[k] = dropped->preds[--dropped->npreds];
                break;
            }
        }
        // Remove dropped from b->succs
        for (int k = 0; k < b->nsuccs; k++) {
            if (b->succs[k] == dropped) {
                b->succs[k] = b->succs[--b->nsuccs];
                break;
            }
        }

        // Rewrite terminator to unconditional jump
        term->kind    = IK_JMP;
        term->target  = taken;
        term->target2 = NULL;
        term->nops    = 0;  // drop the condition operand reference
    }
}

// ── R2D: Copy propagation ─────────────────────────────────────────────────

void opt_copy_prop(Function *f) {
    // After OOS, phi-variables have one IK_COPY per predecessor block; they must
    // not be aliased — collapsing them creates circular self-references in loops.
    // Count how many IK_COPY instructions define each value; only alias values
    // with exactly one copy definition.
    int *copy_count = calloc(f->nvalues, sizeof(int));
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (!inst->is_dead && inst->kind == IK_COPY && inst->dst)
                copy_count[inst->dst->id]++;
        }
    }

    // Pass 1: alias each single-definition IK_COPY dst to its canonical source.
    // val_resolve() chains through the aliases we set.
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead) continue;
            if (inst->kind != IK_COPY || inst->nops < 1 || !inst->ops[0]) continue;
            Value *dst = inst->dst;
            if (copy_count[dst->id] > 1) continue;  // phi-variable: multiple defs
            Value *src = val_resolve(inst->ops[0]);
            if (src->vtype != dst->vtype) continue;  // type-coercion copy (e.g. i32↔u32)
            if (!src->def) continue;                 // VAL_CONST or VAL_UNDEF source
            if (src->def->kind == IK_CALL   ||
                src->def->kind == IK_ICALL  ||
                src->def->kind == IK_PARAM  ||
                src->def->kind == IK_COPY) continue; // constrained or swap-cycle source
            dst->alias = src;
        }
    }
    free(copy_count);

    // Pass 2: rewrite every operand to its canonical value and recount use_count.
    // Aliased copy dsts will end up with use_count == 0 and be removed by IRC DCE.
    for (int i = 0; i < f->nvalues; i++) f->values[i]->use_count = 0;
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead) continue;
            for (int j = 0; j < inst->nops; j++) {
                if (inst->ops[j]) {
                    inst->ops[j] = val_resolve(inst->ops[j]);
                    inst->ops[j]->use_count++;
                }
            }
        }
    }
}

// ── R2B: Dead block elimination ───────────────────────────────────────────

void opt_remove_dead_blocks(Function *f) {
    int changed = 1;
    while (changed) {
        changed = 0;
        int new_n = 0;
        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            if (bi == 0 || b->npreds > 0) {
                f->blocks[new_n++] = b;
            } else {
                // Dead block: strip outgoing edges from successors' pred lists
                for (int k = 0; k < b->nsuccs; k++) {
                    Block *succ = b->succs[k];
                    for (int j = 0; j < succ->npreds; j++) {
                        if (succ->preds[j] == b) {
                            succ->preds[j] = succ->preds[--succ->npreds];
                            break;
                        }
                    }
                }
                changed = 1;
            }
        }
        f->nblocks = new_n;
    }
}
