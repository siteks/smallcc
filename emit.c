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

// vtype_size is defined in ssa.c / declared in ssa.h

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

// Map fuseable comparison kind to F3b branch mnemonic (P5 compare+branch fusion)
static const char *fused_branch_mnemonic(InstKind kind) {
    switch (kind) {
    case IK_EQ:  return "beq";
    case IK_NE:  return "bne";
    case IK_LT:  return "blts";
    case IK_LE:  return "bles";
    case IK_ULT: return "blt";
    case IK_ULE: return "ble";
    default:     return NULL;
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
        {
        int is_signed = vtype_signed(dst->vtype);
        Value *op0 = inst->ops[0] ? val_resolve(inst->ops[0]) : NULL;
        Value *op1 = inst->ops[1] ? val_resolve(inst->ops[1]) : NULL;
        int c0 = op0 && op0->kind == VAL_CONST;
        int c1 = op1 && op1->kind == VAL_CONST;
        // Also detect IK_CONST-defined values (VAL_INST from legalize Pass D)
        int k0 = c0 || (op0 && op0->kind == VAL_INST && op0->def && op0->def->kind == IK_CONST && !op0->def->fname);
        int k1 = c1 || (op1 && op1->kind == VAL_INST && op1->def && op1->def->kind == IK_CONST && !op1->def->fname);
        // p0/p1: physical register of each operand (-1 if operand is const)
        int p0 = c0 ? -1 : (op0 ? preg(op0) : rd);
        int p1 = c1 ? -1 : (op1 ? preg(op1) : rd);

        // P7: Both operands are compile-time constants — fold at emit time.
        // Handles both VAL_CONST operands and VAL_INST from IK_CONST instructions
        // (e.g. AND(mask, const) from legalize Pass D TRUNC/ZEXT lowering).
        // Folding here is safe: IRC is done, no interference-graph impact.
        if (k0 && k1) {
            int32_t a = c0 ? op0->iconst : op0->def->imm;
            int32_t b = c1 ? op1->iconst : op1->def->imm;
            uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
            int32_t result;
            switch (inst->kind) {
            case IK_ADD:  result = a + b;  break;
            case IK_SUB:  result = a - b;  break;
            case IK_MUL:  result = a * b;  break;
            case IK_AND:  result = a & b;  break;
            case IK_OR:   result = a | b;  break;
            case IK_XOR:  result = a ^ b;  break;
            case IK_SHL:  result = (int32_t)(ua << (ub & 31)); break;
            case IK_SHR:  result = (int32_t)(ua >> (ub & 31)); break;
            case IK_USHR: result = (int32_t)(ua >> (ub & 31)); break;
            case IK_EQ:   result = (a == b); break;
            case IK_NE:   result = (a != b); break;
            case IK_LT:   result = (a < b);  break;
            case IK_ULT:  result = (ua < ub); break;
            case IK_LE:   result = (a <= b);  break;
            case IK_ULE:  result = (ua <= ub); break;
            default: goto p7_no_fold;
            }
            emit_imm(out, rd, result);
            break;
            p7_no_fold:;
        }

        // P8: AND(x, 0xFF) → zxb; AND(x, 0xFFFF) → zxw
        // In-place (rd == preg(x)): just zxb/zxw rd (2 bytes).
        // Cross-register: or rd, ps, ps; zxb/zxw rd (4 bytes vs 5 for immw+and).
        if (inst->kind == IK_AND && (k0 ^ k1)) {
            int kv = k1 ? (c1 ? op1->iconst : op1->def->imm)
                        : (c0 ? op0->iconst : op0->def->imm);
            int ps = k1 ? p0 : p1;
            if (kv == 0xff || kv == 0xffff) {
                const char *zx = kv == 0xff ? "zxb" : "zxw";
                if (ps != rd)
                    fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(ps), regname(ps));
                fprintf(out, "    %s %s\n", zx, regname(rd));
                break;
            }
        }

        // P14: AND(x, k) where k in 0..127 → andi (F2, in-place, 2 bytes)
        if (inst->kind == IK_AND && (k0 ^ k1)) {
            int kv = k1 ? (c1 ? op1->iconst : op1->def->imm)
                        : (c0 ? op0->iconst : op0->def->imm);
            int ps = k1 ? p0 : p1;
            if (kv >= 0 && kv <= 127) {
                if (ps != rd)
                    fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(ps), regname(ps));
                fprintf(out, "    andi %s, %d\n", regname(rd), kv);
                break;
            }
        }

        // P2/P3/P4: compact encoding for ADD/SUB with exactly one const operand
        if ((inst->kind == IK_ADD || inst->kind == IK_SUB) && (c0 ^ c1)) {
            int k = 0, ps = -1, ok = 1;
            if (inst->kind == IK_ADD) {
                // ADD is commutative: either operand can be the constant
                k  = c1 ? op1->iconst : op0->iconst;
                ps = c1 ? p0 : p1;
            } else {
                // IK_SUB: only optimise when ops[1] is the constant
                // rd = p0 - k  →  rd = p0 + (-k), use addli rd, p0, -k
                if (!c1) { ok = 0; }
                else { k = -op1->iconst; ps = p0; }
            }
            if (ok) {
                if (k == 1 && ps == rd)
                    { fprintf(out, "    inc %s\n", regname(rd)); break; }
                if (k == -1 && ps == rd)
                    { fprintf(out, "    dec %s\n", regname(rd)); break; }
                if (k >= -64 && k <= 63 && ps == rd)
                    { fprintf(out, "    addi %s, %d\n", regname(rd), k); break; }
                if (k >= -512 && k <= 511)
                    { fprintf(out, "    addli %s, %s, %d\n", regname(rd), regname(ps), k); break; }
            }
        }

        // P11: SHL with constant shift amount → shli (F2, in-place, 2 bytes)
        // In-place: shli rd, k (2 bytes vs 5 for immw+shl)
        // Cross-reg: or rd, ps, ps; shli rd, k (4 bytes vs 5 for immw+shl)
        if (inst->kind == IK_SHL && c1 && !c0) {
            int k = op1->iconst & 31;
            if (k >= 0 && k <= 63) {
                if (p0 != rd)
                    fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(p0), regname(p0));
                fprintf(out, "    shli %s, %d\n", regname(rd), k);
                break;
            }
        }

        // P13: Right shift with constant amount → shrsi (F2, in-place, 2 bytes)
        // Signed: always safe (shrsi is arithmetic).
        // Unsigned: safe when operand is ≤16 bits (upper bits are 0, arith == logical).
        if ((inst->kind == IK_SHR || inst->kind == IK_USHR) && c1 && !c0) {
            int k = op1->iconst & 31;
            int safe = is_signed;
            if (!safe && inst->dst) {
                ValType dvt = inst->dst->vtype;
                safe = (dvt != VT_U32 && dvt != VT_I32);
            }
            if (safe && k >= 0 && k <= 63) {
                if (p0 != rd)
                    fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(p0), regname(p0));
                fprintf(out, "    shrsi %s, %d\n", regname(rd), k);
                break;
            }
        }

        if (!c0 && !c1) {
            // Both VAL_INST: straightforward three-register emit.
            fprintf(out, "    %s %s, %s, %s\n",
                    alu_mnemonic(inst->kind, is_signed),
                    regname(rd), regname(p0), regname(p1));
        } else if (c1) {
            // ops[1] is a constant.  Materialize into rd if safe, else scratch.
            if (c0) {
                // Both const: load op0 into rd, load op1 into scratch.
                emit_imm(out, rd, op0->iconst);
                p0 = rd;
            }
            if (p0 == rd) {
                // ops[0] occupies rd — can't use rd for the constant.
                int sc = (rd == 0) ? 1 : 0;
                fprintf(out, "    pushr %s\n", regname(sc));
                emit_imm(out, sc, op1->iconst);
                fprintf(out, "    %s %s, %s, %s\n",
                        alu_mnemonic(inst->kind, is_signed),
                        regname(rd), regname(rd), regname(sc));
                fprintf(out, "    popr %s\n", regname(sc));
            } else {
                // ops[0] is in p0 != rd: load const into rd, emit op(p0, rd).
                emit_imm(out, rd, op1->iconst);
                fprintf(out, "    %s %s, %s, %s\n",
                        alu_mnemonic(inst->kind, is_signed),
                        regname(rd), regname(p0), regname(rd));
            }
        } else {
            // ops[0] is const, ops[1] is VAL_INST.
            if (p1 == rd) {
                // ops[1] occupies rd — can't use rd for the constant.
                int sc = (rd == 0) ? 1 : 0;
                fprintf(out, "    pushr %s\n", regname(sc));
                emit_imm(out, sc, op0->iconst);
                fprintf(out, "    %s %s, %s, %s\n",
                        alu_mnemonic(inst->kind, is_signed),
                        regname(rd), regname(sc), regname(rd));
                fprintf(out, "    popr %s\n", regname(sc));
            } else {
                // ops[1] in p1 != rd: load const into rd, emit op(rd, p1).
                emit_imm(out, rd, op0->iconst);
                fprintf(out, "    %s %s, %s, %s\n",
                        alu_mnemonic(inst->kind, is_signed),
                        regname(rd), regname(rd), regname(p1));
            }
        }
        break;
        }
    }

    // IK_NEG and IK_NOT are lowered by legalize_function() to two-operand
    // IK_SUB/IK_FSUB and IK_EQ respectively, with an explicit IK_CONST(0)
    // operand so IRC allocates the zero register.  These cases are dead.

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
        Value *sv = inst->ops[0] ? val_resolve(inst->ops[0]) : NULL;
        if (sv && sv->kind == VAL_CONST) {
            int v = (int8_t)(sv->iconst & 0xff);
            emit_imm(out, rd, v);
        } else if (sv && sv->kind == VAL_INST && sv->def && sv->def->kind == IK_CONST && !sv->def->fname) {
            int v = (int8_t)(sv->def->imm & 0xff);
            emit_imm(out, rd, v);
        } else if (sv && sv->kind == VAL_INST && sv->def &&
                   sv->def->kind == IK_LOAD && sv->def->size == 1 &&
                   vtype_signed(sv->vtype)) {
            // P10: load already used lbx (sign-extending); sxb is redundant
            int r1 = get_val_reg(out, inst->ops[0], rd);
            if (r1 != rd) fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(r1), regname(r1));
        } else {
            int r1 = get_val_reg(out, inst->ops[0], rd);
            if (r1 != rd) fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(r1), regname(r1));
            fprintf(out, "    sxb %s\n", regname(rd));
        }
        break;
    }
    case IK_SEXT16: {
        if (!dst || inst->nops < 1) break;
        Value *sv = inst->ops[0] ? val_resolve(inst->ops[0]) : NULL;
        if (sv && sv->kind == VAL_CONST) {
            int v = (int16_t)(sv->iconst & 0xffff);
            emit_imm(out, rd, v);
        } else if (sv && sv->kind == VAL_INST && sv->def && sv->def->kind == IK_CONST && !sv->def->fname) {
            int v = (int16_t)(sv->def->imm & 0xffff);
            emit_imm(out, rd, v);
        } else if (sv && sv->kind == VAL_INST && sv->def &&
                   sv->def->kind == IK_LOAD && sv->def->size == 2 &&
                   vtype_signed(sv->vtype)) {
            // P10: load already used lwx/llwx (sign-extending); sxw is redundant
            int r1 = get_val_reg(out, inst->ops[0], rd);
            if (r1 != rd) fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(r1), regname(r1));
        } else {
            int r1 = get_val_reg(out, inst->ops[0], rd);
            if (r1 != rd) fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(r1), regname(r1));
            fprintf(out, "    sxw %s\n", regname(rd));
        }
        break;
    }
    case IK_ZEXT:
    case IK_TRUNC: {
        // legalize_function() lowered ZEXT/TRUNC with mask_size < 4 to
        // IK_CONST + IK_AND before IRC, so any remaining ZEXT/TRUNC here
        // is the mask_size >= 4 case (no masking needed — just copy).
        if (!dst || inst->nops < 1) break;
        int r1 = get_val_reg(out, inst->ops[0], rd);
        if (r1 != rd) fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(r1), regname(r1));
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
            // by IK_COPY instructions inserted by legalize_function() (Pass B).
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
        // ops[0] = fp (pre-colored r0 by IK_COPY inserted by legalize_function() Pass B)
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
        // fp is in r0 (pre-colored by IK_COPY from legalize_function() Pass B)
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

typedef struct {
    int         fused;     // 0=none, 1=P5 two-reg, 2=P5+ const-operand, 3=P6 zero-test
    const char *mnem;      // F3b branch mnemonic (beq/bne/blt/ble/blts/bles)
    int         p0, p1;    // physical registers of the comparison operands
    int         const_val; // P5+: the constant operand's value
    int         swap;      // P5+: 1 if constant is the first (lhs) operand
    int         is_eq;     // P6: 1 for EQ(x,0)→jz, 0 for NE(x,0)→jnz
} BranchFuse;

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

    // P7/P8 pre-pass: mark IK_CONST instructions dead when their only consumer
    // is an ALU op that P7 will fold (both operands constant) or P8 will replace
    // with zxb/zxw (AND with mask 0xFF/0xFFFF).
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead) continue;
            // P9: SEXT8/SEXT16 of constant → immw; source IK_CONST not needed
            if ((inst->kind == IK_SEXT8 || inst->kind == IK_SEXT16) && inst->nops >= 1) {
                Value *sv = inst->ops[0] ? val_resolve(inst->ops[0]) : NULL;
                if (sv && sv->kind == VAL_INST && sv->def && sv->def->kind == IK_CONST
                    && !sv->def->fname && sv->use_count <= 1)
                    sv->def->is_dead = 1;
                continue;
            }
            if (inst->kind < IK_ADD || inst->kind > IK_FNE) continue;
            if (inst->nops < 2) continue;
            Value *o0 = inst->ops[0] ? val_resolve(inst->ops[0]) : NULL;
            Value *o1 = inst->ops[1] ? val_resolve(inst->ops[1]) : NULL;
            int ik0 = o0 && o0->kind == VAL_INST && o0->def && o0->def->kind == IK_CONST && !o0->def->fname;
            int ik1 = o1 && o1->kind == VAL_INST && o1->def && o1->def->kind == IK_CONST && !o1->def->fname;
            int is_k0 = (o0 && o0->kind == VAL_CONST) || ik0;
            int is_k1 = (o1 && o1->kind == VAL_CONST) || ik1;

            // P7: both operands constant — fold eliminates entire ALU op
            if (is_k0 && is_k1) {
                if (ik0 && o0->use_count <= 1) o0->def->is_dead = 1;
                if (ik1 && o1->use_count <= 1) o1->def->is_dead = 1;
                continue;
            }
            // P8: AND(x, 0xFF/0xFFFF) → zxb/zxw; mask IK_CONST is not needed
            if (inst->kind == IK_AND && (is_k0 ^ is_k1)) {
                Value *kv = is_k1 ? o1 : o0;
                int mask = (kv->kind == VAL_CONST) ? kv->iconst : kv->def->imm;
                int is_ik = is_k1 ? ik1 : ik0;
                if ((mask == 0xff || mask == 0xffff) && is_ik && kv->use_count <= 1)
                    kv->def->is_dead = 1;
            }
        }
    }

    // P5 pre-pass: detect comparison+branch pairs that can be fused into a single
    // F3b branch instruction (beq/bne/blt/ble/blts/bles ra, rb, label).
    // Criteria: tail is IK_BR, condition has use_count==1, defining instruction
    // is a supported comparison in the same block, both operands have phys regs.
    BranchFuse *fuse = arena_alloc(f->nblocks * sizeof(BranchFuse));
    memset(fuse, 0, f->nblocks * sizeof(BranchFuse));
    for (int fbi = 0; fbi < f->nblocks; fbi++) {
        Block *fb  = f->blocks[fbi];
        Inst  *term = fb->tail;
        if (!term || term->kind != IK_BR || term->nops < 1) continue;
        Value *cond = val_resolve(term->ops[0]);
        if (!cond || cond->kind != VAL_INST || cond->use_count != 1) continue;
        Inst *def = cond->def;
        if (!def || def->block != fb || def->is_dead || def->nops < 2) continue;
        const char *mnem = fused_branch_mnemonic(def->kind);
        if (!mnem) continue;
        Value *dop0 = def->ops[0] ? val_resolve(def->ops[0]) : NULL;
        Value *dop1 = def->ops[1] ? val_resolve(def->ops[1]) : NULL;
        // F3b branches use a 10-bit PC-relative offset (±511 bytes).
        // Estimate the byte distance to the true target block; skip fusion
        // if it might overflow the signed 10-bit range.
        int target_bi = -1;
        for (int k = 0; k < f->nblocks; k++) {
            if (f->blocks[k] == term->target) { target_bi = k; break; }
        }
        if (target_bi < 0) continue;
        int lo = (fbi < target_bi) ? fbi + 1 : target_bi;
        int hi = (fbi < target_bi) ? target_bi : fbi;
        int est_bytes = 0;
        for (int k = lo; k < hi; k++) {
            for (Inst *ii = f->blocks[k]->head; ii; ii = ii->next)
                if (!ii->is_dead) est_bytes += 3; // worst-case 3 bytes/instruction
        }
        int in_range = (est_bytes <= 450);

        // P5: both operands are registers
        if (in_range &&
            dop0 && dop0->kind == VAL_INST && dop0->phys_reg >= 0 &&
            dop1 && dop1->kind == VAL_INST && dop1->phys_reg >= 0) {
            def->is_dead     = 1;
            fuse[fbi].fused  = 1;
            fuse[fbi].mnem   = mnem;
            fuse[fbi].p0     = dop0->phys_reg;
            fuse[fbi].p1     = dop1->phys_reg;
            continue;
        }

        // P5+: one operand is VAL_CONST, the other has a phys_reg.
        // Use cond->phys_reg (dead after this block, use_count==1) as scratch
        // for materialising the constant, then emit a fused F3b branch.
        if (in_range && cond->phys_reg >= 0) {
            Value *reg_op = NULL, *const_op = NULL;
            int const_is_lhs = 0;
            if (dop1 && dop1->kind == VAL_CONST &&
                dop0 && dop0->kind == VAL_INST && dop0->phys_reg >= 0) {
                reg_op = dop0; const_op = dop1;
            } else if (dop0 && dop0->kind == VAL_CONST &&
                       dop1 && dop1->kind == VAL_INST && dop1->phys_reg >= 0) {
                reg_op = dop1; const_op = dop0; const_is_lhs = 1;
            }
            if (reg_op && const_op) {
                int sc = cond->phys_reg;
                int rp = reg_op->phys_reg;
                // If scratch == reg_op's register, look through IK_COPY to find
                // the source register (e.g. type-coercing copies u8→i16).  The
                // source is still live at this point, so its register is valid.
                Inst *looked_through = NULL;
                if (sc == rp && reg_op->def && reg_op->def->kind == IK_COPY
                    && reg_op->def->nops >= 1) {
                    Value *src = val_resolve(reg_op->def->ops[0]);
                    if (src && src->kind == VAL_INST && src->phys_reg >= 0
                        && src->phys_reg != sc) {
                        rp = src->phys_reg;
                        if (reg_op->use_count <= 1)
                            looked_through = reg_op->def;
                    }
                }
                if (sc != rp) {
                    def->is_dead       = 1;
                    if (looked_through) looked_through->is_dead = 1;
                    fuse[fbi].fused     = 2;
                    fuse[fbi].mnem      = mnem;
                    fuse[fbi].p0        = rp;
                    fuse[fbi].p1        = sc;
                    fuse[fbi].const_val = const_op->iconst;
                    fuse[fbi].swap      = const_is_lhs;
                    continue;
                }
            }
        }

        // P6: NE(x, 0) or EQ(x, 0) — eliminate comparison, use jnz/jz directly.
        // jnz/jz (F3a) only test r0; if x is in another register, emit a MOV first.
        if ((def->kind == IK_NE || def->kind == IK_EQ) && def->nops >= 2) {
            Value *test_val = NULL;
            if (dop1 && dop1->kind == VAL_CONST && dop1->iconst == 0 &&
                dop0 && dop0->kind == VAL_INST && dop0->phys_reg >= 0)
                test_val = dop0;
            else if (dop0 && dop0->kind == VAL_CONST && dop0->iconst == 0 &&
                     dop1 && dop1->kind == VAL_INST && dop1->phys_reg >= 0)
                test_val = dop1;
            if (test_val) {
                def->is_dead     = 1;
                fuse[fbi].fused  = 3;
                fuse[fbi].p0     = test_val->phys_reg;
                fuse[fbi].is_eq  = (def->kind == IK_EQ);
            }
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
            } else if (inst->kind == IK_BR && !inst->is_dead && inst->nops >= 1) {
                // P1 + P5 + P15: branch elimination, fusion, and branch inversion
                Block *next_blk = (bi + 1 < f->nblocks) ? f->blocks[bi + 1] : NULL;
                // P15: when true target is the next block, invert the branch to
                // target false directly — saves the unconditional j instruction.
                int inverted = 0;
                if (fuse[bi].fused == 1) {
                    // P5: fused compare+branch — single F3b instruction
                    if (inst->target)
                        fprintf(out, "    %s %s, %s, _%s_B%d\n",
                                fuse[bi].mnem,
                                regname(fuse[bi].p0), regname(fuse[bi].p1),
                                f->name, inst->target->id);
                } else if (fuse[bi].fused == 2) {
                    // P5+: load constant into scratch, then F3b branch
                    emit_imm(out, fuse[bi].p1, fuse[bi].const_val);
                    if (inst->target) {
                        if (fuse[bi].swap)
                            fprintf(out, "    %s %s, %s, _%s_B%d\n",
                                    fuse[bi].mnem,
                                    regname(fuse[bi].p1), regname(fuse[bi].p0),
                                    f->name, inst->target->id);
                        else
                            fprintf(out, "    %s %s, %s, _%s_B%d\n",
                                    fuse[bi].mnem,
                                    regname(fuse[bi].p0), regname(fuse[bi].p1),
                                    f->name, inst->target->id);
                    }
                } else if (fuse[bi].fused == 3) {
                    // P6: NE(x,0)/EQ(x,0) → jnz/jz rX directly
                    if (inst->target == next_blk && inst->target2) {
                        // P15: true is next block — invert sense, target false
                        fprintf(out, "    %s %s, _%s_B%d\n",
                                fuse[bi].is_eq ? "jnz" : "jz",
                                regname(fuse[bi].p0),
                                f->name, inst->target2->id);
                        inverted = 1;
                    } else if (inst->target) {
                        fprintf(out, "    %s %s, _%s_B%d\n",
                                fuse[bi].is_eq ? "jz" : "jnz",
                                regname(fuse[bi].p0),
                                f->name, inst->target->id);
                    }
                } else {
                    // Standard: jnz/jz cond
                    int r1 = get_val_reg(out, inst->ops[0], 0);
                    if (inst->target == next_blk && inst->target2) {
                        // P15: true is next block — invert to jz false_target
                        fprintf(out, "    jz %s, _%s_B%d\n",
                                regname(r1), f->name, inst->target2->id);
                        inverted = 1;
                    } else if (inst->target) {
                        fprintf(out, "    jnz %s, _%s_B%d\n",
                                regname(r1), f->name, inst->target->id);
                    }
                }
                // P1: omit j F when F is the immediately following block
                // (skip entirely when P15 inverted — false was already targeted)
                if (!inverted && inst->target2 && inst->target2 != next_blk)
                    fprintf(out, "    j _%s_B%d\n", f->name, inst->target2->id);
            } else if (inst->kind == IK_JMP && inst->target &&
                       inst->target == ((bi + 1 < f->nblocks) ? f->blocks[bi + 1] : NULL)) {
                // P1: omit unconditional j to the immediately following block
            } else if (inst->kind == IK_JMP && inst->target) {
                // P12: loop rotation — when jumping to a header that only
                // contains IK_BR, duplicate the branch here to eliminate the
                // unconditional jump per iteration.
                Block *hdr = inst->target;
                // Find the IK_BR terminator in the header and check that all
                // other instructions are dead (OOS copies already executed here).
                Inst *hdr_br = NULL;
                int hdr_clean = 1;
                for (Inst *hi = hdr->head; hi; hi = hi->next) {
                    if (hi->kind == IK_BR && !hi->is_dead && hi->nops >= 1) {
                        hdr_br = hi;
                    } else if (!hi->is_dead && hi->kind != IK_JMP) {
                        hdr_clean = 0;
                        break;
                    }
                }
                if (hdr_clean && hdr_br && hdr_br->nops >= 1) {
                    Value *cond = val_resolve(hdr_br->ops[0]);
                    if (cond && cond->kind == VAL_INST && cond->phys_reg >= 0) {
                        // Find the header's block index for fuse table lookup
                        int hbi = -1;
                        for (int hi = 0; hi < f->nblocks; hi++) {
                            if (f->blocks[hi] == hdr) { hbi = hi; break; }
                        }
                        Block *next_blk = (bi + 1 < f->nblocks) ? f->blocks[bi + 1] : NULL;
                        int rotated = 0;
                        if (hbi >= 0 && fuse[hbi].fused == 0) {
                            // Standard branch: jnz cond, true_target
                            if (hdr_br->target)
                                fprintf(out, "    jnz %s, _%s_B%d\n",
                                        regname(cond->phys_reg), f->name, hdr_br->target->id);
                            rotated = 1;
                        } else if (hbi >= 0 && fuse[hbi].fused == 1) {
                            // P5 fused: two-reg compare+branch
                            if (hdr_br->target)
                                fprintf(out, "    %s %s, %s, _%s_B%d\n",
                                        fuse[hbi].mnem,
                                        regname(fuse[hbi].p0), regname(fuse[hbi].p1),
                                        f->name, hdr_br->target->id);
                            rotated = 1;
                        } else if (hbi >= 0 && fuse[hbi].fused == 2) {
                            // P5+ fused: constant-operand compare+branch
                            emit_imm(out, fuse[hbi].p1, fuse[hbi].const_val);
                            if (hdr_br->target) {
                                if (fuse[hbi].swap)
                                    fprintf(out, "    %s %s, %s, _%s_B%d\n",
                                            fuse[hbi].mnem,
                                            regname(fuse[hbi].p1), regname(fuse[hbi].p0),
                                            f->name, hdr_br->target->id);
                                else
                                    fprintf(out, "    %s %s, %s, _%s_B%d\n",
                                            fuse[hbi].mnem,
                                            regname(fuse[hbi].p0), regname(fuse[hbi].p1),
                                            f->name, hdr_br->target->id);
                            }
                            rotated = 1;
                        } else if (hbi >= 0 && fuse[hbi].fused == 3) {
                            // P6 fused: zero-test jnz/jz
                            if (hdr_br->target)
                                fprintf(out, "    %s %s, _%s_B%d\n",
                                        fuse[hbi].is_eq ? "jz" : "jnz",
                                        regname(fuse[hbi].p0),
                                        f->name, hdr_br->target->id);
                            rotated = 1;
                        }
                        if (rotated) {
                            if (hdr_br->target2 && hdr_br->target2 != next_blk)
                                fprintf(out, "    j _%s_B%d\n", f->name, hdr_br->target2->id);
                        } else {
                            emit_inst(inst, out);
                        }
                    } else {
                        emit_inst(inst, out);
                    }
                } else {
                    emit_inst(inst, out);
                }
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
