/*
 * verify.c — IR consistency verifier (debug facility)
 *
 * See verify.h for the check list. The verifier never mutates the IR:
 * operand resolution uses val_resolve without writing back, and the
 * use_count recount goes into a scratch array.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "verify.h"

int ir_verify_enabled(void)
{
    static int cached = -1;
    if (cached < 0) cached = getenv("IR_VERIFY") != NULL;
    return cached;
}

static int nerrors;
static const char *cur_stage;
static Function *cur_f;

static void verr(const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "IR_VERIFY [%s] %s: ", cur_stage, cur_f->name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    nerrors++;
}

static int is_terminator(Inst *inst)
{
    return inst->kind == IK_BR || inst->kind == IK_JMP ||
           inst->kind == IK_RET || inst->kind == IK_SWITCH;
}

static int block_in_function(Function *f, Block *b)
{
    for (int i = 0; i < f->nblocks; i++)
        if (f->blocks[i] == b) return 1;
    return 0;
}

static int in_pred_list(Block *b, Block *p)
{
    for (int i = 0; i < b->npreds; i++)
        if (b->preds[i] == p) return 1;
    return 0;
}

static int in_succ_list(Block *b, Block *s)
{
    for (int i = 0; i < b->nsuccs; i++)
        if (b->succs[i] == s) return 1;
    return 0;
}

static void check_target(Block *b, Block *t, const char *what)
{
    if (!t) { verr("B%d: %s target is NULL", b->id, what); return; }
    if (!block_in_function(cur_f, t))
        verr("B%d: %s target B%d is not in the function", b->id, what, t->id);
    else if (!in_succ_list(b, t))
        verr("B%d: %s target B%d missing from succs", b->id, what, t->id);
}

void verify_function(Function *f, const char *stage, VerifyPhase phase)
{
    if (!ir_verify_enabled() || !f) return;

    nerrors   = 0;
    cur_stage = stage;
    cur_f     = f;

    // ---- CFG structure ----
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];

        // pred/succ symmetry
        for (int i = 0; i < b->nsuccs; i++)
            if (!in_pred_list(b->succs[i], b))
                verr("B%d: succ B%d does not list it as pred", b->id, b->succs[i]->id);
        for (int i = 0; i < b->npreds; i++)
            if (!in_succ_list(b->preds[i], b))
                verr("B%d: pred B%d does not list it as succ", b->id, b->preds[i]->id);

        // terminators
        Inst *term = NULL;
        int nterm = 0;
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead) continue;
            if (is_terminator(inst)) { term = inst; nterm++; }
            else if (term && inst->kind != IK_PHI)
                verr("B%d: %s after terminator", b->id,
                     inst->kind == IK_COPY ? "copy" : "instruction");
        }
        // Unreachable blocks may be left unterminated by dead-block-style
        // rewrites; only require a terminator on reachable blocks.
        int reachable = (bi == 0) || b->npreds > 0;
        if (nterm == 0 && reachable)
            verr("B%d: no terminator", b->id);
        if (nterm > 1)
            verr("B%d: %d terminators", b->id, nterm);

        if (term) {
            if (term->kind == IK_BR) {
                check_target(b, term->target,  "IK_BR true");
                check_target(b, term->target2, "IK_BR false");
            } else if (term->kind == IK_JMP) {
                check_target(b, term->target, "IK_JMP");
            } else if (term->kind == IK_SWITCH) {
                for (int ci = 0; ci < term->switch_ncase; ci++)
                    check_target(b, term->switch_targets[ci], "IK_SWITCH case");
                check_target(b, term->switch_default, "IK_SWITCH default");
            }
        }

        // phis
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead || inst->kind != IK_PHI) continue;
            if (phase != VERIFY_PRE_OOS)
                verr("B%d: phi survives past out_of_ssa", b->id);
            else if (inst->nops != b->npreds)
                verr("B%d: phi arity %d != npreds %d", b->id, inst->nops, b->npreds);
        }
    }

    // ---- Operand sanity + use_count recount ----
    int *counted = calloc((size_t)f->nvalues, sizeof(int));
    if (counted) {
        for (int bi = 0; bi < f->nblocks; bi++) {
            for (Inst *inst = f->blocks[bi]->head; inst; inst = inst->next) {
                if (inst->is_dead) continue;
                for (int j = 0; j < inst->nops; j++) {
                    Value *v = inst->ops[j];
                    if (!v) continue;
                    v = val_resolve(v);
                    if (v->kind == VAL_INST) {
                        // Post-IRC exception: coalescing marks IK_COPYs dead
                        // without aliasing their dst — the dst lives on with
                        // the merged color, so uses of it are fine.
                        int coalesced = phase == VERIFY_POST_IRC &&
                                        v->def && v->def->kind == IK_COPY;
                        if (v->def && v->def->is_dead && !coalesced)
                            verr("B%d: live %s uses v%d defined by a dead instruction",
                                 f->blocks[bi]->id,
                                 inst->kind == IK_PHI ? "phi" : "instruction", v->id);
                        if (v->id >= 0 && v->id < f->nvalues)
                            counted[v->id]++;
                    }
                }
            }
        }
        // The stored count must not UNDERCOUNT actual uses: peepholes that
        // key on use_count == 1 (P5/P17 branch fusion, P16 bitex) and
        // emission's remap_single_use_values would delete or repurpose a
        // computation that still has other consumers.
        //
        // Only enforced post-OOS: Braun construction leaves approximate
        // counts (trivial-phi removal rewires uses without recounting), and
        // every use_count consumer runs after one of the opt passes has
        // called recount_uses. From copy_prop onward, exactness must hold.
        if (phase != VERIFY_PRE_OOS && phase != VERIFY_OOS) {
            for (int i = 0; i < f->nvalues; i++) {
                Value *v = f->values[i];
                if (v->kind != VAL_INST || v->alias) continue;
                if (v->use_count < counted[v->id])
                    verr("v%d: stored use_count %d < actual %d (fusion-unsafe undercount)",
                         v->id, v->use_count, counted[v->id]);
            }
        }
        free(counted);
    }

    // ---- Post-IRC: physical registers assigned ----
    if (phase == VERIFY_POST_IRC) {
        for (int bi = 0; bi < f->nblocks; bi++) {
            for (Inst *inst = f->blocks[bi]->head; inst; inst = inst->next) {
                if (inst->is_dead) continue;
                // Spilled values (spill_slot >= 0) legitimately carry
                // phys_reg -1: their def feeds the spill store through an
                // emission scratch register, and every real use was
                // replaced by a reload. Uncolored AND slotless is the
                // failure mode (the IRC livelock bug).
                if (inst->dst) {
                    Value *d = val_resolve(inst->dst);
                    if ((d->phys_reg < 0 && d->spill_slot < 0) || d->phys_reg > 7) {
                        // One benign shape: a def that was spilled via an
                        // IG-coalescing alias (its slot lives on the
                        // union-find canonical, invisible here) is emitted
                        // into scratch r0 and consumed by the IMMEDIATELY
                        // following spill store. Fragile-but-correct today;
                        // anything else uncolored is the IRC livelock bug.
                        Inst *nx = inst->next;
                        while (nx && nx->is_dead) nx = nx->next;
                        int spill_def = nx && nx->kind == IK_STORE &&
                                        nx->nops == 1 && nx->ops[0] &&
                                        val_resolve(nx->ops[0]) == d;
                        if (!spill_def)
                            verr("B%d: dst v%d has phys_reg %d (no spill slot)",
                                 f->blocks[bi]->id, d->id, d->phys_reg);
                    }
                }
                for (int j = 0; j < inst->nops; j++) {
                    Value *v = inst->ops[j];
                    if (!v) continue;
                    v = val_resolve(v);
                    if (v->kind == VAL_INST &&
                        ((v->phys_reg < 0 && v->spill_slot < 0) || v->phys_reg > 7)) {
                        // Matching exemption: the spill store itself reads
                        // the uncolored def (see above).
                        int is_spill_store_use =
                            inst->kind == IK_STORE && inst->nops == 1;
                        if (!is_spill_store_use)
                            verr("B%d: operand v%d has phys_reg %d (no spill slot)",
                                 f->blocks[bi]->id, v->id, v->phys_reg);
                    }
                }
            }
        }
    }

    if (nerrors) {
        fprintf(stderr, "IR_VERIFY [%s] %s: %d violation%s — IR dump follows\n",
                stage, f->name, nerrors, nerrors == 1 ? "" : "s");
        print_function(f, stderr);
        exit(1);
    }
}
