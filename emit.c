#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emit.h"

// ============================================================
// Helpers
// ============================================================

static const char *regname(int r) {
    static const char *names[] = {"r0","r1","r2","r3","r4","r5","r6","r7"};
    if (r >= 0 && r < 8) return names[r];
    return "??";
}

// Physical registers live at the end of the current block being emitted.
// Set by emit_function before emitting each block's instructions.
static unsigned g_block_live_out_regs = 0;
static const char *g_cur_func_name = "";

// Return a bitmask of physical registers that are read by instructions
// AFTER inst (within the same block).
static unsigned upcoming_reads(Inst *inst) {
    unsigned mask = 0;
    for (Inst *nx = inst->next; nx; nx = nx->next) {
        for (int i = 0; i < nx->nops; i++) {
            Value *v = val_resolve(nx->ops[i]);
            if (v && v->kind == VAL_INST && v->phys_reg >= 0)
                mask |= (1u << v->phys_reg);
        }
    }
    return mask;
}

// Pick a scratch register that is not in forbidden (combined already_used|blocked).
// Falls back to r0 if nothing is clean.
static int pick_scratch(unsigned already_used, unsigned forbidden) {
    for (int r = 0; r < 8; r++) {
        if (!((already_used | forbidden) & (1u << r)))
            return r;
    }
    return 0; // fallback
}

// Pick a scratch register safe for constant materialisation:
// avoids registers live at block exit AND upcoming within-block reads.
static int pick_safe_scratch(unsigned also_avoid, Inst *inst) {
    unsigned forbidden = g_block_live_out_regs | upcoming_reads(inst) | also_avoid;
    return pick_scratch(0, forbidden);
}

// Get the physical register for a value (must be allocated)
static int preg(Value *v) {
    v = val_resolve(v);
    if (!v) return 0;
    if (v->kind == VAL_CONST) return 0; // shouldn't happen — consts are materialized
    return v->phys_reg >= 0 ? v->phys_reg : 0;
}

// Emit an immediate into a register (uses immw + immwh for large values)
static void emit_imm(FILE *out, int rd, int val) {
    unsigned uval = (unsigned)val;
    if (uval <= 0xffff) {
        fprintf(out, "    immw %s, %u\n", regname(rd), uval);
    } else {
        fprintf(out, "    immw %s, %u\n",  regname(rd), uval & 0xffff);
        fprintf(out, "    immwh %s, %u\n", regname(rd), uval >> 16);
    }
}

// Materialize a VAL_CONST value into rd
static void emit_const_into(FILE *out, Value *v, int rd) {
    emit_imm(out, rd, v->iconst);
}

// Get effective register for a value operand.
// If it's a constant, materialize it into scratch reg r0 (caller must handle).
// Returns the register number holding the value.
// NOTE: for constants we always spill into rd (caller-provided temp).
static int get_val_reg(FILE *out, Value *v, int scratch_rd) {
    v = val_resolve(v);
    if (!v) { emit_imm(out, scratch_rd, 0); return scratch_rd; }
    if (v->kind == VAL_CONST) {
        emit_const_into(out, v, scratch_rd);
        return scratch_rd;
    }
    if (v->kind == VAL_UNDEF) {
        emit_imm(out, scratch_rd, 0);
        return scratch_rd;
    }
    return v->phys_reg >= 0 ? v->phys_reg : scratch_rd;
}

// F2 range check for bp-relative loads/stores
// lw/sw: offset is imm7 * 2, range -128..+126 (must be even)
// ll/sl: offset is imm7 * 4, range -256..+252 (must be mult of 4)
// lb/sb: offset is imm7, range -64..+63

static int f2_range_byte(int off) { return off >= -64 && off <= 63; }
static int f2_range_word(int off) { return (off % 2) == 0 && off >= -128 && off <= 126; }
static int f2_range_long(int off) { return (off % 4) == 0 && off >= -256 && off <= 252; }

static int f2_range(int off, int size) {
    if (size == 1) return f2_range_byte(off);
    if (size == 2) return f2_range_word(off);
    if (size == 4) return f2_range_long(off);
    return 0;
}

// CPU4 load mnemonic for size (bp-relative F2)
static const char *load_f2(int size, int is_signed) {
    if (size == 1) return is_signed ? "lbx" : "lb";
    if (size == 2) return is_signed ? "lwx" : "lw";
    return "ll";
}

// CPU4 store mnemonic for size (bp-relative F2)
static const char *store_f2(int size) {
    if (size == 1) return "sb";
    if (size == 2) return "sw";
    return "sl";
}

// CPU4 load mnemonic for size (register-relative F3b)
static const char *load_f3b(int size, int is_signed) {
    if (size == 1) return is_signed ? "llbx" : "llb";
    if (size == 2) return is_signed ? "llwx" : "llw";
    return "lll";
}

// CPU4 store mnemonic for size (register-relative F3b)
static const char *store_f3b(int size) {
    if (size == 1) return "slb";
    if (size == 2) return "slw";
    return "sll";
}

// Is a ValType signed?
static int vtype_signed(ValType vt) {
    return vt == VT_I8 || vt == VT_I16 || vt == VT_I32;
}

// Size in bytes for ValType
static int vtype_size(ValType vt) {
    switch (vt) {
    case VT_I8:  case VT_U8:  return 1;
    case VT_I16: case VT_U16: return 2;
    case VT_I32: case VT_U32: case VT_F32: return 4;
    case VT_PTR: return 2; // 16-bit address space
    default: return 2;
    }
}

// Signed/unsigned ALU op suffix
static const char *alu_mnemonic(InstKind k, int is_signed) {
    switch (k) {
    case IK_ADD:  return "add";
    case IK_SUB:  return "sub";
    case IK_MUL:  return "mul";
    case IK_DIV:  return is_signed ? "divs" : "div";
    case IK_UDIV: return "div";
    case IK_MOD:  return is_signed ? "mods" : "mod";
    case IK_UMOD: return "mod";
    case IK_SHL:  return "shl";
    case IK_SHR:  return is_signed ? "shrs" : "shr";
    case IK_USHR: return "shr";
    case IK_AND:  return "and";
    case IK_OR:   return "or";
    case IK_XOR:  return "xor";
    case IK_LT:   return is_signed ? "lts" : "lt";
    case IK_ULT:  return "lt";
    case IK_LE:   return is_signed ? "les" : "le";
    case IK_ULE:  return "le";
    case IK_EQ:   return "eq";
    case IK_NE:   return "ne";
    case IK_FADD: return "fadd";
    case IK_FSUB: return "fsub";
    case IK_FMUL: return "fmul";
    case IK_FDIV: return "fdiv";
    case IK_FLT:  return "flt";
    case IK_FLE:  return "fle";
    case IK_FEQ:  return "eq";   // bitwise equality
    case IK_FNE:  return "ne";
    default: return "???";
    }
}

// ============================================================
// Emit one instruction
// ============================================================

static void emit_inst(Inst *inst, FILE *out) {
    if (!inst || inst->is_dead) return;

    Value *dst  = inst->dst;
    int    rd   = dst ? preg(dst) : 0;

    switch (inst->kind) {
    case IK_CONST: {
        if (!dst) break;
        if (inst->fname) {
            // Symbolic address
            fprintf(out, "    immw %s, %s\n", regname(rd), inst->fname);
        } else {
            emit_imm(out, rd, inst->imm);
        }
        break;
    }

    case IK_COPY: {
        if (!dst || inst->nops < 1) break;
        int rs = get_val_reg(out, inst->ops[0], rd);
        if (rs != rd)
            fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(rs), regname(rs));
        break;
    }

    case IK_PARAM: {
        // Register-based ABI: param i pre-colored to ri in braun.c.
        // IRC honors pre-coloring, so rd == param_idx normally.
        // Emit nothing — value is already in rd at function entry.
        // (If IRC chose a different register despite pre-coloring, emit a move.)
        if (!dst) break;
        int pidx = inst->param_idx;
        if (pidx > 7) pidx = 7;
        if (rd != pidx)
            fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(pidx), regname(pidx));
        break;
    }

    case IK_PHI:
        // Phis should have been eliminated by out_of_ssa
        break;

    // ALU binary ops
    case IK_ADD: case IK_SUB: case IK_MUL:
    case IK_DIV: case IK_UDIV:
    case IK_MOD: case IK_UMOD:
    case IK_SHL: case IK_SHR: case IK_USHR:
    case IK_AND: case IK_OR:  case IK_XOR:
    case IK_LT:  case IK_ULT:
    case IK_LE:  case IK_ULE:
    case IK_EQ:  case IK_NE:
    case IK_FADD: case IK_FSUB: case IK_FMUL: case IK_FDIV:
    case IK_FLT:  case IK_FLE:  case IK_FEQ:  case IK_FNE: {
        if (!dst || inst->nops < 2) break;
        int is_signed = dst ? vtype_signed(dst->vtype) : 0;
        Value *ov0 = val_resolve(inst->ops[0]);
        Value *ov1 = val_resolve(inst->ops[1]);
        int ops0_reg = (ov0 && ov0->kind == VAL_INST) ? preg(ov0) : -1;
        int ops1_reg = (ov1 && ov1->kind == VAL_INST) ? preg(ov1) : -1;
        // Constant materialisation strategy:
        // Prefer using rd as scratch (safe because CPU4 reads sources before
        // writing the destination — CPU4 reads sources before writing dst, so
        // `immw rd, K; op rd, live_src, rd` is correct).
        // But if the non-const source is already in rd (ops_reg == rd), loading
        // the constant into rd would clobber it before it is read.
        // In that case we push a scratch register, materialize into it, do the
        // op, then pop the scratch (always correct; no liveness analysis needed).
        int r1, r2;
        int saved_scratch = -1;

        // Helper lambda (inlined): pick first reg not in avoid mask, push it.
        #define PUSH_SCRATCH(avoid_mask) do { \
            unsigned _av = (avoid_mask); \
            int _sc = -1; \
            for (int _r = 0; _r < 8; _r++) if (!(_av & (1u<<_r))) { _sc = _r; break; } \
            if (_sc < 0) _sc = (__builtin_ctz(~_av)) & 7; \
            fprintf(out, "    pushr %s\n", regname(_sc)); \
            saved_scratch = _sc; \
        } while (0)

        int both_const = (ov0 && ov0->kind == VAL_CONST) &&
                         (ov1 && ov1->kind == VAL_CONST);
        if (both_const) {
            // Use rd for const0, push a scratch for const1.
            PUSH_SCRATCH(1u << rd);
            r1 = get_val_reg(out, inst->ops[0], rd);
            r2 = get_val_reg(out, inst->ops[1], saved_scratch);
        } else if (ov0 && ov0->kind == VAL_CONST) {
            // ops[0] is constant; ops[1] is in ops1_reg.
            if (ops1_reg == rd) {
                // Can't use rd (= ops1_reg) as scratch — push another reg.
                PUSH_SCRATCH(1u << rd);
                r1 = get_val_reg(out, inst->ops[0], saved_scratch);
                r2 = ops1_reg;
            } else {
                // rd is a safe scratch (written last, after sources are read).
                r1 = get_val_reg(out, inst->ops[0], rd);
                r2 = ops1_reg >= 0 ? ops1_reg : rd;
            }
        } else if (ov1 && ov1->kind == VAL_CONST) {
            // ops[1] is constant; ops[0] is in ops0_reg.
            if (ops0_reg == rd) {
                // Can't use rd (= ops0_reg) as scratch — push another reg.
                PUSH_SCRATCH(1u << rd);
                r1 = ops0_reg;
                r2 = get_val_reg(out, inst->ops[1], saved_scratch);
            } else {
                // rd is a safe scratch.
                r1 = ops0_reg >= 0 ? ops0_reg : rd;
                r2 = get_val_reg(out, inst->ops[1], rd);
            }
        } else {
            r1 = ops0_reg >= 0 ? ops0_reg : rd;
            r2 = ops1_reg >= 0 ? ops1_reg : rd;
        }
        #undef PUSH_SCRATCH

        fprintf(out, "    %s %s, %s, %s\n",
                alu_mnemonic(inst->kind, is_signed),
                regname(rd), regname(r1), regname(r2));
        if (saved_scratch >= 0)
            fprintf(out, "    popr %s\n", regname(saved_scratch));
        break;
    }

    // Unary ops
    case IK_NEG: {
        if (!dst || inst->nops < 1) break;
        int r1 = get_val_reg(out, inst->ops[0], rd);
        // neg rd = sub rd, zero, r1. Need a zero register.
        // Pick a scratch not equal to rd or r1, save/restore via pushr/popr.
        unsigned avoid = (1u << rd) | (1u << r1);
        int tmp = -1;
        for (int r = 0; r < 8; r++) if (!(avoid & (1u << r))) { tmp = r; break; }
        if (tmp < 0) tmp = (rd + 1) & 7;
        fprintf(out, "    pushr %s\n", regname(tmp));
        emit_imm(out, tmp, 0);
        fprintf(out, "    sub %s, %s, %s\n", regname(rd), regname(tmp), regname(r1));
        fprintf(out, "    popr %s\n", regname(tmp));
        break;
    }
    case IK_NOT: {
        if (!dst || inst->nops < 1) break;
        int r1 = get_val_reg(out, inst->ops[0], rd);
        // logical NOT: rd = (r1 == 0) ? 1 : 0. Need a zero register.
        // Pick a scratch not equal to rd or r1, save/restore via pushr/popr.
        unsigned avoid = (1u << rd) | (1u << r1);
        int tmp = -1;
        for (int r = 0; r < 8; r++) if (!(avoid & (1u << r))) { tmp = r; break; }
        if (tmp < 0) tmp = (rd + 1) & 7;
        fprintf(out, "    pushr %s\n", regname(tmp));
        emit_imm(out, tmp, 0);
        fprintf(out, "    eq %s, %s, %s\n", regname(rd), regname(r1), regname(tmp));
        fprintf(out, "    popr %s\n", regname(tmp));
        break;
    }

    case IK_ITOF: {
        if (!dst || inst->nops < 1) break;
        int r1 = get_val_reg(out, inst->ops[0], rd);
        if (r1 != rd) fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(r1), regname(r1));
        fprintf(out, "    sxw %s\n", regname(rd));
        fprintf(out, "    itof %s\n", regname(rd));
        break;
    }
    case IK_FTOI: {
        if (!dst || inst->nops < 1) break;
        int r1 = get_val_reg(out, inst->ops[0], rd);
        if (r1 != rd) fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(r1), regname(r1));
        fprintf(out, "    ftoi %s\n", regname(rd));
        break;
    }
    case IK_SEXT8: {
        if (!dst || inst->nops < 1) break;
        int r1 = get_val_reg(out, inst->ops[0], rd);
        if (r1 != rd) fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(r1), regname(r1));
        fprintf(out, "    sxb %s\n", regname(rd));
        break;
    }
    case IK_SEXT16: {
        if (!dst || inst->nops < 1) break;
        int r1 = get_val_reg(out, inst->ops[0], rd);
        if (r1 != rd) fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(r1), regname(r1));
        fprintf(out, "    sxw %s\n", regname(rd));
        break;
    }
    case IK_ZEXT:
    case IK_TRUNC: {
        // Zero-extend or truncate: handled by masking or just copy
        if (!dst || inst->nops < 1) break;
        int r1 = get_val_reg(out, inst->ops[0], rd);
        int src_size = vtype_size(inst->ops[0] ? val_resolve(inst->ops[0])->vtype : VT_I16);
        int dst_size = vtype_size(dst->vtype);
        if (r1 != rd) fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(r1), regname(r1));
        // IK_ZEXT must always mask to src_size bits: upper bits may be dirty after
        // sign-extending loads (lwx/lbx), and we cannot assume they are zero.
        // IK_TRUNC masks to dst_size bits when shrinking.
        int mask_size = (inst->kind == IK_ZEXT) ? src_size : dst_size;
        if (mask_size < 4) {
            unsigned avoid = 1u << rd;
            int tmp = -1;
            for (int r = 0; r < 8; r++) if (!(avoid & (1u << r))) { tmp = r; break; }
            if (tmp < 0) tmp = (rd + 1) & 7;
            unsigned mask = (mask_size == 1) ? 0xff : 0xffff;
            fprintf(out, "    pushr %s\n", regname(tmp));
            emit_imm(out, tmp, (int)mask);
            fprintf(out, "    and %s, %s, %s\n", regname(rd), regname(rd), regname(tmp));
            fprintf(out, "    popr %s\n", regname(tmp));
        }
        break;
    }

    case IK_LOAD: {
        if (!dst || inst->nops < 1) break;
        Value *base = val_resolve(inst->ops[0]);
        int size    = inst->size ? inst->size : vtype_size(dst->vtype);
        int is_s    = vtype_signed(dst->vtype);
        int off     = inst->imm;

        // F3b imm10 and F2 imm7 are both scaled by access size
        int scaled = (size >= 4) ? off / 4 : (size == 2) ? off / 2 : off;
        if (!base || base->kind == VAL_UNDEF) {
            // spill load (base is implicit bp frame slot)
            // imm is the bp-relative byte offset
            if (f2_range(off, size)) {
                // F2: mnem rx, scaled_imm7
                fprintf(out, "    %s %s, %d\n",
                        is_s ? (size==1 ? "lbx" : (size==2 ? "lwx" : "ll")) :
                               load_f2(size, 0),
                        regname(rd), scaled);
            } else {
                // Out of F2 range: lea rd, off; load rd, [rd+0]
                // Using rd as both the address temp and the destination avoids
                // clobbering any other live register (e.g. a phi-copy just
                // computed above this instruction in the same block).
                fprintf(out, "    lea %s, %d\n", regname(rd), off);
                fprintf(out, "    %s %s, %s, 0\n",
                        load_f3b(size, is_s), regname(rd), regname(rd));
            }
        } else {
            int rb = preg(base);
            if (base->kind == VAL_CONST && base->iconst == 0) {
                // null base, use imm as absolute (unusual)
                int tmp = (rd == 0) ? 1 : 0;
                emit_imm(out, tmp, off);
                fprintf(out, "    %s %s, %s, 0\n", load_f3b(size, is_s), regname(rd), regname(tmp));
            } else {
                // F3b: mnem rx, ry, scaled_imm10
                fprintf(out, "    %s %s, %s, %d\n",
                        load_f3b(size, is_s), regname(rd), regname(rb), scaled);
            }
        }
        break;
    }

    case IK_STORE: {
        // ops[0] = address (or NULL for spill), ops[1] = value to store
        if (inst->nops < 1) break;
        Value *base = val_resolve(inst->nops >= 2 ? inst->ops[0] : NULL);
        Value *val  = val_resolve(inst->nops >= 2 ? inst->ops[1] : inst->ops[0]);
        int size    = inst->size ? inst->size : (val ? vtype_size(val->vtype) : 2);
        int off     = inst->imm;

        // Pick scratch registers for base and val, avoiding conflicts
        int rb = base ? (base->kind == VAL_CONST ? -1 : preg(base)) : -1;
        // Materialize constants
        int rv_scratch = 0; // default scratch for value
        if (rb == 0) rv_scratch = 1;
        int rv = get_val_reg(out, val, rv_scratch);

        int rb_scratch = (rv == 0) ? 1 : 0;
        if (base && base->kind == VAL_CONST) {
            emit_const_into(out, base, rb_scratch);
            rb = rb_scratch;
        }

        int scaled = (size >= 4) ? off / 4 : (size == 2) ? off / 2 : off;
        if (!base || (inst->nops < 2)) {
            // spill store: base is implicit bp
            if (f2_range(off, size)) {
                // F2: mnem rv, scaled_imm7
                fprintf(out, "    %s %s, %d\n", store_f2(size), regname(rv), scaled);
            } else {
                // Out of F2 range: lea + F3b store
                int tmp = (rv == 0) ? 1 : 0;
                if (tmp == rv) tmp = 2;
                fprintf(out, "    lea %s, %d\n", regname(tmp), off);
                fprintf(out, "    %s %s, %s, 0\n", store_f3b(size), regname(rv), regname(tmp));
            }
        } else {
            // F3b: mnem rv, ry, scaled_imm10
            fprintf(out, "    %s %s, %s, %d\n",
                    store_f3b(size), regname(rv), regname(rb), scaled);
        }
        break;
    }

    case IK_ADDR: {
        // dst = &frame_slot[imm]  (bp-relative address)
        if (!dst) break;
        fprintf(out, "    lea %s, %d\n", regname(rd), inst->imm);
        break;
    }

    case IK_GADDR: {
        // dst = &global "fname"
        if (!dst || !inst->fname) break;
        fprintf(out, "    immw %s, %s\n", regname(rd), inst->fname);
        break;
    }

    case IK_MEMCPY: {
        // memcpy(ops[0], ops[1], imm bytes)
        // Emit as a simple byte loop via inline expansion (small copies) or libcall
        // For now: call a memcpy helper (not available yet) or expand inline
        // Simple inline: assume imm is small and compile to word/byte moves
        if (inst->nops < 2) break;
        int rdst = get_val_reg(out, inst->ops[0], 0);
        int rsrc = get_val_reg(out, inst->ops[1], 1);
        int bytes = inst->imm;
        int tmp   = 2;
        for (int off2 = 0; off2 < bytes; off2 += 2) {
            int rem = bytes - off2;
            if (rem >= 2) {
                fprintf(out, "    llw %s, %s, %d\n", regname(tmp), regname(rsrc), off2/2);
                fprintf(out, "    slw %s, %s, %d\n", regname(tmp), regname(rdst), off2/2);
            } else {
                fprintf(out, "    llb %s, %s, %d\n", regname(tmp), regname(rsrc), off2);
                fprintf(out, "    slb %s, %s, %d\n", regname(tmp), regname(rdst), off2);
            }
        }
        break;
    }

    case IK_CALL: {
        if (!inst->fname) break;
        int is_var = inst->calldesc && inst->calldesc->is_variadic;
        if (is_var) {
            // Variadic call: use stack ABI — push all args right-to-left.
            // CPU4 push is 32-bit (4 bytes per slot); callee reads at bp+4, bp+8, ...
            // Compute mask of all registers holding non-const args; consts get a
            // scratch register that doesn't alias any pending arg register.
            unsigned arg_regs = 0;
            for (int ai = 0; ai < inst->nops; ai++) {
                Value *av = val_resolve(inst->ops[ai]);
                if (av && av->kind == VAL_INST && av->phys_reg >= 0)
                    arg_regs |= (1u << av->phys_reg);
            }
            for (int ai = inst->nops - 1; ai >= 0; ai--) {
                Value *av = val_resolve(inst->ops[ai]);
                if (av && av->kind == VAL_CONST) {
                    // Use a scratch not in arg_regs to avoid clobbering live args.
                    int sc = pick_scratch(0, arg_regs);
                    if (sc < 0) sc = 0;  // fallback (should not happen for ≤8 args)
                    emit_const_into(out, av, sc);
                    fprintf(out, "    pushr %s\n", regname(sc));  // 4-byte push
                } else {
                    int ar = av ? preg(av) : -1;
                    if (ar >= 0) fprintf(out, "    pushr %s\n", regname(ar));  // 4-byte push
                }
            }
            fprintf(out, "    jl %s\n", inst->fname);
            if (inst->nops > 0) fprintf(out, "    adjw %d\n", inst->nops * 4);
        } else {
            // Non-variadic: register ABI — args 0-2 in r1-r3 (caller-saved); args 3+ on stack.
            // Using only caller-saved registers avoids clobbering the caller's callee-saved
            // values (r4-r7) which the caller may hold as long-lived variables.
            int nreg  = inst->nops < 3 ? inst->nops : 3;
            int nextra = inst->nops > 3 ? inst->nops - 3 : 0;
            // Push extra args in reverse order
            for (int ai = inst->nops - 1; ai >= 3; ai--) {
                Value *av = val_resolve(inst->ops[ai]);
                int ar = av ? (av->kind == VAL_CONST ? -1 : preg(av)) : -1;
                if (av && av->kind == VAL_CONST) {
                    // Use r0 as scratch for constant stack args (pushr r0)
                    int sc = pick_scratch(0, 0);  // r0 is always scratch here (not a call arg)
                    emit_const_into(out, av, sc);
                    fprintf(out, "    pushr %s\n", regname(sc));
                } else if (ar >= 0) {
                    fprintf(out, "    pushr %s\n", regname(ar));
                }
            }
            // Set up register args in r1..r(nreg): parallel-copy to handle swap cycles.
            // Collect reg-to-reg moves (constants handled separately at end).
            int mv_src[8], mv_dst[8], mv_done[8], nmv = 0;
            int const_target[8], const_val[8], nconst = 0;
            for (int ai = 0; ai < nreg; ai++) {
                Value *av = val_resolve(inst->ops[ai]);
                int target = ai + 1;
                if (!av) continue;
                if (av->kind == VAL_CONST) {
                    const_target[nconst] = target;
                    const_val[nconst]    = av->iconst;
                    nconst++;
                } else {
                    int s = preg(av);
                    mv_src[nmv] = s; mv_dst[nmv] = target;
                    mv_done[nmv] = (s == target) ? 1 : 0;
                    nmv++;
                }
            }
            // Phase 1: emit moves where dst is not a pending source (cycle-free).
            int progress = 1;
            while (progress) {
                progress = 0;
                for (int i = 0; i < nmv; i++) {
                    if (mv_done[i]) continue;
                    int blocked = 0;
                    for (int j = 0; j < nmv; j++) {
                        if (!mv_done[j] && j != i && mv_src[j] == mv_dst[i]) { blocked=1; break; }
                    }
                    if (!blocked) {
                        fprintf(out, "    or %s, %s, %s\n", regname(mv_dst[i]), regname(mv_src[i]), regname(mv_src[i]));
                        mv_done[i] = 1; progress = 1;
                    }
                }
            }
            // Phase 2: break remaining cycles using XOR swap (no scratch register needed).
            for (int i = 0; i < nmv; i++) {
                if (mv_done[i]) continue;
                int buf = mv_src[i];
                mv_done[i] = 1;
                int cur_target = mv_dst[i];
                while (cur_target != buf) {
                    // XOR swap: buf gets cur_target's old value; cur_target gets old buf value.
                    fprintf(out, "    xor %s, %s, %s\n", regname(buf), regname(buf), regname(cur_target));
                    fprintf(out, "    xor %s, %s, %s\n", regname(cur_target), regname(cur_target), regname(buf));
                    fprintf(out, "    xor %s, %s, %s\n", regname(buf), regname(buf), regname(cur_target));
                    int found = -1;
                    for (int j = 0; j < nmv; j++) {
                        if (!mv_done[j] && mv_src[j] == cur_target) { found = j; break; }
                    }
                    if (found < 0) break;
                    mv_done[found] = 1;
                    cur_target = mv_dst[found];
                }
                // When cur_target == buf: buf already holds the correct value for its slot.
            }
            // Phase 3: load constants into their targets (after all reg moves).
            for (int ci = 0; ci < nconst; ci++) {
                emit_imm(out, const_target[ci], const_val[ci]);
            }
            fprintf(out, "    jl %s\n", inst->fname);
            if (nextra > 0) fprintf(out, "    adjw %d\n", nextra * 4);
        }
        // Result in r0; if dst phys_reg != 0, copy
        if (dst && preg(dst) != 0)
            fprintf(out, "    or %s, r0, r0\n", regname(preg(dst)));
        break;
    }

    case IK_ICALL: {
        // indirect call: ops[0] = function pointer; ops[1..] = args
        // ABI: args in r1..r3 (caller-saved); fp in r0 (for jlr); args 3+ on stack.
        if (inst->nops < 1) break;
        int nreg  = (inst->nops - 1) < 3 ? (inst->nops - 1) : 3;
        int nextra = (inst->nops - 1) > 3 ? (inst->nops - 1) - 3 : 0;
        // Save fp first — it may be in any register that arg loading would overwrite
        int fp_reg = get_val_reg(out, inst->ops[0], 7);
        // Args go in r1..r(nreg); fp cannot be in r0..r(nreg) safely
        if (fp_reg <= nreg) {
            unsigned arg_mask = nreg < 8 ? (((1u << nreg) - 1) << 1) | 1u : 0xffu;
            int safe = pick_safe_scratch(arg_mask, inst);
            fprintf(out, "    or %s, %s, %s\n", regname(safe), regname(fp_reg), regname(fp_reg));
            fp_reg = safe;
        }
        // Push extra args in reverse order
        for (int ai = inst->nops - 2; ai >= 3; ai--) {
            Value *av = val_resolve(inst->ops[ai + 1]);
            int ar = av ? (av->kind == VAL_CONST ? -1 : preg(av)) : -1;
            if (av && av->kind == VAL_CONST) {
                emit_imm(out, 0, av->iconst);
                fprintf(out, "    push\n");
            } else if (ar >= 0) {
                fprintf(out, "    pushr %s\n", regname(ar));
            }
        }
        // Move args into r1..r(nreg): parallel-copy with cycle handling.
        {
            int mv_src[8], mv_dst[8], mv_done[8], nmv = 0;
            int const_target[8], const_val[8], nconst = 0;
            for (int ai = 0; ai < nreg; ai++) {
                Value *av = val_resolve(inst->ops[ai + 1]);
                int target = ai + 1;
                if (!av) continue;
                if (av->kind == VAL_CONST) {
                    const_target[nconst] = target; const_val[nconst] = av->iconst; nconst++;
                } else {
                    int s = preg(av);
                    mv_src[nmv] = s; mv_dst[nmv] = target;
                    mv_done[nmv] = (s == target) ? 1 : 0; nmv++;
                }
            }
            int progress = 1;
            while (progress) {
                progress = 0;
                for (int i = 0; i < nmv; i++) {
                    if (mv_done[i]) continue;
                    int blocked = 0;
                    for (int j = 0; j < nmv; j++) {
                        if (!mv_done[j] && j != i && mv_src[j] == mv_dst[i]) { blocked=1; break; }
                    }
                    if (!blocked) {
                        fprintf(out, "    or %s, %s, %s\n", regname(mv_dst[i]), regname(mv_src[i]), regname(mv_src[i]));
                        mv_done[i] = 1; progress = 1;
                    }
                }
            }
            for (int i = 0; i < nmv; i++) {
                if (mv_done[i]) continue;
                int buf = mv_src[i];
                mv_done[i] = 1;
                int cur_target = mv_dst[i];
                while (cur_target != buf) {
                    fprintf(out, "    xor %s, %s, %s\n", regname(buf), regname(buf), regname(cur_target));
                    fprintf(out, "    xor %s, %s, %s\n", regname(cur_target), regname(cur_target), regname(buf));
                    fprintf(out, "    xor %s, %s, %s\n", regname(buf), regname(buf), regname(cur_target));
                    int found = -1;
                    for (int j = 0; j < nmv; j++) {
                        if (!mv_done[j] && mv_src[j] == cur_target) { found = j; break; }
                    }
                    if (found < 0) break;
                    mv_done[found] = 1;
                    cur_target = mv_dst[found];
                }
            }
            for (int ci = 0; ci < nconst; ci++) emit_imm(out, const_target[ci], const_val[ci]);
        }
        // jlr rd uses any register directly
        fprintf(out, "    jlr %s\n", regname(fp_reg));
        if (nextra > 0) fprintf(out, "    adjw %d\n", nextra * 4);
        if (dst && preg(dst) != 0)
            fprintf(out, "    or %s, r0, r0\n", regname(preg(dst)));
        break;
    }

    case IK_PUTCHAR: {
        if (inst->nops < 1) break;
        int r1 = get_val_reg(out, inst->ops[0], 0);
        fprintf(out, "    putchar %s\n", regname(r1));
        break;
    }

    case IK_BR: {
        // if ops[0] goto target else target2
        if (inst->nops < 1) break;
        int r1 = get_val_reg(out, inst->ops[0], 0);
        if (inst->target)
            fprintf(out, "    jnz %s, _%s_B%d\n", regname(r1), g_cur_func_name, inst->target->id);
        if (inst->target2)
            fprintf(out, "    j _%s_B%d\n", g_cur_func_name, inst->target2->id);
        break;
    }

    case IK_JMP: {
        if (inst->target)
            fprintf(out, "    j _%s_B%d\n", g_cur_func_name, inst->target->id);
        else if (inst->label)
            fprintf(out, "    j %s\n", inst->label);
        break;
    }

    case IK_RET: {
        // Return value in r0
        if (inst->nops > 0) {
            int rv = get_val_reg(out, inst->ops[0], 0);
            if (rv != 0) fprintf(out, "    or r0, %s, %s\n", regname(rv), regname(rv));
        }
        fprintf(out, "    ret\n");
        break;
    }

    default:
        break;
    }
}

// ============================================================
// emit_function
// ============================================================

void emit_function(Function *f, FILE *out) {
    if (!f || f->nblocks == 0) return;

    // DEBUG: dump IR to stderr for variadic functions
    if (getenv("SMALLCC_DEBUG_IR")) {
        fprintf(stderr, "=== IR dump for %s ===\n", f->name);
        extern void print_function(Function *, FILE *);
        print_function(f, stderr);
    }

    g_cur_func_name = f->name;
    fprintf(out, "    align\n");
    fprintf(out, "%s:\n", f->name);

    // Frame: use frame_size rounded up to 4-byte alignment
    int frame = f->frame_size;
    if (frame % 4) frame += (4 - frame % 4);

    // Callee-saved regs: find which r4-r7 are used by non-param values.
    // Param values in r4-r7 are incoming arguments (not the callee's own work
    // registers), so they don't need callee-save save/restore.
    int callee_save[4] = {0, 0, 0, 0};
    for (int i = 0; i < f->nvalues; i++) {
        Value *v = f->values[i];
        if (v->phys_reg < 4 || v->phys_reg > 7) continue;
        int is_param = 0;
        for (int pi = 0; pi < f->nparams; pi++) {
            if (f->params[pi] == v) { is_param = 1; break; }
        }
        if (!is_param)
            callee_save[v->phys_reg - 4] = 1;
    }
    // Count callee-save slots needed; include in frame so enter allocates space.
    int callee_frame = 0;
    for (int r = 4; r <= 7; r++)
        if (callee_save[r - 4]) callee_frame += 4;

    fprintf(out, "    enter %d\n", frame + callee_frame);

    // Emit callee-save stores right after enter.
    int callee_tmp = 0;
    for (int r = 4; r <= 7; r++) {
        if (callee_save[r - 4]) {
            callee_tmp += 4;
            // F2 sl: sl rx, scaled_imm7  (byte_off / 4)
            fprintf(out, "    sl r%d, %d\n", r, -(frame + callee_tmp) / 4);
        }
    }

    // Emit blocks
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        // Compute live-out register mask for this block (used by pick_safe_scratch).
        g_block_live_out_regs = 0;
        if (b->live_out) {
            for (int vi = 0; vi < f->nvalues; vi++) {
                Value *v = f->values[vi];
                if (v->phys_reg >= 0 &&
                    (b->live_out[vi / 32] >> (vi % 32)) & 1)
                    g_block_live_out_regs |= (1u << v->phys_reg);
            }
        }

        // Emit block label (skip for entry block — the function label serves)
        if (bi > 0)
            fprintf(out, "_%s_B%d:\n", f->name, b->id);
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->kind == IK_RET && callee_frame > 0) {
                // Full RET sequence when callee saves exist:
                // 1. Move return value to r0 (before restoring callee regs)
                if (inst->nops > 0) {
                    Value *rv = val_resolve(inst->ops[0]);
                    if (rv && rv->kind == VAL_CONST) {
                        emit_imm(out, 0, rv->iconst);
                    } else if (rv && rv->phys_reg >= 0 && rv->phys_reg != 0) {
                        fprintf(out, "    or r0, %s, %s\n",
                                regname(rv->phys_reg), regname(rv->phys_reg));
                    }
                }
                // 2. Restore callee-saved regs (in order, same offsets as save)
                int tmp_frame = 0;
                for (int r = 4; r <= 7; r++) {
                    if (callee_save[r - 4]) {
                        tmp_frame += 4;
                        fprintf(out, "    ll r%d, %d\n", r, -(frame + tmp_frame) / 4);
                    }
                }
                // 3. ret
                fprintf(out, "    ret\n");
            } else {
                emit_inst(inst, out);
            }
        }
    }
}

// ============================================================
// emit_globals — emit data section from Sexp program
// ============================================================

static void emit_sx_data(Sx *sx, FILE *out);

static const char *sx_tag(Sx *sx) {
    if (!sx || sx->kind != SX_PAIR) return NULL;
    Sx *car = sx->car;
    if (!car || car->kind != SX_SYM) return NULL;
    return car->s;
}

static Sx *emit_sx_nth(Sx *sx, int n) {
    // nth element of a list (0-indexed)
    Sx *cur = sx;
    for (int i = 0; i < n; i++) {
        if (!cur || cur->kind != SX_PAIR) return NULL;
        cur = cur->cdr;
    }
    if (!cur || cur->kind != SX_PAIR) return NULL;
    return cur->car;
}

void emit_globals(Sx *program, FILE *out) {
    if (!program) return;
    // program is (program item...)
    // Each item is either (func ...) or (gvar name type value) or (strlit id str)
    Sx *list = program->kind == SX_PAIR ? program->cdr : NULL;
    while (list && list->kind == SX_PAIR) {
        Sx *item = list->car;
        const char *tag = sx_tag(item);
        if (tag) {
            if (strcmp(tag, "gvar") == 0) emit_sx_data(item, out);
            if (strcmp(tag, "strlit") == 0) emit_sx_data(item, out);
        }
        list = list->cdr;
    }
}

static void emit_sx_data(Sx *sx, FILE *out) {
    const char *tag = sx_tag(sx);
    if (!tag) return;

    if (strcmp(tag, "strlit") == 0) {
        // (strlit "label" byte0 byte1 ... byteN)
        Sx *name_sx = emit_sx_nth(sx, 1);
        if (!name_sx) return;
        const char *label = (name_sx->kind == SX_STR || name_sx->kind == SX_SYM)
                            ? name_sx->s : NULL;
        if (!label) return;
        fprintf(out, "%s:\n", label);
        // Bytes start at element 2
        Sx *cur = sx->cdr; // skip "strlit"
        if (cur) cur = cur->cdr; // skip label name
        while (cur && cur->kind == SX_PAIR) {
            Sx *b = cur->car;
            if (b && b->kind == SX_INT)
                fprintf(out, "    byte %d\n", b->i & 0xff);
            cur = cur->cdr;
        }
        return;
    }

    if (strcmp(tag, "gvar") == 0) {
        // Format: (gvar name size [init_value])
        // init_value may be: SX_INT (scalar), (strref "label") (pointer to string), absent (zero)
        Sx *name_sx = emit_sx_nth(sx, 1);
        Sx *size_sx = emit_sx_nth(sx, 2);
        Sx *init_sx = emit_sx_nth(sx, 3);  // optional
        if (!name_sx) return;
        const char *name = (name_sx->kind == SX_SYM || name_sx->kind == SX_STR) ? name_sx->s : "?";
        int size = size_sx && size_sx->kind == SX_INT ? size_sx->i : 2;

        // Emit alignment directive for multi-byte globals
        if (size >= 2) fprintf(out, "    align\n");
        fprintf(out, "%s:\n", name);

        // Check for strref init: (strref "label") → emit word label
        int handled = 0;
        if (init_sx && init_sx->kind == SX_PAIR) {
            Sx *tag2 = init_sx->car;
            if (tag2 && tag2->kind == SX_SYM && strcmp(tag2->s, "strref") == 0) {
                Sx *lbl = init_sx->cdr ? init_sx->cdr->car : NULL;
                if (lbl && (lbl->kind == SX_STR || lbl->kind == SX_SYM))
                    fprintf(out, "    word %s\n", lbl->s);
                else
                    fprintf(out, "    allocb %d\n", size);
                handled = 1;
            } else if (tag2 && tag2->kind == SX_SYM && strcmp(tag2->s, "strbytes") == 0) {
                // Inline bytes for char[] = "string"
                Sx *cur2 = init_sx->cdr;
                while (cur2 && cur2->kind == SX_PAIR) {
                    Sx *b = cur2->car;
                    if (b && b->kind == SX_INT)
                        fprintf(out, "    byte %d\n", b->i & 0xff);
                    cur2 = cur2->cdr;
                }
                handled = 1;
            }
        }
        // Array initializer list: (ginit elem_size v0 v1 ...)
        if (!handled && init_sx && init_sx->kind == SX_PAIR) {
            Sx *tag2 = init_sx->car;
            if (tag2 && tag2->kind == SX_SYM && strcmp(tag2->s, "ginit") == 0) {
                Sx *es_sx = init_sx->cdr ? init_sx->cdr->car : NULL;
                int esize = (es_sx && es_sx->kind == SX_INT) ? es_sx->i : 2;
                Sx *vals = init_sx->cdr ? init_sx->cdr->cdr : NULL;
                int emitted = 0;
                while (vals && vals->kind == SX_PAIR) {
                    Sx *v = vals->car;
                    if (v && v->kind == SX_INT) {
                        if (esize == 1)      fprintf(out, "    byte %d\n", v->i & 0xff);
                        else if (esize == 2) fprintf(out, "    word %d\n", v->i & 0xffff);
                        else if (esize == 4) fprintf(out, "    long %d\n", v->i);
                        emitted += esize;
                    } else if (v && v->kind == SX_PAIR) {
                        // strref element: (strref "label") → word label
                        Sx *t2 = v->car;
                        if (t2 && t2->kind == SX_SYM && strcmp(t2->s, "strref") == 0) {
                            Sx *lbl = v->cdr ? v->cdr->car : NULL;
                            if (lbl && (lbl->kind == SX_STR || lbl->kind == SX_SYM))
                                fprintf(out, "    word %s\n", lbl->s);
                            emitted += 2;
                        }
                    }
                    vals = vals->cdr;
                }
                int remaining = size - emitted;
                if (remaining > 0) fprintf(out, "    allocb %d\n", remaining);
                handled = 1;
            }
        }
        if (!handled) {
            if (!init_sx || (init_sx->kind == SX_INT && init_sx->i == 0)) {
                fprintf(out, "    allocb %d\n", size);
            } else if (init_sx->kind == SX_INT) {
                int val = init_sx->i;
                if (size == 1)      fprintf(out, "    byte %d\n", val & 0xff);
                else if (size == 2) fprintf(out, "    word %d\n", val & 0xffff);
                else if (size == 4) fprintf(out, "    long %d\n", val);
                else                fprintf(out, "    allocb %d\n", size);
            } else {
                fprintf(out, "    allocb %d\n", size);
            }
        }
        return;
    }
}
