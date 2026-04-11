#include "opt.h"
#include "ssa.h"
#include "dom.h"
#include <stdlib.h>
#include <string.h>

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
 * R2E  opt_cse():            Hash-based common subexpression elimination.
 *      Walk blocks in dominator-tree pre-order; for each pure instruction,
 *      look up its (kind, operands, imm, fname) in a fixed hash table.  If an
 *      equivalent instruction already exists in the SAME block, alias the
 *      current dst to that earlier result and mark it dead.  Recounts
 *      use_count afterward.  Pure = ALU, comparisons, type conversions, GADDR.
 *      Loads, stores, calls, and control flow are never CSE'd.
 *      Cross-block CSE is deliberately disabled: in the post-OOS IR, values
 *      can be redefined by OOS copy insertions (phi-variable copies), so SSA
 *      dominance does not guarantee semantic equality across blocks.  More
 *      practically, cross-block CSE in loop-heavy functions extends canonical
 *      value live ranges through parallel-branch loops (loops not on the idom
 *      path but still on CFG paths), causing IRC register pressure to exceed
 *      K=8 and producing miscoloured or uncoloured values.
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

// ── R2E: Common subexpression elimination ─────────────────────────────────

#define CSE_BUCKETS 256

typedef struct { Inst *inst; Value *val; } CseEntry;

static int is_cse_pure(InstKind k) {
    switch (k) {
    case IK_ADD:  case IK_SUB:  case IK_MUL:
    case IK_DIV:  case IK_UDIV: case IK_MOD:  case IK_UMOD:
    case IK_SHL:  case IK_SHR:  case IK_USHR:
    case IK_AND:  case IK_OR:   case IK_XOR:
    case IK_NEG:  case IK_NOT:
    case IK_LT:   case IK_ULT:  case IK_LE:   case IK_ULE:
    case IK_EQ:   case IK_NE:
    case IK_FADD: case IK_FSUB: case IK_FMUL: case IK_FDIV:
    case IK_FLT:  case IK_FLE:  case IK_FEQ:  case IK_FNE:
    case IK_ITOF: case IK_FTOI:
    case IK_SEXT8: case IK_SEXT16: case IK_ZEXT: case IK_TRUNC:
    case IK_GADDR:
        return 1;
    default:
        return 0;
    }
}

static uint32_t cse_strhash(const char *s) {
    uint32_t h = 0;
    while (*s) h = h * 31u + (unsigned char)*s++;
    return h;
}

// Canonical key for operand comparison: constants compared by value, others by id.
static uint32_t cse_opkey(Value *v) {
    return (v->kind == VAL_CONST) ? (uint32_t)v->iconst + 0x80000000u
                                  : (uint32_t)v->id;
}

static uint32_t cse_hash(Inst *inst) {
    uint32_t h = (uint32_t)inst->kind * 2654435761u;
    for (int j = 0; j < inst->nops; j++)
        h ^= cse_opkey(val_resolve(inst->ops[j])) * (2654435761u * (uint32_t)(j + 2));
    h ^= (uint32_t)inst->imm * 1234567891u;
    if (inst->fname) h ^= cse_strhash(inst->fname);
    return h;
}

static int cse_opseq(Value *va, Value *vb) {
    if (va == vb) return 1;
    return va->kind == VAL_CONST && vb->kind == VAL_CONST
        && va->iconst == vb->iconst && va->vtype == vb->vtype;
}

static int cse_equiv(Inst *a, Inst *b) {
    if (a->kind != b->kind || a->nops != b->nops || a->imm != b->imm) return 0;
    // Result type must match: IK_TRUNC/IK_ZEXT/IK_SEXT8/IK_SEXT16 to different
    // widths have the same kind+operands but differ in what bits they produce.
    if (a->dst->vtype != b->dst->vtype) return 0;
    for (int j = 0; j < a->nops; j++)
        if (!cse_opseq(val_resolve(a->ops[j]), val_resolve(b->ops[j]))) return 0;
    if (a->fname != b->fname) {
        if (!a->fname || !b->fname || strcmp(a->fname, b->fname) != 0) return 0;
    }
    return 1;
}

static int cmp_dom_pre(const void *a, const void *b) {
    return (*(const Block **)a)->dom_pre - (*(const Block **)b)->dom_pre;
}

void opt_cse(Function *f) {
    CseEntry table[CSE_BUCKETS];
    memset(table, 0, sizeof(table));

    // Process blocks in dominator-tree pre-order so every block's dominators
    // are visited before the block itself.
    Block **order = malloc(f->nblocks * sizeof(Block *));
    for (int i = 0; i < f->nblocks; i++) order[i] = f->blocks[i];
    qsort(order, f->nblocks, sizeof(Block *), cmp_dom_pre);

    int changed = 0;
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = order[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead || !inst->dst) continue;
            if (!is_cse_pure(inst->kind))    continue;

            uint32_t h = cse_hash(inst) & (CSE_BUCKETS - 1);
            int done = 0;
            for (int k = 0; k < CSE_BUCKETS && !done; k++) {
                CseEntry *e = &table[(h + k) & (CSE_BUCKETS - 1)];
                if (!e->inst) {
                    e->inst = inst;
                    e->val  = inst->dst;
                    done    = 1;
                } else if (cse_equiv(e->inst, inst)) {
                    if (dominates(e->inst->block, b)) {
                        // Guard: only CSE within the same basic block OR when the
                        // canonical block directly dominates the current block with
                        // no increase in loop depth on the entire dom path.
                        //
                        // Cross-block CSE in loops can extend a value's live range
                        // across an entire loop body (or across sibling-branch loops
                        // that the dom path doesn't traverse but the CFG does).
                        // Either case pushes register pressure past K=8 and causes
                        // IRC convergence failure on loop-heavy functions.
                        // Only CSE within the same basic block.  Cross-block CSE
                        // extends the canonical value's live range, which in loop-heavy
                        // functions can push register pressure past K=8 and cause IRC
                        // convergence failure or miscoloring.  Same-block CSE is always
                        // safe: no live range extension, no register pressure increase.
                        int safe = (e->inst->block == b);
                        if (safe) {
                            Value *canon = val_resolve(e->val);
                            if (getenv("CSE_DEBUG")) {
                                fprintf(stderr, "CSE: func=%s alias v%d->v%d (kind=%d B%d->B%d)\n",
                                    f->name, inst->dst->id, canon->id,
                                    inst->kind, inst->block->id, e->inst->block->id);
                            }
                            inst->dst->alias = canon;
                            inst->is_dead    = 1;
                            changed          = 1;
                        }
                    }
                    done = 1;
                }
                // else: hash collision with a different expression; probe next slot
            }
        }
    }
    free(order);

    if (!changed) return;

    // Recount use_count after aliasing (same pattern as opt_copy_prop).
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
