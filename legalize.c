#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "legalize.h"
#include "opt.h"     // opt_flags, OPT_LEG_*
#include "dom.h"     // dominates, compute_dominators
#include "alloc.h"   // IRC_CALLER_REGS


// ── Pass H support: frame-slot store→load forwarding ──────────────────────
static int is_pure_kind(Inst *inst);
static unsigned char *mark_live_values(Function *f);
typedef struct { int off, size; Value *v; } SlotEnt;
#define SLOT_MAX 48
typedef struct { SlotEnt e[SLOT_MAX]; int n; } SlotMap;

static int ranges_overlap(int a, int as, int b, int bs) { return a < b + bs && b < a + as; }

static void slotmap_invalidate(SlotMap *m, int off, int size) {
    int w = 0;
    for (int i = 0; i < m->n; i++)
        if (!ranges_overlap(m->e[i].off, m->e[i].size, off, size)) m->e[w++] = m->e[i];
    m->n = w;
}
static void slotmap_set(SlotMap *m, int off, int size, Value *v) {
    slotmap_invalidate(m, off, size);
    if (!v) return;
    if (m->n == SLOT_MAX) { memmove(&m->e[0], &m->e[1], (SLOT_MAX - 1) * sizeof(SlotEnt)); m->n--; }
    m->e[m->n].off = off; m->e[m->n].size = size; m->e[m->n].v = v; m->n++;
}
static Value *slotmap_get(SlotMap *m, int off, int size) {
    for (int i = m->n - 1; i >= 0; i--)
        if (m->e[i].off == off && m->e[i].size == size) return m->e[i].v;
    return NULL;
}

static void unlink_inst(Block *b, Inst *inst) {
    if (inst->prev) inst->prev->next = inst->next; else b->head = inst->next;
    if (inst->next) inst->next->prev = inst->prev; else b->tail = inst->prev;
    inst->is_dead = 1;
}

static int load_size(Inst *inst) {
    if (inst->size) return inst->size;
    return inst->dst ? vtype_size(inst->dst->vtype) : 4;
}
static int store_size(Inst *inst) {
    if (inst->size) return inst->size;
    Value *v = val_resolve(inst->ops[inst->nops - 1]);
    return v ? vtype_size(v->vtype) : 4;
}
static int is_slot_load(Inst *inst)  { return inst->kind == IK_LOAD  && inst->nops >= 1 && !inst->ops[0] && inst->dst; }
static int is_slot_store(Inst *inst) { return inst->kind == IK_STORE && inst->nops == 1; }

// Escaped frame ranges: every IK_ADDR that still has a use after Pass G is a
// frame address that reaches memory-unaware code (a call argument, a memcpy,
// a pointer store).  Its extent is unknown, so it is taken to run up to the
// next IK_ADDR base above it (locals are laid out contiguously and every
// addressed local has its own base), or to bp / the top of the param area.
typedef struct { int lo, hi; } Range;

static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

static int collect_escapes(Function *f, Range **out) {
    int nb = 0, cap = 16;
    int *bases = malloc(cap * sizeof(int));
    for (int bi = 0; bi < f->nblocks; bi++)
        for (Inst *inst = f->blocks[bi]->head; inst; inst = inst->next)
            if (!inst->is_dead && inst->kind == IK_ADDR) {
                if (nb == cap) { cap *= 2; bases = realloc(bases, cap * sizeof(int)); }
                bases[nb++] = inst->imm;
            }
    qsort(bases, nb, sizeof(int), cmp_int);
    // Which IK_ADDR values are used by *live* instructions.  Pass G leaves
    // the folded address arithmetic behind for IRC's dead-code pass to
    // remove; a mark-and-sweep from the impure instructions (nothing is
    // unlinked here — that was measured to perturb IRC's spill choices)
    // tells us which uses will survive.
    unsigned char *live = mark_live_values(f);
    unsigned char *used = calloc(f->nvalues + 1, 1);
    for (int bi = 0; bi < f->nblocks; bi++)
        for (Inst *inst = f->blocks[bi]->head; inst; inst = inst->next) {
            if (inst->is_dead) continue;
            if (is_pure_kind(inst) && !(inst->dst->id < f->nvalues && live[inst->dst->id])) continue;
            for (int j = 0; j < inst->nops; j++) {
                Value *v = val_resolve(inst->ops[j]);
                if (v && v->kind == VAL_INST && v->def && v->def->kind == IK_ADDR && v->id < f->nvalues) {
                    used[v->id] = 1;
                    if (getenv("LEG_DEBUG")) fprintf(stderr, "  [H] %s: addr v%d (bp%+d) used by kind %d nops %d\n", f->name, v->id, v->def->imm, inst->kind, inst->nops);
                }
            }
        }
    free(live);
    Range *r = malloc((nb + 1) * sizeof(Range)); int nr = 0;
    for (int bi = 0; bi < f->nblocks; bi++)
        for (Inst *inst = f->blocks[bi]->head; inst; inst = inst->next) {
            if (inst->is_dead || inst->kind != IK_ADDR || !inst->dst) continue;
            if (inst->dst->id >= f->nvalues || !used[inst->dst->id]) continue;
            int lo = inst->imm, hi = (lo < 0) ? 0 : 0x7fff;
            for (int k = 0; k < nb; k++) if (bases[k] > lo) { hi = bases[k]; break; }
            if (lo >= 0 && hi < 0) hi = 0x7fff;
            r[nr].lo = lo; r[nr].hi = hi; nr++;
        }
    if (getenv("LEG_DEBUG"))
        for (int i = 0; i < nr; i++) fprintf(stderr, "  [H] %s: escaped range [%d,%d)\n", f->name, r[i].lo, r[i].hi);
    free(bases); free(used);
    *out = r;
    return nr;
}

static int escaped(Range *r, int nr, int off, int size) {
    for (int i = 0; i < nr; i++)
        if (ranges_overlap(r[i].lo, r[i].hi - r[i].lo, off, size)) return 1;
    return 0;
}

// Which values have a live definition: mark-and-sweep from the impure
// instructions (stores, calls, control flow) through operand defs.  Values
// pre-coloured to a register count as live (IRC keeps their defs).
static int is_pure_kind(Inst *inst) {
    switch (inst->kind) {
    case IK_STORE: case IK_CALL: case IK_ICALL: case IK_BR: case IK_JMP:
    case IK_RET: case IK_SWITCH: case IK_PUTCHAR: case IK_MEMCPY:
        return 0;
    default:
        return inst->dst != NULL;
    }
}
static unsigned char *mark_live_values(Function *f) {
    unsigned char *live = calloc(f->nvalues + 1, 1);
    Value **work = malloc((f->nvalues + 1) * sizeof(Value *)); int nw = 0;
    for (int bi = 0; bi < f->nblocks; bi++)
        for (Inst *inst = f->blocks[bi]->head; inst; inst = inst->next) {
            if (inst->is_dead) continue;
            int root = !is_pure_kind(inst) || (inst->dst && inst->dst->phys_reg >= 0);
            if (!root) continue;
            if (inst->dst && inst->dst->id < f->nvalues) live[inst->dst->id] = 1;
            for (int j = 0; j < inst->nops; j++) {
                Value *v = val_resolve(inst->ops[j]);
                if (v && v->kind == VAL_INST && v->id < f->nvalues && !live[v->id]) { live[v->id] = 1; work[nw++] = v; }
            }
        }
    while (nw > 0) {
        Value *v = work[--nw];
        if (!v->def) continue;
        for (int j = 0; j < v->def->nops; j++) {
            Value *o = val_resolve(v->def->ops[j]);
            if (o && o->kind == VAL_INST && o->id < f->nvalues && !live[o->id]) { live[o->id] = 1; work[nw++] = o; }
        }
    }
    free(work);
    return live;
}

// A value pre-coloured to a physical register (a param landing in r1-r3, a
// call result in r0) must die at its immediate working copy: keeping it live
// would collide with the next pre-coloured value in the same register, which
// IRC cannot repair.  Forwarding therefore goes through a fresh working copy
// placed right after the definition, made once per value.
static Value *forwardable(Function *f, Value *v, Value ***cache, int *cache_n) {
    if (!v || v->kind != VAL_INST || v->phys_reg < 0 || !v->def) return v;
    if (v->id < *cache_n && (*cache)[v->id]) return (*cache)[v->id];
    Value *w = new_value(f, VAL_INST, v->vtype);
    Inst  *cp = new_inst(f, v->def->block, IK_COPY, w);
    cp->line = v->def->line;
    inst_insert_after(v->def, cp);
    inst_add_op(cp, v);
    v->use_count++;
    if (v->id >= *cache_n) {
        int nn = v->id + 64;
        *cache = realloc(*cache, nn * sizeof(Value *));
        memset(*cache + *cache_n, 0, (nn - *cache_n) * sizeof(Value *));
        *cache_n = nn;
    }
    (*cache)[v->id] = w;
    return w;
}

// Pass H body.  Blocks are visited in reverse post-order; a block with a
// single predecessor inherits that predecessor's exit state.
static void legalize_slot_forward(Function *f) {
    Range *esc; int nesc = collect_escapes(f, &esc);
    Value **wcache = NULL; int wcache_n = 0;

    int nb = f->nblocks;
    Block **order = malloc(nb * sizeof(Block *));
    int no = 0;
    for (int bi = 0; bi < nb; bi++) if (f->blocks[bi]->rpo_index >= 0) order[no++] = f->blocks[bi];
    for (int i = 1; i < no; i++) {              // insertion sort by rpo_index (nb is small)
        Block *b = order[i]; int j = i - 1;
        while (j >= 0 && order[j]->rpo_index > b->rpo_index) { order[j + 1] = order[j]; j--; }
        order[j + 1] = b;
    }
    SlotMap *exit = calloc(nb, sizeof(SlotMap));
    unsigned char *done = calloc(nb, 1);
    SlotMap m;

    for (int oi = 0; oi < no; oi++) {
        Block *b = order[oi];
        int bidx = -1;
        for (int k = 0; k < nb; k++) if (f->blocks[k] == b) { bidx = k; break; }
        m.n = 0;
        if (b->npreds == 1) {
            for (int k = 0; k < nb; k++)
                if (f->blocks[k] == b->preds[0] && done[k]) { m = exit[k]; break; }
        }
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead) continue;
            switch (inst->kind) {
            case IK_LOAD:
                if (!is_slot_load(inst)) break;
                if (load_size(inst) == 4) {
                    Value *v = slotmap_get(&m, inst->imm, 4);
                    if (v) {
                        if (getenv("LEG_DEBUG")) fprintf(stderr, "  [H] %s: load [bp%+d]:4 forwarded from v%d (%s)\n", f->name, inst->imm, v->id, v->vtype == inst->dst->vtype ? "alias" : "copy");
                        if (v->vtype == inst->dst->vtype) {
                            // Same type: the load simply *is* the stored value.
                            v->use_count += inst->dst->use_count;
                            inst->dst->alias = v;
                            unlink_inst(b, inst);
                        } else {
                            // Same bits, different C type (the union pun): a
                            // type-preserving IK_COPY keeps the load's vtype for
                            // emission's signedness decisions; IRC coalesces it.
                            inst->kind = IK_COPY; inst->ops[0] = v; inst->nops = 1;
                            inst->imm = 0; inst->size = 0;
                            v->use_count++;
                        }
                    } else {
                        slotmap_set(&m, inst->imm, 4, inst->dst);
                    }
                }
                break;
            case IK_STORE:
                if (is_slot_store(inst)) {
                    int sz = store_size(inst);
                    Value *v = val_resolve(inst->ops[0]);
                    if (sz == 4 && v && v->kind == VAL_INST) v = forwardable(f, v, &wcache, &wcache_n);
                    slotmap_set(&m, inst->imm, sz, (sz == 4 && v && v->kind == VAL_INST) ? v : NULL);
                } else {
                    // pointer store: may hit any escaped slot
                    int w = 0;
                    for (int i = 0; i < m.n; i++)
                        if (!escaped(esc, nesc, m.e[i].off, m.e[i].size)) m.e[w++] = m.e[i];
                    m.n = w;
                }
                break;
            case IK_MEMCPY: case IK_CALL: case IK_ICALL: {
                int w = 0;
                for (int i = 0; i < m.n; i++)
                    if (!escaped(esc, nesc, m.e[i].off, m.e[i].size)) m.e[w++] = m.e[i];
                m.n = w;
                break;
            }
            default: break;
            }
        }
        if (bidx >= 0) { exit[bidx] = m; done[bidx] = 1; }
    }

    // Dead-store elimination for private slots: a store whose range no
    // remaining (live) slot load overlaps, and which no escaped address can reach.
    unsigned char *live = mark_live_values(f);
    for (int bi = 0; bi < nb; bi++) {
        Block *b = f->blocks[bi];
        Inst *inst = b->head;
        while (inst) {
            Inst *next = inst->next;
            if (!inst->is_dead && is_slot_store(inst)) {
                int sz = store_size(inst);
                if (!escaped(esc, nesc, inst->imm, sz)) {
                    int read = 0;
                    for (int bj = 0; bj < nb && !read; bj++)
                        for (Inst *q = f->blocks[bj]->head; q; q = q->next)
                            if (!q->is_dead && is_slot_load(q) && q->dst->id < f->nvalues && live[q->dst->id] &&
                                ranges_overlap(q->imm, load_size(q), inst->imm, sz)) { read = 1; break; }
                    if (getenv("LEG_DEBUG")) fprintf(stderr, "  [H] %s: store [bp%+d]:%d %s\n", f->name, inst->imm, sz, read ? "kept (loaded)" : "dead");
                    if (!read) {
                        Value *sv = val_resolve(inst->ops[0]);
                        if (sv && sv->kind == VAL_INST && sv->use_count > 0) sv->use_count--;
                        unlink_inst(b, inst);
                    }
                }
            }
            inst = next;
        }
    }
    free(order); free(exit); free(done); free(esc); free(wcache); free(live);
}

void legalize_function(Function *f) {
    if (!f || f->nblocks == 0) return;

    // ── Pass A: Pre-color IK_PARAM landing values ───────────────────────
    // braun.c emits IK_PARAM with phys_reg=-1; assign r1/r2/r3 here so
    // all ABI register knowledge lives in one place.
    // param_idx == idx+1 (1-based: r1 for first arg, r2 for second, r3 for third).
    for (Inst *inst = f->blocks[0]->head; inst; inst = inst->next) {
        if (inst->kind == IK_PARAM && inst->dst && inst->param_idx >= 1)
            inst->dst->phys_reg = inst->param_idx;
    }

    // ── Pass B: Pre-color call argument copies and ICALL fp copy ────────
    // Replaces emit_reg_arg_copies() that used to be called from braun.c.
    // For non-variadic IK_CALL: insert IK_COPY pre-colored to r1/r2/r3 before call.
    // For IK_ICALL: same for register args, plus r0 for the function pointer
    //   (emitted AFTER arg copies so fp stays live through them, preventing
    //    IRC from assigning fp to r1/r2/r3 via interference).
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->kind != IK_CALL && inst->kind != IK_ICALL) continue;
            if (inst->calldesc && inst->calldesc->is_variadic) continue;

            int is_icall = (inst->kind == IK_ICALL);
            int arg_base = is_icall ? 1 : 0;
            int nargs    = inst->nops - arg_base;
            int nreg     = nargs < (IRC_CALLER_REGS - 1) ? nargs : (IRC_CALLER_REGS - 1);

            for (int i = 0; i < nreg; i++) {
                Value *arg = val_resolve(inst->ops[arg_base + i]);
                if (!arg) continue;
                Value *v_ri = new_value(f, VAL_INST, arg->vtype);
                v_ri->phys_reg = i + 1;  // r1, r2, r3
                Inst  *cp = new_inst(f, b, IK_COPY, v_ri);
                cp->line = inst->line;
                inst_add_op(cp, arg);       // +1 on arg (copy's use)
                arg->use_count--;           // call no longer uses arg directly
                inst_insert_before(inst, cp);
                inst->ops[arg_base + i] = v_ri;
                v_ri->use_count++;          // call's new reference
            }

            if (is_icall) {
                // fp copy inserted last so fp remains live through the arg copies
                Value *fp = val_resolve(inst->ops[0]);
                if (fp) {
                    Value *v_r0 = new_value(f, VAL_INST, fp->vtype);
                    v_r0->phys_reg = 0;  // r0 for jlr
                    Inst  *fp_cp = new_inst(f, b, IK_COPY, v_r0);
                    fp_cp->line = inst->line;
                    inst_add_op(fp_cp, fp);   // +1 on fp (copy's use)
                    fp->use_count--;          // call no longer uses fp directly
                    inst_insert_before(inst, fp_cp);
                    inst->ops[0] = v_r0;
                    v_r0->use_count++;        // call's new reference
                }
            }
        }
    }

    // ── Pass B2: Attach a scratch def to IK_MEMCPY ──────────────────────
    // emit.c expands IK_MEMCPY as an inline load/store loop and needs a
    // data register. A hardcoded scratch clobbers whatever lives there
    // (historically r2 — the second struct parameter's incoming pointer).
    // Give the memcpy a dst value so IRC allocates the scratch;
    // build_interference_graph adds explicit dst↔operand edges since the
    // scratch is written between reads of the pointer operands.
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->kind != IK_MEMCPY || inst->dst) continue;
            Value *scratch = new_value(f, VAL_INST, VT_I32);
            scratch->def = inst;
            inst->dst = scratch;
        }
    }

    // ── Pass C: Lower IK_NEG (float) / IK_NOT ──────────────────────────
    // Integer IK_NEG is kept as-is: emit.c emits the native `neg rd` F1b
    // instruction (2 bytes) which does not need a zero register.
    // Float IK_NEG → IK_FSUB(0, src): no native float-neg, still needs a zero.
    // IK_NOT src → IK_EQ(src, 0): needs a zero register.
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->kind != IK_NEG && inst->kind != IK_NOT) continue;
            if (!inst->dst || inst->nops < 1) continue;
            // Integer NEG: emit.c handles natively with F1b `neg rd`.
            if (inst->kind == IK_NEG && inst->dst->vtype != VT_F32) continue;

            Value *zero_v = new_value(f, VAL_INST, inst->dst->vtype);
            Inst  *zero_i = new_inst(f, b, IK_CONST, zero_v);
            zero_i->imm   = 0;
            zero_i->line  = inst->line;
            inst_insert_before(inst, zero_i);

            Value  *src      = inst->ops[0];
            Value **new_ops  = arena_alloc(2 * sizeof(Value *));

            if (inst->kind == IK_NEG) {
                // IK_NEG (float) src → IK_FSUB(0, src)
                new_ops[0] = zero_v;  zero_v->use_count++;
                new_ops[1] = src;     // use_count unchanged (was already counted)
                inst->kind = IK_FSUB;
            } else {
                // IK_NOT src → IK_EQ(src, 0)
                new_ops[0] = src;
                new_ops[1] = zero_v;  zero_v->use_count++;
                inst->kind = IK_EQ;
            }
            inst->ops  = new_ops;
            inst->nops = 2;
        }
    }

    // ── Pass D: Lower IK_ZEXT / IK_TRUNC to IK_CONST + IK_AND ─────────
    // emit.c previously used PUSH_SCRATCH for the AND mask register,
    // hiding the register traffic from IRC.  Insert an explicit IK_CONST
    // so IRC allocates a physical register for the mask, then replace the
    // zext/trunc with IK_AND.  When mask_size >= 4, no masking is needed.
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead) continue;
            if (inst->kind != IK_ZEXT && inst->kind != IK_TRUNC) continue;
            if (!inst->dst || inst->nops < 1) continue;

            Value *src_val  = inst->ops[0] ? val_resolve(inst->ops[0]) : NULL;
            int    src_size = src_val ? vtype_size(src_val->vtype) : 2;
            int    dst_size = vtype_size(inst->dst->vtype);
            int    mask_size = (inst->kind == IK_ZEXT) ? src_size : dst_size;
            if (mask_size >= 4) continue;  // 32-bit; no masking needed

            int mask_val = (mask_size == 1) ? 0xff : 0xffff;

            // Fast path: constant source — fold to IK_CONST(src & mask).
            // Handles both VAL_CONST and VAL_INST defined by IK_CONST
            // (e.g. LICM-hoisted constants demoted from VAL_CONST).
            {
                int is_src_const = 0;
                int src_k = 0;
                if (src_val && src_val->kind == VAL_CONST) {
                    is_src_const = 1; src_k = src_val->iconst;
                } else if (src_val && src_val->kind == VAL_INST &&
                           src_val->def && src_val->def->kind == IK_CONST) {
                    is_src_const = 1; src_k = src_val->def->imm;
                }
                if (is_src_const) {
                    int folded = src_k & mask_val;
                    inst->kind = IK_CONST;
                    inst->imm  = folded;
                    inst->nops = 0;
                    continue;
                }
            }

            // Insert %mask = IK_CONST mask_val before this instruction
            Value *mask_v = new_value(f, VAL_INST, VT_I16);
            Inst  *mask_i = new_inst(f, b, IK_CONST, mask_v);
            mask_i->imm   = mask_val;
            mask_i->line  = inst->line;
            inst_insert_before(inst, mask_i);

            // Replace the ZEXT/TRUNC in-place with IK_AND(src, mask)
            inst->kind = IK_AND;
            inst_add_op(inst, mask_v);  // ops[0]=src already; ops[1]=mask
        }
    }

    // ── Pass E: AND-chain constant folding ──────────────────────────────
    // Fold AND(AND(x, c1), c2) → AND(x, c1 & c2) when both c1, c2 are constants.
    // Also looks through type-coercing IK_COPY instructions (e.g. u8→i16 zero-
    // extension after TRUNC lowering), since copy_prop leaves those intact.
    // Typical source: (unsigned char)expr & 1 — Pass D lowers TRUNC(expr) to
    // AND(expr, 255); the result is sign/zero-copied to i16; then AND(copy, 1).
    // Fold: AND(copy(AND(expr, 255)), 1) → AND(expr, 1).  Correct because the
    // copy zero-extends (high bits 0) so AND with any mask c2 ≤ 0xff is safe.
    // Dead inner AND + IK_CONST(255) + copy are removed by irc_allocate DCE.
    if (opt_flags & OPT_LEG_E) {
        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            for (Inst *inst = b->head; inst; inst = inst->next) {
                if (inst->is_dead || inst->kind != IK_AND || inst->nops < 2) continue;
                int c2;
                if (!get_iconst(val_resolve(inst->ops[1]), &c2)) continue;

                // Look through copy_prop-surviving IK_COPY chains (e.g. u8→i16 coercion).
                // CAUTION: under ILP32 this look-through has caused subtle
                // miscompiles (CoreMark crcs went off without it being obvious why);
                // keeping it for now but worth re-investigating if anything fails.
                // Bounded walk: post-OOS copy chains that implement
                // loop-carried phis can be CYCLIC (a = copy b in one pred,
                // b = copy a on the back edge) — an unbounded walk here
                // hangs the compiler (found by tools/fuzz.py, seed 77).
                Value *orig0 = val_resolve(inst->ops[0]);  // the value the AND actually uses
                Value *inner = orig0;
                for (int hops = 0;
                     inner && inner->kind == VAL_INST && inner->def &&
                     inner->def->kind == IK_COPY && inner->def->nops >= 1 &&
                     hops < 16; hops++)
                    inner = val_resolve(inner->def->ops[0]);

                if (!inner || inner->kind != VAL_INST || !inner->def) continue;
                if (inner->def->kind != IK_AND || inner->def->nops < 2) continue;
                // ILP32 safety: the inner AND must be in the same block as the
                // outer AND. Without this, the repoint at line 200 can move a
                // use of `inner->def->ops[1-inner_const_pos]` to a point that
                // its definition no longer dominates — silently producing wrong
                // values when blocks are reordered or only sometimes reachable.
                if (inner->def->block != b) continue;

                // Check both operand positions for the inner AND's constant
                // (braun.c may emit AND(const, x) or AND(x, const))
                int c1;
                int inner_const_pos = -1;
                if (get_iconst(val_resolve(inner->def->ops[1]), &c1))      inner_const_pos = 1;
                else if (get_iconst(val_resolve(inner->def->ops[0]), &c1)) inner_const_pos = 0;
                if (inner_const_pos < 0) continue;

                int combined = c1 & c2;
                // Repoint outer AND's first operand past the inner AND (and any
                // copies). The use being dropped is on orig0 — the head of the
                // copy chain — not on `inner` (they differ when a chain was
                // walked; decrementing inner undercounted it, tripping the
                // verifier once const canonicalization made chains common).
                orig0->use_count--;
                inst->ops[0] = val_resolve(inner->def->ops[1 - inner_const_pos]);
                inst->ops[0]->use_count++;

                if (combined != c2) {
                    // Need a new constant; replace ops[1] with IK_CONST(combined)
                    Value *new_mask_v = new_value(f, VAL_INST, VT_I16);
                    Inst  *new_mask_i = new_inst(f, b, IK_CONST, new_mask_v);
                    new_mask_i->imm  = combined;
                    new_mask_i->line = inst->line;
                    inst_insert_before(inst, new_mask_i);
                    Value *old1 = val_resolve(inst->ops[1]);
                    if (old1 && old1->kind == VAL_INST) old1->use_count--;
                    inst->ops[1] = new_mask_v;
                    new_mask_v->use_count++;
                }
                // If combined == c2, ops[1] is already the right constant; leave it alone.
            }
        }
    }

    // ── Pass F: Materialize large VAL_CONST binary ALU operands ────────────
    // Also run early (start of the post-OOS pipeline) so CSE and LICM see the
    // materialised IK_CONSTs; this call catches anything created since.
    legalize_materialize_consts(f);

    // ── Pass G: fold IK_ADDR(bp+slot) bases into bp-relative loads/stores ──
    // A frame-slot access written as `IK_ADDR a, slot; IK_LOAD d, [a+off]`
    // costs a lea plus an F3c load.  The bp-relative form (NULL base,
    // imm = slot+off) is the single F2 instruction the spill code already
    // uses, and emit_bp_load/store fall back to lea+F3c when the combined
    // offset is out of F2 range, so the rewrite is always safe.  The IK_ADDR
    // survives for any other use (an escaping pointer) and is otherwise
    // removed by IRC's dead-code elimination.
    if (opt_flags & OPT_LEG_G) {
        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            for (Inst *inst = b->head; inst; inst = inst->next) {
                if (inst->is_dead) continue;
                if (inst->kind != IK_LOAD && inst->kind != IK_STORE) continue;
                if (inst->kind == IK_LOAD  && inst->nops < 1) continue;
                if (inst->kind == IK_STORE && inst->nops != 2) continue;
                if (!inst->ops[0]) continue;
                Value *base = val_resolve(inst->ops[0]);
                if (!base || base->kind != VAL_INST || !base->def || base->def->is_dead) continue;
                int size = (inst->kind == IK_LOAD) ? load_size(inst) : store_size(inst);
                // Look through one `base = p + k` (field / element offset): the
                // constant folds into the access's own offset.  For a plain
                // pointer p the folded offset must stay within the F3c scaled
                // imm10 (±511 elements); for a frame address any offset works.
                int k = 0;
                if (base->def->kind == IK_ADD && base->def->nops == 2) {
                    Value *a0 = val_resolve(base->def->ops[0]);
                    Value *a1 = val_resolve(base->def->ops[1]);
                    int k0, k1;
                    Value *p = NULL;
                    if (a1 && get_iconst(a1, &k1) && a0 && !get_iconst(a0, &k0))      { p = a0; k = k1; }
                    else if (a0 && get_iconst(a0, &k0) && a1 && !get_iconst(a1, &k1)) { p = a1; k = k0; }
                    if (!p || p->kind != VAL_INST || !p->def) continue;
                    int noff = inst->imm + k;
                    if (p->def->kind == IK_ADDR && !p->def->is_dead) {
                        // fall into the frame case below (base keeps its use until then)
                    } else {
                        if (noff % size != 0 || noff / size < -511 || noff / size > 511) continue;
                        inst->imm    = noff;
                        inst->ops[0] = p;
                        if (base->use_count > 0) base->use_count--;
                        p->use_count++;
                        continue;
                    }
                    base = p;
                }
                if (base->def->kind != IK_ADDR) continue;
                Value *old = val_resolve(inst->ops[0]);
                if (old && old->use_count > 0) old->use_count--;
                inst->imm += base->def->imm + k;
                if (inst->kind == IK_LOAD) {
                    inst->ops[0] = NULL;
                } else {
                    inst->ops[0] = inst->ops[1];
                    inst->ops[1] = NULL;
                    inst->nops   = 1;
                }
            }
        }
    }

    // ── Pass H: forward frame-slot stores to later loads, drop dead private stores ──
    // After Pass G a local that never escapes is just a set of bp offsets; a
    // store followed (on every path) by a same-sized load of the same offset
    // becomes a register copy, and a store nothing ever loads is deleted.
    // This is what makes an inlined out-param vector live in registers and
    // makes a union type-pun free.
    if (opt_flags & OPT_LEG_H) {
        const char *only = getenv("LEG_H_ONLY");          // debugging aid: restrict Pass H to one function
        if (!only || (f->name && strstr(f->name, only)))
            legalize_slot_forward(f);
    }

    // ── Pass I: fmadd/fmsub formation (opt-in) ─────────────────────────────
    // FADD(x, FMUL(a,b)) / FADD(FMUL(a,b), x) → FMADD(x, a, b);
    // FSUB(x, FMUL(a,b)) → FMSUB(x, a, b), when the product has no other use.
    // The ISA defines fmadd as the same two roundings, so this is exact.
    if (opt_flags & OPT_FMADD) {
        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            for (Inst *inst = b->head; inst; inst = inst->next) {
                if (inst->is_dead || inst->nops != 2 || !inst->dst) continue;
                if (inst->kind != IK_FADD && inst->kind != IK_FSUB) continue;
                Value *x = val_resolve(inst->ops[0]), *y = val_resolve(inst->ops[1]);
                Value *acc = NULL, *prod = NULL;
                if (y && y->kind == VAL_INST && y->def && y->def->kind == IK_FMUL && !y->def->is_dead && y->use_count == 1)
                    { acc = x; prod = y; }
                else if (inst->kind == IK_FADD && x && x->kind == VAL_INST && x->def && x->def->kind == IK_FMUL &&
                         !x->def->is_dead && x->use_count == 1)
                    { acc = y; prod = x; }
                if (!acc || !prod || acc->kind != VAL_INST) continue;
                Inst *mul = prod->def;
                if (mul->nops != 2) continue;
                Value *a = val_resolve(mul->ops[0]), *bb = val_resolve(mul->ops[1]);
                if (!a || !bb || a->kind != VAL_INST || bb->kind != VAL_INST) continue;
                inst->kind   = (inst->kind == IK_FADD) ? IK_FMADD : IK_FMSUB;
                inst->ops[0] = acc; inst->ops[1] = a; inst->nops = 2;
                inst_add_op(inst, bb);                    // ops = (acc, a, b)
                prod->use_count = 0;                      // the product's only use is gone
                unlink_inst(mul->block, mul);             // a, b keep their use counts (moved, not added)
            }
        }
    }
}

// Materialize VAL_CONST operands that no compact emit-time encoding can
// absorb, as explicit IK_CONST instructions.  Called at the start of the
// post-OOS pipeline (so CSE dedupes and LICM hoists them) and again from
// legalize_function for anything created later.  Idempotent.
void legalize_materialize_consts(Function *f) {
    // When a binary ALU or comparison instruction has a VAL_CONST operand
    // whose value cannot be handled by any compact emit-time encoding,
    // insert an explicit IK_CONST before it.  This lets IRC allocate a
    // register for the constant, eliminating emit.c's pushr/popr fallback
    // that borrows a scratch register at runtime.
    //
    // Compact encodings that CAN handle VAL_CONST in emit.c:
    //   ADD/SUB: P2 (±1 inc/dec), P3 (addi ±64), P4 (addli ±255)
    //   AND:     P14 (andi 0..127), P8 (zxb 0xFF, zxw 0xFFFF)
    //   CMP=0:   P6 (jz/jnz) — only EQ/NE against zero
    // Everything else must be materialized into a register.
    if (opt_flags & OPT_LEG_F) {
        for (int bi = 0; bi < f->nblocks; bi++) {
            Block *b = f->blocks[bi];
            for (Inst *inst = b->head; inst; inst = inst->next) {
                if (inst->is_dead || inst->nops < 2 || !inst->dst) continue;
                // Only binary ALU and bitwise (not comparisons — P5+ handles those)
                switch (inst->kind) {
                case IK_OR: case IK_XOR:
                case IK_MUL: case IK_DIV: case IK_UDIV:
                case IK_MOD: case IK_UMOD:
                case IK_SHL: case IK_SHR: case IK_USHR:
                case IK_FADD: case IK_FSUB: case IK_FMUL: case IK_FDIV:
                case IK_FLT:  case IK_FLE:  case IK_FEQ:  case IK_FNE:
                    // Float compares: FEQ/FNE emit as integer eq/ne (P17/P19 take
                    // |k| ≤ 255); FLT/FLE have no immediate form, but a zero
                    // operand is a 1-byte zeroN, so only larger values are worth
                    // a register.
                    // F0b immediate handles |k| ≤ 255 (P18 mulli, P19 general);
                    // P11/P13 handle SHL/SHR/USHR shifts via resolve_const.
                    // Only materialize constants outside those ranges.
                    for (int j = 0; j < inst->nops; j++) {
                        Value *v = inst->ops[j] ? val_resolve(inst->ops[j]) : NULL;
                        if (!v || v->kind != VAL_CONST) continue;
                        int k = v->iconst;
                        if (k >= -256 && k <= 255) continue;  // F0b immediate range
                        int is_cmp = inst->kind == IK_FLT || inst->kind == IK_FLE ||
                                     inst->kind == IK_FEQ || inst->kind == IK_FNE;
                        Value *cv = new_value(f, VAL_INST, is_cmp ? v->vtype : inst->dst->vtype);
                        Inst  *ci = new_inst(f, b, IK_CONST, cv);
                        ci->imm  = v->iconst;
                        ci->line = inst->line;
                        inst_insert_before(inst, ci);
                        inst->ops[j] = cv;
                        cv->use_count++;
                    }
                    break;
                case IK_ADD: case IK_SUB:
                    // P2/P3/P4 handle |k| ≤ 511; materialize only larger constants
                    for (int j = 0; j < inst->nops; j++) {
                        Value *v = inst->ops[j] ? val_resolve(inst->ops[j]) : NULL;
                        if (!v || v->kind != VAL_CONST) continue;
                        int k = v->iconst;
                        if (inst->kind == IK_SUB && j == 1) k = -k;
                        if (k >= -512 && k <= 511) continue;
                        Value *cv = new_value(f, VAL_INST, inst->dst->vtype);
                        Inst  *ci = new_inst(f, b, IK_CONST, cv);
                        ci->imm  = v->iconst;
                        ci->line = inst->line;
                        inst_insert_before(inst, ci);
                        inst->ops[j] = cv;
                        cv->use_count++;
                    }
                    break;
                case IK_AND:
                    // P14 handles 0..255 (andi 0..127, andli 128..255); P8 handles 0xFFFF
                    for (int j = 0; j < inst->nops; j++) {
                        Value *v = inst->ops[j] ? val_resolve(inst->ops[j]) : NULL;
                        if (!v || v->kind != VAL_CONST) continue;
                        int k = v->iconst;
                        if (k >= 0 && k <= 255) continue;  // P14 andi/andli
                        if (k == 0xffff) continue;  // P8 zxw
                        Value *cv = new_value(f, VAL_INST, inst->dst->vtype);
                        Inst  *ci = new_inst(f, b, IK_CONST, cv);
                        ci->imm  = v->iconst;
                        ci->line = inst->line;
                        inst_insert_before(inst, ci);
                        inst->ops[j] = cv;
                        cv->use_count++;
                    }
                    break;
                default:
                    break;
                }
            }
        }
    }
}
