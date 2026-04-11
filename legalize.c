#include <string.h>
#include "legalize.h"
#include "alloc.h"   // IRC_CALLER_REGS

// Return 1 and set *out if v is a compile-time integer constant (VAL_CONST or IK_CONST).
static int get_iconst(Value *v, int *out) {
    if (!v) return 0;
    if (v->kind == VAL_CONST)                            { *out = v->iconst;   return 1; }
    if (v->kind == VAL_INST && v->def->kind == IK_CONST) { *out = v->def->imm; return 1; }
    return 0;
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
                inst_add_op(cp, arg);
                inst_insert_before(inst, cp);
                inst->ops[arg_base + i] = v_ri;
            }

            if (is_icall) {
                // fp copy inserted last so fp remains live through the arg copies
                Value *fp = val_resolve(inst->ops[0]);
                if (fp) {
                    Value *v_r0 = new_value(f, VAL_INST, fp->vtype);
                    v_r0->phys_reg = 0;  // r0 for jlr
                    Inst  *fp_cp = new_inst(f, b, IK_COPY, v_r0);
                    fp_cp->line = inst->line;
                    inst_add_op(fp_cp, fp);
                    inst_insert_before(inst, fp_cp);
                    inst->ops[0] = v_r0;
                }
            }
        }
    }

    // ── Pass C: Lower IK_NEG / IK_NOT ──────────────────────────────────
    // IK_NEG: sub rd, 0, r1  — needs a zero register.
    // IK_NOT: eq  rd, r1, 0  — needs a zero register.
    // emit.c previously used pushr/popr to borrow a scratch register.
    // Lower to a two-operand form with an explicit IK_CONST(0) so IRC
    // allocates the zero register properly.
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->kind != IK_NEG && inst->kind != IK_NOT) continue;
            if (!inst->dst || inst->nops < 1) continue;

            Value *zero_v = new_value(f, VAL_INST, inst->dst->vtype);
            Inst  *zero_i = new_inst(f, b, IK_CONST, zero_v);
            zero_i->imm   = 0;
            zero_i->line  = inst->line;
            inst_insert_before(inst, zero_i);

            Value  *src      = inst->ops[0];
            Value **new_ops  = arena_alloc(2 * sizeof(Value *));

            if (inst->kind == IK_NEG) {
                // IK_NEG src → IK_FSUB(0, src)  or  IK_SUB(0, src)
                new_ops[0] = zero_v;  zero_v->use_count++;
                new_ops[1] = src;     // use_count unchanged (was already counted)
                inst->kind = (inst->dst->vtype == VT_F32) ? IK_FSUB : IK_SUB;
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
            // Restricted to mask_size == 1 (u8 truncation) only.  The u16 fold
            // (mask_size == 2) changes the IRC interference graph in a way that
            // causes IRC to drop the OOS phi-copy for the CRC loop variable in
            // crcu8 (the shifted partial value r3 = crc>>1 ends up in the phi
            // slot that B2 reads, instead of the full post-update crc in r5).
            // The u16 fold is mathematically correct but exposes a latent IRC
            // coalescing bug; defer until IRC phi-copy handling is hardened.
            if (src_val && src_val->kind == VAL_CONST && mask_size == 1) {
                int folded = src_val->iconst & mask_val;
                inst->kind = IK_CONST;
                inst->imm  = folded;
                inst->nops = 0;
                continue;
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
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead || inst->kind != IK_AND || inst->nops < 2) continue;
            int c2;
            if (!get_iconst(val_resolve(inst->ops[1]), &c2)) continue;

            // Look through copy_prop-surviving IK_COPY chains (e.g. u8→i16 coercion)
            Value *inner = val_resolve(inst->ops[0]);
            while (inner && inner->kind == VAL_INST && inner->def &&
                   inner->def->kind == IK_COPY && inner->def->nops >= 1)
                inner = val_resolve(inner->def->ops[0]);

            if (!inner || inner->kind != VAL_INST || !inner->def) continue;
            if (inner->def->kind != IK_AND || inner->def->nops < 2) continue;

            int c1;
            if (!get_iconst(val_resolve(inner->def->ops[1]), &c1)) continue;

            int combined = c1 & c2;
            // Repoint outer AND's first operand past the inner AND (and any copies)
            inst->ops[0] = val_resolve(inner->def->ops[0]);

            if (combined != c2) {
                // Need a new constant; replace ops[1] with IK_CONST(combined)
                Value *new_mask_v = new_value(f, VAL_INST, VT_I16);
                Inst  *new_mask_i = new_inst(f, b, IK_CONST, new_mask_v);
                new_mask_i->imm  = combined;
                new_mask_i->line = inst->line;
                inst_insert_before(inst, new_mask_i);
                inst->ops[1] = new_mask_v;
            }
            // If combined == c2, ops[1] is already the right constant; leave it alone.
        }
    }

}
