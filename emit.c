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
static uint8_t     g_blk_live_out_regs = 0xff; // conservative default: all busy

// Find a caller-saved scratch register (r0-r3) that is genuinely dead at `inst`.
// `exclude` is a bitmask of registers that must not be chosen (operands of inst).
// Returns register number 0-3, or -1 if none is provably free.
//
// Use this when the IR at `inst` doesn't give you a free register but the block
// does — it forward-scans the remaining instructions to prove no live register
// is clobbered.  Prefer pick_scratch() when the caller already knows which
// registers are in use (faster; no forward scan).
static int find_free_scratch(Inst *inst, int exclude) {
    // Build bitmask of busy registers: live-out + forward-referenced + excluded
    unsigned busy = g_blk_live_out_regs | (unsigned)exclude;

    // Forward scan: mark registers read by remaining instructions in this block.
    // Do NOT skip is_dead instructions — P5 fusion marks comparisons dead but
    // the fused branch still needs their operand registers.
    for (Inst *p = inst->next; p; p = p->next) {
        for (int i = 0; i < p->nops; i++) {
            Value *v = val_resolve(p->ops[i]);
            if (v && v->kind == VAL_INST && v->phys_reg >= 0 && v->phys_reg < 8)
                busy |= (1u << v->phys_reg);
        }
    }

    // Pick first free caller-saved register
    for (int r = 0; r <= 3; r++)
        if (!(busy & (1u << r))) return r;
    return -1;
}

// Pick a scratch register not in (already_used | forbidden).  Falls back to r0
// if nothing is clean — callers must tolerate the fallback (typically by
// pushr/popr around the use).  Use this when the caller has already built a
// precise busy mask for the surrounding context (e.g. call-arg emission with
// known arg registers).  Prefer find_free_scratch() when the busy set depends
// on forward uses within the block.
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

// CPU4 load mnemonic for size (register-relative F3c)
static const char *load_f3c(int size, int is_signed) {
    if (size == 1) return is_signed ? "llbx" : "llb";
    if (size == 2) return is_signed ? "llwx" : "llw";
    return "lll";
}

// CPU4 store mnemonic for size (register-relative F3c)
static const char *store_f3c(int size) {
    if (size == 1) return "slb";
    if (size == 2) return "slw";
    return "sll";
}

// Emit a bp-relative load into `rd`.  Uses F2 (2 bytes) when the offset fits;
// otherwise emits lea + optional addi (to correct non-4-aligned offsets,
// since CPU4 lea encodes only imm14*4) followed by an F3c load through `rd`.
// `rd` must be free at this point — the fallback reuses it as both address
// temp and destination.
static void emit_bp_load(FILE *out, int rd, int off, int size, int is_signed) {
    if (f2_range(off, size)) {
        int scaled = (size >= 4) ? off / 4 : (size == 2) ? off / 2 : off;
        fprintf(out, "    %s %s, %d\n",
                load_f2(size, is_signed), regname(rd), scaled);
        return;
    }
    int adj  = off % 4;
    int base = off - adj;
    fprintf(out, "    lea %s, %d\n", regname(rd), base);
    if (adj) fprintf(out, "    addi %s, %d\n", regname(rd), adj);
    fprintf(out, "    %s %s, %s, 0\n",
            load_f3c(size, is_signed), regname(rd), regname(rd));
}

// Emit a bp-relative store of `rv`.  Uses F2 when the offset fits; otherwise
// emits lea + optional addi into `tmp`, then an F3c store through `tmp`.
// `tmp` is used only in the fallback and must differ from `rv`.
static void emit_bp_store(FILE *out, int rv, int tmp, int off, int size) {
    if (f2_range(off, size)) {
        int scaled = (size >= 4) ? off / 4 : (size == 2) ? off / 2 : off;
        fprintf(out, "    %s %s, %d\n", store_f2(size), regname(rv), scaled);
        return;
    }
    int adj  = off % 4;
    int base = off - adj;
    fprintf(out, "    lea %s, %d\n", regname(tmp), base);
    if (adj) fprintf(out, "    addi %s, %d\n", regname(tmp), adj);
    fprintf(out, "    %s %s, %s, 0\n", store_f3c(size), regname(rv), regname(tmp));
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

// Emit a 3-operand ALU instruction where one source is a register (`r_reg`)
// and the other source is the compile-time constant `kv`.  `const_on_left`
// selects the operand position for the constant (1 = first alu source,
// 0 = second).  Materializes the constant into `rd` when `r_reg != rd`;
// otherwise into a scratch register (using pushr/popr if none is free).
static void emit_alu_const(FILE *out, Inst *inst, int is_signed, int rd,
                           int r_reg, int kv, int const_on_left) {
    const char *m = alu_mnemonic(inst->kind, is_signed);
    if (r_reg != rd) {
        // rd is free: materialize constant into rd.
        emit_imm(out, rd, kv);
        if (const_on_left)
            fprintf(out, "    %s %s, %s, %s\n", m, regname(rd), regname(rd), regname(r_reg));
        else
            fprintf(out, "    %s %s, %s, %s\n", m, regname(rd), regname(r_reg), regname(rd));
        return;
    }
    // r_reg occupies rd — materialize constant into a scratch, spilling if
    // no free register is available.
    int sc = find_free_scratch(inst, 1u << rd);
    int spilled = (sc < 0);
    if (spilled) {
        sc = (rd == 0) ? 1 : 0;
        fprintf(out, "    pushr %s\n", regname(sc));
    }
    emit_imm(out, sc, kv);
    if (const_on_left)
        fprintf(out, "    %s %s, %s, %s\n", m, regname(rd), regname(sc), regname(rd));
    else
        fprintf(out, "    %s %s, %s, %s\n", m, regname(rd), regname(rd), regname(sc));
    if (spilled) fprintf(out, "    popr %s\n", regname(sc));
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

// True if v is defined by an IK_CONST instruction (not VAL_CONST, not a
// symbolic GADDR).  Callers use this to decide whether a constant's def
// can be safely marked is_dead after peephole folding.
static int is_ik_const(Value *v) {
    return v && v->kind == VAL_INST && v->def &&
           v->def->kind == IK_CONST && !v->def->fname;
}

// resolve_const: try to resolve a Value to a compile-time integer constant.
// Handles VAL_CONST, IK_CONST-defined values, and AND(const, const) from
// legalize TRUNC lowering (e.g. AND(LICM_hoisted_1, 0xFFFF) = 1).
static int resolve_const(Value *v, int *out) {
    v = val_resolve(v);
    if (!v) return 0;
    if (v->kind == VAL_CONST) { *out = v->iconst; return 1; }
    if (v->kind != VAL_INST || !v->def) return 0;
    if (v->def->kind == IK_CONST) { *out = v->def->imm; return 1; }
    if (v->def->kind == IK_AND && v->def->nops >= 2) {
        int a, b;
        if (resolve_const(v->def->ops[0], &a) &&
            resolve_const(v->def->ops[1], &b)) {
            *out = a & b; return 1;
        }
    }
    return 0;
}

// P15 helper: compute a bitmask of possibly-set bits for a value.
// Returns a non-negative mask, or -1 if unknown.
// Used to eliminate redundant AND(x, mask) when x's bits are already within mask.
static int known_bits_mask(Value *v, int depth) {
    if (depth <= 0) return -1;
    v = val_resolve(v);
    if (!v) return -1;
    if (v->kind == VAL_CONST) return v->iconst >= 0 ? v->iconst : -1;
    if (v->kind != VAL_INST || !v->def) return -1;
    Inst *d = v->def;
    if (d->is_dead) return -1;
    if (d->kind == IK_CONST) return d->imm >= 0 ? d->imm : -1;
    if (d->kind == IK_AND && d->nops >= 2) {
        int m0 = known_bits_mask(d->ops[0], depth - 1);
        int m1 = known_bits_mask(d->ops[1], depth - 1);
        if (m0 >= 0 && m1 >= 0) return m0 & m1;
        if (m0 >= 0) return m0;
        if (m1 >= 0) return m1;
        return -1;
    }
    if ((d->kind == IK_XOR || d->kind == IK_OR) && d->nops >= 2) {
        int m0 = known_bits_mask(d->ops[0], depth - 1);
        int m1 = known_bits_mask(d->ops[1], depth - 1);
        if (m0 >= 0 && m1 >= 0) return m0 | m1;
        return -1;
    }
    return -1;
}

// P16: bitex fusion tables (populated by detect_bitex_fusions, read by emit_inst)
static int *g_bitex_src;   // [value_id] → phys reg of SHR source, or -1
static int *g_bitex_imm;   // [value_id] → bitex imm9 encoding

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

        // P16: bitex fusion — AND(SHR(x, k_shift), k_mask) → bitex rd, src, imm9
        // Detected by detect_bitex_fusions pre-pass; the SHR is already marked dead.
        if (inst->kind == IK_AND && dst && g_bitex_src && g_bitex_src[dst->id] >= 0) {
            fprintf(out, "    bitex %s, %s, %d\n",
                    regname(rd), regname(g_bitex_src[dst->id]),
                    g_bitex_imm[dst->id]);
            break;
        }

        // P15: Redundant AND elimination via known-bits analysis.
        // AND(x, mask) is a no-op when all bits of x are already within mask.
        // Common pattern: AND(XOR(AND(a,1),AND(b,1)), 0xFF) — the XOR of two
        // 1-bit values is 1-bit, so the outer AND with 0xFF is redundant.
        if (inst->kind == IK_AND && (k0 ^ k1)) {
            int kv = k1 ? (c1 ? op1->iconst : op1->def->imm)
                        : (c0 ? op0->iconst : op0->def->imm);
            Value *src = k1 ? op0 : op1;
            int ps = k1 ? p0 : p1;
            if (kv > 0) {
                int src_mask = known_bits_mask(src, 4);
                if (src_mask >= 0 && (src_mask & ~kv) == 0) {
                    // AND is redundant — just move source to dest
                    if (ps != rd)
                        fprintf(out, "    or %s, %s, %s\n", regname(rd), regname(ps), regname(ps));
                    break;
                }
            }
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
        //       AND(x, k) where k in 128..255 → andli (F0b, 3 bytes vs 5 for immw+and)
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
            if (kv >= 128 && kv <= 255) {
                fprintf(out, "    andli %s, %s, %d\n", regname(rd), regname(ps), kv);
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
                if (k >= -256 && k <= 255)
                    { fprintf(out, "    addli %s, %s, %d\n", regname(rd), regname(ps), k); break; }
            }
        }

        // P18: MUL(x, k) where k fits sext9 → mulli (F0b, 3 bytes vs 5 for immw+mul)
        if (inst->kind == IK_MUL && (c0 ^ c1)) {
            int kv = c1 ? op1->iconst : op0->iconst;
            int ps = c1 ? p0 : p1;
            if (kv >= -256 && kv <= 255)
                { fprintf(out, "    mulli %s, %s, %d\n", regname(rd), regname(ps), kv); break; }
        }

        // P11: SHL with constant shift amount
        // In-place: shli rd, k (F2, 2 bytes)
        // Cross-reg: shlli rd, ps, k (F0b, 3 bytes vs 4 for or+shli)
        if (inst->kind == IK_SHL && !k0) {
            int shift_k;
            if (resolve_const(op1, &shift_k)) {
                int k = shift_k & 31;
                if (k >= 0 && k <= 63) {
                    if (p0 == rd)
                        fprintf(out, "    shli %s, %d\n", regname(rd), k);
                    else
                        fprintf(out, "    shlli %s, %s, %d\n", regname(rd), regname(p0), k);
                    break;
                }
            }
        }

        // P13: Right shift with constant amount
        // Signed: shrsi (F2 in-place, 2 bytes) or shrsli (F0b cross-reg, 3 bytes)
        // Unsigned: shrsi safe when ≤16 bits; else shrli (F0b, 3 bytes, logical)
        if ((inst->kind == IK_SHR || inst->kind == IK_USHR) && !k0) {
            int shift_k;
            if (resolve_const(op1, &shift_k)) {
                int k = shift_k & 31;
                int safe = is_signed;
                if (!safe && inst->dst) {
                    ValType dvt = inst->dst->vtype;
                    safe = (dvt != VT_U32 && dvt != VT_I32);
                }
                if (safe && k >= 0 && k <= 63) {
                    if (p0 == rd)
                        fprintf(out, "    shrsi %s, %d\n", regname(rd), k);
                    else
                        fprintf(out, "    shrsli %s, %s, %d\n", regname(rd), regname(p0), k);
                    break;
                }
                if (!safe && k >= 0 && k <= 31) {
                    // Unsigned 32-bit: use shrli (F0b, logical right shift)
                    fprintf(out, "    shrli %s, %s, %d\n", regname(rd), regname(p0), k);
                    break;
                }
            }
        }

        // P19: F0b immediate ALU (3 bytes vs 5 for immw+alu)
        // Covers comparisons, div/mod, or/xor, reverse-sub not handled by P2-P18.
        if ((c0 ^ c1)) {
            int kv = c1 ? op1->iconst : op0->iconst;
            if (kv >= -256 && kv <= 255) {
                const char *mnem = NULL;
                int ps = c1 ? p0 : p1;
                switch (inst->kind) {
                // Comparisons: EQ/NE are commutative
                case IK_EQ:   mnem = "eqli"; break;
                case IK_NE:   mnem = "neli"; break;
                // LE with const on right → lesli/leli
                // LE with const on left → a > const-1 → gtsli/gtli
                case IK_LE:
                    if (c1) { mnem = is_signed ? "lesli" : "leli"; }
                    else if (kv > (is_signed ? -256 : 0))
                        { mnem = is_signed ? "gtsli" : "gtli"; kv--; }
                    break;
                case IK_ULE:
                    if (c1) { mnem = "leli"; }
                    else if (kv > 0) { mnem = "gtli"; kv--; }
                    break;
                // LT with const on left → a > const → gtsli/gtli
                // LT with const on right → a <= const-1 → lesli/leli
                case IK_LT:
                    if (c0) { mnem = is_signed ? "gtsli" : "gtli"; }
                    else if (kv > (is_signed ? -256 : 0))
                        { mnem = is_signed ? "lesli" : "leli"; kv--; }
                    break;
                case IK_ULT:
                    if (c0) { mnem = "gtli"; }
                    else if (kv > 0) { mnem = "leli"; kv--; }
                    break;
                // SUB with const on left → rsubli (SUB(a,const) handled by P4)
                case IK_SUB:  if (c0) mnem = "rsubli"; break;
                // Division and modulo
                case IK_DIV:  mnem = c1 ? "divsli" : "rdivsli"; break;
                case IK_UDIV: mnem = c1 ? "divli"  : "rdivli";  break;
                case IK_MOD:  if (c1) mnem = "modsli"; break;
                case IK_UMOD: mnem = c1 ? "modli"  : "rmodli";  break;
                // Commutative bitwise ops
                case IK_OR:   mnem = "orli"; break;
                case IK_XOR:  mnem = "xorli"; break;
                default: break;
                }
                if (mnem) {
                    fprintf(out, "    %s %s, %s, %d\n", mnem, regname(rd), regname(ps), kv);
                    break;
                }
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
            emit_alu_const(out, inst, is_signed, rd, p0, op1->iconst, /*const_on_left=*/0);
        } else {
            // ops[0] is const, ops[1] is VAL_INST.
            emit_alu_const(out, inst, is_signed, rd, p1, op0->iconst, /*const_on_left=*/1);
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

        if (!base || base->kind == VAL_UNDEF) {
            // Spill load: base is implicit bp.  Using rd as both address
            // temp and destination in the out-of-F2-range fallback avoids
            // clobbering any other live register.
            emit_bp_load(out, rd, off, size, is_s);
        } else {
            int rb = preg(base);
            if (base->kind == VAL_CONST && base->iconst == 0) {
                // null base, use imm as absolute (unusual)
                int tmp = (rd == 0) ? 1 : 0;
                emit_imm(out, tmp, off);
                fprintf(out, "    %s %s, %s, 0\n", load_f3c(size, is_s), regname(rd), regname(tmp));
            } else {
                // F3c: mnem rx, ry, scaled_imm10
                int scaled = (size >= 4) ? off / 4 : (size == 2) ? off / 2 : off;
                fprintf(out, "    %s %s, %s, %d\n",
                        load_f3c(size, is_s), regname(rd), regname(rb), scaled);
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

        if (!base || (inst->nops < 2)) {
            // Spill store: base is implicit bp.  Need a scratch distinct
            // from rv for the out-of-F2-range lea base.
            int tmp = (rv == 0) ? 1 : 0;
            if (tmp == rv) tmp = 2;
            emit_bp_store(out, rv, tmp, off, size);
        } else {
            // F3c: mnem rv, ry, scaled_imm10
            int scaled = (size >= 4) ? off / 4 : (size == 2) ? off / 2 : off;
            fprintf(out, "    %s %s, %s, %d\n",
                    store_f3c(size), regname(rv), regname(rb), scaled);
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
// emit_function — helpers
// ============================================================

typedef struct {
    int         fused;     // 0=none, 1=P5 two-reg, 2=P5+ const-operand, 3=P6 zero-test, 4=P17 cbeq/cbne
    const char *mnem;      // F3b branch mnemonic (beq/bne/blt/ble/blts/bles)
    int         p0, p1;    // physical registers of the comparison operands
    int         const_val; // P5+: the constant operand's value
    int         swap;      // P5+: 1 if constant is the first (lhs) operand
    int         is_eq;     // P6: 1 for EQ(x,0)→jz, 0 for NE(x,0)→jnz
} BranchFuse;

// Post-IRC register remapping: when a single-use value feeds directly into
// an instruction whose destination register is different, remap the value's
// register to match — eliminating a copy at emission time.
//
// Common pattern: SHR(x,2)→r0, COPY→r0, AND(r0,15)→r7 generates
//   or r0,r6,r6; shrsi r0,2; or r7,r0,r0; andi r7,15
// After remapping SHR result to r7:
//   or r7,r6,r6; shrsi r7,2; andi r7,15
//
// Processed in reverse order so copy chains resolve naturally.
static void remap_single_use_values(Function *f) {
    int debug_remap = getenv("DEBUG_REMAP") != NULL;
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];

        // Physical register live_in mask
        uint8_t live_in_regs = 0;
        if (b->live_in) {
            for (int w = 0; w < b->nwords; w++) {
                uint32_t bits = b->live_in[w];
                while (bits) {
                    int bit = __builtin_ctz(bits);
                    int vid = w * 32 + bit;
                    if (vid < f->nvalues) {
                        int pr = f->values[vid]->phys_reg;
                        if (pr >= 0 && pr < 8)
                            live_in_regs |= (1u << pr);
                    }
                    bits &= bits - 1;
                }
            }
        }

        for (Inst *inst = b->tail; inst; inst = inst->prev) {
            if (inst->is_dead || !inst->dst) continue;
            Value *v = inst->dst;
            if (v->use_count != 1 || v->phys_reg < 0) {
                if (debug_remap && inst->kind == IK_CONST && v->phys_reg >= 0)
                    fprintf(stderr, "remap skip: %s v%d r%d use_count=%d\n",
                            f->name, v->id, v->phys_reg, v->use_count);
                continue;
            }
            // Don't remap call results (must be r0) or params
            if (inst->kind == IK_CALL || inst->kind == IK_ICALL ||
                inst->kind == IK_PARAM || inst->kind == IK_PUTCHAR)
                continue;

            int rd = v->phys_reg;

            // Find single user: scan forward, following through dead copies
            // that chain from v (e.g. dead type-coercing copies left by IRC).
            Inst *user = NULL;
            Value *search_val = v;
            int steps = 0;
            for (Inst *p = inst->next; p && steps < 4; p = p->next) {
                if (p->is_dead) {
                    if (p->kind == IK_COPY && p->dst && p->nops >= 1 &&
                        val_resolve(p->ops[0]) == search_val)
                        search_val = p->dst;
                    continue;
                }
                steps++;
                for (int i = 0; i < p->nops; i++) {
                    if (val_resolve(p->ops[i]) == search_val) { user = p; goto found; }
                }
            }
            found:
            if (!user || !user->dst) continue;
            // Don't remap to match call/putchar destinations — their dst
            // register (r0) is ABI-fixed and unrelated to operand placement.
            if (user->kind == IK_CALL || user->kind == IK_ICALL ||
                user->kind == IK_PUTCHAR)
                continue;

            int target = user->dst->phys_reg;
            if (target < 0 || target == rd) continue;

            // Safety 1: target not live-in to block
            if (live_in_regs & (1u << target)) continue;

            // Safety 2: target not an operand of inst
            int conflict = 0;
            for (int i = 0; i < inst->nops; i++) {
                Value *op = val_resolve(inst->ops[i]);
                if (op && op->kind == VAL_INST && op->phys_reg == target)
                    { conflict = 1; break; }
            }
            if (conflict) continue;

            // Safety 3: target not written by any earlier inst in block
            int prior = 0;
            for (Inst *p = b->head; p != inst; p = p->next) {
                if (p->is_dead) continue;
                if (p->dst && p->dst->phys_reg == target)
                    { prior = 1; break; }
            }
            if (prior) continue;

            // Safety 4: target not read/written between inst and user
            int between = 0;
            for (Inst *p = inst->next; p != user; p = p->next) {
                if (p->is_dead) continue;
                if (p->dst && p->dst->phys_reg == target)
                    { between = 1; break; }
                for (int i = 0; i < p->nops; i++) {
                    Value *op = val_resolve(p->ops[i]);
                    if (op && op->kind == VAL_INST && op->phys_reg == target)
                        { between = 1; break; }
                }
                if (between) break;
            }
            if (between) continue;

            v->phys_reg = target;
            // Also remap dead copies that chain from v to user
            Value *cv = v;
            for (Inst *p = inst->next; p && p != user; p = p->next) {
                if (p->is_dead && p->kind == IK_COPY && p->dst && p->nops >= 1 &&
                    val_resolve(p->ops[0]) == cv) {
                    p->dst->phys_reg = target;
                    cv = p->dst;
                }
            }
        }
    }
}

// Emit function prologue: enter, callee-save stores.
// Returns callee_frame (total bytes for callee-saved registers).
static int emit_prologue(Function *f, FILE *out, int frame, int callee_save[4]) {
    // Detect which r4-r7 are used by non-param values.
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
    int callee_frame = 0;
    for (int r = 4; r <= 7; r++)
        if (callee_save[r - 4]) callee_frame += 4;

    fprintf(out, "    enter %d\n", frame + callee_frame);

    int callee_tmp = 0;
    for (int r = 4; r <= 7; r++) {
        if (callee_save[r - 4]) {
            callee_tmp += 4;
            fprintf(out, "    sl r%d, %d\n", r, -(frame + callee_tmp) / 4);
        }
    }
    return callee_frame;
}

// Pre-pass: sink IK_CONST instructions within each block to just before
// their first use, reducing register pressure from early materialization.
//
// Example: braun.c emits `C[i*N+j] = 0` as `IK_CONST 0` at source order,
// but scalar promotion turns it into an accumulator init whose first use
// is 10+ instructions later.  Sinking the const frees the register during
// the intervening address-setup code.
static void sink_consts(Function *f) {
    // Two-pass approach to avoid linked-list mutation during iteration.
    // Pass 1: collect (IK_CONST inst, first_use inst) pairs.
    // Pass 2: perform all unlink/insert operations.
    typedef struct { Inst *konst; Inst *before; } SinkPair;
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        SinkPair pairs[64];
        int npairs = 0;

        // Pass 1: collect sinkable IK_CONST → first_use pairs
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (npairs >= 64) break;
            if (inst->is_dead || inst->kind != IK_CONST || !inst->dst ||
                inst->dst->phys_reg < 0)
                continue;

            int reg = inst->dst->phys_reg;
            Value *val = inst->dst;

            // Find first non-dead use of val in this block
            Inst *first_use = NULL;
            for (Inst *scan = inst->next; scan; scan = scan->next) {
                if (scan->is_dead) continue;
                for (int j = 0; j < scan->nops; j++) {
                    Value *v = scan->ops[j] ? val_resolve(scan->ops[j]) : NULL;
                    if (v == val) { first_use = scan; goto sink_found; }
                }
            }
            sink_found:

            if (!first_use || first_use == inst->next)
                continue;

            // Safety: no intermediate (non-dead) instruction may define or
            // read the same physical register.
            int safe = 1;
            for (Inst *scan = inst->next; scan && scan != first_use; scan = scan->next) {
                if (scan->is_dead) continue;
                if (scan->dst && scan->dst->phys_reg == reg) { safe = 0; break; }
                for (int j = 0; j < scan->nops; j++) {
                    Value *v = scan->ops[j] ? val_resolve(scan->ops[j]) : NULL;
                    if (v && v->kind == VAL_INST && v->phys_reg == reg)
                        { safe = 0; break; }
                }
                if (!safe) break;
            }
            if (!safe) continue;

            pairs[npairs++] = (SinkPair){inst, first_use};
        }

        // Pass 2: perform all sinks
        for (int i = 0; i < npairs; i++) {
            Inst *inst = pairs[i].konst;
            Inst *before = pairs[i].before;

            // Re-check: inst must still be before `before` in the list
            // (a prior sink in this batch may have moved things around)
            int still_before = 0;
            for (Inst *scan = inst->next; scan; scan = scan->next) {
                if (scan == before) { still_before = 1; break; }
            }
            if (!still_before || before == inst->next) continue;

            // Unlink from current position
            if (inst->prev) inst->prev->next = inst->next;
            else b->head = inst->next;
            if (inst->next) inst->next->prev = inst->prev;
            else b->tail = inst->prev;

            // Insert before target
            inst->prev = before->prev;
            inst->next = before;
            if (before->prev) before->prev->next = inst;
            else b->head = inst;
            before->prev = inst;
        }
    }
}

// Greedy trace-based block reordering to minimize explicit jumps.
//
// For each block, the "hot successor" becomes the next block:
//   - IK_JMP: follow target (if unplaced) — eliminates the j via P1.
//   - IK_BR: prefer the successor with higher loop_depth (loop body over
//            exit); on ties, prefer target2 (the fall-through path) so the
//            trailing `j target2` after a taken branch is omitted.
// When no successor is followable, start a new trace from the highest
// loop_depth unplaced block to keep hot bodies contiguous.
//
// This does not reorder instructions within a block, add branches, or
// touch live_in/live_out — it only permutes the f->blocks[] array. All
// downstream passes (detect_bitex_fusions, detect_branch_fusions, emit)
// read blocks from this array, so they automatically see the new layout.
static void reorder_blocks(Function *f) {
    if (f->nblocks <= 2) return;
    int n = f->nblocks;
    Block **new_order = arena_alloc(n * sizeof(Block *));
    uint8_t *placed = arena_alloc(n);
    for (int i = 0; i < n; i++) placed[i] = 0;

    int n_placed = 0;
    int cur_idx = 0;  // block 0 is the function entry; must come first

    while (n_placed < n) {
        Block *cur = f->blocks[cur_idx];
        new_order[n_placed++] = cur;
        placed[cur_idx] = 1;

        // Find effective terminator (skip dead tail instructions)
        Inst *term = cur->tail;
        while (term && term->is_dead) term = term->prev;

        Block *pref1 = NULL, *pref2 = NULL;
        if (term) {
            if (term->kind == IK_JMP && term->target) {
                pref1 = term->target;
            } else if (term->kind == IK_BR) {
                Block *t1 = term->target;
                Block *t2 = term->target2;
                int d1 = t1 ? t1->loop_depth : -1;
                int d2 = t2 ? t2->loop_depth : -1;
                if (d1 > d2)      { pref1 = t1; pref2 = t2; }
                else if (d2 > d1) { pref1 = t2; pref2 = t1; }
                else              { pref1 = t2; pref2 = t1; }  // tie: fall-through first
            }
        }

        int next_idx = -1;
        Block *cands[2] = { pref1, pref2 };
        for (int c = 0; c < 2 && next_idx < 0; c++) {
            if (!cands[c]) continue;
            for (int i = 0; i < n; i++) {
                if (f->blocks[i] == cands[c] && !placed[i]) {
                    next_idx = i;
                    break;
                }
            }
        }

        if (next_idx < 0) {
            int best = -1, best_d = -1;
            for (int i = 0; i < n; i++) {
                if (!placed[i] && f->blocks[i]->loop_depth > best_d) {
                    best_d = f->blocks[i]->loop_depth;
                    best = i;
                }
            }
            next_idx = best;
        }

        if (next_idx < 0) break;
        cur_idx = next_idx;
    }

    memcpy(f->blocks, new_order, n * sizeof(Block *));
}

// Pre-pass: mark IK_CONST instructions dead when their only consumer
// is an ALU op that a peephole will fold.
static void mark_dead_consts(Function *f) {
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead) continue;
            // P9: SEXT8/SEXT16 of constant → immw; source IK_CONST not needed
            if ((inst->kind == IK_SEXT8 || inst->kind == IK_SEXT16) && inst->nops >= 1) {
                Value *sv = inst->ops[0] ? val_resolve(inst->ops[0]) : NULL;
                if (is_ik_const(sv) && sv->use_count <= 1)
                    sv->def->is_dead = 1;
                continue;
            }
            if (inst->kind < IK_ADD || inst->kind > IK_FNE) continue;
            if (inst->nops < 2) continue;
            Value *o0 = inst->ops[0] ? val_resolve(inst->ops[0]) : NULL;
            Value *o1 = inst->ops[1] ? val_resolve(inst->ops[1]) : NULL;
            int ik0 = is_ik_const(o0);
            int ik1 = is_ik_const(o1);
            int is_k0 = (o0 && o0->kind == VAL_CONST) || ik0;
            int is_k1 = (o1 && o1->kind == VAL_CONST) || ik1;

            // P7: both operands constant — fold eliminates entire ALU op.
            // Only mark IK_CONST dead for operations P7 actually folds;
            // DIV/MOD/float ops fall through and need registers.
            if (is_k0 && is_k1) {
                switch (inst->kind) {
                case IK_ADD: case IK_SUB: case IK_MUL:
                case IK_AND: case IK_OR:  case IK_XOR:
                case IK_SHL: case IK_SHR: case IK_USHR:
                case IK_EQ:  case IK_NE:
                case IK_LT:  case IK_ULT:
                case IK_LE:  case IK_ULE:
                    if (ik0 && o0->use_count <= 1) o0->def->is_dead = 1;
                    if (ik1 && o1->use_count <= 1) o1->def->is_dead = 1;
                    break;
                default:
                    break;
                }
                continue;
            }
            // P8/P14: AND(x, k) → zxb/zxw/andi; mask IK_CONST is not needed
            // P8 fires for k == 0xFF/0xFFFF; P14 fires for k in [0,127].
            if (inst->kind == IK_AND && (is_k0 ^ is_k1)) {
                Value *kv = is_k1 ? o1 : o0;
                int mask = (kv->kind == VAL_CONST) ? kv->iconst : kv->def->imm;
                int is_ik = is_k1 ? ik1 : ik0;
                if (is_ik && kv->use_count <= 1 &&
                    ((mask >= 0 && mask <= 255) || mask == 0xffff))
                    kv->def->is_dead = 1;
            }
            // P11/P13: SHL/SHR/USHR with constant shift amount → shli/shrsi;
            // the IK_CONST (or AND chain) holding the shift amount is not needed.
            if ((inst->kind == IK_SHL || inst->kind == IK_SHR || inst->kind == IK_USHR) && !is_k0) {
                int shift_k;
                if (resolve_const(o1, &shift_k)) {
                    int k = shift_k & 31;
                    int will_fire = 0;
                    if (inst->kind == IK_SHL && k >= 0 && k <= 63) {
                        will_fire = 1;
                    } else if (k >= 0 && k <= 63) {
                        ValType dvt = inst->dst ? inst->dst->vtype : VT_I16;
                        int safe = (dvt == VT_I8 || dvt == VT_I16 || dvt == VT_I32);
                        if (!safe) safe = (dvt != VT_U32 && dvt != VT_I32);
                        will_fire = safe;
                    }
                    if (will_fire && o1 && o1->kind == VAL_INST && o1->def && o1->use_count <= 1) {
                        Inst *d = o1->def;
                        if (d->kind == IK_CONST && !d->fname)
                            d->is_dead = 1;
                        else if (d->kind == IK_AND && d->nops >= 2) {
                            d->is_dead = 1;
                            for (int j2 = 0; j2 < d->nops; j2++) {
                                Value *a = d->ops[j2] ? val_resolve(d->ops[j2]) : NULL;
                                if (is_ik_const(a) && a->use_count <= 1)
                                    a->def->is_dead = 1;
                            }
                        }
                    }
                }
            }
        }
    }
}

// P16: Detect SHR(x, k_shift) + AND(result, k_mask) pairs that can be fused
// into a single bitex instruction.  bitex rd, ry, imm9 computes
// rd = (ry >> shift) & ((1 << width) - 1) in one F0b instruction (3 bytes),
// replacing mov+shrsi+andi (6 bytes) or shrsi+andi (4 bytes).
// Table indexed by AND destination value id; g_bitex_src[id] = -1 means no fusion.

static int bitex_width(int mask) {
    // Check if mask is (1 << n) - 1 for n in 1..16.
    // Returns n (the extraction width), or 0 if not a valid bitex mask.
    if (mask <= 0) return 0;
    unsigned m = (unsigned)mask + 1;
    if ((m & (m - 1)) != 0) return 0;  // not a power of 2
    int n = __builtin_ctz(m);
    return (n >= 1 && n <= 16) ? n : 0;
}

static void detect_bitex_fusions(Function *f) {
    g_bitex_src = arena_alloc(f->nvalues * sizeof(int));
    g_bitex_imm = arena_alloc(f->nvalues * sizeof(int));
    for (int i = 0; i < f->nvalues; i++) g_bitex_src[i] = -1;

    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (inst->is_dead || inst->kind != IK_AND || inst->nops < 2 || !inst->dst)
                continue;

            Value *o0 = inst->ops[0] ? val_resolve(inst->ops[0]) : NULL;
            Value *o1 = inst->ops[1] ? val_resolve(inst->ops[1]) : NULL;

            // Identify which operand is the mask constant and which is the data
            int mask_val;
            Value *data_val;
            if (o1 && resolve_const(o1, &mask_val) && o0 && o0->kind == VAL_INST) {
                data_val = o0;
            } else if (o0 && resolve_const(o0, &mask_val) && o1 && o1->kind == VAL_INST) {
                data_val = o1;
            } else {
                continue;
            }

            int width = bitex_width(mask_val);
            if (!width) continue;

            // Try SHR+AND → bitex fusion (shift > 0): data must come from
            // IK_SHR/IK_USHR in the same block with constant shift amount.
            Inst *shr_def = data_val->def;
            if (shr_def &&
                (shr_def->kind == IK_SHR || shr_def->kind == IK_USHR) &&
                shr_def->nops >= 2 && shr_def->block == b &&
                data_val->use_count == 1) {
                int shift_val;
                if (resolve_const(shr_def->ops[1] ? val_resolve(shr_def->ops[1]) : NULL, &shift_val)) {
                    shift_val &= 31;
                    if (shr_def->kind == IK_SHR && shift_val + width > 32)
                        goto try_standalone;
                    Value *shr_src = shr_def->ops[0] ? val_resolve(shr_def->ops[0]) : NULL;
                    if (!shr_src || shr_src->kind != VAL_INST || shr_src->phys_reg < 0)
                        goto try_standalone;
                    int src_preg = shr_src->phys_reg;
                    int clobbered = 0;
                    for (Inst *scan = shr_def->next; scan && scan != inst; scan = scan->next) {
                        if (scan->is_dead) continue;
                        if (scan->dst && scan->dst->phys_reg == src_preg)
                            { clobbered = 1; break; }
                    }
                    if (clobbered) goto try_standalone;

                    // Record SHR+AND fusion
                    int width_code = width - 1;
                    int vid = inst->dst->id;
                    g_bitex_src[vid] = src_preg;
                    g_bitex_imm[vid] = shift_val | (width_code << 5);
                    shr_def->is_dead = 1;
                    // Mark SHR's shift constant dead if single use
                    Value *shift_op = shr_def->ops[1] ? val_resolve(shr_def->ops[1]) : NULL;
                    if (shift_op && shift_op->kind == VAL_INST && shift_op->def &&
                        shift_op->def->kind == IK_CONST && !shift_op->def->fname &&
                        shift_op->use_count <= 1)
                        shift_op->def->is_dead = 1;
                    // Mark AND's mask constant dead if single use
                    Value *mask_op = (data_val == o0) ? o1 : o0;
                    if (mask_op && mask_op->kind == VAL_INST && mask_op->def &&
                        mask_op->def->kind == IK_CONST && !mask_op->def->fname &&
                        mask_op->use_count <= 1)
                        mask_op->def->is_dead = 1;
                    continue;
                }
            }

            // Standalone AND → bitex with shift=0.  Only for masks not
            // already handled by cheaper peepholes:
            //   P14 andi:  masks 0–127 (2 bytes)
            //   P8  zxb:   mask 0xFF   (2 bytes)
            //   P8  zxw:   mask 0xFFFF (2 bytes)
            // bitex is 3 bytes, so only beneficial for masks 256–32767.
            try_standalone:
            if (mask_val <= 0xFF || mask_val == 0xFFFF) continue;
            if (data_val->phys_reg < 0) continue;
            {
                int width_code = width - 1;
                int vid = inst->dst->id;
                g_bitex_src[vid] = data_val->phys_reg;
                g_bitex_imm[vid] = width_code << 5;  // shift=0
                // Mark AND's mask value dead if single use.  The mask may
                // be an IK_CONST or an IK_AND(const, const) from legalize
                // TRUNC lowering.  Cascade to operands if they become unused.
                Value *mask_op = (data_val == o0) ? o1 : o0;
                if (mask_op && mask_op->kind == VAL_INST && mask_op->def &&
                    !mask_op->def->fname && mask_op->use_count <= 1) {
                    Inst *md = mask_op->def;
                    md->is_dead = 1;
                    // Cascade: mark single-use IK_CONST operands dead too
                    for (int oi = 0; oi < md->nops; oi++) {
                        Value *mop = md->ops[oi] ? val_resolve(md->ops[oi]) : NULL;
                        if (mop && mop->kind == VAL_INST && mop->def &&
                            mop->def->kind == IK_CONST && !mop->def->fname &&
                            mop->use_count <= 1)
                            mop->def->is_dead = 1;
                    }
                }
            }
        }
    }
}

// Check if a value is a known zero: VAL_CONST 0, IK_CONST 0, or
// SEXT8/SEXT16 of a zero source (e.g. sext16 of NULL pointer literal).
static int is_known_zero(Value *v) {
    if (!v) return 0;
    if (v->kind == VAL_CONST && v->iconst == 0) return 1;
    if (v->kind != VAL_INST || !v->def) return 0;
    Inst *d = v->def;
    if (d->kind == IK_CONST && !d->fname && d->imm == 0) return 1;
    if ((d->kind == IK_SEXT8 || d->kind == IK_SEXT16) && d->nops >= 1) {
        Value *src = d->ops[0] ? val_resolve(d->ops[0]) : NULL;
        if (src && src->kind == VAL_CONST && src->iconst == 0) return 1;
        if (src && src->kind == VAL_INST && src->def &&
            src->def->kind == IK_CONST && !src->def->fname && src->def->imm == 0)
            return 1;
    }
    return 0;
}

// Byte count for a CPU4 mnemonic. Returns 0 for unknown / non-instructions
// (labels, directives, blank lines). See docs/isa/cpu4.md.
static int cpu4_mnem_bytes(const char *m) {
    if (!m || !*m) return 0;
    // F0a (1 byte)
    if (!strcmp(m, "halt") || !strcmp(m, "ret")) return 1;
    // F1a (2 bytes): three-reg ALU + pseudo-ops
    static const char *const two[] = {
        "add","sub","mul","div","mod","shl","shr","lt","le","eq","ne",
        "and","or","xor","lts","les","divs","mods","shrs",
        "fadd","fsub","fmul","fdiv","flt","fle",
        "gt","ge","gts","ges","fgt","fge","mov",
        // F1b (2 bytes): single-reg
        "sxb","sxw","inc","dec","pushr","popr","zxb","zxw",
        "itof","ftoi","jlr","jr","ssp","putchar",
        // F2 (2 bytes): bp-relative load/store/addi
        "lb","lw","ll","sb","sw","sl","lbx","lwx",
        "addi","shli","andi","shrsi",
    };
    for (size_t i = 0; i < sizeof(two)/sizeof(two[0]); i++)
        if (!strcmp(m, two[i])) return 2;
    // Everything else (F0b, F0c, F3a-F3e): 3 bytes
    return 3;
}

// Parse an assembly buffer emitted for this function's body and compute
// per-block byte offsets. On entry block_start[] is a scratch array of
// length f->nblocks indexed by block id (must be ≥ max id + 1).
// Sets block_start[b->id] = cumulative byte offset at which that block
// begins. Block 0 has no label emitted (bi>0 guard), so block_start[
// f->blocks[0]->id ] = 0. Block labels look like "_<funcname>_B<id>:".
static void parse_block_offsets(Function *f, const char *buf, size_t len,
                                int *block_start /* indexed by block id */,
                                int *total_bytes /* optional */) {
    // Init all block starts to -1 (unreached). Block 0 starts at 0.
    int max_id = 0;
    for (int i = 0; i < f->nblocks; i++)
        if (f->blocks[i]->id > max_id) max_id = f->blocks[i]->id;
    for (int i = 0; i <= max_id; i++) block_start[i] = -1;
    block_start[f->blocks[0]->id] = 0;

    char prefix[64];
    int plen = snprintf(prefix, sizeof(prefix), "_%s_B", f->name);

    int cum = 0;
    const char *p = buf, *end = buf + len;
    while (p < end) {
        const char *eol = memchr(p, '\n', end - p);
        const char *lend = eol ? eol : end;
        // Skip leading whitespace
        const char *q = p;
        while (q < lend && (*q == ' ' || *q == '\t')) q++;
        if (q < lend && *q != ';') {
            // Detect block label: "_<funcname>_B<id>:"
            if ((lend - q) > plen && !memcmp(q, prefix, plen)) {
                const char *r = q + plen;
                int id = 0, any = 0;
                while (r < lend && *r >= '0' && *r <= '9') {
                    id = id * 10 + (*r - '0');
                    r++;
                    any = 1;
                }
                if (any && r < lend && *r == ':') {
                    if (id >= 0 && id <= max_id)
                        block_start[id] = cum;
                    goto next_line;
                }
            }
            // Generic label?  "<name>:" — skip (no byte cost, not our block)
            {
                const char *r = q;
                while (r < lend && (isalnum((unsigned char)*r) || *r == '_')) r++;
                if (r > q && r < lend && *r == ':') goto next_line;
            }
            // Instruction: count bytes by mnemonic (first word).
            {
                const char *mstart = q;
                const char *mend = q;
                while (mend < lend && (isalnum((unsigned char)*mend) || *mend == '_'))
                    mend++;
                size_t mlen = mend - mstart;
                if (mlen > 0 && mlen < 16) {
                    char mnem[16];
                    memcpy(mnem, mstart, mlen);
                    mnem[mlen] = 0;
                    cum += cpu4_mnem_bytes(mnem);
                }
            }
        }
    next_line:
        p = eol ? eol + 1 : end;
    }
    if (total_bytes) *total_bytes = cum;
}

// Return the byte count of block bi, given per-id block_start[] and the
// total byte count of the function body. Uses block layout order
// (f->blocks[]) to find the "next" block. For the last block, uses
// total_bytes as its end.
static int block_size_from_offsets(Function *f, int bi,
                                   const int *block_start, int total_bytes) {
    int start = block_start[f->blocks[bi]->id];
    if (start < 0) return 0;
    int end;
    if (bi + 1 < f->nblocks) {
        end = block_start[f->blocks[bi + 1]->id];
        if (end < 0) end = total_bytes;
    } else {
        end = total_bytes;
    }
    return end > start ? end - start : 0;
}

// Detect comparison+branch pairs that can be fused into F3b branches.
// block_size[] (may be NULL) gives exact byte sizes indexed by layout
// position (f->blocks[bi]). When non-NULL it replaces the coarse
// "3 bytes × instruction count" heuristic with measured bytes.
// fuse[] must be zeroed by the caller on the first call. On subsequent
// calls (fixpoint iteration), already-committed entries are preserved:
// a block with fuse[bi].fused != 0 is skipped. Any committed fusion
// only shrinks blocks (marks cmp as is_dead, compact terminator), so
// re-measuring after each call is monotonic.
static int detect_branch_fusions(Function *f, BranchFuse *fuse,
                                 const int *block_size) {
    int committed = 0;
    for (int fbi = 0; fbi < f->nblocks; fbi++) {
        if (fuse[fbi].fused != 0) continue;
        Block *fb  = f->blocks[fbi];
        Inst  *term = fb->tail;
        if (!term || term->kind != IK_BR || term->nops < 1) continue;
        Value *cond = val_resolve(term->ops[0]);
        if (!cond || cond->kind != VAL_INST) continue;
        // Find the defining instruction for cond within THIS block.
        Inst *def = NULL;
        Value *seek = cond;
        for (Inst *scan = term->prev; scan; scan = scan->prev) {
            if (scan->dst != seek) { if (!scan->is_dead) continue; else continue; }
            if (scan->is_dead && scan->kind == IK_COPY && scan->nops >= 1) {
                Value *src = val_resolve(scan->ops[0]);
                if (src && src->kind == VAL_INST) { seek = src; continue; }
            }
            if (scan->is_dead) continue;
            def = scan;
            break;
        }
        if (!def || def->nops < 2) continue;
        // Look through IK_COPY: v_phi = copy v_cmp; br v_phi
        if (def->kind == IK_COPY && def->nops >= 1) {
            Value *src = val_resolve(def->ops[0]);
            if (src && src->kind == VAL_INST) {
                for (Inst *scan = def->prev; scan; scan = scan->prev) {
                    if (scan->is_dead) continue;
                    if (scan->dst == src) {
                        if (scan->nops >= 2) def = scan;
                        break;
                    }
                }
            }
        }
        // Look through NE(SHL/MUL(cmp, k), 0) → use cmp for fusion
        Inst *chain_ne = NULL, *chain_shift = NULL;
        if (def->kind == IK_NE && def->nops >= 2) {
            Value *a = val_resolve(def->ops[0]);
            Value *b = val_resolve(def->ops[1]);
            Value *zero_side = NULL, *other_side = NULL;
            if (b && b->kind == VAL_CONST && b->iconst == 0) { zero_side = b; other_side = a; }
            else if (a && a->kind == VAL_CONST && a->iconst == 0) { zero_side = a; other_side = b; }
            (void)zero_side;
            if (other_side && other_side->kind == VAL_INST && other_side->def &&
                other_side->def->block == fb && !other_side->def->is_dead &&
                (other_side->def->kind == IK_SHL || other_side->def->kind == IK_MUL) &&
                other_side->def->nops >= 2) {
                Inst *shift = other_side->def;
                Value *sv0 = val_resolve(shift->ops[0]);
                Value *sv1 = val_resolve(shift->ops[1]);
                Value *inner = NULL;
                if (sv1 && sv1->kind == VAL_CONST && sv1->iconst != 0) inner = sv0;
                else if (sv0 && sv0->kind == VAL_CONST && sv0->iconst != 0) inner = sv1;
                if (inner && inner->kind == VAL_INST) {
                    for (Inst *scan = shift->prev; scan; scan = scan->prev) {
                        if (scan->is_dead) continue;
                        if (scan->dst == inner) {
                            if (scan->nops >= 2 && fused_branch_mnemonic(scan->kind)) {
                                chain_ne = def;
                                chain_shift = shift;
                                def = scan;
                            }
                            break;
                        }
                    }
                }
            }
        }
        const char *mnem = fused_branch_mnemonic(def->kind);
        if (!mnem) continue;

        // Fusion correctness invariant: every value that will be marked
        // is_dead by the paths below (P6 / P5 / P17 / P5+) must have no
        // live user other than the next link in the compare-branch chain.
        //
        // - P5 / P6 / P17 drop the comparison instruction; its dst register
        //   is never written, so any downstream user reads garbage.
        // - P5+ additionally overwrites cond->phys_reg with the materialised
        //   constant (it uses cond's register as a scratch), so even if the
        //   register would have been written by an earlier compare, P5+
        //   trashes it before the branch.
        //
        // use_count here is the post-DCE recount from alloc.c's dce_pass,
        // which walks operands via val_resolve — so it correctly includes
        // uses reached through alias chains (e.g. phis aliased to cond by
        // R2K's phi-select fold, or CSE'd boolean materialisations).
        {
            int safe = (cond->use_count <= 1);
            if (chain_shift && chain_shift->dst &&
                chain_shift->dst->use_count > 1) safe = 0;
            if (def->dst && def->dst != cond &&
                def->dst->use_count > 1) safe = 0;
            if (!safe) continue;
        }

        Value *dop0 = def->ops[0] ? val_resolve(def->ops[0]) : NULL;
        Value *dop1 = def->ops[1] ? val_resolve(def->ops[1]) : NULL;
        // Estimate byte distance to true target; skip if out of F3b range.
        int target_bi = -1;
        for (int k = 0; k < f->nblocks; k++) {
            if (f->blocks[k] == term->target) { target_bi = k; break; }
        }
        if (target_bi < 0) continue;
        int lo = (fbi < target_bi) ? fbi + 1 : target_bi;
        int hi = (fbi < target_bi) ? target_bi : fbi;
        int est_bytes = 0;
        if (block_size) {
            for (int k = lo; k < hi; k++) est_bytes += block_size[k];
        } else {
            for (int k = lo; k < hi; k++)
                for (Inst *ii = f->blocks[k]->head; ii; ii = ii->next)
                    if (!ii->is_dead) est_bytes += 3;
        }
        // F3c displacement is ±511 bytes (10-bit signed, PC-relative from
        // end of branch instruction). Use 500 when measured, 450 for the
        // conservative heuristic fallback.
        int p5_cap = block_size ? 500 : 450;
        int in_range = (est_bytes <= p5_cap);

        // P6: NE(x, 0) or EQ(x, 0) → jnz/jz directly (checked before P5
        // because P6 needs no scratch register and saves the immw entirely).
        // Must also recognise IK_CONST-materialised zeros (VAL_INST from
        // legalize Pass F), not just raw VAL_CONST.
        if ((def->kind == IK_NE || def->kind == IK_EQ) && def->nops >= 2) {
            int d0_zero = is_known_zero(dop0);
            int d1_zero = is_known_zero(dop1);
            Value *test_val = NULL;
            if (d1_zero && dop0 && dop0->kind == VAL_INST && dop0->phys_reg >= 0)
                test_val = dop0;
            else if (d0_zero && dop1 && dop1->kind == VAL_INST && dop1->phys_reg >= 0)
                test_val = dop1;
            if (test_val) {
                def->is_dead     = 1;
                // Mark the zero-producing instruction dead if it has no
                // other live uses (saves the immw instruction).
                Value *zero_v = (test_val == dop0) ? dop1 : dop0;
                if (zero_v && zero_v->kind == VAL_INST && zero_v->def &&
                    zero_v->use_count <= 1)
                    zero_v->def->is_dead = 1;
                fuse[fbi].fused  = 3;
                fuse[fbi].p0     = test_val->phys_reg;
                fuse[fbi].is_eq  = (def->kind == IK_EQ);
                committed++;
                continue;
            }
        }

        // P17: cbeq/cbne — EQ/NE with 7-bit unsigned const, short-range.
        // Checked before P5 because when a constant has been materialised to
        // an IK_CONST (by CSE or LICM const-hoisting), both operands are
        // VAL_INST with phys_reg and P5 would otherwise fire first, giving
        // `immw rX, K; beq rY, rX, T` (6 bytes) instead of `cbeq rY, K, T`
        // (3 bytes). resolve_const peeks through IK_CONST to catch those.
        if ((def->kind == IK_EQ || def->kind == IK_NE) && def->nops >= 2) {
            Value *cb_reg = NULL;
            int cb_k = 0, tmp = 0;
            if (dop0 && dop0->kind == VAL_INST && dop0->phys_reg >= 0 &&
                resolve_const(dop1, &tmp) && tmp >= 0 && tmp <= 127)
                { cb_reg = dop0; cb_k = tmp; }
            else if (dop1 && dop1->kind == VAL_INST && dop1->phys_reg >= 0 &&
                     resolve_const(dop0, &tmp) && tmp >= 0 && tmp <= 127)
                { cb_reg = dop1; cb_k = tmp; }
            if (cb_reg) {
                // Determine which target we'll actually branch to
                Block *next_det = (fbi + 1 < f->nblocks) ? f->blocks[fbi + 1] : NULL;
                int will_inv = (term->target == next_det && term->target2 != NULL);
                Block *cb_tgt = will_inv ? term->target2 : term->target;
                int cb_tbi = -1;
                for (int k = 0; k < f->nblocks; k++)
                    if (f->blocks[k] == cb_tgt) { cb_tbi = k; break; }
                if (cb_tbi >= 0) {
                    int cb_lo = (fbi < cb_tbi) ? fbi + 1 : cb_tbi;
                    int cb_hi = (fbi < cb_tbi) ? cb_tbi : fbi;
                    int cb_est = 0;
                    if (block_size) {
                        for (int k = cb_lo; k < cb_hi; k++) cb_est += block_size[k];
                    } else {
                        for (int k = cb_lo; k < cb_hi; k++)
                            for (Inst *ii = f->blocks[k]->head; ii; ii = ii->next)
                                if (!ii->is_dead) cb_est += 3;
                    }
                    // F0c displacement is ±511 bytes (10-bit signed,
                    // PC-relative). 500 when measured, 500 for the
                    // conservative heuristic fallback.
                    int p17_cap = block_size ? 500 : 500;
                    if (cb_est <= p17_cap) {
                        def->is_dead        = 1;
                        fuse[fbi].fused     = 4;
                        fuse[fbi].p0        = cb_reg->phys_reg;
                        fuse[fbi].const_val = cb_k;
                        fuse[fbi].is_eq     = (def->kind == IK_EQ);
                        committed++;
                        continue;
                    }
                }
            }
        }

        // P5: both operands are registers
        if (in_range &&
            dop0 && dop0->kind == VAL_INST && dop0->phys_reg >= 0 &&
            dop1 && dop1->kind == VAL_INST && dop1->phys_reg >= 0) {
            def->is_dead     = 1;
            if (chain_ne)    chain_ne->is_dead = 1;
            if (chain_shift) chain_shift->is_dead = 1;
            fuse[fbi].fused  = 1;
            fuse[fbi].mnem   = mnem;
            fuse[fbi].p0     = dop0->phys_reg;
            fuse[fbi].p1     = dop1->phys_reg;
            committed++;
            continue;
        }

        // P5+: one operand is VAL_CONST, use cond's register as scratch
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
                    if (chain_ne)    chain_ne->is_dead = 1;
                    if (chain_shift) chain_shift->is_dead = 1;
                    if (looked_through) looked_through->is_dead = 1;
                    fuse[fbi].fused     = 2;
                    fuse[fbi].mnem      = mnem;
                    fuse[fbi].p0        = rp;
                    fuse[fbi].p1        = sc;
                    fuse[fbi].const_val = const_op->iconst;
                    fuse[fbi].swap      = const_is_lhs;
                    committed++;
                    continue;
                }
            }
        }
    }
    return committed;
}

// Emit a rotated branch (P12): duplicate header's IK_BR at the end of a latch block.
static int emit_rotated_branch(Function *f, FILE *out, Inst *inst,
                               BranchFuse *fuse, Block *next_blk, int bi) {
    Block *hdr = inst->target;
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
    if (!hdr_clean || !hdr_br || hdr_br->nops < 1) return 0;
    Value *cond = val_resolve(hdr_br->ops[0]);
    if (!cond || cond->kind != VAL_INST || cond->phys_reg < 0) return 0;

    int hbi = -1;
    for (int hi = 0; hi < f->nblocks; hi++) {
        if (f->blocks[hi] == hdr) { hbi = hi; break; }
    }
    if (hbi < 0) return 0;

    int rotated = 0;
    if (fuse[hbi].fused == 0) {
        if (hdr_br->target)
            fprintf(out, "    jnz %s, _%s_B%d\n",
                    regname(cond->phys_reg), f->name, hdr_br->target->id);
        rotated = 1;
    } else if (fuse[hbi].fused == 1) {
        if (hdr_br->target)
            fprintf(out, "    %s %s, %s, _%s_B%d\n",
                    fuse[hbi].mnem,
                    regname(fuse[hbi].p0), regname(fuse[hbi].p1),
                    f->name, hdr_br->target->id);
        rotated = 1;
    } else if (fuse[hbi].fused == 2) {
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
    } else if (fuse[hbi].fused == 3) {
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
    }
    return rotated;
}

// ============================================================
// emit_function — main entry point
// ============================================================

#define MAX_JT 16

// Emit the function body (per-block instructions + jump tables) to `out`.
// Invoked twice: once to a memstream for exact byte-size measurement, once
// to the real output after branch fusions are committed.
static void emit_function_body(Function *f, FILE *out, BranchFuse *fuse,
                               uint8_t *blk_live_regs,
                               int callee_frame, int callee_save[4],
                               int frame, int ann_live) {
    struct { Inst *inst; int mn, mx; } jt_info[MAX_JT];
    int jt_count = 0;
    int ann_prev_line = 0;

    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        g_blk_live_out_regs = blk_live_regs[bi];

        // Per-instruction live register mask (backward dataflow)
        uint8_t *inst_live = NULL;
        if (ann_live) {
            int ninst = 0;
            for (Inst *p = b->head; p; p = p->next) ninst++;
            inst_live = arena_alloc(ninst * sizeof(uint8_t));
            uint8_t live = blk_live_regs[bi];
            int idx = ninst;
            for (Inst *p = b->tail; p; p = p->prev) {
                inst_live[--idx] = live;
                if (!p->is_dead && p->dst && p->dst->phys_reg >= 0 && p->dst->phys_reg < 8)
                    live &= ~(1u << p->dst->phys_reg);
                if (!p->is_dead) {
                    for (int i = 0; i < p->nops; i++) {
                        Value *v = val_resolve(p->ops[i]);
                        if (v && v->kind == VAL_INST && v->phys_reg >= 0 && v->phys_reg < 8)
                            live |= (1u << v->phys_reg);
                    }
                }
            }
        }
        int inst_idx = 0;

        if (bi > 0)
            fprintf(out, "_%s_B%d:\n", f->name, b->id);
        FILE *real_out = out;
        for (Inst *inst = b->head; inst; inst = inst->next) {
            if (flag_annotate && inst->line && inst->line != ann_prev_line) {
                emit_src_comment(inst->line, real_out);
                ann_prev_line = inst->line;
            }
            // Redirect output to buffer so we can append live annotation
            char *membuf = NULL; size_t memlen = 0;
            FILE *memf = NULL;
            if (inst_live && !inst->is_dead) {
                memf = open_memstream(&membuf, &memlen);
                out = memf;
            }
            if (inst->kind == IK_RET && callee_frame > 0) {
                // Full RET sequence: move retval to r0, restore callee saves, ret
                if (inst->nops > 0) {
                    Value *rv = val_resolve(inst->ops[0]);
                    if (rv && rv->kind == VAL_CONST) {
                        emit_imm(out, 0, rv->iconst);
                    } else if (rv && rv->phys_reg >= 0 && rv->phys_reg != 0) {
                        fprintf(out, "    or r0, %s, %s\n",
                                regname(rv->phys_reg), regname(rv->phys_reg));
                    }
                }
                int tmp_frame = 0;
                for (int r = 4; r <= 7; r++) {
                    if (callee_save[r - 4]) {
                        tmp_frame += 4;
                        fprintf(out, "    ll r%d, %d\n", r, -(frame + tmp_frame) / 4);
                    }
                }
                fprintf(out, "    ret\n");
            } else if (inst->kind == IK_BR && !inst->is_dead && inst->nops >= 1) {
                Block *next_blk = (bi + 1 < f->nblocks) ? f->blocks[bi + 1] : NULL;
                int inverted = 0;
                if (fuse[bi].fused == 1) {
                    if (inst->target)
                        fprintf(out, "    %s %s, %s, _%s_B%d\n",
                                fuse[bi].mnem,
                                regname(fuse[bi].p0), regname(fuse[bi].p1),
                                f->name, inst->target->id);
                } else if (fuse[bi].fused == 2) {
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
                    if (inst->target == next_blk && inst->target2) {
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
                } else if (fuse[bi].fused == 4) {
                    if (inst->target == next_blk && inst->target2) {
                        fprintf(out, "    %s %s, %d, _%s_B%d\n",
                                fuse[bi].is_eq ? "cbne" : "cbeq",
                                regname(fuse[bi].p0), fuse[bi].const_val,
                                f->name, inst->target2->id);
                        inverted = 1;
                    } else if (inst->target) {
                        fprintf(out, "    %s %s, %d, _%s_B%d\n",
                                fuse[bi].is_eq ? "cbeq" : "cbne",
                                regname(fuse[bi].p0), fuse[bi].const_val,
                                f->name, inst->target->id);
                    }
                } else {
                    int r1 = get_val_reg(out, inst->ops[0], 0);
                    if (inst->target == next_blk && inst->target2) {
                        fprintf(out, "    jz %s, _%s_B%d\n",
                                regname(r1), f->name, inst->target2->id);
                        inverted = 1;
                    } else if (inst->target) {
                        fprintf(out, "    jnz %s, _%s_B%d\n",
                                regname(r1), f->name, inst->target->id);
                    }
                }
                if (!inverted && inst->target2 && inst->target2 != next_blk)
                    fprintf(out, "    j _%s_B%d\n", f->name, inst->target2->id);
            } else if (inst->kind == IK_JMP && inst->target &&
                       inst->target == ((bi + 1 < f->nblocks) ? f->blocks[bi + 1] : NULL)) {
                // P1: omit unconditional j to the immediately following block
            } else if (inst->kind == IK_JMP && inst->target) {
                // P12: loop rotation
                Block *next_blk = (bi + 1 < f->nblocks) ? f->blocks[bi + 1] : NULL;
                if (!emit_rotated_branch(f, out, inst, fuse, next_blk, bi))
                    emit_inst(inst, out);
            } else if (inst->kind == IK_SWITCH && inst->nops >= 1) {
                int sel_r = get_val_reg(out, inst->ops[0], 0);
                int scr = (sel_r == 0) ? 1 : 0;
                int nc = inst->switch_ncase;
                int mn = inst->switch_vals[0], mx = inst->switch_vals[0];
                for (int i = 1; i < nc; i++) {
                    if (inst->switch_vals[i] < mn) mn = inst->switch_vals[i];
                    if (inst->switch_vals[i] > mx) mx = inst->switch_vals[i];
                }
                int def_id = inst->switch_default->id;
                fprintf(out, "    pushr %s\n", regname(sel_r));
                emit_imm(out, scr, mn);
                fprintf(out, "    blts %s, %s, _%s_sw%d\n",
                        regname(sel_r), regname(scr), f->name, jt_count);
                emit_imm(out, scr, mx);
                fprintf(out, "    blts %s, %s, _%s_sw%d\n",
                        regname(scr), regname(sel_r), f->name, jt_count);
                if (mn == 0) {
                    fprintf(out, "    add %s, %s, %s\n",
                            regname(scr), regname(sel_r), regname(sel_r));
                } else {
                    emit_imm(out, scr, mn);
                    fprintf(out, "    sub %s, %s, %s\n",
                            regname(scr), regname(sel_r), regname(scr));
                    fprintf(out, "    shli %s, 1\n", regname(scr));
                }
                fprintf(out, "    immw %s, _%s_jt%d\n", regname(sel_r), f->name, jt_count);
                fprintf(out, "    add %s, %s, %s\n",
                        regname(scr), regname(sel_r), regname(scr));
                fprintf(out, "    llw %s, %s, 0\n", regname(scr), regname(scr));
                fprintf(out, "    popr %s\n", regname(sel_r));
                fprintf(out, "    jr %s\n", regname(scr));
                fprintf(out, "_%s_sw%d:\n", f->name, jt_count);
                fprintf(out, "    popr %s\n", regname(sel_r));
                fprintf(out, "    j _%s_B%d\n", f->name, def_id);
                if (jt_count < MAX_JT) {
                    jt_info[jt_count].inst = inst;
                    jt_info[jt_count].mn = mn;
                    jt_info[jt_count].mx = mx;
                }
                jt_count++;
            } else {
                emit_inst(inst, out);
            }
            if (memf) {
                fclose(memf);
                out = real_out;
                if (memlen > 0) {
                    uint8_t m = inst_live[inst_idx];
                    char ann[32];
                    snprintf(ann, sizeof(ann), " ; live:%c%c%c%c%c%c%c%c",
                        (m&0x80)?'1':'0', (m&0x40)?'1':'0', (m&0x20)?'1':'0', (m&0x10)?'1':'0',
                        (m&0x08)?'1':'0', (m&0x04)?'1':'0', (m&0x02)?'1':'0', (m&0x01)?'1':'0');
                    // Find last newline, insert annotation at fixed column
                    char *last_nl = NULL;
                    for (size_t i = memlen; i > 0; i--)
                        if (membuf[i-1] == '\n') { last_nl = &membuf[i-1]; break; }
                    if (last_nl) {
                        // Measure columns on the last line (from prev newline or start)
                        char *line_start = membuf;
                        for (char *p = last_nl - 1; p >= membuf; p--)
                            if (*p == '\n') { line_start = p + 1; break; }
                        int col = (int)(last_nl - line_start);
                        fwrite(membuf, 1, last_nl - membuf, out);
                        for (int i = col; i < 40; i++) fputc(' ', out);
                        fputs(ann, out);
                        fwrite(last_nl, 1, memlen - (last_nl - membuf), out);
                    } else {
                        fwrite(membuf, 1, memlen, out);
                        fputs(ann, out);
                        fputc('\n', out);
                    }
                }
                free(membuf);
            }
            inst_idx++;
        }
    }

    // Emit jump tables after function body
    for (int ti = 0; ti < jt_count && ti < MAX_JT; ti++) {
        Inst *sw = jt_info[ti].inst;
        int mn = jt_info[ti].mn, mx = jt_info[ti].mx;
        int def_id = sw->switch_default->id;
        fprintf(out, "    align\n");
        fprintf(out, "_%s_jt%d:\n", f->name, ti);
        for (int v = mn; v <= mx; v++) {
            int found = 0;
            for (int ci = 0; ci < sw->switch_ncase; ci++) {
                if (sw->switch_vals[ci] == v) {
                    fprintf(out, "    word _%s_B%d\n", f->name, sw->switch_targets[ci]->id);
                    found = 1;
                    break;
                }
            }
            if (!found)
                fprintf(out, "    word _%s_B%d\n", f->name, def_id);
        }
    }
}

void emit_function(Function *f, FILE *out) {
    if (!f || f->nblocks == 0) return;

    if (getenv("SMALLCC_DEBUG_IR")) {
        fprintf(stderr, "=== IR dump for %s ===\n", f->name);
        extern void print_function(Function *, FILE *);
        print_function(f, stderr);
    }

    g_cur_func_name = f->name;
    fprintf(out, "    align\n");
    fprintf(out, "%s:\n", f->name);

    int frame = f->frame_size;
    if (frame % 4) frame += (4 - frame % 4);

    int callee_save[4] = {0, 0, 0, 0};
    int callee_frame = emit_prologue(f, out, frame, callee_save);

    mark_dead_consts(f);
    remap_single_use_values(f);
    sink_consts(f);
    reorder_blocks(f);
    detect_bitex_fusions(f);

    // Precompute per-block physical-register live_out masks
    uint8_t *blk_live_regs = arena_alloc(f->nblocks * sizeof(uint8_t));
    for (int bi = 0; bi < f->nblocks; bi++) {
        Block *b = f->blocks[bi];
        uint8_t mask = 0;
        if (b->live_out) {
            for (int w = 0; w < b->nwords; w++) {
                uint32_t bits = b->live_out[w];
                while (bits) {
                    int bit = __builtin_ctz(bits);
                    int vid = w * 32 + bit;
                    if (vid < f->nvalues) {
                        int pr = f->values[vid]->phys_reg;
                        if (pr >= 0 && pr < 8)
                            mask |= (1u << pr);
                    }
                    bits &= bits - 1;
                }
            }
        }
        blk_live_regs[bi] = mask;
    }

    BranchFuse *fuse = arena_alloc(f->nblocks * sizeof(BranchFuse));
    memset(fuse, 0, f->nblocks * sizeof(BranchFuse));

    // Two-pass branch-fusion range check with fixpoint iteration:
    //   1. Dry-run body emission using current fuse[] state. This gives an
    //      upper bound on real block sizes (uncommitted range-sensitive
    //      fusions emit the unfused 8-byte cmp+jnz+j sequence).
    //   2. Parse block-start offsets and derive per-block byte sizes.
    //   3. Call detect_branch_fusions with exact sizes. It skips blocks
    //      already committed (fuse[bi].fused != 0) and only adds new fusions.
    //   4. Repeat until no new commits: each commit shrinks its block, which
    //      may bring previously-out-of-range candidates into range.
    int max_id = 0;
    for (int i = 0; i < f->nblocks; i++)
        if (f->blocks[i]->id > max_id) max_id = f->blocks[i]->id;
    int *block_start = arena_alloc((max_id + 1) * sizeof(int));
    int *block_size  = arena_alloc(f->nblocks * sizeof(int));

    int committed;
    do {
        char *mbuf = NULL; size_t mlen = 0;
        FILE *mf = open_memstream(&mbuf, &mlen);
        emit_function_body(f, mf, fuse, blk_live_regs, callee_frame,
                           callee_save, frame, /*ann_live=*/0);
        fflush(mf);
        fclose(mf);
        int total_bytes = 0;
        parse_block_offsets(f, mbuf, mlen, block_start, &total_bytes);
        free(mbuf);
        for (int bi = 0; bi < f->nblocks; bi++)
            block_size[bi] = block_size_from_offsets(f, bi, block_start, total_bytes);
        committed = detect_branch_fusions(f, fuse, block_size);
    } while (committed > 0);

    // Real emission pass.
    int ann_live = (getenv("LIVE_REGS") != NULL);
    emit_function_body(f, out, fuse, blk_live_regs, callee_frame,
                       callee_save, frame, ann_live);
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
