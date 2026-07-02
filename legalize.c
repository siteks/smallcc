#include <string.h>
#include "legalize.h"
#include "opt.h"     // opt_flags, OPT_LEG_*
#include "dom.h"     // dominates, compute_dominators
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
                Value *inner = val_resolve(inst->ops[0]);
                while (inner && inner->kind == VAL_INST && inner->def &&
                       inner->def->kind == IK_COPY && inner->def->nops >= 1)
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
                // Repoint outer AND's first operand past the inner AND (and any copies)
                inst->ops[0] = val_resolve(inner->def->ops[1 - inner_const_pos]);

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

    // ── Pass F: Materialize large VAL_CONST binary ALU operands ────────────
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
                    // F0b immediate handles |k| ≤ 255 (P18 mulli, P19 general);
                    // P11/P13 handle SHL/SHR/USHR shifts via resolve_const.
                    // Only materialize constants outside those ranges.
                    for (int j = 0; j < inst->nops; j++) {
                        Value *v = inst->ops[j] ? val_resolve(inst->ops[j]) : NULL;
                        if (!v || v->kind != VAL_CONST) continue;
                        int k = v->iconst;
                        if (k >= -256 && k <= 255) continue;  // F0b immediate range
                        Value *cv = new_value(f, VAL_INST, inst->dst->vtype);
                        Inst  *ci = new_inst(f, b, IK_CONST, cv);
                        ci->imm  = v->iconst;
                        ci->line = inst->line;
                        inst_insert_before(inst, ci);
                        inst->ops[j] = cv;
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
                    }
                    break;
                default:
                    break;
                }
            }
        }
    }
}
