/*
 * braun.c — Braun et al. 2013 SSA construction from Sexp AST
 *
 * Algorithm:
 *   writeVariable(block, name, value)
 *   readVariable(block, name) → value
 *   readVariableRecursive(block, name) → value
 *   addPhiOperands(name, phi_inst) → value
 *   tryRemoveTrivialPhi(phi_inst) → value
 *   sealBlock(block)
 *
 * Sexp nodes handled:
 *   (int v), (flt bits), (var "x"), (addr "x"), (gaddr "name")
 *   (load size ptr), (store size ptr val), (assign "x" val)
 *   (binop "op" lhs rhs), (unop "op" e), (cast e)
 *   (call "f" args...), (icall fp args...), (putchar e)
 *   (if cond then else) — ternary
 *   (seq stmt... expr) — side effects then value
 *   (memcpy dst src size_int)
 *   Statements: block, expr, if, while, for, do, return, break, continue,
 *               goto, label, switch, case, default, skip, va_start
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "braun.h"
#include "ssa.h"
#include "sx.h"
#include "smallcc.h"

// ============================================================
// BraunCtx
// ============================================================

typedef struct {
    Function *f;
    TypeMap  *tm;
    // Named label blocks (for goto/label)
    char    **label_names;
    Block   **label_blocks;
    int       nlabels;
    int       label_cap;
    // Current break/continue targets
    Block    *brk;
    Block    *cont;
} BraunCtx;

static Block *get_label_block(BraunCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->nlabels; i++)
        if (strcmp(ctx->label_names[i], name) == 0)
            return ctx->label_blocks[i];
    // Create new
    if (ctx->nlabels >= ctx->label_cap) {
        ctx->label_cap = ctx->label_cap ? ctx->label_cap * 2 : 8;
        ctx->label_names  = realloc(ctx->label_names,  ctx->label_cap * sizeof(char *));
        ctx->label_blocks = realloc(ctx->label_blocks, ctx->label_cap * sizeof(Block *));
    }
    Block *b = new_block(ctx->f);
    b->label = arena_strdup(name);
    ctx->label_names [ctx->nlabels] = (char *)b->label;
    ctx->label_blocks[ctx->nlabels] = b;
    ctx->nlabels++;
    return b;
}

// ============================================================
// Variable maps (Braun algorithm)
// ============================================================

static void write_var(Block *b, const char *name, Value *v) {
    // Linear search in block's def map
    for (int i = 0; i < b->ndef; i++) {
        if (strcmp(b->def_names[i], name) == 0) {
            b->def_vals[i] = v;
            return;
        }
    }
    if (b->ndef >= BRAUN_MAP_MAX) return; // should not happen in practice
    b->def_names[b->ndef] = (char *)name;
    b->def_vals [b->ndef] = v;
    b->ndef++;
}

static Value *read_var(BraunCtx *ctx, Block *b, const char *name);
static Value *read_var_recursive(BraunCtx *ctx, Block *b, const char *name);
static Value *try_remove_trivial_phi(BraunCtx *ctx, Inst *phi);
static Value *add_phi_operands(BraunCtx *ctx, const char *name, Inst *phi);

static Value *read_var(BraunCtx *ctx, Block *b, const char *name) {
    // Check block-local definition
    for (int i = 0; i < b->ndef; i++)
        if (strcmp(b->def_names[i], name) == 0)
            return val_resolve(b->def_vals[i]);
    return read_var_recursive(ctx, b, name);
}

static Value *read_var_recursive(BraunCtx *ctx, Block *b, const char *name) {
    Value *v;
    if (!b->sealed) {
        // Incomplete phi: block doesn't have all predecessors yet
        Value *dst = new_value(ctx->f, VAL_INST, VT_I16);
        Inst  *phi = new_inst(ctx->f, b, IK_PHI, dst);
        inst_append(b, phi);
        write_var(b, name, dst);
        if (b->niphi >= BRAUN_MAP_MAX) return dst; // shouldn't happen
        b->iphi_insts[b->niphi] = phi;
        b->iphi_names[b->niphi] = (char *)name;
        b->niphi++;
        return dst;
    }
    if (b->npreds == 0) {
        // Entry block or disconnected: treat as zero (matches zero-initialized stack/memory).
        // Returning VAL_UNDEF would leave the register holding a stale value from prior
        // computations, causing subtle bugs when C code uses uninitialized local variables.
        Value *u = new_value(ctx->f, VAL_CONST, VT_I16);
        u->iconst = 0;
        write_var(b, name, u);
        return u;
    }
    if (b->npreds == 1) {
        v = read_var(ctx, b->preds[0], name);
        write_var(b, name, v);
        return v;
    }
    // Multiple predecessors: place phi
    Value *dst = new_value(ctx->f, VAL_INST, VT_I16);
    Inst  *phi = new_inst(ctx->f, b, IK_PHI, dst);
    // Insert phi at front of block
    phi->next   = b->head;
    phi->prev   = NULL;
    if (b->head) b->head->prev = phi;
    else         b->tail = phi;
    b->head = phi;
    phi->block = b;
    write_var(b, name, dst);
    v = add_phi_operands(ctx, name, phi);
    write_var(b, name, v);
    return v;
}

static Value *add_phi_operands(BraunCtx *ctx, const char *name, Inst *phi) {
    Block *b = phi->block;
    for (int i = 0; i < b->npreds; i++) {
        Value *pv = read_var(ctx, b->preds[i], name);
        inst_add_op(phi, pv);
    }
    return try_remove_trivial_phi(ctx, phi);
}

static Value *try_remove_trivial_phi(BraunCtx *ctx, Inst *phi) {
    Value *same = NULL;
    for (int i = 0; i < phi->nops; i++) {
        Value *op = val_resolve(phi->ops[i]);
        if (op == phi->dst || op == same) continue;
        if (same) return phi->dst; // non-trivial
        same = op;
    }
    if (!same) {
        // All operands are the phi itself or undef
        same = new_value(ctx->f, VAL_UNDEF, phi->dst->vtype);
    }
    phi->dst->alias = same;
    phi->is_dead    = 1;
    return same;
}

static void seal_block(BraunCtx *ctx, Block *b) {
    // Fill in incomplete phis
    for (int i = 0; i < b->niphi; i++) {
        add_phi_operands(ctx, b->iphi_names[i], b->iphi_insts[i]);
    }
    b->niphi = 0;
    b->sealed = 1;
}

// ============================================================
// Expression code generation
// ============================================================

// Forward declarations
static Value *cg_expr(BraunCtx *ctx, Block **cur, Sx *node);
static Block *cg_stmt(BraunCtx *ctx, Block *b, Sx *node);

// Helper: look up ValType for an sexp node from TypeMap
static ValType vt_of(BraunCtx *ctx, Sx *node) {
    if (!node) return VT_I16;
    ValType vt = typemap_vtype(ctx->tm, node->id);
    return vt != VT_VOID ? vt : VT_I16;
}

// Helper: map sexp binop string → InstKind (signed/unsigned from vtype)
static InstKind binop_kind(const char *op, ValType vt) {
    bool is_unsigned = (vt == VT_U8 || vt == VT_U16 || vt == VT_U32 || vt == VT_PTR);
    bool is_float    = (vt == VT_F32);
    if (is_float) {
        if (!strcmp(op,"+")) return IK_FADD;
        if (!strcmp(op,"-")) return IK_FSUB;
        if (!strcmp(op,"*")) return IK_FMUL;
        if (!strcmp(op,"/")) return IK_FDIV;
        if (!strcmp(op,"<")) return IK_FLT;
        if (!strcmp(op,"<=")) return IK_FLE;
        if (!strcmp(op,"==")) return IK_FEQ;
        if (!strcmp(op,"!=")) return IK_FNE;
        if (!strcmp(op,">")) { /* swap operands */ return IK_FLT; }
        if (!strcmp(op,">=")) { return IK_FLE; }
    }
    if (!strcmp(op,"+"))  return IK_ADD;
    if (!strcmp(op,"-"))  return IK_SUB;
    if (!strcmp(op,"*"))  return IK_MUL;
    if (!strcmp(op,"/"))  return is_unsigned ? IK_UDIV : IK_DIV;
    if (!strcmp(op,"%"))  return is_unsigned ? IK_UMOD : IK_MOD;
    if (!strcmp(op,"&"))  return IK_AND;
    if (!strcmp(op,"|"))  return IK_OR;
    if (!strcmp(op,"^"))  return IK_XOR;
    if (!strcmp(op,"<<")) return IK_SHL;
    if (!strcmp(op,">>")) return is_unsigned ? IK_USHR : IK_SHR;
    if (!strcmp(op,"==")) return IK_EQ;
    if (!strcmp(op,"!=")) return IK_NE;
    if (!strcmp(op,"<"))  return is_unsigned ? IK_ULT : IK_LT;
    if (!strcmp(op,"<=")) return is_unsigned ? IK_ULE : IK_LE;
    if (!strcmp(op,">"))  return is_unsigned ? IK_ULT : IK_LT;  // operands swapped by caller
    if (!strcmp(op,">=")) return is_unsigned ? IK_ULE : IK_LE;  // operands swapped by caller
    if (!strcmp(op,"&&") || !strcmp(op,"||")) return IK_AND;  // handled separately
    return IK_ADD;
}

// Emit a binary instruction
// Constant-fold helper: fold binary op on two integer constants
static int can_fold_binop(InstKind kind) {
    switch (kind) {
    case IK_ADD: case IK_SUB: case IK_MUL:
    case IK_AND: case IK_OR:  case IK_XOR:
    case IK_SHL: case IK_SHR: case IK_USHR:
    case IK_EQ:  case IK_NE:
    case IK_LT:  case IK_ULT:
    case IK_LE:  case IK_ULE:
        return 1;
    default: return 0;
    }
}

static int32_t fold_binop(InstKind kind, int32_t a, int32_t b) {
    uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
    switch (kind) {
    case IK_ADD:  return a + b;
    case IK_SUB:  return a - b;
    case IK_MUL:  return a * b;
    case IK_AND:  return a & b;
    case IK_OR:   return a | b;
    case IK_XOR:  return a ^ b;
    case IK_SHL:  return ua << (ub & 31);
    case IK_SHR:  return ua >> (ub & 31);
    case IK_USHR: return ua >> (ub & 31);
    case IK_EQ:   return a == b ? 1 : 0;
    case IK_NE:   return a != b ? 1 : 0;
    case IK_LT:   return (int32_t)a < (int32_t)b ? 1 : 0;
    case IK_ULT:  return ua < ub ? 1 : 0;
    case IK_LE:   return (int32_t)a <= (int32_t)b ? 1 : 0;
    case IK_ULE:  return ua <= ub ? 1 : 0;
    default: return 0;
    }
}

static Value *emit_binop(BraunCtx *ctx, Block *b, InstKind kind, Value *lhs, Value *rhs, ValType vt) {
    // Constant fold when both operands are compile-time constants
    if (lhs && lhs->kind == VAL_CONST && rhs && rhs->kind == VAL_CONST
        && can_fold_binop(kind)) {
        int32_t result = fold_binop(kind, lhs->iconst, rhs->iconst);
        return new_const(ctx->f, result, vt);
    }
    Value *dst  = new_value(ctx->f, VAL_INST, vt);
    Inst  *inst = new_inst(ctx->f, b, kind, dst);
    inst_add_op(inst, lhs);
    inst_add_op(inst, rhs);
    inst_append(b, inst);
    return dst;
}

static Value *emit_unop(BraunCtx *ctx, Block *b, InstKind kind, Value *v, ValType vt) {
    // Constant fold unary ops on constants
    if (v && v->kind == VAL_CONST) {
        if (kind == IK_NEG) return new_const(ctx->f, -v->iconst, vt);
        if (kind == IK_NOT) return new_const(ctx->f, !v->iconst, vt);
    }
    Value *dst  = new_value(ctx->f, VAL_INST, vt);
    Inst  *inst = new_inst(ctx->f, b, kind, dst);
    inst_add_op(inst, v);
    inst_append(b, inst);
    return dst;
}

// Emit a load
static Value *emit_load(BraunCtx *ctx, Block *b, Value *ptr, int size, int offset, ValType vt) {
    Value *dst  = new_value(ctx->f, VAL_INST, vt);
    Inst  *inst = new_inst(ctx->f, b, IK_LOAD, dst);
    inst_add_op(inst, ptr);
    inst->imm  = offset;
    inst->size = size;
    inst_append(b, inst);
    return dst;
}

// Emit a store
static void emit_store(BraunCtx *ctx, Block *b, Value *ptr, Value *val, int size, int offset) {
    // Materialize constant value into a fresh register so the allocator can
    // assign it a non-conflicting physical register (avoids clobbering live regs).
    if (val && val->kind == VAL_CONST) {
        ValType vt = val->vtype != VT_VOID ? val->vtype : VT_I16;
        Value *mat = new_value(ctx->f, VAL_INST, vt);
        Inst  *ci  = new_inst(ctx->f, b, IK_CONST, mat);
        ci->imm = val->iconst;
        inst_append(b, ci);
        val = mat;
    }
    Inst *inst = new_inst(ctx->f, b, IK_STORE, NULL);
    inst_add_op(inst, ptr);
    inst_add_op(inst, val);
    inst->imm  = offset;
    inst->size = size;
    inst_append(b, inst);
}

// Emit a jump (unsealed predecessor)
static void emit_jmp(Block *from, Block *to) {
    // Add edge
    block_add_succ(from, to);
    block_add_pred(to, from);
    // Emit IK_JMP if block not yet terminated
    if (from->filled) return;
    Inst *inst = arena_alloc(sizeof(Inst));
    inst->kind   = IK_JMP;
    inst->target = to;
    inst->block  = from;
    inst_append(from, inst);
    from->filled = 1;
}

// Add a conditional branch.
// f is the enclosing Function, needed to materialise constant conditions.
static void emit_br(Function *f, Block *cond_blk, Value *cond_val,
                    Block *then_blk, Block *else_blk) {
    block_add_succ(cond_blk, then_blk); block_add_pred(then_blk, cond_blk);
    block_add_succ(cond_blk, else_blk); block_add_pred(else_blk, cond_blk);
    // If condition is a compile-time constant, materialise it as a real instruction.
    // jnz now takes any register, so no pre-coloring to r0 is needed.
    if (cond_val && cond_val->kind == VAL_CONST) {
        Value *dst = new_value(f, VAL_INST, cond_val->vtype);
        Inst *ci  = new_inst(f, cond_blk, IK_CONST, dst);
        ci->imm   = cond_val->iconst;
        inst_append(cond_blk, ci);
        cond_val = dst;
    }
    Inst *inst    = arena_alloc(sizeof(Inst));
    inst->kind    = IK_BR;
    inst->dst     = NULL;
    inst->target  = then_blk;
    inst->target2 = else_blk;
    inst->block   = cond_blk;
    inst_add_op(inst, cond_val);
    inst_append(cond_blk, inst);
    cond_blk->filled = 1;
}

// Emit a short-circuit logical AND / OR
static Value *cg_logand(BraunCtx *ctx, Block **cur, Sx *lhs_sx, Sx *rhs_sx, bool is_or);

// ============================================================
// cg_expr — generate SSA value for an expression sexp
// *cur may advance if the expression contains control flow (ternary, seq)
// ============================================================

static Value *cg_expr(BraunCtx *ctx, Block **cur, Sx *node) {
    if (!node || !(*cur)) {
        Value *u = new_value(ctx->f, VAL_UNDEF, VT_I16);
        return u;
    }

    const char *tag = sx_car_sym(node);
    if (!tag) {
        // Atom
        if (node->kind == SX_INT) {
            return new_const(ctx->f, node->i, VT_I16);
        }
        Value *u = new_value(ctx->f, VAL_UNDEF, VT_I16);
        return u;
    }

    Block *b = *cur;
    ValType vt = vt_of(ctx, node);

    // (int v)
    if (!strcmp(tag, "int")) {
        Sx *arg = sx_nth(node, 1);
        int ival = arg ? arg->i : 0;
        return new_const(ctx->f, ival, vt != VT_VOID ? vt : VT_I16);
    }
    // (flt bits)
    if (!strcmp(tag, "flt")) {
        Sx *arg = sx_nth(node, 1);
        int bits = arg ? arg->i : 0;
        return new_const(ctx->f, bits, VT_F32);
    }
    // (var "x") — SSA read
    if (!strcmp(tag, "var")) {
        Sx *name_sx = sx_nth(node, 1);
        const char *name = (name_sx && name_sx->kind == SX_STR) ? name_sx->s : "";
        Value *v = read_var(ctx, b, name);
        v = val_resolve(v);
        // Propagate type from TypeMap
        if (vt != VT_VOID) v->vtype = vt;
        return v;
    }
    // (assign "x" val)
    if (!strcmp(tag, "assign")) {
        Sx *name_sx = sx_nth(node, 1);
        Sx *val_sx  = sx_nth(node, 2);
        const char *name = (name_sx && name_sx->kind == SX_STR) ? name_sx->s : "";
        Value *v = cg_expr(ctx, cur, val_sx);
        b = *cur;
        // assign nodes are never registered in the TypeMap (lval_store doesn't call
        // reg_type), so vt_of returns VT_I16 as the default — do NOT clobber the rhs
        // value's vtype with that default.  Only override if there's an explicit entry.
        { ValType explicit_vt = typemap_vtype(ctx->tm, node->id);
          if (explicit_vt != VT_VOID) v->vtype = explicit_vt; }
        write_var(b, name, v);
        return v;
    }
    // (addr "x" offset) — frame-relative address (offset is bp-relative byte offset)
    if (!strcmp(tag, "addr")) {
        Sx *name_sx = sx_nth(node, 1);
        Sx *off_sx  = sx_nth(node, 2);
        const char *name = (name_sx && name_sx->kind == SX_STR) ? name_sx->s : "";
        int frame_off = (off_sx && off_sx->kind == SX_INT) ? off_sx->i : 0;
        Value *dst = new_value(ctx->f, VAL_INST, VT_PTR);
        Inst  *inst = new_inst(ctx->f, b, IK_ADDR, dst);
        inst->fname = (char *)name;
        inst->imm   = frame_off;
        inst_append(b, inst);
        return dst;
    }
    // (gaddr "name") — global/static label address
    if (!strcmp(tag, "gaddr")) {
        Sx *name_sx = sx_nth(node, 1);
        const char *name = (name_sx && name_sx->kind == SX_STR) ? name_sx->s : "";
        Value *dst = new_value(ctx->f, VAL_INST, VT_PTR);
        Inst  *inst = new_inst(ctx->f, b, IK_GADDR, dst);
        inst->fname = (char *)name;
        inst_append(b, inst);
        return dst;
    }
    // (load size ptr)
    if (!strcmp(tag, "load")) {
        Sx *sz_sx  = sx_nth(node, 1);
        Sx *ptr_sx = sx_nth(node, 2);
        int size = sz_sx ? sz_sx->i : 2;
        Value *ptr = cg_expr(ctx, cur, ptr_sx);
        b = *cur;
        return emit_load(ctx, b, ptr, size, 0, vt != VT_VOID ? vt : VT_I16);
    }
    // (store size ptr val)
    if (!strcmp(tag, "store")) {
        Sx *sz_sx  = sx_nth(node, 1);
        Sx *ptr_sx = sx_nth(node, 2);
        Sx *val_sx = sx_nth(node, 3);
        int size = sz_sx ? sz_sx->i : 2;
        Value *ptr = cg_expr(ctx, cur, ptr_sx);
        b = *cur;
        Value *val = cg_expr(ctx, cur, val_sx);
        b = *cur;
        emit_store(ctx, b, ptr, val, size, 0);
        // store returns void; return the value
        return val;
    }
    // (memcpy dst src size_int)
    if (!strcmp(tag, "memcpy")) {
        Sx *dst_sx  = sx_nth(node, 1);
        Sx *src_sx  = sx_nth(node, 2);
        Sx *sz_sx   = sx_nth(node, 3);
        int size = sz_sx ? sz_sx->i : 0;
        Value *dst_v = cg_expr(ctx, cur, dst_sx); b = *cur;
        Value *src_v = cg_expr(ctx, cur, src_sx); b = *cur;
        Inst *inst = new_inst(ctx->f, b, IK_MEMCPY, NULL);
        inst->imm  = size;
        inst->size = size;
        inst_add_op(inst, dst_v);
        inst_add_op(inst, src_v);
        inst_append(b, inst);
        return dst_v;
    }
    // (unop "op" e)
    if (!strcmp(tag, "unop")) {
        Sx *op_sx = sx_nth(node, 1);
        Sx *e_sx  = sx_nth(node, 2);
        const char *op = (op_sx && op_sx->kind == SX_STR) ? op_sx->s : "-";
        Value *v = cg_expr(ctx, cur, e_sx);
        b = *cur;
        InstKind kind = !strcmp(op,"-") ? IK_NEG :
                        !strcmp(op,"~") ? IK_XOR : // ~x = x XOR -1; handled specially
                        IK_NOT; // "!"
        if (!strcmp(op, "~")) {
            // bitwise NOT: emit XOR with -1
            Value *minus1 = new_const(ctx->f, -1, vt);
            return emit_binop(ctx, b, IK_XOR, v, minus1, vt);
        }
        return emit_unop(ctx, b, kind, v, vt != VT_VOID ? vt : VT_I16);
    }
    // (binop "op" lhs rhs)
    if (!strcmp(tag, "binop")) {
        Sx *op_sx  = sx_nth(node, 1);
        Sx *lhs_sx = sx_nth(node, 2);
        Sx *rhs_sx = sx_nth(node, 3);
        const char *op = (op_sx && op_sx->kind == SX_STR) ? op_sx->s : "+";

        // Short-circuit logical ops
        if (!strcmp(op, "&&")) return cg_logand(ctx, cur, lhs_sx, rhs_sx, false);
        if (!strcmp(op, "||")) return cg_logand(ctx, cur, lhs_sx, rhs_sx, true);

        Value *lv = cg_expr(ctx, cur, lhs_sx); b = *cur;
        Value *rv = cg_expr(ctx, cur, rhs_sx); b = *cur;

        // For operand type, use the lhs type
        ValType op_vt = lv->vtype != VT_VOID ? lv->vtype : vt;

        // Handle GT/GE by swapping operands
        bool swap = (!strcmp(op, ">") || !strcmp(op, ">="));
        if (swap) { Value *tmp = lv; lv = rv; rv = tmp; }

        InstKind kind = binop_kind(op, op_vt);
        // Comparison result is always i16 (0 or 1)
        ValType result_vt = (kind >= IK_LT && kind <= IK_NE) ? VT_I16 :
                            (kind >= IK_FLT && kind <= IK_FNE) ? VT_I16 :
                            op_vt;
        return emit_binop(ctx, b, kind, lv, rv, result_vt != VT_VOID ? result_vt : VT_I16);
    }
    // (cast e)
    if (!strcmp(tag, "cast")) {
        Sx *e_sx = sx_nth(node, 1);
        Value *v = cg_expr(ctx, cur, e_sx);
        b = *cur;
        // Insert type conversion if needed
        if (vt == VT_VOID || vt == v->vtype) return v;
        // Determine conversion instruction
        InstKind kind = IK_COPY;
        if (vt == VT_F32 && v->vtype != VT_F32)       kind = IK_ITOF;
        else if (vt != VT_F32 && v->vtype == VT_F32)  kind = IK_FTOI;
        else if ((vt == VT_I8 || vt == VT_U8) && v->vtype != VT_I8
                 && v->vtype != VT_U8)                 kind = IK_TRUNC;
        // Truncate 32-bit to 16-bit integer
        else if ((vt == VT_I16 || vt == VT_U16) &&
                 (v->vtype == VT_I32 || v->vtype == VT_U32)) kind = IK_TRUNC;
        // Same-width signedness reinterpretation
        // signed→unsigned: mask off any sign-extension bits (TRUNC to same width)
        else if (vt == VT_U8  && v->vtype == VT_I8)   kind = IK_TRUNC;
        else if (vt == VT_U16 && v->vtype == VT_I16)  kind = IK_TRUNC;
        // unsigned→signed: sign-extend so irsim vreg holds canonical signed form
        else if (vt == VT_I8  && v->vtype == VT_U8)   kind = IK_SEXT8;
        else if (vt == VT_I16 && v->vtype == VT_U16)  kind = IK_SEXT16;
        // 32-bit signedness change: all bits used, no conversion needed
        else if ((vt == VT_U32 && v->vtype == VT_I32) || (vt == VT_I32 && v->vtype == VT_U32))
                                                       kind = IK_COPY;
        // Sign-extend signed narrow types to wider types
        else if (v->vtype == VT_I8)                    kind = IK_SEXT8;
        else if (v->vtype == VT_I16)                   kind = IK_SEXT16;
        Value *dst = new_value(ctx->f, VAL_INST, vt);
        Inst  *inst = new_inst(ctx->f, b, kind, dst);
        inst->imm  = (vt == VT_I8 || vt == VT_U8) ? 0xff :
                     (vt == VT_I16 || vt == VT_U16) ? 0xffff : 0;
        inst_add_op(inst, v);
        inst_append(b, inst);
        return dst;
    }
    // (call "fname" args...)
    if (!strcmp(tag, "call")) {
        Sx *name_sx = sx_nth(node, 1);
        const char *fname = (name_sx && name_sx->kind == SX_STR) ? name_sx->s : "";
        ValType call_vt = vt != VT_VOID ? vt : VT_I16;
        // Landing: pre-colored to r0 by irc_allocate; live only at the call point.
        Value *landing = new_value(ctx->f, VAL_INST, call_vt);
        Inst  *call = new_inst(ctx->f, b, IK_CALL, landing);
        call->fname    = (char *)fname;
        call->calldesc = typemap_calldesc(ctx->tm, node->id);
        Sx *args = sx_cdr(sx_cdr(node));  // skip "call" and fname
        while (args && args->kind == SX_PAIR) {
            Value *av = cg_expr(ctx, cur, args->car);
            b = *cur;
            inst_add_op(call, av);
            args = args->cdr;
        }
        inst_append(b, call);
        // Working copy: freely allocated by IRC; callers use this, not the landing.
        // This prevents the landing (r0) from aliasing a BR condition that also
        // needs r0, which would cause IRC to assign conflicting pre-colored values.
        Value *working = new_value(ctx->f, VAL_INST, call_vt);
        Inst  *cp = new_inst(ctx->f, b, IK_COPY, working);
        inst_add_op(cp, landing);
        inst_append(b, cp);
        return working;
    }
    // (icall fp args...)
    if (!strcmp(tag, "icall")) {
        Sx *fp_sx = sx_nth(node, 1);
        Value *fp = cg_expr(ctx, cur, fp_sx); b = *cur;
        ValType call_vt = vt != VT_VOID ? vt : VT_I16;
        Value *landing = new_value(ctx->f, VAL_INST, call_vt);
        Inst  *call = new_inst(ctx->f, b, IK_ICALL, landing);
        call->calldesc = typemap_calldesc(ctx->tm, node->id);
        inst_add_op(call, fp);
        Sx *args = sx_cdr(sx_cdr(node));  // skip "icall" and fp
        while (args && args->kind == SX_PAIR) {
            Value *av = cg_expr(ctx, cur, args->car);
            b = *cur;
            inst_add_op(call, av);
            args = args->cdr;
        }
        inst_append(b, call);
        Value *working = new_value(ctx->f, VAL_INST, call_vt);
        Inst  *cp = new_inst(ctx->f, b, IK_COPY, working);
        inst_add_op(cp, landing);
        inst_append(b, cp);
        return working;
    }
    // (putchar e)
    if (!strcmp(tag, "putchar")) {
        Sx *e_sx = sx_nth(node, 1);
        Value *v = cg_expr(ctx, cur, e_sx); b = *cur;
        Inst *inst = new_inst(ctx->f, b, IK_PUTCHAR, NULL);
        inst_add_op(inst, v);
        inst_append(b, inst);
        return new_const(ctx->f, 0, VT_I16);
    }
    // (if cond then else) — ternary expression
    if (!strcmp(tag, "if")) {
        Sx *cond_sx = sx_nth(node, 1);
        Sx *then_sx = sx_nth(node, 2);
        Sx *else_sx = sx_nth(node, 3);
        // Use phi to merge
        Block *then_blk = new_block(ctx->f);
        Block *else_blk = else_sx ? new_block(ctx->f) : NULL;
        Block *merge    = new_block(ctx->f);

        Value *cond = cg_expr(ctx, cur, cond_sx); b = *cur;
        emit_br(ctx->f, b, cond, then_blk, else_blk ? else_blk : merge);
        seal_block(ctx, then_blk);

        *cur = then_blk;
        Value *then_v = cg_expr(ctx, cur, then_sx);
        Block *then_exit = *cur;
        if (!then_exit->filled) {
            block_add_succ(then_exit, merge);
            block_add_pred(merge, then_exit);
            Inst *j = arena_alloc(sizeof(Inst));
            j->kind = IK_JMP; j->target = merge; j->block = then_exit;
            inst_append(then_exit, j); then_exit->filled = 1;
        }

        Value *else_v = NULL;
        Block *else_exit = NULL;
        if (else_blk) {
            seal_block(ctx, else_blk);
            *cur = else_blk;
            else_v = cg_expr(ctx, cur, else_sx);
            else_exit = *cur;
            if (!else_exit->filled) {
                block_add_succ(else_exit, merge);
                block_add_pred(merge, else_exit);
                Inst *j = arena_alloc(sizeof(Inst));
                j->kind = IK_JMP; j->target = merge; j->block = else_exit;
                inst_append(else_exit, j); else_exit->filled = 1;
            }
        }
        seal_block(ctx, merge);
        *cur = merge;

        // Emit phi
        if (then_v && else_v) {
            Value *phi_dst = new_value(ctx->f, VAL_INST, vt != VT_VOID ? vt : then_v->vtype);
            Inst  *phi     = new_inst(ctx->f, merge, IK_PHI, phi_dst);
            inst_add_op(phi, then_v);
            inst_add_op(phi, else_v);
            // Insert at front of merge
            phi->next = merge->head;
            phi->prev = NULL;
            if (merge->head) merge->head->prev = phi;
            else             merge->tail = phi;
            merge->head = phi;
            phi->block = merge;
            return phi_dst;
        }
        return then_v ? then_v : new_const(ctx->f, 0, VT_I16);
    }
    // (seq stmt... expr) — side effects then value
    if (!strcmp(tag, "seq")) {
        Sx *rest = sx_cdr(node);
        // All but last are statements; last is the result expression
        while (rest && rest->cdr) {
            *cur = cg_stmt(ctx, *cur, rest->car);
            if (!*cur) return new_value(ctx->f, VAL_UNDEF, VT_I16);
            rest = rest->cdr;
        }
        if (rest && rest->car)
            return cg_expr(ctx, cur, rest->car);
        return new_value(ctx->f, VAL_UNDEF, VT_I16);
    }

    // Unknown: return undef
    return new_value(ctx->f, VAL_UNDEF, VT_I16);
}

// Short-circuit logical AND / OR
static Value *cg_logand(BraunCtx *ctx, Block **cur, Sx *lhs_sx, Sx *rhs_sx, bool is_or) {
    Block *b = *cur;
    Block *rhs_blk = new_block(ctx->f);
    Block *end_blk = new_block(ctx->f);

    Value *lhs = cg_expr(ctx, cur, lhs_sx);
    b = *cur;

    if (is_or)
        emit_br(ctx->f, b, lhs, end_blk, rhs_blk);
    else
        emit_br(ctx->f, b, lhs, rhs_blk, end_blk);

    seal_block(ctx, rhs_blk);
    *cur = rhs_blk;
    Value *rhs = cg_expr(ctx, cur, rhs_sx);
    Block *rhs_exit = *cur;
    // Normalize rhs to boolean (0 or 1): rhs != 0
    Value *zero = new_const(ctx->f, 0, VT_I16);
    Value *rhs_bool = emit_binop(ctx, rhs_exit, IK_NE, rhs, zero, VT_I16);
    if (!rhs_exit->filled) emit_jmp(rhs_exit, end_blk);

    seal_block(ctx, end_blk);
    *cur = end_blk;

    // Merge with phi
    Value *phi_dst = new_value(ctx->f, VAL_INST, VT_I16);
    Inst  *phi     = new_inst(ctx->f, end_blk, IK_PHI, phi_dst);
    // From lhs block: 0 for AND (lhs was false), 1 for OR (lhs was true)
    Value *lhs_contrib = is_or ? new_const(ctx->f, 1, VT_I16) : new_const(ctx->f, 0, VT_I16);
    inst_add_op(phi, lhs_contrib);
    inst_add_op(phi, rhs_bool);
    phi->next = end_blk->head;
    phi->prev = NULL;
    if (end_blk->head) end_blk->head->prev = phi;
    else end_blk->tail = phi;
    end_blk->head = phi;
    phi->block = end_blk;
    return phi_dst;
}

// ============================================================
// cg_stmt — generate SSA for a statement sexp
// Returns the current block after the statement
// ============================================================

static Block *cg_stmt(BraunCtx *ctx, Block *b, Sx *node) {
    if (!b) return b;
    if (!node) return b;

    const char *tag = sx_car_sym(node);
    if (!tag) return b;

    // (skip)
    if (!strcmp(tag, "skip")) return b;

    // Labels create new blocks — must process even from a filled block.
    // We do NOT seal the label block here because a backward goto (loop) may
    // add a predecessor edge later. Unsealed blocks collect "incomplete phis"
    // that are resolved when the block is eventually sealed.
    // All label blocks are sealed in braun_function() after the full body is generated.
    if (!strcmp(tag, "label")) {
        Sx *name_sx = sx_nth(node, 1);
        Sx *body_sx = sx_nth(node, 2);
        const char *name = (name_sx && name_sx->kind == SX_STR) ? name_sx->s : "";
        Block *target = get_label_block(ctx, name);
        // Jump from current block to target (if not already terminated)
        if (!b->filled) emit_jmp(b, target);
        // Continue in target (do NOT seal yet — backward gotos may add predecessors)
        return cg_stmt(ctx, target, body_sx);
    }

    // All other statements require an unfilled block
    if (b->filled) return b;

    // (expr e)
    if (!strcmp(tag, "expr")) {
        Sx *e = sx_nth(node, 1);
        if (e) { Block *cur = b; cg_expr(ctx, &cur, e); b = cur; }
        return b;
    }

    // (block s0 s1 ...)
    if (!strcmp(tag, "block")) {
        Sx *rest = sx_cdr(node);
        while (rest && rest->kind == SX_PAIR) {
            b = cg_stmt(ctx, b, rest->car);
            rest = rest->cdr;
        }
        return b;
    }

    // (assign "x" e) — statement form
    if (!strcmp(tag, "assign")) {
        Sx *name_sx = sx_nth(node, 1);
        Sx *val_sx  = sx_nth(node, 2);
        const char *name = (name_sx && name_sx->kind == SX_STR) ? name_sx->s : "";
        Block *cur = b;
        Value *v = cg_expr(ctx, &cur, val_sx);
        b = cur;
        write_var(b, name, v);
        return b;
    }

    // (store size ptr val) — statement form
    if (!strcmp(tag, "store")) {
        Sx *sz_sx  = sx_nth(node, 1);
        Sx *ptr_sx = sx_nth(node, 2);
        Sx *val_sx = sx_nth(node, 3);
        int size = sz_sx ? sz_sx->i : 2;
        Block *cur = b;
        Value *ptr = cg_expr(ctx, &cur, ptr_sx); b = cur;
        Value *val = cg_expr(ctx, &cur, val_sx); b = cur;
        emit_store(ctx, b, ptr, val, size, 0);
        return b;
    }

    // (memcpy dst src size_int) — statement form
    if (!strcmp(tag, "memcpy")) {
        Sx *dst_sx = sx_nth(node, 1);
        Sx *src_sx = sx_nth(node, 2);
        Sx *sz_sx  = sx_nth(node, 3);
        int size = sz_sx ? sz_sx->i : 0;
        Block *cur = b;
        Value *dv = cg_expr(ctx, &cur, dst_sx); b = cur;
        Value *sv = cg_expr(ctx, &cur, src_sx); b = cur;
        Inst *inst = new_inst(ctx->f, b, IK_MEMCPY, NULL);
        inst->imm  = size; inst->size = size;
        inst_add_op(inst, dv);
        inst_add_op(inst, sv);
        inst_append(b, inst);
        return b;
    }

    // (if cond then) or (if cond then else)
    if (!strcmp(tag, "if")) {
        Sx *cond_sx = sx_nth(node, 1);
        Sx *then_sx = sx_nth(node, 2);
        Sx *else_sx = sx_nth(node, 3);

        Block *cur = b;
        Value *cond = cg_expr(ctx, &cur, cond_sx);
        b = cur;

        Block *then_blk = new_block(ctx->f);
        Block *else_blk = new_block(ctx->f);
        Block *merge    = new_block(ctx->f);

        emit_br(ctx->f, b, cond, then_blk, else_blk);
        seal_block(ctx, then_blk);
        Block *then_exit = cg_stmt(ctx, then_blk, then_sx);
        if (!then_exit->filled) emit_jmp(then_exit, merge);

        seal_block(ctx, else_blk);
        if (else_sx) {
            Block *else_exit = cg_stmt(ctx, else_blk, else_sx);
            if (!else_exit->filled) emit_jmp(else_exit, merge);
        } else {
            emit_jmp(else_blk, merge);
        }

        seal_block(ctx, merge);
        return merge;
    }

    // (while cond body)
    if (!strcmp(tag, "while")) {
        Sx *cond_sx = sx_nth(node, 1);
        Sx *body_sx = sx_nth(node, 2);

        Block *header = new_block(ctx->f); // cond block — sealed after body
        Block *body   = new_block(ctx->f);
        Block *exit   = new_block(ctx->f);

        emit_jmp(b, header);

        // Generate cond in header (not yet sealed)
        Block *cur = header;
        Value *cond = cg_expr(ctx, &cur, cond_sx);
        emit_br(ctx->f, cur, cond, body, exit);
        seal_block(ctx, body);

        Block *saved_brk  = ctx->brk;
        Block *saved_cont = ctx->cont;
        ctx->brk  = exit;
        ctx->cont = header;

        Block *body_exit = cg_stmt(ctx, body, body_sx);
        if (!body_exit->filled) emit_jmp(body_exit, header);
        seal_block(ctx, header);

        ctx->brk  = saved_brk;
        ctx->cont = saved_cont;

        seal_block(ctx, exit);
        return exit;
    }

    // (for init cond step body) — correct continue handling
    if (!strcmp(tag, "for")) {
        Sx *init_sx = sx_nth(node, 1);
        Sx *cond_sx = sx_nth(node, 2);
        Sx *step_sx = sx_nth(node, 3);
        Sx *body_sx = sx_nth(node, 4);

        b = cg_stmt(ctx, b, init_sx);

        Block *cond_blk = new_block(ctx->f); // unsealed
        Block *body_blk = new_block(ctx->f);
        Block *step_blk = new_block(ctx->f);
        Block *exit_blk = new_block(ctx->f);

        emit_jmp(b, cond_blk);

        // Cond block
        Block *cur = cond_blk;
        // Check for infinite loop (int 1)
        const char *ctag = sx_car_sym(cond_sx);
        Value *cond;
        if (ctag && !strcmp(ctag, "int") && sx_nth(cond_sx, 1) &&
            sx_nth(cond_sx, 1)->i == 1) {
            // Infinite loop: jump straight to body
            seal_block(ctx, body_blk);
            seal_block(ctx, cond_blk);
            Inst *j = arena_alloc(sizeof(Inst));
            j->kind = IK_JMP; j->target = body_blk; j->block = cond_blk;
            inst_append(cond_blk, j); cond_blk->filled = 1;
            block_add_succ(cond_blk, body_blk); block_add_pred(body_blk, cond_blk);
            cond = new_const(ctx->f, 1, VT_I16);
        } else {
            cond = cg_expr(ctx, &cur, cond_sx);
            emit_br(ctx->f, cur, cond, body_blk, exit_blk);
            seal_block(ctx, body_blk);
        }

        // Body with continue → step, break → exit
        Block *saved_brk  = ctx->brk;
        Block *saved_cont = ctx->cont;
        ctx->brk  = exit_blk;
        ctx->cont = step_blk;

        Block *body_exit = cg_stmt(ctx, body_blk, body_sx);
        if (!body_exit->filled) emit_jmp(body_exit, step_blk);

        ctx->brk  = saved_brk;
        ctx->cont = saved_cont;

        // Step block
        seal_block(ctx, step_blk);
        Block *step_exit = cg_stmt(ctx, step_blk, step_sx);
        if (!step_exit->filled) emit_jmp(step_exit, cond_blk);

        seal_block(ctx, cond_blk);
        seal_block(ctx, exit_blk);
        return exit_blk;
    }

    // (do body cond)
    if (!strcmp(tag, "do")) {
        Sx *body_sx = sx_nth(node, 1);
        Sx *cond_sx = sx_nth(node, 2);

        // body_blk has two predecessors: entry (b) and cond_blk (back-edge).
        // It must be left UNSEALED until cond_blk has emitted its branch.
        Block *body_blk = new_block(ctx->f);
        Block *cond_blk = new_block(ctx->f);
        Block *exit_blk = new_block(ctx->f);

        // Entry edge: b → body_blk
        emit_jmp(b, body_blk);
        // body_blk NOT sealed yet (cond_blk back-edge not yet known)

        Block *saved_brk  = ctx->brk;
        Block *saved_cont = ctx->cont;
        ctx->brk  = exit_blk;
        ctx->cont = cond_blk;

        Block *body_exit = cg_stmt(ctx, body_blk, body_sx);

        ctx->brk  = saved_brk;
        ctx->cont = saved_cont;

        // Body fall-through to cond_blk
        if (!body_exit->filled) emit_jmp(body_exit, cond_blk);
        seal_block(ctx, cond_blk);

        Block *cur = cond_blk;
        Value *cond = cg_expr(ctx, &cur, cond_sx);
        // emit_br adds cond_blk→body_blk and cond_blk→exit_blk edges
        emit_br(ctx->f, cur, cond, body_blk, exit_blk);

        // Now that cond_blk is a predecessor of body_blk, seal it
        seal_block(ctx, body_blk);
        seal_block(ctx, exit_blk);
        return exit_blk;
    }

    // (return) or (return e)
    if (!strcmp(tag, "return")) {
        Sx *val_sx = sx_nth(node, 1);
        Inst *ret  = arena_alloc(sizeof(Inst));
        ret->kind  = IK_RET;
        ret->block = b;
        if (val_sx) {
            Block *cur = b;
            Value *v = cg_expr(ctx, &cur, val_sx);
            b = cur;
            ret->block = b;
            inst_add_op(ret, v);
        }
        inst_append(b, ret);
        b->filled = 1;
        return b;
    }

    // (break)
    if (!strcmp(tag, "break")) {
        if (ctx->brk) emit_jmp(b, ctx->brk);
        b->filled = 1;
        return b;
    }

    // (continue)
    if (!strcmp(tag, "continue")) {
        if (ctx->cont) emit_jmp(b, ctx->cont);
        b->filled = 1;
        return b;
    }

    // (goto "label")
    if (!strcmp(tag, "goto")) {
        Sx *name_sx = sx_nth(node, 1);
        const char *name = (name_sx && name_sx->kind == SX_STR) ? name_sx->s : "";
        Block *target = get_label_block(ctx, name);
        emit_jmp(b, target);
        b->filled = 1;
        return b;
    }

    // (label "name" stmt)
    // (switch expr body)
    // body is a flat (block ...) where case/default labels are interleaved with statements.
    // C case labels are jump targets only; the body follows as sibling statements.
    // Example: switch(x){case 1: s1; case 2: s2; default: s3;}
    // → (block (case (int 1) (skip)) s1 (case (int 2) (skip)) s2 (default (skip)) s3)
    if (!strcmp(tag, "switch")) {
        Sx *sel_sx  = sx_nth(node, 1);
        Sx *body_sx = sx_nth(node, 2);

        // Evaluate selector
        Block *cur = b;
        Value *sel = cg_expr(ctx, &cur, sel_sx);
        b = cur;

        // Create exit block for break targets
        Block *exit_blk = new_block(ctx->f);

        // Unwrap "block" wrapper if present
        Sx *items = body_sx;
        if (items && items->kind == SX_PAIR) {
            const char *btag = sx_car_sym(items);
            if (btag && !strcmp(btag, "block"))
                items = sx_cdr(items);
        }

        // Phase 1: pre-scan to collect case labels and create blocks
        enum { MAX_CASES = 256 };
        int    case_vals[MAX_CASES];
        Block *case_blks[MAX_CASES];
        int    ncases = 0;
        Block *default_blk = NULL;  // NULL = no default

        for (Sx *it = items; it && it->kind == SX_PAIR; it = it->cdr) {
            Sx *s = it->car;
            const char *stag = sx_car_sym(s);
            if (!stag) continue;
            if (!strcmp(stag, "case") && ncases < MAX_CASES) {
                Sx *val_sx = sx_nth(s, 1);
                int cval = (val_sx && val_sx->kind == SX_PAIR) ?
                    (sx_nth(val_sx, 1) ? sx_nth(val_sx, 1)->i : 0) : 0;
                case_vals[ncases] = cval;
                case_blks[ncases] = new_block(ctx->f);
                ncases++;
            } else if (!strcmp(stag, "default")) {
                default_blk = new_block(ctx->f);
            }
        }

        Block *dispatch_default = default_blk ? default_blk : exit_blk;

        // Phase 2: emit dispatch chain
        for (int ci = 0; ci < ncases; ci++) {
            Value *kv  = new_const(ctx->f, case_vals[ci], VT_I16);
            Value *cmp = emit_binop(ctx, b, IK_EQ, sel, kv, VT_I16);
            Block *nc  = new_block(ctx->f);
            seal_block(ctx, nc);  // nc has exactly one pred: b (added by emit_br)
            emit_br(ctx->f, b, cmp, case_blks[ci], nc);
            b = nc;
        }
        emit_jmp(b, dispatch_default);

        // Phase 3: walk flat body list, emit statements into current case block
        Block *saved_brk = ctx->brk;
        ctx->brk = exit_blk;

        // Statements before the first label are unreachable; emit into a dead block
        Block *body_cur = new_block(ctx->f);
        seal_block(ctx, body_cur);  // no predecessors (unreachable)

        int case_idx = 0;
        for (Sx *it = items; it && it->kind == SX_PAIR; it = it->cdr) {
            Sx *s = it->car;
            const char *stag = sx_car_sym(s);
            if (stag && !strcmp(stag, "case")) {
                Block *cb = case_blks[case_idx++];
                if (!body_cur->filled) emit_jmp(body_cur, cb);
                seal_block(ctx, cb);
                body_cur = cb;
            } else if (stag && !strcmp(stag, "default")) {
                if (!body_cur->filled) emit_jmp(body_cur, default_blk);
                seal_block(ctx, default_blk);
                body_cur = default_blk;
            } else {
                // Regular statement
                body_cur = cg_stmt(ctx, body_cur, s);
            }
        }
        // Fall-through from last case to exit
        if (!body_cur->filled) emit_jmp(body_cur, exit_blk);

        ctx->brk = saved_brk;
        seal_block(ctx, exit_blk);
        return exit_blk;
    }

    // (case val stmt) / (default stmt) — fallback if encountered outside switch
    if (!strcmp(tag, "case") || !strcmp(tag, "default")) {
        Sx *body_sx = sx_nth(node, sx_len(node) - 1);
        return cg_stmt(ctx, b, body_sx);
    }


    // (seq stmt... expr) as statement: evaluate and discard result
    if (!strcmp(tag, "seq")) {
        Sx *rest = sx_cdr(node);
        while (rest && rest->kind == SX_PAIR) {
            Block *cur = b;
            if (!rest->cdr) {
                // Last element: could be expression or statement
                cg_expr(ctx, &cur, rest->car);
                b = cur;
            } else {
                b = cg_stmt(ctx, b, rest->car);
            }
            rest = rest->cdr;
        }
        return b;
    }

    return b;
}

// ============================================================
// braun_function — entry point
// ============================================================

Function *braun_function(Sx *func_sx, TypeMap *tm) {
    if (!func_sx) return NULL;
    // (func "name" frame_size is_variadic (params "p"...) body)
    Sx *name_sx      = sx_nth(func_sx, 1);
    Sx *frame_sz_sx  = sx_nth(func_sx, 2);
    Sx *variadic_sx  = sx_nth(func_sx, 3);
    Sx *params_sx    = sx_nth(func_sx, 4);
    Sx *body_sx      = sx_nth(func_sx, 5);

    const char *name = (name_sx && name_sx->kind == SX_STR) ? name_sx->s : "unknown";
    int frame_size   = (frame_sz_sx && frame_sz_sx->kind == SX_INT) ? frame_sz_sx->i : 0;
    int is_variadic  = (variadic_sx && variadic_sx->kind == SX_INT) ? variadic_sx->i : 0;

    Function *f = new_function(name);
    f->frame_size = frame_size;
    BraunCtx ctx = {0};
    ctx.f  = f;
    ctx.tm = tm;

    // Create entry block (sealed immediately — no predecessors)
    Block *entry = new_block(f);
    entry->sealed = 1;

    // Emit param setup for each parameter.
    // For variadic functions: all params come from the stack (old stack ABI).
    //   Layout: param 0 at bp+4, param 1 at bp+8, ..., variadic args continue after.
    // For non-variadic functions: first 7 params in r1..r7; extras on stack (4-byte slots).
    if (params_sx && sx_car_sym(params_sx) &&
        !strcmp(sx_car_sym(params_sx), "params")) {
        int idx = 0;
        Sx *plist = sx_cdr(params_sx);  // skip "params"
        int stack_slot = 0;  // stack params beyond the 3 register slots
        // NREG_PARAMS: number of params passed in registers (r1..r3).
        // Using only caller-saved registers avoids conflict between callee-save
        // restore and the caller's own live values in r4-r7.
        enum { NREG_PARAMS = 3 };
        while (plist && plist->kind == SX_PAIR) {
            Sx *pname_sx = plist->car;
            if (pname_sx && pname_sx->kind == SX_STR) {
                if (is_variadic) {
                    // Variadic function: all params come from the stack.
                    // Caller uses stack ABI (push all args right-to-left, 4-byte slots).
                    // Layout: param 0 at bp+4, param 1 at bp+8 (4-byte CPU4 push slots).
                    int bp_off = 4 + idx * 4;  // 4-byte slots (CPU4 push is 32-bit)
                    Value *pv = new_value(f, VAL_INST, VT_I16);
                    Inst *li = new_inst(f, entry, IK_LOAD, pv);
                    li->imm  = bp_off;
                    li->size = 2;
                    inst_add_op(li, NULL);  // NULL base = bp-relative load
                    inst_append(entry, li);
                    write_var(entry, pname_sx->s, pv);
                } else if (idx < NREG_PARAMS) {
                    // Register param: "Landing + copy" scheme.
                    // Landing is pre-colored to r{idx+1} (r1..r3, caller-saved).
                    // Working copy is freely allocated by IRC.
                    Value *landing = new_value(f, VAL_INST, VT_I16);
                    landing->phys_reg = idx + 1;
                    Inst *pi = new_inst(f, entry, IK_PARAM, landing);
                    pi->param_idx = idx + 1;
                    inst_append(entry, pi);
                    if (f->nparams >= f->param_cap) {
                        f->param_cap = f->param_cap ? f->param_cap * 2 : 8;
                        f->params    = malloc(f->param_cap * sizeof(Value *));
                    }
                    f->params[f->nparams++] = landing;
                    Value *pv = new_value(f, VAL_INST, VT_I16);
                    Inst *copy = new_inst(f, entry, IK_COPY, pv);
                    inst_add_op(copy, landing);
                    inst_append(entry, copy);
                    write_var(entry, pname_sx->s, pv);
                } else {
                    // Stack param: beyond NREG_PARAMS register slots, passed on stack.
                    // CPU4 push is 4 bytes; caller uses pushr, so slot = 4 bytes.
                    // Layout after callee enter: bp+4 = first stack param, bp+8 = second, ...
                    int bp_off = 4 + stack_slot * 4;
                    Value *pv = new_value(f, VAL_INST, VT_I16);
                    Inst *li = new_inst(f, entry, IK_LOAD, pv);
                    li->imm  = bp_off;
                    li->size = 2;  // int-size param
                    inst_add_op(li, NULL);  // NULL base = bp-relative load
                    inst_append(entry, li);
                    write_var(entry, pname_sx->s, pv);
                    stack_slot++;
                }
                idx++;
            }
            plist = plist->cdr;
        }
    }

    // Generate body
    Block *exit_blk = cg_stmt(&ctx, entry, body_sx);
    // Ensure function ends with a return
    if (exit_blk && !exit_blk->filled) {
        Inst *ret = arena_alloc(sizeof(Inst));
        ret->kind  = IK_RET;
        ret->block = exit_blk;
        inst_append(exit_blk, ret);
        exit_blk->filled = 1;
    }

    // Seal all named label blocks that weren't sealed eagerly.
    // This handles backward gotos (loops): the label block accumulates
    // incomplete phis until all predecessors (including back edges) are known.
    for (int i = 0; i < ctx.nlabels; i++) {
        Block *lb = ctx.label_blocks[i];
        if (!lb->sealed) seal_block(&ctx, lb);
    }

    return f;
}
