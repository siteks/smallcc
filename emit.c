#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "smallcc.h"
#include "emit.h"

// ============================================================
// Source annotation (-ann)
// ============================================================

int         flag_annotate = 0;

static char       *ann_src    = NULL;
static const char **ann_lines = NULL;
static int          ann_nlines = 0;

void set_ann_source(const char *src)
{
    ann_src    = NULL;
    ann_lines  = NULL;
    ann_nlines = 0;
    if (!src) return;

    ann_src = arena_strdup(src);

    // Count physical lines for allocation bound
    int count = 1;
    for (const char *p = ann_src; *p; p++)
        if (*p == '\n') count++;

    ann_lines  = arena_alloc((count + 2) * sizeof(char *));
    ann_nlines = count + 1;

    // Walk preprocessed source tracking logical line numbers via linemarkers.
    int cur_line = 1;
    char *p = ann_src;
    while (*p) {
        char *line_start = p;
        if (*p == '#') {
            const char *q = p + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (isdigit((unsigned char)*q)) {
                char *end;
                cur_line = (int)strtol(q, &end, 10);
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
                continue;
            }
        }
        if (cur_line >= 1 && cur_line <= ann_nlines)
            ann_lines[cur_line - 1] = line_start;
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        cur_line++;
    }
}

// Emit the text of source line N (1-based) as an assembly comment.
static void emit_src_comment(int line, FILE *out)
{
    if (!ann_lines || line < 1 || line > ann_nlines) return;
    const char *s = ann_lines[line - 1];
    if (!s) return;
    const char *e = s;
    while (*e && *e != '\n') e++;
    while (s < e && (*s == ' ' || *s == '\t')) s++;
    if (s < e)
        fprintf(out, ";; %.*s\n", (int)(e - s), s);
}

// ============================================================
// Helpers
// ============================================================

static const char *regname(int r) {
    static const char *names[] = {"r0","r1","r2","r3","r4","r5","r6","r7"};
    if (r >= 0 && r < 8) return names[r];
    return "??";
}

static const char *g_cur_func_name = "";

// Pick a scratch register that is not in forbidden (combined already_used|blocked).
// Falls back to r0 if nothing is clean.
static int pick_scratch(unsigned already_used, unsigned forbidden) {
    for (int r = 0; r < 8; r++) {
        if (!((already_used | forbidden) & (1u << r)))
            return r;
    }
    return 0; // fallback
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
        int is_float_neg = (dst->vtype == VT_F32);
        // Pick a scratch not equal to rd or r1, save/restore via pushr/popr.
        unsigned avoid = (1u << rd) | (1u << r1);
        int tmp = -1;
        for (int r = 0; r < 8; r++) if (!(avoid & (1u << r))) { tmp = r; break; }
        if (tmp < 0) tmp = (rd + 1) & 7;
        fprintf(out, "    pushr %s\n", regname(tmp));
        if (is_float_neg) {
            // Float negation: load 0.0f into tmp, then fsub rd, tmp, r1
            emit_imm(out, tmp, 0);  // 0 as raw bits = 0.0f
            fprintf(out, "    fsub %s, %s, %s\n", regname(rd), regname(tmp), regname(r1));
        } else {
            emit_imm(out, tmp, 0);
            fprintf(out, "    sub %s, %s, %s\n", regname(rd), regname(tmp), regname(r1));
        }
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
        // Sign-extend only for sub-32-bit integer operands; 32-bit values need no sign-extension.
        {
            ValType ovt = inst->ops[0] ? inst->ops[0]->vtype : VT_I16;
            if (ovt == VT_I8)  fprintf(out, "    sxb %s\n", regname(rd));
            else if (ovt != VT_I32 && ovt != VT_U32 && ovt != VT_F32)
                fprintf(out, "    sxw %s\n", regname(rd));
        }
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
                // The new CPU4 'lea' only represents multiples of 4; correct
                // non-aligned offsets with addi.
                int adj_l  = off % 4;
                int base_l = off - adj_l;
                fprintf(out, "    lea %s, %d\n", regname(rd), base_l);
                if (adj_l) fprintf(out, "    addi %s, %d\n", regname(rd), adj_l);
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
                // The new CPU4 'lea' only represents multiples of 4; correct
                // non-aligned offsets with addi.
                int tmp = (rv == 0) ? 1 : 0;
                if (tmp == rv) tmp = 2;
                int adj_s  = off % 4;
                int base_s = off - adj_s;
                fprintf(out, "    lea %s, %d\n", regname(tmp), base_s);
                if (adj_s) fprintf(out, "    addi %s, %d\n", regname(tmp), adj_s);
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
        // The new CPU4 'lea' encodes the offset as imm14*4, so only 4-byte-aligned
        // offsets are represented exactly. For 2-byte-aligned locals, use lea to the
        // nearest 4-byte-aligned address and correct with addi (imm7, range -64..63).
        if (!dst) break;
        int off = inst->imm;
        int adj = off % 4;   // in C: result has sign of dividend; 0, ±1, ±2, ±3
        if (adj == 0) {
            fprintf(out, "    lea %s, %d\n", regname(rd), off);
        } else {
            int base = off - adj;   // 4-byte-aligned
            fprintf(out, "    lea %s, %d\n", regname(rd), base);
            fprintf(out, "    addi %s, %d\n", regname(rd), adj);
        }
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
            // Variadic: push all args right-to-left (stack ABI).
            // Compute mask of all registers holding non-const args.
            unsigned arg_regs = 0;
            for (int ai = 0; ai < inst->nops; ai++) {
                Value *av = val_resolve(inst->ops[ai]);
                if (av && av->kind == VAL_INST && av->phys_reg >= 0)
                    arg_regs |= (1u << av->phys_reg);
            }
            for (int ai = inst->nops - 1; ai >= 0; ai--) {
                Value *av = val_resolve(inst->ops[ai]);
                if (av && av->kind == VAL_CONST) {
                    int sc = pick_scratch(0, arg_regs);
                    emit_const_into(out, av, sc);
                    fprintf(out, "    pushr %s\n", regname(sc));
                } else {
                    int ar = av ? preg(av) : -1;
                    if (ar >= 0) fprintf(out, "    pushr %s\n", regname(ar));
                }
            }
            fprintf(out, "    jl %s\n", inst->fname);
            if (inst->nops > 0) fprintf(out, "    adjw %d\n", inst->nops * 4);
        } else {
            // Non-variadic: register args (ops[0..nreg-1]) are pre-colored to r1..r3
            // by IK_COPY instructions inserted by emit_reg_arg_copies in braun.c.
            // IRC's interference analysis guarantees sequential emission is cycle-free.
            int nreg   = inst->nops < 3 ? inst->nops : 3;
            int nextra = inst->nops > 3 ? inst->nops - 3 : 0;
            // Compute mask of all registers holding extra (stack) args, plus r1/r2/r3.
            unsigned extra_avoid = 0x0e; // r1|r2|r3 always off-limits for scratch
            for (int ai = nreg; ai < inst->nops; ai++) {
                Value *av = val_resolve(inst->ops[ai]);
                if (av && av->kind == VAL_INST && av->phys_reg >= 0)
                    extra_avoid |= (1u << av->phys_reg);
            }
            // Push extra stack args right-to-left
            for (int ai = inst->nops - 1; ai >= nreg; ai--) {
                Value *av = val_resolve(inst->ops[ai]);
                if (av && av->kind == VAL_CONST) {
                    int sc = pick_scratch(0, extra_avoid);
                    emit_const_into(out, av, sc);
                    fprintf(out, "    pushr %s\n", regname(sc));
                } else {
                    int ar = av ? preg(av) : -1;
                    if (ar >= 0) fprintf(out, "    pushr %s\n", regname(ar));
                }
            }
            fprintf(out, "    jl %s\n", inst->fname);
            if (nextra > 0) fprintf(out, "    adjw %d\n", nextra * 4);
        }
        if (dst && preg(dst) != 0)
            fprintf(out, "    or %s, r0, r0\n", regname(preg(dst)));
        break;
    }

    case IK_ICALL: {
        // ops[0] = fp (pre-colored r0 by IK_COPY in braun.c)
        // ops[1..nreg] = pre-colored reg args (r1..r3)
        // ops[nreg+1..] = extra stack args
        if (inst->nops < 1) break;
        int nreg   = (inst->nops - 1) < 3 ? (inst->nops - 1) : 3;
        int nextra = (inst->nops - 1) > 3 ? (inst->nops - 1) - 3 : 0;
        // Compute mask of registers holding extra args, plus r0/r1/r2/r3.
        unsigned extra_avoid = 0x0f; // r0|r1|r2|r3 always off-limits for scratch
        for (int ai = nreg + 1; ai < inst->nops; ai++) {
            Value *av = val_resolve(inst->ops[ai]);
            if (av && av->kind == VAL_INST && av->phys_reg >= 0)
                extra_avoid |= (1u << av->phys_reg);
        }
        // Push extra stack args right-to-left
        for (int ai = inst->nops - 1; ai > nreg; ai--) {
            Value *av = val_resolve(inst->ops[ai]);
            if (av && av->kind == VAL_CONST) {
                int sc = pick_scratch(0, extra_avoid);
                emit_const_into(out, av, sc);
                fprintf(out, "    pushr %s\n", regname(sc));
            } else {
                int ar = av ? preg(av) : -1;
                if (ar >= 0) fprintf(out, "    pushr %s\n", regname(ar));
            }
        }
        // fp is in r0 (pre-colored by IK_COPY in braun.c); jlr uses r0 implicitly
        fprintf(out, "    jlr r0\n");
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
    int ann_prev_line = 0;
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];

        // Emit block label (skip for entry block — the function label serves)
        if (bi > 0)
            fprintf(out, "_%s_B%d:\n", f->name, b->id);
        for (Inst *inst = b->head; inst; inst = inst->next) {
            // Emit source-line comment when line changes
            if (flag_annotate && inst->line && inst->line != ann_prev_line) {
                emit_src_comment(inst->line, out);
                ann_prev_line = inst->line;
            }
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
