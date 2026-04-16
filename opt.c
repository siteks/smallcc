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

// ── Shared helpers ────────────────────────────────────────────────────────

/*
 * Rewrite every operand to its canonical value (chasing Value.alias chains
 * via val_resolve) and rebuild use_count from the surviving instruction
 * stream.  Aliased instruction dsts naturally end up with use_count == 0
 * and are removed later by IRC DCE.
 *
 * This is the standard post-transform cleanup after any pass that aliases
 * Value dsts or marks instructions dead.  Passes that physically unlink
 * dead instructions should do so BEFORE calling this helper, so the walk
 * sees the final list; the is_dead guard here then handles any residual
 * is_dead instructions that were not unlinked.
 */
static void recount_uses(Function *f) {
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
            if (src->vtype != dst->vtype)
                continue;  // type-coercion copy (e.g. i32↔u32)
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
    recount_uses(f);
}

// ── R2E: Global value numbering (dominator-tree CSE) ──────────────────────

#define CSE_BUCKETS 256

typedef struct { Inst *inst; Value *val; } CseEntry;
typedef struct { int slot; CseEntry prev; } CseUndo;

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

// GVN state (module-level for recursive walk)
static CseEntry  gvn_table[CSE_BUCKETS];
static CseUndo  *gvn_undo;
static int       gvn_undo_top;
static int      *gvn_cc;      // copy_count: >0 means phi-variable
static int       gvn_changed;
static int       gvn_pre_oos; // 0 = post-OOS (strict xblock), 1 = pre-OOS (relaxed)

static void gvn_push(int slot) {
    gvn_undo[gvn_undo_top].slot = slot;
    gvn_undo[gvn_undo_top].prev = gvn_table[slot];
    gvn_undo_top++;
}

// Check that all operands are non-phi (safe for cross-block elimination).
// Phi-variables can be redefined by OOS copies on different paths, so a
// dominated instruction using the same Value* may see a different runtime value.
static int gvn_ops_safe(Inst *inst) {
    for (int i = 0; i < inst->nops; i++) {
        Value *v = inst->ops[i];
        if (!v) continue;
        v = val_resolve(v);
        if (v->kind == VAL_CONST) continue;
        if (gvn_cc[v->id] > 0) return 0;
    }
    return 1;
}

static void gvn_walk(Block *b, Function *f) {
    int mark = gvn_undo_top;

    for (Inst *inst = b->head; inst; inst = inst->next) {
        if (inst->is_dead || !inst->dst) continue;
        if (!is_cse_pure(inst->kind))    continue;

        uint32_t h = cse_hash(inst) & (CSE_BUCKETS - 1);
        int done = 0;
        for (int k = 0; k < CSE_BUCKETS && !done; k++) {
            int idx = (int)((h + (uint32_t)k) & (CSE_BUCKETS - 1));
            CseEntry *e = &gvn_table[idx];
            if (!e->inst) {
                gvn_push(idx);
                e->inst = inst;
                e->val  = inst->dst;
                done    = 1;
            } else if (cse_equiv(e->inst, inst)) {
                Value *canon = val_resolve(e->val);
                // Same-block CSE is always safe.  Cross-block CSE requires:
                // (1) all operands are non-phi-variables (single-def values
                //     whose semantic value is the same at both program points)
                // (2) the eliminated block is NOT at a deeper loop depth
                //     than the canonical (avoids extending live ranges into
                //     loops, which pushes register pressure past K=8)
                // Pre-OOS: no phi-variables, cross-block CSE safe when
                // canonical dominates target and the idom path doesn't
                // cross through a loop deeper than both endpoints
                // (avoids extending live ranges through intermediate loops).
                // Post-OOS: strict predecessor + non-loop guard.
                int xblock_ok = 0;
                if (gvn_pre_oos) {
                    Block *cb = e->inst->block;
                    // Only CSE from outer loop into inner loop: the value
                    // is already live at the outer level, so reusing it
                    // inside the inner loop is free.  Same-depth cross-block
                    // CSE extends live ranges and increases pressure.
                    //
                    // Exception: skip cross-depth CSE when all operands are
                    // VAL_CONST (the instruction is trivially rematerializable).
                    // Eliminating such an instruction extends the canonical's
                    // live range from the preheader through the loop body,
                    // wasting a register for a value that costs only an immw
                    // to recreate locally.
                    if (dominates(cb, b) &&
                        b->loop_depth > cb->loop_depth) {
                        int all_const = 1;
                        for (int oi = 0; oi < inst->nops; oi++) {
                            Value *ov = inst->ops[oi];
                            if (ov) ov = val_resolve(ov);
                            if (ov && ov->kind != VAL_CONST) { all_const = 0; break; }
                        }
                        if (all_const && inst->nops >= 0) {
                            /* rematerializable — keep the local copy */
                        } else {
                            int crosses = 0;
                            for (Block *w = b; w && w != cb; w = w->idom)
                                if (w->loop_depth > b->loop_depth) { crosses = 1; break; }
                            if (!crosses) xblock_ok = 1;
                        }
                    }
                } else {
                    if (gvn_ops_safe(inst)) {
                        Block *cb = e->inst->block;
                        // Conservative: direct predecessor, both at depth 0.
                        // Wider CSE (any dominator, same loop depth) extends
                        // canonical live ranges across loop exits, which
                        // defeats the P12 emit-time loop rotation peephole.
                        if (b->loop_depth == 0 && cb->loop_depth == 0) {
                            for (int p = 0; p < b->npreds; p++)
                                if (b->preds[p] == cb) { xblock_ok = 1; break; }
                        }
                    }
                }
                if (inst->dst != canon &&
                    (e->inst->block == b || xblock_ok)) {
                    if (getenv("CSE_DEBUG"))
                        fprintf(stderr, "GVN: func=%s alias v%d->v%d (kind=%d B%d->B%d)\n",
                            f->name, inst->dst->id, canon->id, inst->kind,
                            e->inst->block->id, b->id);
                    inst->dst->alias = canon;
                    inst->is_dead    = 1;
                    gvn_changed      = 1;
                }
                done = 1;
            }
        }
    }

    for (int i = 0; i < b->ndom_children; i++)
        gvn_walk(b->dom_children[i], f);

    // Undo table entries added by this block and its subtree
    while (gvn_undo_top > mark) {
        --gvn_undo_top;
        gvn_table[gvn_undo[gvn_undo_top].slot] = gvn_undo[gvn_undo_top].prev;
    }
}

void opt_cse(Function *f) {
    // Compute copy_count to identify phi-variables (multi-def OOS copies).
    gvn_cc = calloc(f->nvalues, sizeof(int));
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (!inst->is_dead && inst->kind == IK_COPY && inst->dst)
                gvn_cc[inst->dst->id]++;
        }
    }

    memset(gvn_table, 0, sizeof(gvn_table));
    gvn_undo = malloc(f->nvalues * sizeof(CseUndo));
    gvn_undo_top = 0;
    gvn_changed  = 0;

    // Walk dominator tree from the entry block
    if (f->nblocks > 0)
        gvn_walk(f->blocks[0], f);

    free(gvn_undo);
    free(gvn_cc);
    gvn_undo = NULL;
    gvn_cc   = NULL;

    if (!gvn_changed) return;

    // Recount use_count after aliasing (same pattern as opt_copy_prop).
    recount_uses(f);
}

void opt_pre_oos_cse(Function *f) {
    gvn_pre_oos = 1;
    opt_cse(f);
    gvn_pre_oos = 0;
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
    recount_uses(f);
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
    // Also fold SEXT8/SEXT16 of a signed load → IK_COPY.
    // The ISA's sign-extending loads (llbx/llwx/lbx/lwx) already produce
    // a sign-extended result, so the SEXT is redundant.  Replacing it with
    // IK_COPY lets IRC coalesce the load result and the widened value into
    // the same register, eliminating a spurious mov.
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead || !inst->dst || inst->nops < 1) continue;
            if (inst->kind != IK_SEXT8 && inst->kind != IK_SEXT16) continue;
            Value *src = val_resolve(inst->ops[0]);
            if (!src || src->kind != VAL_INST || !src->def) continue;
            if (src->def->kind != IK_LOAD) continue;
            int lsz = src->def->size;
            // SEXT8 is redundant after a 1-byte signed load;
            // SEXT16 is redundant after a 2-byte signed load.
            int src_signed = (src->vtype == VT_I8 || src->vtype == VT_I16 || src->vtype == VT_I32);
            if (inst->kind == IK_SEXT8 && lsz == 1 && src_signed) {
                inst->kind = IK_COPY;
                changed = 1;
            } else if (inst->kind == IK_SEXT16 && lsz == 2 && src_signed) {
                inst->kind = IK_COPY;
                changed = 1;
            }
        }
    }

    if (!changed) return;

    // Recount use_count
    recount_uses(f);
}

// ── Known-bits analysis ──────────────────────────────────────────────────
//
// Forward analysis computing which bits of each SSA value are provably
// 0 or 1.  Drives multiple simplifications from one mechanism instead of
// ad-hoc pattern matches for TRUNC look-through, AND-chain folding, etc.
//
// Per-value state: known_zero (bits provably 0), known_one (bits provably 1).
// A bit may be in neither set (unknown) but never in both.

typedef struct { uint32_t zero, one; } KnownBits;

static KnownBits *kb_table;   // indexed by Value.id

static KnownBits kb_get(Value *v) {
    v = val_resolve(v);
    if (v->kind == VAL_CONST)
        return (KnownBits){ ~(uint32_t)v->iconst, (uint32_t)v->iconst };
    if (v->kind == VAL_INST && v->id >= 0)
        return kb_table[v->id];
    return (KnownBits){ 0, 0 };
}

static uint32_t kb_trunc_mask(ValType vt) {
    if (vt == VT_U8  || vt == VT_I8)  return 0xFFu;
    if (vt == VT_U16 || vt == VT_I16 || vt == VT_PTR) return 0xFFFFu;
    return 0xFFFFFFFFu;
}

static void compute_known_bits(Function *f) {
    kb_table = arena_alloc(f->nvalues * sizeof(KnownBits));
    memset(kb_table, 0, f->nvalues * sizeof(KnownBits));

    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead || !inst->dst) continue;
            int id = inst->dst->id;
            if (id < 0 || id >= f->nvalues) continue;

            KnownBits r = { 0, 0 };
            switch (inst->kind) {
            case IK_CONST:
                // IK_CONST stores its value in inst->imm (dst->iconst is only
                // populated for VAL_CONST values, not VAL_INST dsts).
                r.zero = ~(uint32_t)inst->imm;
                r.one  =  (uint32_t)inst->imm;
                break;
            case IK_COPY:
                if (inst->nops >= 1) r = kb_get(inst->ops[0]);
                break;
            case IK_AND:
                if (inst->nops >= 2) {
                    KnownBits a = kb_get(inst->ops[0]);
                    KnownBits b2 = kb_get(inst->ops[1]);
                    r.zero = a.zero | b2.zero;  // 0 if either is 0
                    r.one  = a.one  & b2.one;   // 1 only if both are 1
                }
                break;
            case IK_OR:
                if (inst->nops >= 2) {
                    KnownBits a = kb_get(inst->ops[0]);
                    KnownBits b2 = kb_get(inst->ops[1]);
                    r.zero = a.zero & b2.zero;  // 0 only if both are 0
                    r.one  = a.one  | b2.one;   // 1 if either is 1
                }
                break;
            case IK_XOR:
                if (inst->nops >= 2) {
                    KnownBits a = kb_get(inst->ops[0]);
                    KnownBits b2 = kb_get(inst->ops[1]);
                    r.zero = (a.zero & b2.zero) | (a.one & b2.one);
                    r.one  = (a.zero & b2.one)  | (a.one & b2.zero);
                }
                break;
            case IK_SHL:
                if (inst->nops >= 2) {
                    Value *sh = val_resolve(inst->ops[1]);
                    if (sh->kind == VAL_CONST && sh->iconst >= 0 && sh->iconst < 32) {
                        KnownBits a = kb_get(inst->ops[0]);
                        int n = sh->iconst;
                        r.zero = (a.zero << n) | ((1u << n) - 1);
                        r.one  = a.one << n;
                    }
                }
                break;
            case IK_USHR:
                if (inst->nops >= 2) {
                    Value *sh = val_resolve(inst->ops[1]);
                    if (sh->kind == VAL_CONST && sh->iconst > 0 && sh->iconst < 32) {
                        KnownBits a = kb_get(inst->ops[0]);
                        int n = sh->iconst;
                        r.zero = (a.zero >> n) | (~0u << (32 - n));
                        r.one  = a.one >> n;
                    }
                }
                break;
            case IK_SHR:  // arithmetic: sign bit fills, not zero
                // Conservative: we don't track which bit is the sign bit
                // (bit 15 for i16, bit 31 for i32), so we can't determine
                // the fill bits.  Leave result unknown.
                break;
            case IK_TRUNC: case IK_ZEXT:
                if (inst->nops >= 1) {
                    KnownBits a = kb_get(inst->ops[0]);
                    uint32_t mask = kb_trunc_mask(inst->dst->vtype);
                    r.zero = a.zero | ~mask;   // bits above width known 0
                    r.one  = a.one  &  mask;
                }
                break;
            case IK_SEXT8:
                if (inst->nops >= 1) {
                    KnownBits a = kb_get(inst->ops[0]);
                    if (a.one & 0x80)       { r.zero = a.zero & 0x7F; r.one = a.one | ~0xFFu; }
                    else if (a.zero & 0x80) { r.zero = a.zero | ~0xFFu; r.one = a.one & 0x7F; }
                    else                    { r.zero = a.zero & 0xFF; r.one = a.one & 0xFF; }
                }
                break;
            case IK_SEXT16:
                if (inst->nops >= 1) {
                    KnownBits a = kb_get(inst->ops[0]);
                    if (a.one & 0x8000)       { r.zero = a.zero & 0x7FFF; r.one = a.one | ~0xFFFFu; }
                    else if (a.zero & 0x8000) { r.zero = a.zero | ~0xFFFFu; r.one = a.one & 0x7FFF; }
                    else                      { r.zero = a.zero & 0xFFFF; r.one = a.one & 0xFFFF; }
                }
                break;
            case IK_LOAD:
                // Unsigned loads produce zero-extended results
                if (inst->size == 1) r.zero = ~0xFFu;
                else if (inst->size == 2) r.zero = ~0xFFFFu;
                break;
            // Comparisons produce 0 or 1
            case IK_LT: case IK_LE: case IK_EQ: case IK_NE:
            case IK_ULT: case IK_ULE:
            case IK_FLT: case IK_FLE: case IK_FEQ: case IK_FNE:
                r.zero = ~1u;
                break;
            case IK_PHI:
                if (inst->nops > 0) {
                    r = kb_get(inst->ops[0]);
                    for (int i = 1; i < inst->nops; i++) {
                        KnownBits p = kb_get(inst->ops[i]);
                        r.zero &= p.zero;  // known 0 only if ALL paths agree
                        r.one  &= p.one;   // known 1 only if ALL paths agree
                    }
                }
                break;
            default:
                break;
            }
            kb_table[id] = r;
        }
    }
}

// ── Unwrap for mask ─────────────────────────────────────────────────────
//
// Given a value v and a bitmask, walk backwards through operations that
// only affect bits outside the mask (COPY, TRUNC, ZEXT, AND with wider
// mask).  Returns the "unwrapped" source — the deepest value whose bits
// within `mask` are identical to v's.
//
// This is the core known-bits utility: it answers "can I use a simpler
// value here, given that I only care about these bits?"

static Value *unwrap_for_mask(Value *v, uint32_t mask) {
    for (int i = 0; i < 8; i++) {
        v = val_resolve(v);
        if (v->kind != VAL_INST || !v->def || v->def->nops < 1) break;
        Inst *d = v->def;
        if (d->kind == IK_COPY) {
            v = d->ops[0]; continue;
        }
        if (d->kind == IK_TRUNC || d->kind == IK_ZEXT) {
            // TRUNC/ZEXT clears bits above its width; if mask doesn't
            // touch those bits, the operation is invisible.
            uint32_t width_mask = kb_trunc_mask(d->dst->vtype);
            if ((mask & ~width_mask) == 0) { v = d->ops[0]; continue; }
        }
        if (d->kind == IK_AND && d->nops >= 2) {
            // AND(x, c) clears bits outside c; if mask is a subset of c,
            // the AND is invisible.
            Value *c = val_resolve(d->ops[1]);
            Value *x = d->ops[0];
            if (c->kind != VAL_CONST) { c = val_resolve(d->ops[0]); x = d->ops[1]; }
            if (c->kind == VAL_CONST && (mask & ~(uint32_t)c->iconst) == 0) {
                v = x; continue;
            }
        }
        break;
    }
    return val_resolve(v);
}

// ── R2K: Known-bits simplification ──────────────────────────────────────
//
// Uses computed known bits to eliminate redundant operations:
//   - AND(x, mask) where all bits outside mask are already known zero → x
//   - TRUNC/ZEXT(x) where high bits are already known zero → x
//   - AND(x, mask) with TRUNC/ZEXT/AND chain → AND(unwrapped_x, mask)

void opt_known_bits(Function *f) {
    compute_known_bits(f);
    int changed = 0;

    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead || !inst->dst) continue;

            // Phi-select fold: phi(1, 0) or phi(0, 1) selected by a
            // known-boolean branch condition → alias to cond directly.
            // Eliminates the `immw 1` / `immw 0` materialisations for
            // patterns like `carry = (x == 1) ? 1 : 0`.
            //
            // SAFETY: this introduces new uses of `cond` via the phi's alias
            // chain.  Emission's P5/P5+/P6/P17 fusion peepholes guard against
            // that by bailing out when cond->use_count > 1 (see emit.c
            // detect_branch_fusions) and OOS skips aliased phis so their
            // parallel copies never overwrite cond's register.
            if (inst->kind == IK_PHI && inst->nops == 2 &&
                b->npreds == 2) {
                KnownBits k0 = kb_get(inst->ops[0]);
                KnownBits k1 = kb_get(inst->ops[1]);
                uint32_t unk0 = ~k0.zero & ~k0.one;
                uint32_t unk1 = ~k1.zero & ~k1.one;
                // Both operands must be exact constants in {0, 1}.
                if (unk0 == 0 && unk1 == 0 &&
                    (k0.one | k1.one) <= 1u &&
                    k0.one != k1.one) {
                    // Trace each predecessor back through any 1-pred/1-succ
                    // intermediate blocks (introduced by critical-edge
                    // splitting) to find the common branching ancestor `h`.
                    Block *side[2];
                    Block *anc[2];
                    for (int s = 0; s < 2; s++) {
                        Block *p = b->preds[s];
                        side[s] = p;
                        while (p->npreds == 1 && p->preds[0]->nsuccs == 1) {
                            side[s] = p;
                            p = p->preds[0];
                        }
                        anc[s] = (p->npreds == 1) ? p->preds[0] : NULL;
                        if (anc[s] == NULL) side[s] = NULL;
                    }
                    if (anc[0] && anc[0] == anc[1] && side[0] != side[1]) {
                        Block *h = anc[0];
                        Inst *term = h->tail;
                        if (term && term->kind == IK_BR && term->nops >= 1 &&
                            term->target && term->target2) {
                            Value *cond = val_resolve(term->ops[0]);
                            KnownBits kc = kb_get(cond);
                            // cond must be known-boolean (bits 1-31 known zero).
                            // vtype mismatch is safe for booleans: {0, 1} has
                            // the same bit representation under sign- and zero-
                            // extension alike, so aliasing a wider cond to a
                            // narrower phi (or vice versa) preserves semantics.
                            if ((kc.zero & ~1u) == ~1u) {
                                // Determine which side of h each predecessor
                                // chain came from.  side[s] is an immediate
                                // successor of h (first block after the chain
                                // originating from h).
                                int t0 = (side[0] == term->target);
                                int f0 = (side[0] == term->target2);
                                int t1 = (side[1] == term->target);
                                int f1 = (side[1] == term->target2);
                                if ((t0 && f1) || (f0 && t1)) {
                                    // side[0] is true-path if t0; then op0 is
                                    // value-when-cond=1.  Alias to cond iff
                                    // that value is 1.
                                    int v_when_true = t0 ? (int)k0.one : (int)k1.one;
                                    if (v_when_true == 1) {
                                        inst->dst->alias = cond;
                                        inst->is_dead = 1;
                                        changed = 1;
                                        continue;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            if (inst->kind == IK_AND && inst->nops == 2) {
                Value *v0 = val_resolve(inst->ops[0]);
                Value *v1 = val_resolve(inst->ops[1]);
                Value *mask_v = NULL, *src = NULL;
                if (v1->kind == VAL_CONST) { mask_v = v1; src = v0; }
                else if (v0->kind == VAL_CONST) { mask_v = v0; src = v1; }
                if (!mask_v) continue;
                uint32_t mask = (uint32_t)mask_v->iconst;

                // If all bits outside mask are already known zero, AND is a no-op.
                // Only safe when source and dest have the same vtype —
                // aliasing across different types loses signedness information
                // that affects downstream widening (sign-extend vs zero-extend).
                KnownBits kb = kb_get(src);
                if ((~mask & ~kb.zero) == 0 &&
                    src->vtype == inst->dst->vtype) {
                    inst->dst->alias = src;
                    inst->is_dead = 1;
                    changed = 1;
                    continue;
                }

                // Try to unwrap the source through TRUNC/ZEXT/AND/COPY
                Value *unwrapped = unwrap_for_mask(src, mask);
                if (unwrapped != src) {
                    if (v1->kind == VAL_CONST) inst->ops[0] = unwrapped;
                    else                       inst->ops[1] = unwrapped;
                    changed = 1;
                }
            }

            // Boolean comparison simplification for known-boolean sources
            // (bits 1-31 known zero, bit 0 possibly set — i.e. value ∈ {0, 1}):
            //   EQ(x, 1) → x         (alias)
            //   NE(x, 0) → x         (alias) — catches cases R2G misses because
            //                                  x isn't directly a comparison result
            //                                  (e.g. x = xor(and a 1, and b 1))
            //   EQ(x, 1) → NE(x, 0)  (rotation — enables P6 jnz when alias vtype
            //                         guard would fail; currently unused since
            //                         alias is always safe for booleans, but kept
            //                         as a no-op fallback for clarity)
            //   NE(x, 1) → EQ(x, 0)  (rotation — enables P6 jz)
            //
            // Aliasing across vtypes is safe for booleans: {0, 1} has the same
            // bit representation under sign- and zero-extension alike, so no
            // downstream widening is affected.  R2G uses the same approach.
            if ((inst->kind == IK_EQ || inst->kind == IK_NE) && inst->nops == 2) {
                Value *v0 = val_resolve(inst->ops[0]);
                Value *v1 = val_resolve(inst->ops[1]);
                Value *src = NULL;
                int const_idx = -1;
                int const_val = -1;
                if (v1->kind == VAL_CONST && (v1->iconst == 0 || v1->iconst == 1)) {
                    src = v0; const_idx = 1; const_val = v1->iconst;
                } else if (v0->kind == VAL_CONST && (v0->iconst == 0 || v0->iconst == 1)) {
                    src = v1; const_idx = 0; const_val = v0->iconst;
                }
                if (src) {
                    KnownBits kb = kb_get(src);
                    if ((kb.zero & ~1u) == ~1u) {   // bits 1-31 known zero
                        int is_alias = (inst->kind == IK_EQ && const_val == 1) ||
                                       (inst->kind == IK_NE && const_val == 0);
                        if (is_alias) {
                            inst->dst->alias = src;
                            inst->is_dead = 1;
                            changed = 1;
                            continue;
                        }
                        // Compare-with-1 → compare-with-0 rotation for P6
                        if (const_val == 1) {
                            Value *zero = new_const(f, 0, src->vtype);
                            inst->ops[const_idx] = zero;
                            zero->use_count++;
                            inst->kind = (inst->kind == IK_EQ) ? IK_NE : IK_EQ;
                            changed = 1;
                        }
                    }
                }
            }

            // Note: TRUNC/ZEXT elimination is intentionally omitted.
            // Aliasing across vtypes loses signedness that downstream
            // widening depends on.  The important case — TRUNC under AND —
            // is handled by unwrap_for_mask above.
        }
    }

    if (!changed) return;
    recount_uses(f);
}

// ── R2L: Bitwise distribution ────────────────────────────────────────────
//
// OP(AND(a, c), AND(b, c)) → AND(OP(a, b), c)  where OP ∈ {XOR, OR, AND}.
// Uses unwrap_for_mask to see through TRUNC/ZEXT/COPY chains on the inner
// AND operands, so (data & 1) ^ ((unsigned char)crc & 1) becomes
// (data ^ crc) & 1 — the cast is irrelevant because mask 1 is narrower
// than the truncation width.

void opt_bitwise_dist(Function *f) {
    int changed = 0;
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead || !inst->dst || inst->nops != 2) continue;
            if (inst->kind != IK_XOR && inst->kind != IK_OR &&
                inst->kind != IK_AND) continue;

            Value *lhs = val_resolve(inst->ops[0]);
            Value *rhs = val_resolve(inst->ops[1]);
            if (!lhs || !rhs) continue;
            if (lhs->kind != VAL_INST || rhs->kind != VAL_INST) continue;
            Inst *li = lhs->def, *ri = rhs->def;
            if (!li || !ri) continue;
            if (li->kind != IK_AND || ri->kind != IK_AND) continue;
            if (li->nops != 2 || ri->nops != 2) continue;

            // Find the common constant mask
            Value *la0 = val_resolve(li->ops[0]), *la1 = val_resolve(li->ops[1]);
            Value *ra0 = val_resolve(ri->ops[0]), *ra1 = val_resolve(ri->ops[1]);
            if (!la0 || !la1 || !ra0 || !ra1) continue;

            Value *lc = NULL, *lv = NULL, *rc = NULL, *rv = NULL;
            if (la1->kind == VAL_CONST) { lc = la1; lv = la0; }
            else if (la0->kind == VAL_CONST) { lc = la0; lv = la1; }
            else continue;
            if (ra1->kind == VAL_CONST) { rc = ra1; rv = ra0; }
            else if (ra0->kind == VAL_CONST) { rc = ra0; rv = ra1; }
            else continue;

            if (lc->iconst != rc->iconst) continue;
            if (lhs->use_count > 1 || rhs->use_count > 1) continue;

            // Unwrap operands through operations invisible to the mask
            uint32_t mask = (uint32_t)lc->iconst;
            lv = unwrap_for_mask(lv, mask);
            rv = unwrap_for_mask(rv, mask);

            // Create OP(lv, rv) before inst
            Value *nv = new_value(f, VAL_INST, inst->dst->vtype);
            Inst *ni = new_inst(f, b, inst->kind, nv);
            ni->ops = arena_alloc(2 * sizeof(Value *));
            ni->nops = 2;
            ni->ops[0] = lv;
            ni->ops[1] = rv;
            nv->def = ni;
            ni->prev = inst->prev;
            ni->next = inst;
            if (inst->prev) inst->prev->next = ni;
            else b->head = ni;
            inst->prev = ni;

            // Rewrite inst to AND(nv, c)
            inst->kind = IK_AND;
            inst->ops[0] = nv;
            inst->ops[1] = lc;
            changed = 1;
        }
    }

    if (!changed) return;
    recount_uses(f);
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

        // Count non-dead instructions in loop body to estimate complexity.
        int body_insts = 0;
        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            if (!dominates(h, b)) continue;
            for (Inst *inst = b->head; inst; inst = inst->next)
                if (!inst->is_dead) body_insts++;
        }

        int budget = (body_insts > 16) ? 3 : 6;
        if (nlive + 1 > budget) {
            // Budget exceeded for general hoisting.  Still try to hoist the
            // loop-bound constant (VAL_CONST operand of the header's
            // terminator comparison).  P12 (emit_rotated_branch) duplicates
            // the comparison in the latch, so the constant is materialized
            // twice per iteration.  Allow one hoist with a relaxed cap.
            if (nlive + 1 > 6) continue;
            Inst *hdr_term = h->tail;
            if (!hdr_term || hdr_term->kind != IK_BR || hdr_term->nops < 1)
                continue;
            Value *hdr_cond = val_resolve(hdr_term->ops[0]);
            if (!hdr_cond || hdr_cond->kind != VAL_INST || !hdr_cond->def
                || hdr_cond->def->block != h)
                continue;
            Inst *cmp = hdr_cond->def;
            int did_hoist = 0;
            for (int j = 0; j < cmp->nops && !did_hoist; j++) {
                Value *v = val_resolve(cmp->ops[j]);
                if (!v || v->kind != VAL_CONST) continue;
                Value *nv = new_value(f, VAL_INST, v->vtype);
                Inst *ni = new_inst(f, preheader, IK_CONST, nv);
                ni->imm = v->iconst;
                nv->iconst = v->iconst;
                inst_insert_before(preheader->tail, ni);
                for (int bi2 = 0; bi2 < f->nblocks; bi2++) {
                    Block *b2 = f->blocks[bi2];
                    if (!dominates(h, b2)) continue;
                    for (Inst *inst = b2->head; inst; inst = inst->next) {
                        if (inst->is_dead) continue;
                        for (int k = 0; k < inst->nops; k++) {
                            Value *op = val_resolve(inst->ops[k]);
                            if (op && op->kind == VAL_CONST &&
                                op->iconst == v->iconst && op->vtype == v->vtype)
                                inst->ops[k] = nv;
                        }
                    }
                }
                did_hoist = 1;
                changed = 1;
            }
            continue;
        }

        int max_hoist = 4;

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
        // Step 5: Sort candidates by use count (descending) and pick top N.
        // Only hoist constants with uses >= 3 (enough savings to justify the
        // register cost across the loop body).
        int nhoisted = 0;
        for (int pass = 0; pass < max_hoist; pass++) {
            int best = -1, best_uses = 2;   // hoist when uses > 2 (i.e. >= 3)
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
    recount_uses(f);
}

// ── R2K: General LICM (loop-invariant code motion) ──────────────────────
//
// Move pure loop-invariant instructions from loop bodies to pre-headers.
// A pure instruction is loop-invariant if all its operands are either
// constants, defined outside the loop, or themselves loop-invariant.
// Only processes leaf loops with a single pre-header.
// Excludes division/mod (potential traps if loop executes 0 times).

void opt_licm(Function *f) {
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

        // Only leaf loops (no inner loop headers dominated by h).
        int has_inner = 0;
        for (int k = 0; k < nheaders; k++) {
            if (k != li && dominates(h, headers[k])) {
                has_inner = 1; break;
            }
        }
        if (has_inner) continue;

        // Find pre-header (single outside predecessor).
        Block *preheader = NULL;
        int outside_count = 0;
        for (int k = 0; k < h->npreds; k++) {
            Block *p = h->preds[k];
            if (!dominates(h, p)) {
                preheader = p;
                outside_count++;
            }
        }
        if (outside_count != 1 || !preheader || !preheader->tail) continue;

        // Register pressure guard: count distinct VAL_INST values used
        // inside the loop body that are defined OUTSIDE the loop.  Each
        // hoisted instruction adds one more cross-loop live value, so we
        // need headroom.  Also count unique definitions inside the loop
        // to estimate peak internal pressure.
        #define LICM_GEN_LIVE_CAP 64
        int live_ids[LICM_GEN_LIVE_CAP];
        int nlive = 0;          // values defined outside, used inside
        int nloop_defs = 0;     // unique values defined inside
        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            if (!dominates(h, b)) continue;
            for (Inst *inst = b->head; inst; inst = inst->next) {
                if (inst->is_dead) continue;
                // Count defs
                if (inst->dst) {
                    int vid = inst->dst->id, dup2 = 0;
                    for (int k = 0; k < nlive + nloop_defs && !dup2; k++)
                        if (live_ids[k] == vid) dup2 = 1;
                    if (!dup2 && nlive + nloop_defs < LICM_GEN_LIVE_CAP) {
                        live_ids[nlive + nloop_defs] = vid;
                        nloop_defs++;
                    }
                }
                // Count uses defined outside
                for (int j = 0; j < inst->nops; j++) {
                    Value *v = val_resolve(inst->ops[j]);
                    if (!v || v->kind != VAL_INST || !v->def) continue;
                    if (!dominates(h, v->def->block)) {
                        int vid = v->id, dup2 = 0;
                        for (int k = 0; k < nlive && !dup2; k++)
                            if (live_ids[k] == vid) dup2 = 1;
                        if (!dup2 && nlive < LICM_GEN_LIVE_CAP)
                            live_ids[nlive++] = vid;
                    }
                }
            }
        }

        // Conservative budget: need at least 4 free registers for in-body
        // temporaries (address computations, intermediate values), and each
        // hoist adds 1 to the cross-loop pressure.  Cap budget when loop-
        // internal definitions are high (tight loops with many values).
        int budget = 8 - 4 - nlive;   // reserve 4 regs for in-body temps
        if (budget <= 0) continue;
        if (nloop_defs > 10)
            budget = budget < 1 ? 0 : 1;
        else if (nloop_defs > 6)
            budget = budget < 2 ? budget : 2;
        if (budget > 4) budget = 4;

        // Build def-count per value inside the loop.
        int nv = f->nvalues;
        int *loop_defined = calloc(nv, sizeof(int));
        int *def_count    = calloc(nv, sizeof(int));
        if (!loop_defined || !def_count) goto next_loop;

        for (int bi = 0; bi < f->nblocks; bi++) {
            if (!dominates(h, f->blocks[bi])) continue;
            Block *b = f->blocks[bi];
            for (Inst *inst = b->head; inst; inst = inst->next) {
                if (!inst->dst) continue;
                // Resolve through aliases so that CSE'd duplicates (e.g.
                // v50→v28 after CSE aliased v50=shl to v28=shl) are
                // counted against the canonical value.
                int did = val_resolve(inst->dst)->id;
                // Only count live definitions.  Dead defs (CSE-killed
                // duplicates) are now unlinked before liveness analysis
                // (alloc.c pre-pass), so they don't affect register pressure.
                if (!inst->is_dead) {
                    def_count[did]++;
                    loop_defined[did] = 1;
                }
            }
        }

        // Check if loop body has no memory-modifying instructions.
        // When true, loads from invariant addresses can be hoisted.
        int loop_store_free = 1;
        for (int bi = 0; bi < f->nblocks && loop_store_free; bi++) {
            if (!dominates(h, f->blocks[bi])) continue;
            Block *b = f->blocks[bi];
            for (Inst *inst = b->head; inst; inst = inst->next) {
                if (inst->is_dead) continue;
                if (inst->kind == IK_STORE || inst->kind == IK_MEMCPY ||
                    inst->kind == IK_CALL  || inst->kind == IK_ICALL  ||
                    inst->kind == IK_PUTCHAR) {
                    loop_store_free = 0;
                    break;
                }
            }
        }

        // Iterative invariance marking.
        int *inv = calloc(nv, sizeof(int));
        if (!inv) goto next_loop;

        int progress = 1;
        while (progress) {
            progress = 0;
            for (int bi = 0; bi < f->nblocks; bi++) {
                if (!dominates(h, f->blocks[bi])) continue;
                Block *b = f->blocks[bi];
                for (Inst *inst = b->head; inst; inst = inst->next) {
                    if (inst->is_dead || !inst->dst) continue;
                    int did = val_resolve(inst->dst)->id;
                    if (inv[did]) continue;
                    if (!is_cse_pure(inst->kind) &&
                        !(inst->kind == IK_LOAD && loop_store_free)) continue;
                    // Exclude division/mod — could trap if loop runs 0 times
                    if (inst->kind == IK_DIV  || inst->kind == IK_UDIV ||
                        inst->kind == IK_MOD  || inst->kind == IK_UMOD ||
                        inst->kind == IK_FDIV) continue;
                    // Exclude IK_GADDR — cheap to recompute (3 bytes, 1 cycle);
                    // not worth occupying a register across the entire loop.
                    if (inst->kind == IK_GADDR) continue;
                    // Exclude type conversions — they lower to a single AND or
                    // sign-extend instruction (1–2 cycles); hoisting extends a
                    // live range across the loop for negligible savings.
                    if (inst->kind == IK_SEXT8  || inst->kind == IK_SEXT16 ||
                        inst->kind == IK_ZEXT   || inst->kind == IK_TRUNC) continue;
                    // Must be the only definition in the loop (no phi-vars
                    // or CSE-eliminated duplicates)
                    if (def_count[did] != 1) continue;

                    int all_inv = 1;
                    for (int j = 0; j < inst->nops; j++) {
                        Value *v = val_resolve(inst->ops[j]);
                        if (v->kind == VAL_CONST || v->kind == VAL_UNDEF)
                            continue;
                        if (!loop_defined[v->id]) continue; // outside → ok
                        if (inv[v->id]) continue;           // marked inv → ok
                        all_inv = 0;
                        break;
                    }
                    if (all_inv) {
                        inv[did] = 1;
                        progress = 1;
                    }
                }
            }
        }

        // Collect invariant instructions in IR order, limited by budget.
        Inst **to_hoist = malloc(budget * sizeof(Inst *));
        int nhoist = 0;
        if (!to_hoist) goto next_loop;

        for (int bi = 0; bi < f->nblocks && nhoist < budget; bi++) {
            if (!dominates(h, f->blocks[bi])) continue;
            Block *b = f->blocks[bi];
            for (Inst *inst = b->head; inst && nhoist < budget; inst = inst->next) {
                if (inst->is_dead || !inst->dst) continue;
                int rid = val_resolve(inst->dst)->id;
                if (inv[rid])
                    to_hoist[nhoist++] = inst;
            }
        }

        // Hoist: unlink from current block, insert in pre-header before terminator.
        if (getenv("LICM_DEBUG")) {
            if (nhoist > 0) {
                fprintf(stderr, "LICM: func=%s loop B%d→pre B%d, hoisting %d (budget=%d nlive=%d nloop_defs=%d):\n",
                        f->name, h->id, preheader->id, nhoist, budget, nlive, nloop_defs);
                for (int i = 0; i < nhoist; i++) {
                    Inst *di = to_hoist[i];
                    fprintf(stderr, "  v%d (kind=%d) from B%d\n",
                            di->dst->id, di->kind, di->block->id);
                }
            }
        }
        for (int i = 0; i < nhoist; i++) {
            Inst *inst = to_hoist[i];
            Block *b = inst->block;

            // Unlink
            if (inst->prev) inst->prev->next = inst->next;
            else            b->head = inst->next;
            if (inst->next) inst->next->prev = inst->prev;
            else            b->tail = inst->prev;

            // Insert before pre-header terminator
            inst_insert_before(preheader->tail, inst);
            changed = 1;
        }

        free(to_hoist);
    next_loop:
        free(loop_defined);
        free(def_count);
        free(inv);
    }
    (void)changed;
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

// ── Loop Strength Reduction (pre-OOS) ────────────────────────────────────
//
// For each leaf loop, find basic induction variables (phis whose back-edge
// input is iv + const_step).  For each IK_MUL(iv, loop_invariant), replace
// with a strength-reduced induction variable: sr_phi = phi(iv_init * inv,
// sr_prev + step * inv).  The original MUL becomes dead after aliasing.

#define LSR_MAX_IVS  8
#define LSR_MAX_REDS 4

typedef struct {
    Value *phi_val;    // the phi's dst value
    Inst  *phi_inst;   // the IK_PHI instruction
    int    init_idx;   // index in ops[] of the pre-header operand
    int    back_idx;   // index in ops[] of the back-edge operand
    Value *init_val;   // initial value (from pre-header)
    Value *step_val;   // constant step value
    Inst  *add_inst;   // the IK_ADD instruction (iv_next = iv + step)
} IVInfo;

void opt_lsr(Function *f) {
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
                        Block **nh = malloc(hdr_cap * sizeof(Block *));
                        if (headers) { memcpy(nh, headers, nheaders * sizeof(Block *)); free(headers); }
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

        // Only leaf loops (no inner loop headers dominated by h).
        int has_inner = 0;
        for (int k = 0; k < nheaders; k++) {
            if (k != li && dominates(h, headers[k])) {
                has_inner = 1; break;
            }
        }
        if (has_inner) continue;

        // Find pre-header (single non-back-edge predecessor) and back-edge pred.
        Block *preheader = NULL;
        int pre_idx = -1, back_idx = -1;
        int outside_count = 0;
        for (int k = 0; k < h->npreds; k++) {
            Block *p = h->preds[k];
            if (dominates(h, p)) {
                back_idx = k;
            } else {
                preheader = p;
                pre_idx = k;
                outside_count++;
            }
        }
        if (outside_count != 1 || !preheader || pre_idx < 0 || back_idx < 0) continue;

        // Register pressure guard: count values defined outside the loop
        // that are used inside (cross-loop live-ins), plus header phis.
        // Each LSR reduction adds one new cross-loop phi, so we need room.
        int nlive = 0;
        {
            int live_cap = 64;
            int live_ids[64];
            // Count header phis
            for (Inst *inst = h->head; inst; inst = inst->next) {
                if (inst->is_dead) continue;
                if (inst->kind != IK_PHI) break;
                if (nlive < live_cap) live_ids[nlive++] = inst->dst->id;
            }
            // Count external values used inside the loop
            for (int bi = 0; bi < f->nblocks; bi++) {
                Block *bb = f->blocks[bi];
                if (!dominates(h, bb)) continue;
                for (Inst *inst = bb->head; inst; inst = inst->next) {
                    if (inst->is_dead) continue;
                    for (int j = 0; j < inst->nops; j++) {
                        Value *v = val_resolve(inst->ops[j]);
                        if (!v || v->kind != VAL_INST || !v->def) continue;
                        if (dominates(h, v->def->block)) continue;  // internal
                        int dup = 0;
                        for (int k = 0; k < nlive; k++)
                            if (live_ids[k] == v->id) { dup = 1; break; }
                        if (!dup && nlive < live_cap) live_ids[nlive++] = v->id;
                    }
                }
            }
        }
        int lsr_budget = 8 - 4 - nlive;  // K - scratch - existing cross-loop values
        if (lsr_budget <= 0) continue;

        // Step 2: Detect basic induction variables in header phis.
        IVInfo ivs[LSR_MAX_IVS];
        int niv = 0;

        for (Inst *inst = h->head; inst; inst = inst->next) {
            if (inst->is_dead || inst->kind != IK_PHI) continue;
            if (inst->nops != h->npreds) continue;
            if (niv >= LSR_MAX_IVS) break;

            Value *init = val_resolve(inst->ops[pre_idx]);
            Value *back = val_resolve(inst->ops[back_idx]);

            // back must be iv + step_const (or step_const + iv)
            if (back->kind != VAL_INST || !back->def) continue;
            Inst *add = back->def;
            if (add->kind != IK_ADD || add->nops != 2) continue;
            if (!dominates(h, add->block)) continue;  // must be inside loop

            Value *a0 = val_resolve(add->ops[0]);
            Value *a1 = val_resolve(add->ops[1]);
            Value *step = NULL;
            if (a0 == inst->dst && (a1->kind == VAL_CONST ||
                (a1->kind == VAL_INST && a1->def && a1->def->kind == IK_CONST))) {
                step = a1;
            } else if (a1 == inst->dst && (a0->kind == VAL_CONST ||
                (a0->kind == VAL_INST && a0->def && a0->def->kind == IK_CONST))) {
                step = a0;
            }
            if (!step) continue;

            ivs[niv].phi_val  = inst->dst;
            ivs[niv].phi_inst = inst;
            ivs[niv].init_idx = pre_idx;
            ivs[niv].back_idx = back_idx;
            ivs[niv].init_val = init;
            ivs[niv].step_val = step;
            ivs[niv].add_inst = add;
            niv++;
        }
        if (niv == 0) continue;

        // Step 3: Find MUL candidates in loop body.
        int nreduced = 0;
        int max_reds = lsr_budget < LSR_MAX_REDS ? lsr_budget : LSR_MAX_REDS;
        for (int bi = 0; bi < f->nblocks && nreduced < max_reds; bi++) {
            Block *b = f->blocks[bi];
            if (!dominates(h, b)) continue;
            for (Inst *inst = b->head; inst && nreduced < max_reds; inst = inst->next) {
                if (inst->is_dead || inst->kind != IK_MUL || inst->nops != 2) continue;

                Value *m0 = val_resolve(inst->ops[0]);
                Value *m1 = val_resolve(inst->ops[1]);

                // Match MUL(iv, invariant) or MUL(invariant, iv)
                for (int iv_i = 0; iv_i < niv; iv_i++) {
                    IVInfo *iv = &ivs[iv_i];
                    Value *inv = NULL;
                    if (m0 == iv->phi_val) inv = m1;
                    else if (m1 == iv->phi_val) inv = m0;
                    if (!inv) continue;

                    // inv must be loop-invariant (defined outside or constant)
                    if (inv->kind == VAL_INST && inv->def && dominates(h, inv->def->block))
                        continue;  // defined inside loop — not invariant

                    // Create strength-reduced induction variable:
                    // sr_init = iv_init * inv  (in pre-header)
                    // sr_step = step * inv     (in pre-header)
                    // sr_phi  = phi(sr_init, sr_next)  (in header)
                    // sr_next = sr_phi + sr_step       (after iv's add)

                    ValType vt = inst->dst->vtype;

                    // sr_init = iv_init * inv
                    Value *sr_init_val = new_value(f, VAL_INST, vt);
                    Inst  *sr_init = new_inst(f, preheader, IK_MUL, sr_init_val);
                    inst_add_op(sr_init, iv->init_val);
                    inst_add_op(sr_init, inv);
                    inst_insert_before(preheader->tail, sr_init);

                    // sr_step = step * inv
                    Value *sr_step_val = new_value(f, VAL_INST, vt);
                    Inst  *sr_step = new_inst(f, preheader, IK_MUL, sr_step_val);
                    inst_add_op(sr_step, iv->step_val);
                    inst_add_op(sr_step, inv);
                    inst_insert_before(preheader->tail, sr_step);

                    // sr_phi = phi(sr_init, sr_next) in header
                    // (sr_next is a forward reference, patched below)
                    Value *sr_phi_val = new_value(f, VAL_INST, vt);
                    Inst  *sr_phi = new_inst(f, h, IK_PHI, sr_phi_val);
                    // Add ops in preds[] order
                    for (int p = 0; p < h->npreds; p++) {
                        if (p == pre_idx) inst_add_op(sr_phi, sr_init_val);
                        else              inst_add_op(sr_phi, sr_init_val); // placeholder
                    }
                    // Prepend phi to header (before first non-phi)
                    Inst *first_non_phi = h->head;
                    while (first_non_phi && first_non_phi->kind == IK_PHI)
                        first_non_phi = first_non_phi->next;
                    if (first_non_phi)
                        inst_insert_before(first_non_phi, sr_phi);
                    else
                        inst_append(h, sr_phi);

                    // sr_next = sr_phi + sr_step (placed right after iv's add)
                    Value *sr_next_val = new_value(f, VAL_INST, vt);
                    Inst  *sr_next = new_inst(f, iv->add_inst->block, IK_ADD, sr_next_val);
                    inst_add_op(sr_next, sr_phi_val);
                    inst_add_op(sr_next, sr_step_val);
                    // Insert after iv's add instruction
                    if (iv->add_inst->next)
                        inst_insert_before(iv->add_inst->next, sr_next);
                    else
                        inst_append(iv->add_inst->block, sr_next);

                    // Patch phi's back-edge operand to sr_next
                    sr_phi->ops[back_idx] = sr_next_val;
                    sr_next_val->use_count++;
                    sr_init_val->use_count--;  // was placeholder

                    // Alias original MUL's result to sr_phi
                    inst->dst->alias = sr_phi_val;
                    inst->is_dead = 1;

                    if (getenv("LSR_DEBUG"))
                        fprintf(stderr, "LSR: func=%s v%d=mul(iv=v%d,inv=v%d) → sr_phi=v%d in B%d\n",
                            f->name, inst->dst->id, iv->phi_val->id,
                            inv->id, sr_phi_val->id, h->id);

                    nreduced++;
                    changed = 1;
                    break;  // move to next instruction
                }
            }
        }
    }

    free(headers);

    if (!changed) return;

    // Physically unlink dead instructions (the reduced MULs) so they don't
    // participate in OOS or subsequent passes.
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        Inst *inst = b->head;
        while (inst) {
            Inst *next = inst->next;
            if (inst->is_dead) {
                if (inst->prev) inst->prev->next = inst->next;
                else            b->head = inst->next;
                if (inst->next) inst->next->prev = inst->prev;
                else            b->tail = inst->prev;
            }
            inst = next;
        }
    }

    // Recount use_count after aliasing and unlinking.  The physical unlink
    // loop above has already removed all is_dead instructions from the list.
    recount_uses(f);
}

// ── Scalar Promotion (pre-OOS) ───────────────────────────────────────────
//
// For each leaf loop with a load-modify-store pattern to a loop-invariant
// address, replace the load/store cycle with a register-carried accumulator
// phi.  The init store in the pre-header seeds the phi; an exit store after
// the loop writes the final value back to memory.
//
// This eliminates a load+store per iteration, freeing the address register
// inside the loop body and reducing register pressure.

void opt_scalar_promote(Function *f) {
    if (f->nblocks < 2) return;

    // Find loop headers + latch blocks via back-edge detection
    struct { Block *h; Block *latch; } loops[32];
    int nloops = 0;
    for (int i = 0; i < f->nblocks; i++) {
        Block *b = f->blocks[i];
        for (int j = 0; j < b->nsuccs; j++) {
            Block *h = b->succs[j];
            if (h->rpo_index <= b->rpo_index && dominates(h, b)) {
                int dup = 0;
                for (int k = 0; k < nloops; k++)
                    if (loops[k].h == h) { dup = 1; break; }
                if (!dup && nloops < 32) {
                    loops[nloops].h = h;
                    loops[nloops].latch = b;
                    nloops++;
                }
            }
        }
    }
    if (!nloops) return;

    int changed = 0;

    for (int li = 0; li < nloops; li++) {
        Block *h = loops[li].h;
        Block *latch = loops[li].latch;

        // Leaf loops only
        int inner = 0;
        for (int k = 0; k < nloops && !inner; k++)
            if (k != li && dominates(h, loops[k].h)) inner = 1;
        if (inner) continue;

        // Pre-header + back-edge predecessor indices
        Block *preh = NULL;
        int pre_idx = -1, back_idx = -1, outside = 0;
        for (int k = 0; k < h->npreds; k++) {
            if (dominates(h, h->preds[k])) back_idx = k;
            else { preh = h->preds[k]; pre_idx = k; outside++; }
        }
        if (outside != 1 || !preh || back_idx < 0) continue;

        // Compute natural loop body via worklist
        int mark_sz = f->next_blk_id;
        int *in_body = calloc(mark_sz, sizeof(int));
        if (!in_body) continue;
        in_body[h->id] = 1;
        Block *wl[256]; int wl_n = 0;
        if (latch != h) { in_body[latch->id] = 1; wl[wl_n++] = latch; }
        while (wl_n > 0) {
            Block *c = wl[--wl_n];
            for (int p = 0; p < c->npreds; p++) {
                Block *pr = c->preds[p];
                if (pr->id < mark_sz && !in_body[pr->id]) {
                    in_body[pr->id] = 1;
                    if (wl_n < 256) wl[wl_n++] = pr;
                }
            }
        }

        // Single exit block
        Block *exit_blk = NULL;
        int multi_exit = 0;
        for (int bi = 0; bi < f->nblocks && !multi_exit; bi++) {
            Block *b = f->blocks[bi];
            if (!in_body[b->id]) continue;
            for (int si = 0; si < b->nsuccs && !multi_exit; si++) {
                Block *s = b->succs[si];
                if (s->id < mark_sz && !in_body[s->id]) {
                    if (!exit_blk) exit_blk = s;
                    else if (exit_blk != s) multi_exit = 1;
                }
            }
        }
        if (multi_exit || !exit_blk) { free(in_body); continue; }

        // Safety: no calls or memcpy in loop body
        int unsafe = 0;
        for (int bi = 0; bi < f->nblocks && !unsafe; bi++) {
            Block *b = f->blocks[bi];
            if (!in_body[b->id]) continue;
            for (Inst *inst = b->head; inst && !unsafe; inst = inst->next) {
                if (inst->is_dead) continue;
                if (inst->kind == IK_CALL || inst->kind == IK_ICALL || inst->kind == IK_MEMCPY)
                    unsafe = 1;
            }
        }
        if (unsafe) { free(in_body); continue; }

        // Find a promotable load-modify-store pattern
        int promoted = 0;
        for (int bi = 0; bi < f->nblocks && !promoted; bi++) {
            Block *b = f->blocks[bi];
            if (!in_body[b->id] || b == h) continue;

            for (Inst *store = b->head; store && !promoted; store = store->next) {
                if (store->is_dead || store->kind != IK_STORE) continue;

                Value *addr = val_resolve(store->ops[0]);
                int sz = store->size;
                int imm = store->imm;

                // Address must be loop-invariant (defined outside loop)
                if (addr->kind == VAL_INST && addr->def && in_body[addr->def->block->id])
                    continue;

                // Find matching load before this store in same block
                Inst *load = NULL;
                for (Inst *i = b->head; i != store; i = i->next) {
                    if (i->is_dead || i->kind != IK_LOAD) continue;
                    if (i->size != sz || i->imm != imm) continue;
                    if (val_resolve(i->ops[0]) == addr) { load = i; break; }
                }
                if (!load) continue;

                // Store value must depend on load result (1-hop check)
                Value *sv = val_resolve(store->ops[1]);
                if (sv->kind != VAL_INST || !sv->def) continue;
                int dep = 0;
                for (int j = 0; j < sv->def->nops; j++)
                    if (val_resolve(sv->def->ops[j]) == load->dst) { dep = 1; break; }
                if (!dep) continue;

                // No other stores in loop body (conservative: avoids aliasing issues)
                int other_st = 0;
                for (int bi2 = 0; bi2 < f->nblocks && !other_st; bi2++) {
                    Block *bb = f->blocks[bi2];
                    if (!in_body[bb->id]) continue;
                    for (Inst *i = bb->head; i && !other_st; i = i->next) {
                        if (!i->is_dead && i->kind == IK_STORE && i != store)
                            other_st = 1;
                    }
                }
                if (other_st) continue;

                // Find init store in pre-header (same address, same size)
                Inst *init_st = NULL;
                for (Inst *i = preh->head; i; i = i->next) {
                    if (i->is_dead || i->kind != IK_STORE) continue;
                    if (i->size != sz || i->imm != imm) continue;
                    if (val_resolve(i->ops[0]) == addr) { init_st = i; break; }
                }
                if (!init_st) continue;

                // === Perform scalar promotion ===
                Value *init_val = val_resolve(init_st->ops[1]);

                // Create accumulator phi in header: acc = phi(init_val, sv)
                Value *acc = new_value(f, VAL_INST, load->dst->vtype);
                Inst *phi = new_inst(f, h, IK_PHI, acc);
                for (int p = 0; p < h->npreds; p++) {
                    if (p == pre_idx) inst_add_op(phi, init_val);
                    else              inst_add_op(phi, sv);
                }
                // Insert after existing phis in header
                Inst *ins_pt = h->head;
                while (ins_pt && ins_pt->kind == IK_PHI) ins_pt = ins_pt->next;
                if (ins_pt) inst_insert_before(ins_pt, phi);
                else        inst_append(h, phi);

                // Alias load result → accumulator phi
                load->dst->alias = acc;
                load->is_dead = 1;

                // Kill loop store and init store
                store->is_dead = 1;
                init_st->is_dead = 1;

                // Insert exit store: store [addr]:sz = acc
                Inst *exit_st = new_inst(f, exit_blk, IK_STORE, NULL);
                inst_add_op(exit_st, addr);
                inst_add_op(exit_st, acc);
                exit_st->imm = imm;
                exit_st->size = sz;
                if (exit_blk->head)
                    inst_insert_before(exit_blk->head, exit_st);
                else
                    inst_append(exit_blk, exit_st);

                promoted = 1;
                changed = 1;
            }
        }
        free(in_body);
    }

    if (!changed) return;

    // Unlink dead instructions
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        Inst *inst = b->head;
        while (inst) {
            Inst *next = inst->next;
            if (inst->is_dead) {
                if (inst->prev) inst->prev->next = inst->next;
                else            b->head = inst->next;
                if (inst->next) inst->next->prev = inst->prev;
                else            b->tail = inst->prev;
            }
            inst = next;
        }
    }

    // Recount use_count after aliasing and unlinking.  The physical unlink
    // loop above has already removed all is_dead instructions from the list.
    recount_uses(f);
}

// ── Address Induction Variables (pre-OOS) ────────────────────────────────
//
// For loads whose address is BASE + (OFFSET + f(iv)) << SHIFT, replace
// the address chain with a pointer induction variable:
//   ptr_phi = phi(BASE + OFFSET<<SHIFT, ptr_phi + step)
// where step = iv_step << SHIFT (constant for simple IVs) or
//              COEFF << SHIFT  (loop-invariant for MUL-derived IVs).
//
// Eliminates ADD+SHL+ADD (3 instr) or MUL+ADD+SHL+ADD (4 instr) from the
// loop body, replacing with a single ADD in the latch.

static int aiv_invariant(Value *v, int *in_body) {
    v = val_resolve(v);
    if (v->kind == VAL_CONST) return 1;
    if (v->kind != VAL_INST || !v->def) return 0;
    if (!in_body[v->def->block->id]) return 1;
    // Recursively check: pure computation of invariant operands
    // (e.g. MUL(outer_iv, N) inside inner loop where both are invariant)
    Inst *d = v->def;
    if (d->kind == IK_PHI) return 0;
    if (!is_cse_pure(d->kind)) return 0;
    for (int i = 0; i < d->nops; i++)
        if (!aiv_invariant(d->ops[i], in_body)) return 0;
    return 1;
}

// Clone a loop-invariant value (possibly defined inside the loop) into the
// pre-header so it can be used by init-pointer / step computations.
static Value *aiv_materialize(Value *v, int *in_body, Function *f, Block *preh) {
    v = val_resolve(v);
    if (v->kind == VAL_CONST) return v;
    if (v->kind != VAL_INST || !v->def) return v;
    if (!in_body[v->def->block->id]) return v;  // already outside loop
    // Clone the instruction into the pre-header
    Inst *orig = v->def;
    Value *dst = new_value(f, VAL_INST, v->vtype);
    Inst *ni = new_inst(f, preh, orig->kind, dst);
    ni->imm = orig->imm;
    ni->fname = orig->fname;
    for (int i = 0; i < orig->nops; i++)
        inst_add_op(ni, aiv_materialize(orig->ops[i], in_body, f, preh));
    inst_insert_before(preh->tail, ni);
    return dst;
}

void opt_addr_iv(Function *f) {
    if (f->nblocks < 2) return;

    // Find loop headers + latch blocks via back-edge detection
    struct { Block *h; Block *latch; } loops[32];
    int nloops = 0;
    for (int i = 0; i < f->nblocks; i++) {
        Block *b = f->blocks[i];
        for (int j = 0; j < b->nsuccs; j++) {
            Block *h = b->succs[j];
            if (h->rpo_index <= b->rpo_index && dominates(h, b)) {
                int dup = 0;
                for (int k = 0; k < nloops; k++)
                    if (loops[k].h == h) { dup = 1; break; }
                if (!dup && nloops < 32) {
                    loops[nloops].h = h;
                    loops[nloops].latch = b;
                    nloops++;
                }
            }
        }
    }
    if (!nloops) return;

    int changed = 0;

    for (int li = 0; li < nloops; li++) {
        Block *h = loops[li].h;
        Block *latch = loops[li].latch;

        // Leaf loops only
        int inner = 0;
        for (int k = 0; k < nloops && !inner; k++)
            if (k != li && dominates(h, loops[k].h)) inner = 1;
        if (inner) continue;

        // Pre-header + back-edge predecessor indices
        Block *preh = NULL;
        int pre_idx = -1, back_idx = -1, outside = 0;
        for (int k = 0; k < h->npreds; k++) {
            if (dominates(h, h->preds[k])) back_idx = k;
            else { preh = h->preds[k]; pre_idx = k; outside++; }
        }
        if (outside != 1 || !preh || back_idx < 0) continue;

        // Compute natural loop body
        int mark_sz = f->next_blk_id;
        int *in_body = calloc(mark_sz, sizeof(int));
        if (!in_body) continue;
        in_body[h->id] = 1;
        Block *wl[256]; int wl_n = 0;
        if (latch != h) { in_body[latch->id] = 1; wl[wl_n++] = latch; }
        while (wl_n > 0) {
            Block *c = wl[--wl_n];
            for (int p = 0; p < c->npreds; p++) {
                Block *pr = c->preds[p];
                if (pr->id < mark_sz && !in_body[pr->id]) {
                    in_body[pr->id] = 1;
                    if (wl_n < 256) wl[wl_n++] = pr;
                }
            }
        }

        // Detect basic induction variables (same as LSR)
        IVInfo ivs[LSR_MAX_IVS];
        int niv = 0;
        for (Inst *inst = h->head; inst; inst = inst->next) {
            if (inst->is_dead || inst->kind != IK_PHI) continue;
            if (inst->nops != h->npreds) continue;
            if (niv >= LSR_MAX_IVS) break;

            Value *init = val_resolve(inst->ops[pre_idx]);
            Value *back = val_resolve(inst->ops[back_idx]);
            if (back->kind != VAL_INST || !back->def) continue;
            Inst *add = back->def;
            if (add->kind != IK_ADD || add->nops != 2) continue;
            if (!in_body[add->block->id]) continue;

            Value *a0 = val_resolve(add->ops[0]);
            Value *a1 = val_resolve(add->ops[1]);
            Value *step = NULL;
            if (a0 == inst->dst && (a1->kind == VAL_CONST ||
                (a1->kind == VAL_INST && a1->def && a1->def->kind == IK_CONST)))
                step = a1;
            else if (a1 == inst->dst && (a0->kind == VAL_CONST ||
                (a0->kind == VAL_INST && a0->def && a0->def->kind == IK_CONST)))
                step = a0;
            if (!step) continue;

            ivs[niv].phi_val  = inst->dst;
            ivs[niv].phi_inst = inst;
            ivs[niv].init_idx = pre_idx;
            ivs[niv].back_idx = back_idx;
            ivs[niv].init_val = init;
            ivs[niv].step_val = step;
            ivs[niv].add_inst = add;
            niv++;
        }
        if (niv == 0) { free(in_body); continue; }

        // Scan loads in loop body for reducible address patterns
        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            if (!in_body[b->id] || b == h) continue;

            for (Inst *load = b->head; load; load = load->next) {
                if (load->is_dead || load->kind != IK_LOAD || load->nops < 1) continue;
                if (load->imm != 0) continue;

                // Match: LOAD [ADD(base, SHL(sum, shift_c))]:sz
                Value *addr = val_resolve(load->ops[0]);
                if (addr->kind != VAL_INST || !addr->def) continue;
                Inst *addr_i = addr->def;
                if (addr_i->kind != IK_ADD || addr_i->nops != 2) continue;
                if (!in_body[addr_i->block->id]) continue;

                Value *op0 = val_resolve(addr_i->ops[0]);
                Value *op1 = val_resolve(addr_i->ops[1]);

                // One operand is loop-invariant base, other is SHL result
                Value *base = NULL, *shl_v = NULL;
                for (int sw = 0; sw < 2 && !base; sw++) {
                    Value *mb = sw ? op1 : op0;
                    Value *ms = sw ? op0 : op1;
                    if (!aiv_invariant(mb, in_body)) continue;
                    if (ms->kind != VAL_INST || !ms->def || ms->def->kind != IK_SHL) continue;
                    base = mb; shl_v = ms;
                }
                if (!base) continue;

                // Match SHL(sum, const_shift)
                Inst *shl_i = shl_v->def;
                if (shl_i->nops != 2) continue;
                Value *sum_v = val_resolve(shl_i->ops[0]);
                Value *shift_c = val_resolve(shl_i->ops[1]);
                if (shift_c->kind != VAL_CONST) continue;
                int shift = shift_c->iconst;
                if (shift < 1 || shift > 4) continue;

                // sum must be ADD defined inside loop
                if (sum_v->kind != VAL_INST || !sum_v->def) continue;
                Inst *sum_i = sum_v->def;
                if (sum_i->kind != IK_ADD || sum_i->nops != 2) continue;
                if (!in_body[sum_i->block->id]) continue;

                Value *s0 = val_resolve(sum_i->ops[0]);
                Value *s1 = val_resolve(sum_i->ops[1]);

                // Match against basic IVs:
                // Pattern A: ADD(inv, iv) — step = iv_step << shift
                // Pattern B: ADD(MUL(iv, coeff), inv) — step = coeff << shift
                IVInfo *miv = NULL;
                Value *inv_offset = NULL;
                Value *mul_coeff = NULL;

                for (int iv_i = 0; iv_i < niv && !miv; iv_i++) {
                    IVInfo *iv = &ivs[iv_i];

                    // Pattern A: one operand is iv, other is invariant
                    if (s0 == iv->phi_val && aiv_invariant(s1, in_body)) {
                        miv = iv; inv_offset = s1;
                    } else if (s1 == iv->phi_val && aiv_invariant(s0, in_body)) {
                        miv = iv; inv_offset = s0;
                    }
                    if (miv) break;

                    // Pattern B: one operand is MUL(iv, coeff), other is invariant
                    for (int sw = 0; sw < 2 && !miv; sw++) {
                        Value *mm = val_resolve(sw ? s1 : s0);
                        Value *mi = sw ? s0 : s1;
                        if (!aiv_invariant(mi, in_body)) continue;
                        if (mm->kind != VAL_INST || !mm->def) continue;
                        if (mm->def->kind != IK_MUL || mm->def->nops != 2) continue;
                        if (!in_body[mm->def->block->id]) continue;
                        Value *m0 = val_resolve(mm->def->ops[0]);
                        Value *m1 = val_resolve(mm->def->ops[1]);
                        if (m0 == iv->phi_val && aiv_invariant(m1, in_body)) {
                            miv = iv; inv_offset = mi; mul_coeff = m1;
                        } else if (m1 == iv->phi_val && aiv_invariant(m0, in_body)) {
                            miv = iv; inv_offset = mi; mul_coeff = m0;
                        }
                    }
                }
                if (!miv) continue;

                // Materialize invariant values that may be defined inside the
                // loop (e.g. i*N in the j-loop) into the pre-header.
                inv_offset = aiv_materialize(inv_offset, in_body, f, preh);
                base = aiv_materialize(base, in_body, f, preh);
                if (mul_coeff)
                    mul_coeff = aiv_materialize(mul_coeff, in_body, f, preh);

                // Compute init pointer in pre-header
                Value *iv_init = val_resolve(miv->init_val);
                int init_zero = (iv_init->kind == VAL_CONST && iv_init->iconst == 0);
                Value *init_ptr;

                if (init_zero) {
                    // Common case: init = base + (inv_offset << shift)
                    Value *sv = new_value(f, VAL_INST, VT_I16);
                    Inst *si = new_inst(f, preh, IK_SHL, sv);
                    inst_add_op(si, inv_offset);
                    inst_add_op(si, shift_c);
                    inst_insert_before(preh->tail, si);

                    init_ptr = new_value(f, VAL_INST, addr->vtype);
                    Inst *ai = new_inst(f, preh, IK_ADD, init_ptr);
                    inst_add_op(ai, base);
                    inst_add_op(ai, sv);
                    inst_insert_before(preh->tail, ai);
                } else if (mul_coeff) {
                    // Pattern B: init = base + (iv_init * coeff + inv_offset) << shift
                    Value *mv = new_value(f, VAL_INST, VT_I16);
                    Inst *mi = new_inst(f, preh, IK_MUL, mv);
                    inst_add_op(mi, iv_init);
                    inst_add_op(mi, mul_coeff);
                    inst_insert_before(preh->tail, mi);

                    Value *av = new_value(f, VAL_INST, VT_I16);
                    Inst *ai = new_inst(f, preh, IK_ADD, av);
                    inst_add_op(ai, mv);
                    inst_add_op(ai, inv_offset);
                    inst_insert_before(preh->tail, ai);

                    Value *sv = new_value(f, VAL_INST, VT_I16);
                    Inst *si = new_inst(f, preh, IK_SHL, sv);
                    inst_add_op(si, av);
                    inst_add_op(si, shift_c);
                    inst_insert_before(preh->tail, si);

                    init_ptr = new_value(f, VAL_INST, addr->vtype);
                    Inst *pi = new_inst(f, preh, IK_ADD, init_ptr);
                    inst_add_op(pi, base);
                    inst_add_op(pi, sv);
                    inst_insert_before(preh->tail, pi);
                } else {
                    // Pattern A, non-zero init: base + ((inv_offset + iv_init) << shift)
                    Value *av = new_value(f, VAL_INST, VT_I16);
                    Inst *ai = new_inst(f, preh, IK_ADD, av);
                    inst_add_op(ai, inv_offset);
                    inst_add_op(ai, iv_init);
                    inst_insert_before(preh->tail, ai);

                    Value *sv = new_value(f, VAL_INST, VT_I16);
                    Inst *si = new_inst(f, preh, IK_SHL, sv);
                    inst_add_op(si, av);
                    inst_add_op(si, shift_c);
                    inst_insert_before(preh->tail, si);

                    init_ptr = new_value(f, VAL_INST, addr->vtype);
                    Inst *pi = new_inst(f, preh, IK_ADD, init_ptr);
                    inst_add_op(pi, base);
                    inst_add_op(pi, sv);
                    inst_insert_before(preh->tail, pi);
                }

                // Compute step value
                Value *step_val;
                if (mul_coeff) {
                    // Pattern B: step = coeff << shift (loop-invariant)
                    step_val = new_value(f, VAL_INST, VT_I16);
                    Inst *si = new_inst(f, preh, IK_SHL, step_val);
                    inst_add_op(si, mul_coeff);
                    inst_add_op(si, shift_c);
                    inst_insert_before(preh->tail, si);
                } else {
                    // Pattern A: step = iv_step << shift (compile-time constant)
                    Value *sv = val_resolve(miv->step_val);
                    if (sv->kind == VAL_CONST) {
                        step_val = new_const(f, sv->iconst << shift, VT_I16);
                    } else {
                        step_val = new_value(f, VAL_INST, VT_I16);
                        Inst *si = new_inst(f, preh, IK_SHL, step_val);
                        inst_add_op(si, sv);
                        inst_add_op(si, shift_c);
                        inst_insert_before(preh->tail, si);
                    }
                }

                // Create pointer phi: phi(init_ptr, ptr_next)
                Value *phi_v = new_value(f, VAL_INST, addr->vtype);
                Inst *phi = new_inst(f, h, IK_PHI, phi_v);
                for (int p = 0; p < h->npreds; p++) {
                    if (p == pre_idx) inst_add_op(phi, init_ptr);
                    else              inst_add_op(phi, init_ptr); // placeholder
                }
                Inst *ins_pt = h->head;
                while (ins_pt && ins_pt->kind == IK_PHI) ins_pt = ins_pt->next;
                if (ins_pt) inst_insert_before(ins_pt, phi);
                else        inst_append(h, phi);

                // Create ptr_next = phi + step in latch
                Value *next_v = new_value(f, VAL_INST, addr->vtype);
                Inst *next_i = new_inst(f, latch, IK_ADD, next_v);
                inst_add_op(next_i, phi_v);
                inst_add_op(next_i, step_val);
                inst_insert_before(latch->tail, next_i);

                // Patch phi back-edge operand
                phi->ops[back_idx] = next_v;
                next_v->use_count++;
                init_ptr->use_count--;

                // Replace load address with pointer phi
                load->ops[0] = phi_v;

                changed = 1;
            }
        }
        free(in_body);
    }

    if (!changed) return;

    // Recount use_count.  The replaced address-chain instructions are not
    // marked is_dead here; they simply lose all their users via the direct
    // operand swap above, so their use_count drops to 0 and IRC DCE removes
    // them later.
    recount_uses(f);
}
