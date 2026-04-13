#include "opt.h"
#include "ssa.h"
#include "dom.h"
#include <stdlib.h>
#include <string.h>

unsigned opt_flags = OPT_ALL;

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

// ── R2G: Redundant booleanisation elimination ────────────────────────────

static int is_comparison(InstKind k) {
    switch (k) {
    case IK_EQ: case IK_NE: case IK_LT: case IK_LE:
    case IK_ULT: case IK_ULE:
    case IK_FEQ: case IK_FNE: case IK_FLT: case IK_FLE:
        return 1;
    default: return 0;
    }
}

// Chase through SHL/MUL-by-nonzero-const to find the underlying boolean source.
// SHL(bool, k) and MUL(bool, k) with k != 0 preserve zero/nonzero, so
// NE(SHL(cmp, k), 0) ≡ cmp and NE(MUL(cmp, k), 0) ≡ cmp.
static Value *find_bool_source(Value *v) {
    if (!v || v->kind != VAL_INST || !v->def) return NULL;
    if (is_comparison(v->def->kind)) return v;
    // Look through SHL(x, const) or MUL(x, const) where const != 0
    if ((v->def->kind == IK_SHL || v->def->kind == IK_MUL) && v->def->nops >= 2) {
        Value *a = val_resolve(v->def->ops[0]);
        Value *b = val_resolve(v->def->ops[1]);
        // Check if one operand is a nonzero constant
        int has_nz_const = 0;
        Value *other = NULL;
        if (b && b->kind == VAL_CONST && b->iconst != 0) { has_nz_const = 1; other = a; }
        else if (a && a->kind == VAL_CONST && a->iconst != 0) { has_nz_const = 1; other = b; }
        if (has_nz_const && other)
            return find_bool_source(other);
    }
    return NULL;
}

void opt_redundant_bool(Function *f) {
    // NE(cmp_result, 0) ≡ cmp_result when cmp_result is from a comparison
    // instruction (which already produces exactly 0 or 1).
    // Also handles NE(SHL(cmp, k), 0) and NE(MUL(cmp, k), 0) where k != 0,
    // since multiplying/shifting a boolean by a nonzero constant preserves
    // zero/nonzero.  The intermediate SHL/MUL may go dead after this.
    // Typical source: cg_logand inserts NE(rhs, 0) to booleanise && operands;
    // insert_coercions may apply pointer stride (×4) via MUL/SHL between the
    // comparison and the booleanisation.
    int changed = 0;
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead || inst->nops < 2 || !inst->dst) continue;
            if (inst->kind != IK_NE) continue;

            Value *v0 = val_resolve(inst->ops[0]);
            Value *v1 = val_resolve(inst->ops[1]);
            Value *src = NULL;
            if (v1 && v1->kind == VAL_CONST && v1->iconst == 0)      src = v0;
            else if (v0 && v0->kind == VAL_CONST && v0->iconst == 0)  src = v1;
            if (!src) continue;

            Value *bool_src = find_bool_source(src);
            if (!bool_src) continue;

            inst->dst->alias = bool_src;
            inst->is_dead = 1;
            changed = 1;
        }
    }
    if (!changed) return;

    // Recount use_count after aliasing
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

// ── R2H: Load narrowing ──────────────────────────────────────────────────
//
// AND(LOAD(addr, size=2), 0xFF) → LOAD(addr, size=1) when the load has
// no other uses.  Eliminates a word load + zxb pair in favour of a single
// byte load.  Also handles AND(LOAD(addr, size=4), 0xFFFF) → size=2.

void opt_narrow_loads(Function *f) {
    int changed = 0;
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead || inst->kind != IK_AND || inst->nops < 2 || !inst->dst)
                continue;
            Value *v0 = val_resolve(inst->ops[0]);
            Value *v1 = val_resolve(inst->ops[1]);
            Value *load_v = NULL, *mask_v = NULL;
            if (v1 && v1->kind == VAL_CONST) { load_v = v0; mask_v = v1; }
            else if (v0 && v0->kind == VAL_CONST) { load_v = v1; mask_v = v0; }
            if (!load_v || !mask_v) continue;
            if (load_v->kind != VAL_INST || !load_v->def) continue;
            if (load_v->def->kind != IK_LOAD) continue;
            if (load_v->use_count != 1) continue;

            int mask = mask_v->iconst;
            int lsz = load_v->def->size;
            if (mask == 0xff && lsz >= 2) {
                // Narrow to byte load
                load_v->def->size = 1;
                load_v->vtype = VT_U8;
                inst->dst->alias = load_v;
                inst->is_dead = 1;
                changed = 1;
            } else if (mask == 0xffff && lsz >= 4) {
                // Narrow to word load
                load_v->def->size = 2;
                load_v->vtype = VT_U16;
                inst->dst->alias = load_v;
                inst->is_dead = 1;
                changed = 1;
            }
        }
    }
    if (!changed) return;

    // Recount use_count
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

// ── R2F: LICM for IK_CONST (loop-invariant constant hoisting) ────────────
//
// Constants used inside loop bodies are materialized (immw) on every iteration.
// This pass finds frequently-used VAL_CONST operands inside loops and
// materializes them once in the pre-header as IK_CONST instructions, giving
// IRC a register to hold the constant across the loop body.
//
// Cap at LICM_MAX_HOIST constants per loop to avoid register pressure explosion
// (K=8; typical loops already use 4-5 registers for variables).

#define LICM_MAX_HOIST 4
#define LICM_MAX_CANDS 32

typedef struct {
    int     iconst;
    ValType vtype;
    int     uses;       // total use count inside the loop
    Value  *hoisted;    // non-NULL once materialized in pre-header
} LicmCand;

void opt_licm_const(Function *f) {
    if (f->nblocks < 2) return;
    // Step 1: Find loop headers via back edges.
    Block **headers = NULL;
    int nheaders = 0, hdr_cap = 0;

    for (int i = 0; i < f->nblocks; i++) {
        Block *b = f->blocks[i];
        for (int j = 0; j < b->nsuccs; j++) {
            Block *h = b->succs[j];
            if (h->rpo_index <= b->rpo_index && dominates(h, b)) {
                int dup = 0;
                for (int k = 0; k < nheaders; k++)
                    if (headers[k] == h) { dup = 1; break; }
                if (!dup) {
                    if (nheaders >= hdr_cap) {
                        hdr_cap = hdr_cap ? hdr_cap * 2 : 4;
                        Block **nh = arena_alloc(hdr_cap * sizeof(Block *));
                        if (headers) memcpy(nh, headers, nheaders * sizeof(Block *));
                        headers = nh;
                    }
                    headers[nheaders++] = h;
                }
            }
        }
    }
    if (nheaders == 0) return;

    int changed = 0;

    for (int li = 0; li < nheaders; li++) {
        Block *h = headers[li];

        // Only hoist in leaf loops (no inner loop headers dominated by h).
        // Hoisting in outer loops with nested inner loops makes constants live
        // across the entire function, causing register pressure > K and spill
        // cascades that can produce incorrect code.
        int has_inner = 0;
        for (int k = 0; k < nheaders; k++) {
            if (k != li && dominates(h, headers[k])) {
                has_inner = 1;
                break;
            }
        }
        if (has_inner) continue;

        // Step 2: Find the pre-header.
        Block *preheader = NULL;
        int outside_count = 0;
        for (int k = 0; k < h->npreds; k++) {
            Block *p = h->preds[k];
            if (!dominates(h, p)) {
                preheader = p;
                outside_count++;
            }
        }
        if (outside_count != 1 || !preheader) continue;
        if (!preheader->tail) continue;

        // Guard: estimate how many values are already live across the loop.
        // Count unique VAL_INST operands used in the loop body that are
        // defined outside the loop.  Also count operands of the loop header
        // as a proxy for phi-variables that are live across the back edge.
        // If these already consume most of K=8 registers, hoisting would
        // cause spill cascades.
        int body_size = 0;
        for (int bi = 0; bi < f->nblocks; bi++)
            if (dominates(h, f->blocks[bi])) body_size++;

        #define LIVE_CAP 32
        int live_ids[LIVE_CAP];
        int nlive = 0;
        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            if (!dominates(h, b)) continue;
            for (Inst *inst = b->head; inst; inst = inst->next) {
                if (inst->is_dead) continue;
                for (int j = 0; j < inst->nops; j++) {
                    Value *v = val_resolve(inst->ops[j]);
                    if (!v || v->kind != VAL_INST || !v->def) continue;
                    // Cross-loop: defined outside the loop body
                    if (!dominates(h, v->def->block)) {
                        int vid = v->id, dup = 0;
                        for (int k = 0; k < nlive && !dup; k++)
                            if (live_ids[k] == vid) dup = 1;
                        if (!dup && nlive < LIVE_CAP)
                            live_ids[nlive++] = vid;
                    }
                }
                // Also count dst values that are redefined across the loop
                // header (phi-variables): if an instruction defines a value
                // that is ALSO defined outside the loop, it's live across the
                // header and occupies a register throughout.
            }
        }
        // Also count operands of the header block itself — these are
        // live at loop entry and include phi-variables.
        for (Inst *inst = h->head; inst; inst = inst->next) {
            if (inst->is_dead) continue;
            for (int j = 0; j < inst->nops; j++) {
                Value *v = val_resolve(inst->ops[j]);
                if (!v || v->kind != VAL_INST) continue;
                int vid = v->id, dup = 0;
                for (int k = 0; k < nlive && !dup; k++)
                    if (live_ids[k] == vid) dup = 1;
                if (!dup && nlive < LIVE_CAP)
                    live_ids[nlive++] = vid;
            }
        }

        // K=8; need at least 2 free registers for temporaries in the loop body.
        if (nlive + 1 > 8 - 2) continue;  // +1 for the hoisted constant

        int max_hoist = LICM_MAX_HOIST;

        // Step 4: Count VAL_CONST operand uses inside the loop body.
        LicmCand cands[LICM_MAX_CANDS];
        int ncands = 0;

        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            if (b->loop_depth == 0) continue;
            if (!dominates(h, b)) continue;

            for (Inst *inst = b->head; inst; inst = inst->next) {
                if (inst->is_dead) continue;
                for (int j = 0; j < inst->nops; j++) {
                    Value *v = val_resolve(inst->ops[j]);
                    if (!v || v->kind != VAL_CONST) continue;
                    // Find or create candidate
                    int found = -1;
                    for (int k = 0; k < ncands; k++) {
                        if (cands[k].iconst == v->iconst && cands[k].vtype == v->vtype) {
                            found = k; break;
                        }
                    }
                    if (found >= 0) {
                        cands[found].uses++;
                    } else if (ncands < LICM_MAX_CANDS) {
                        cands[ncands++] = (LicmCand){
                            .iconst = v->iconst, .vtype = v->vtype,
                            .uses = 1, .hoisted = NULL
                        };
                    }
                }
            }
        }

        // Step 5: Sort candidates by use count (descending) and pick top N.
        // Simple selection: find the top max_hoist by uses.
        // Only hoist constants with uses >= 3 (enough savings to justify the
        // register cost across the loop body).
        int nhoisted = 0;
        for (int pass = 0; pass < max_hoist; pass++) {
            int best = -1, best_uses = 2; // threshold: >= 3 uses
            for (int k = 0; k < ncands; k++) {
                if (!cands[k].hoisted && cands[k].uses > best_uses) {
                    best = k;
                    best_uses = cands[k].uses;
                }
            }
            if (best < 0) break;

            // Materialize in pre-header
            Value *nv = new_value(f, VAL_INST, cands[best].vtype);
            Inst *ni = new_inst(f, preheader, IK_CONST, nv);
            ni->imm = cands[best].iconst;
            nv->iconst = cands[best].iconst;
            inst_insert_before(preheader->tail, ni);

            cands[best].hoisted = nv;
            nhoisted++;
        }
        if (nhoisted == 0) continue;

        // Step 6: Replace VAL_CONST operands in loop body with hoisted values.
        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            if (b->loop_depth == 0) continue;
            if (!dominates(h, b)) continue;

            for (Inst *inst = b->head; inst; inst = inst->next) {
                if (inst->is_dead) continue;
                for (int j = 0; j < inst->nops; j++) {
                    Value *v = val_resolve(inst->ops[j]);
                    if (!v || v->kind != VAL_CONST) continue;
                    // Check if this constant was hoisted
                    for (int k = 0; k < ncands; k++) {
                        if (cands[k].hoisted &&
                            cands[k].iconst == v->iconst &&
                            cands[k].vtype == v->vtype) {
                            inst->ops[j] = cands[k].hoisted;
                            break;
                        }
                    }
                }
            }
        }
        changed = 1;
    }

    if (!changed) return;

    // Recount use_count
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

// ── R2I: Jump threading ──────────────────────────────────────────────────
//
// When block P ends with IK_JMP B, and B is a "thin" block whose only live
// instructions are IK_COPY (OOS phi-copies) + a terminator IK_BR on a value
// defined in P, thread the branch into P directly.
//
// After threading, P's IK_JMP becomes IK_BR with B's targets, B's copies
// that define the branch condition are pulled into P, and P's edges are
// updated.  If B becomes predecessor-less it will be cleaned up by
// opt_remove_dead_blocks.
//
// This enables compare+branch fusion (P5) in emit.c for cases like
// core_state_transition where a comparison is separated from its branch
// by a phi-merge block.

static int is_thin_branch_block(Block *b) {
    // A block is "thin" if all non-dead instructions except the terminator
    // are IK_COPY, and the terminator is IK_BR.
    if (!b->tail || b->tail->kind != IK_BR) return 0;
    for (Inst *inst = b->head; inst; inst = inst->next) {
        if (inst->is_dead) continue;
        if (inst == b->tail) continue;  // skip the BR terminator
        if (inst->kind != IK_COPY) return 0;
    }
    return 1;
}

// Check if value v is defined in block b (directly or through val_resolve).
static int value_defined_in(Value *v, Block *b) {
    v = val_resolve(v);
    if (!v || v->kind != VAL_INST || !v->def) return 0;
    return v->def->block == b;
}

// Find an IK_COPY in block P that writes to the Value* cond.
// Returns the resolved source value, or NULL if not found.
static Value *find_copy_src_for(Block *p, Value *cond) {
    for (Inst *inst = p->head; inst; inst = inst->next) {
        if (inst->is_dead) continue;
        if (inst->kind == IK_COPY && inst->dst == cond && inst->nops >= 1)
            return val_resolve(inst->ops[0]);
    }
    return NULL;
}

void opt_jump_thread(Function *f) {
    int changed = 0;
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *p = f->blocks[bi];
        Inst  *term = p->tail;
        if (!term || term->kind != IK_JMP || !term->target) continue;

        Block *b = term->target;
        if (!is_thin_branch_block(b)) continue;

        Inst *br = b->tail;
        Value *cond = val_resolve(br->ops[0]);

        // Case 1: condition is defined (as a computation) in P — thread the branch.
        // Case 2: P copies a constant into the condition (phi-variable) — fold to jump.
        int defined_in_p = value_defined_in(cond, p);

        if (!defined_in_p) {
            // Check if P has an IK_COPY that writes a constant to cond
            Value *src = find_copy_src_for(p, cond);
            if (src && src->kind == VAL_CONST) {
                // Fold: P's JMP → JMP to the taken target directly
                Block *taken   = src->iconst ? br->target : br->target2;

                term->target = taken;
                // term stays IK_JMP — just retarget it

                // Update edges: remove B from P's succs, add taken
                for (int k = 0; k < p->nsuccs; k++) {
                    if (p->succs[k] == b) { p->succs[k] = taken; break; }
                }
                // Remove P from B's preds
                for (int k = 0; k < b->npreds; k++) {
                    if (b->preds[k] == p) {
                        b->preds[k] = b->preds[--b->npreds];
                        break;
                    }
                }
                // Add P to taken's preds
                Block **np = arena_alloc((taken->npreds + 1) * sizeof(Block *));
                for (int k = 0; k < taken->npreds; k++) np[k] = taken->preds[k];
                np[taken->npreds++] = p;
                taken->preds = np;

                changed = 1;
            }
            continue;
        }

        Block *bt = br->target;   // true target
        Block *bf = br->target2;  // false target

        // Rewrite P's terminator: IK_JMP → IK_BR
        term->kind    = IK_BR;
        term->target  = bt;
        term->target2 = bf;
        term->ops     = br->ops;
        term->nops    = br->nops;

        // Update P's succs: remove B, add bt and bf
        // First, remove B from P's succs
        for (int k = 0; k < p->nsuccs; k++) {
            if (p->succs[k] == b) {
                p->succs[k] = p->succs[--p->nsuccs];
                break;
            }
        }
        // Add bt and bf (avoid duplicates if bt == bf)
        {
            int need = (bt == bf) ? 1 : 2;
            Block **ns = arena_alloc((p->nsuccs + need) * sizeof(Block *));
            for (int k = 0; k < p->nsuccs; k++) ns[k] = p->succs[k];
            int already_bt = 0, already_bf = 0;
            for (int k = 0; k < p->nsuccs; k++) {
                if (ns[k] == bt) already_bt = 1;
                if (ns[k] == bf) already_bf = 1;
            }
            if (!already_bt) ns[p->nsuccs++] = bt;
            if (!already_bf && bf != bt) ns[p->nsuccs++] = bf;
            p->succs = ns;
        }

        // Remove P from B's preds
        for (int k = 0; k < b->npreds; k++) {
            if (b->preds[k] == p) {
                b->preds[k] = b->preds[--b->npreds];
                break;
            }
        }

        // Add P to bt's preds and bf's preds
        {
            Block **np = arena_alloc((bt->npreds + 1) * sizeof(Block *));
            for (int k = 0; k < bt->npreds; k++) np[k] = bt->preds[k];
            np[bt->npreds++] = p;
            bt->preds = np;
        }
        if (bf != bt) {
            Block **np = arena_alloc((bf->npreds + 1) * sizeof(Block *));
            for (int k = 0; k < bf->npreds; k++) np[k] = bf->preds[k];
            np[bf->npreds++] = p;
            bf->preds = np;
        }

        changed = 1;
    }

    if (changed) {
        // Remove blocks that became predecessor-less
        opt_remove_dead_blocks(f);
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

// ── R2J: Self-loop unrolling with modulo variable expansion ──────────────

// Clone an instruction into block 'b' with operands remapped through vmap.
// If an operand is not in vmap (its id slot is NULL), the original is kept.
static Inst *clone_inst_mapped(Function *f, Block *b, Inst *orig, Value **vmap) {
    Value *new_dst = NULL;
    if (orig->dst) {
        new_dst = new_value(f, orig->dst->kind, orig->dst->vtype);
        vmap[orig->dst->id] = new_dst;
    }
    Inst *ci = new_inst(f, b, orig->kind, new_dst);
    ci->imm   = orig->imm;
    ci->size  = orig->size;
    ci->fname = orig->fname;
    ci->label = orig->label;
    ci->line  = orig->line;
    ci->calldesc = orig->calldesc;
    for (int j = 0; j < orig->nops; j++) {
        Value *op = orig->ops[j];
        if (op) {
            Value *r = val_resolve(op);
            if (r && r->id < f->nvalues && vmap[r->id]) op = vmap[r->id];
            else                                          op = r;
        }
        inst_add_op(ci, op);
    }
    inst_append(b, ci);
    return ci;
}

void opt_unroll_loops(Function *f) {
    if (!f || f->nblocks == 0) return;

    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        Inst  *term = b->tail;
        if (!term || term->kind != IK_BR) continue;

        // 1. Check self-loop: true branch targets self
        // Also check: false branch targets self (some loops are structured this way)
        int self_is_true = (term->target == b);
        int self_is_false = (term->target2 == b);
        if (!self_is_true && !self_is_false) continue;
        // Canonicalize: ensure self-loop is on true branch
        if (self_is_false && !self_is_true) {
            // Swap: make true=self, false=exit
            Block *tmp = term->target;
            term->target = term->target2;
            term->target2 = tmp;
        }
        if (term->target != b) continue;
        Block *exit_blk = term->target2;
        if (!exit_blk) continue;

        // Must have exactly 2 succs: self and exit
        if (b->nsuccs != 2) continue;

        // 2. Scan for trailing copy chain before terminator
        // Collect copies in reverse order (closest to branch first)
        Inst *copies[16];
        int ncopy = 0;
        for (Inst *inst = term->prev; inst; inst = inst->prev) {
            if (inst->is_dead) continue;
            if (inst->kind != IK_COPY) break;
            if (ncopy >= 16) break;
            copies[ncopy++] = inst;
        }
        if (ncopy < 2) continue;

        // 3. Check no calls and reasonable size
        int inst_count = 0;
        int has_call = 0;
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead) continue;
            inst_count++;
            if (inst->kind == IK_CALL || inst->kind == IK_ICALL) has_call = 1;
        }
        if (has_call || inst_count > 15) continue;

        // 4. Build permutation from copy chain: perm[dst_id] = src_id
        //    and check it forms a single cycle
        //    copies[] are in reverse order (last copy first)
        //    Reverse to get natural order
        for (int i = 0; i < ncopy / 2; i++) {
            Inst *tmp = copies[i];
            copies[i] = copies[ncopy - 1 - i];
            copies[ncopy - 1 - i] = tmp;
        }

        // Validate copies: each must have a valid dst and src
        int valid = 1;
        for (int i = 0; i < ncopy; i++) {
            if (!copies[i]->dst || copies[i]->nops < 1 || !copies[i]->ops[0]) { valid = 0; break; }
            Value *s = val_resolve(copies[i]->ops[0]);
            if (!s || s->kind == VAL_CONST) { valid = 0; break; }
        }
        if (!valid) continue;

        // Unroll factor: ncopy + 1 (the number of distinct values in the
        // shift chain). For 2 copies involving 3 values, unroll 3x.
        // Cap at a reasonable maximum to avoid code bloat.
        int N = ncopy + 1;
        if (N > 4) continue;

        // 5. Collect phi-variables and their copy sources
        Value *phi_vars[16];   // copy destinations (phi-variables)
        Value *phi_srcs[16];   // copy sources (NOT val_resolved — the raw operand)
        for (int i = 0; i < ncopy; i++) {
            phi_vars[i] = copies[i]->dst;
            phi_srcs[i] = copies[i]->ops[0];  // raw, not resolved
        }

        // The branch tests a phi-variable (the last copy's destination)
        Value *br_cond = term->ops[0];
        int br_copy_idx = -1;
        for (int i = 0; i < ncopy; i++) {
            if (phi_vars[i] == br_cond) { br_copy_idx = i; break; }
        }
        if (br_copy_idx < 0) continue;

        // Collect "real" instructions: everything before the first copy
        Inst *first_copy = copies[0];
        Inst *real_insts[64];
        int nreal = 0;
        for (Inst *inst = b->head; inst && inst != first_copy; inst = inst->next) {
            if (inst->is_dead) continue;
            if (nreal >= 64) break;
            real_insts[nreal++] = inst;
        }

        // 6. Perform the unrolling
        //    Strategy: mark iteration 0's copies + branch dead, then append
        //    N-1 fresh continuation blocks with cloned body+copies, where the
        //    last iteration writes back to the original phi-variables.

        // Generously-sized vmap: old value ID → new value for current iteration
        int map_cap = f->nvalues + nreal * N + ncopy * N + 16;
        Value **vmap = calloc(map_cap, sizeof(Value *));

        // iter_vals[k][i] = the fresh value that replaces phi_vars[i] after
        // iteration k's copies execute. For the last iteration (k=N-1),
        // iter_vals[N-1][i] = phi_vars[i] (writes back to originals).
        Value *iter_vals[8][16];
        for (int k = 0; k < N - 1; k++) {
            for (int i = 0; i < ncopy; i++)
                iter_vals[k][i] = new_value(f, phi_vars[i]->kind, phi_vars[i]->vtype);
        }
        for (int i = 0; i < ncopy; i++)
            iter_vals[N - 1][i] = phi_vars[i];

        // Create new blocks
        Block *cont_blks[8];   // cont_blks[k] for iteration k+1
        Block *exit_fixups[8]; // exit fixup for iteration k (k = 0..N-2)
        for (int k = 0; k < N - 1; k++) {
            cont_blks[k] = new_block(f);
            cont_blks[k]->sealed = 1; cont_blks[k]->filled = 1;
            exit_fixups[k] = new_block(f);
            exit_fixups[k]->sealed = 1; exit_fixups[k]->filled = 1;
        }

        // === Iteration 0: modify the original block B ===
        // Remove original copies and branch; replace with fresh copies + new branch.
        // Truncate the instruction list just before the first copy, then append
        // new copies and a new branch.
        {
            Inst *cut = first_copy->prev;  // last real instruction
            if (cut) { cut->next = NULL; b->tail = cut; }
            else     { b->head = b->tail = NULL; }
        }
        for (int i = 0; i < ncopy; i++) {
            Inst *ci = new_inst(f, b, IK_COPY, iter_vals[0][i]);
            ci->line = copies[i]->line;
            inst_add_op(ci, phi_srcs[i]);
            inst_append(b, ci);
        }
        {
            Inst *br = new_inst(f, b, IK_BR, NULL);
            inst_add_op(br, iter_vals[0][br_copy_idx]);
            br->target  = cont_blks[0];
            br->target2 = exit_fixups[0];
            inst_append(b, br);
        }

        // === Iterations 1..N-1: build continuation blocks ===
        for (int k = 1; k < N; k++) {
            Block *cb = (k < N - 1) ? cont_blks[k] : NULL;
            Block *dst_blk = (k < N - 1) ? cont_blks[k - 1] : cont_blks[N - 2];
            // Actually: iteration k's instructions go into cont_blks[k-1]
            // (cont_blks[0] holds iteration 1, cont_blks[1] holds iteration 2, etc.)
            Block *ib = cont_blks[k - 1];

            memset(vmap, 0, map_cap * sizeof(Value *));
            // Map phi-vars → previous iteration's output values
            for (int i = 0; i < ncopy; i++)
                vmap[phi_vars[i]->id] = iter_vals[k - 1][i];

            // Clone real instructions
            for (int ri = 0; ri < nreal; ri++)
                clone_inst_mapped(f, ib, real_insts[ri], vmap);

            // Emit copies: iter_vals[k][i] = mapped(phi_srcs[i])
            for (int i = 0; i < ncopy; i++) {
                Value *src = phi_srcs[i];
                Value *ms = (src && src->id < map_cap && vmap[src->id]) ? vmap[src->id] : src;
                Inst *ci = new_inst(f, ib, IK_COPY, iter_vals[k][i]);
                ci->line = copies[i]->line;
                inst_add_op(ci, ms);
                inst_append(ib, ci);
            }

            // Branch
            Value *test_val = iter_vals[k][br_copy_idx];
            Inst *br = new_inst(f, ib, IK_BR, NULL);
            inst_add_op(br, test_val);
            if (k < N - 1) {
                br->target  = cont_blks[k];      // next continuation
                br->target2 = exit_fixups[k];     // early exit
            } else {
                br->target  = b;                  // loop back to iter 0
                br->target2 = exit_blk;           // original exit
            }
            inst_append(ib, br);
        }

        // === Exit fixup blocks ===
        for (int k = 0; k < N - 1; k++) {
            Block *fb = exit_fixups[k];
            for (int i = 0; i < ncopy; i++) {
                Inst *ci = new_inst(f, fb, IK_COPY, phi_vars[i]);
                ci->line = copies[0]->line;
                inst_add_op(ci, iter_vals[k][i]);
                inst_append(fb, ci);
            }
            Inst *jmp = new_inst(f, fb, IK_JMP, NULL);
            jmp->target = exit_blk;
            inst_append(fb, jmp);
        }

        // === Update CFG edges ===
        // B: remove self-loop and exit edges; add cont_blks[0] and exit_fixups[0]
        {
            Block **ns = arena_alloc(2 * sizeof(Block *));
            ns[0] = cont_blks[0]; ns[1] = exit_fixups[0];
            b->succs = ns; b->nsuccs = 2;
        }
        for (int k = 0; k < b->npreds; k++) {
            if (b->preds[k] == b) { b->preds[k] = b->preds[--b->npreds]; break; }
        }
        for (int k = 0; k < exit_blk->npreds; k++) {
            if (exit_blk->preds[k] == b) { exit_blk->preds[k] = exit_blk->preds[--exit_blk->npreds]; break; }
        }
        // Last continuation block: back-edge to B and edge to exit_blk
        block_add_pred(b, cont_blks[N - 2]);
        block_add_pred(exit_blk, cont_blks[N - 2]);

        // Wire continuation blocks: preds and succs
        for (int k = 0; k < N - 1; k++) {
            Block *ib = cont_blks[k];
            block_add_pred(ib, (k == 0) ? b : cont_blks[k - 1]);
            if (k < N - 2) {
                block_add_succ(ib, cont_blks[k + 1]);
                block_add_succ(ib, exit_fixups[k + 1]);
            } else {
                // Last cont block
                block_add_succ(ib, b);
                block_add_succ(ib, exit_blk);
            }
        }
        // Wire exit fixups
        for (int k = 0; k < N - 1; k++) {
            block_add_pred(exit_fixups[k], (k == 0) ? b : cont_blks[k - 1]);
            block_add_succ(exit_fixups[k], exit_blk);
            block_add_pred(exit_blk, exit_fixups[k]);
        }

        free(vmap);
        break;  // only unroll one loop per function per pass
    }
}
