/*
 * braun.c — Braun et al. 2013 SSA construction directly from Node* AST
 *
 * No S-expression intermediate: walks the parse tree directly.
 * Variable maps use Symbol* pointer keys (no alpha-renaming needed).
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "braun.h"
#include "ssa.h"
#include "sx.h"
#include "smallcc.h"

// ============================================================
// String literal accumulator (flushed by braun_emit_strlits)
// ============================================================

typedef struct { int id; const char *data; int len; } BStrLit;
static BStrLit *g_strlits;
static int      g_nstrlits;
static int      g_strlit_cap;

static void strlits_push(int id, const char *data, int len) {
    if (g_nstrlits >= g_strlit_cap) {
        int nc = g_strlit_cap ? g_strlit_cap * 2 : 16;
        BStrLit *nb = arena_alloc(nc * sizeof(BStrLit));
        memcpy(nb, g_strlits, g_nstrlits * sizeof(BStrLit));
        g_strlits    = nb;
        g_strlit_cap = nc;
    }
    g_strlits[g_nstrlits].id   = id;
    g_strlits[g_nstrlits].data = data;
    g_strlits[g_nstrlits].len  = len;
    g_nstrlits++;
}

// ============================================================
// Static local variable accumulator (flushed by braun_emit_strlits)
// ============================================================

typedef struct { int id; Symbol *sym; Node *init_node; } BStaticLocal;
static BStaticLocal *g_static_locals;
static int           g_nsl, g_sl_cap;

static void static_locals_push(int id, Symbol *sym, Node *init) {
    if (g_nsl >= g_sl_cap) {
        int nc = g_sl_cap ? g_sl_cap * 2 : 8;
        BStaticLocal *nb = arena_alloc(nc * sizeof(BStaticLocal));
        memcpy(nb, g_static_locals, g_nsl * sizeof(BStaticLocal));
        g_static_locals = nb;
        g_sl_cap        = nc;
    }
    g_static_locals[g_nsl].id        = id;
    g_static_locals[g_nsl].sym       = sym;
    g_static_locals[g_nsl].init_node = init;
    g_nsl++;
}

// Emit data-section assembly for a static local variable
static void emit_static_local_data(FILE *out, BStaticLocal *sl) {
    Symbol *sym  = sl->sym;
    Node   *init = sl->init_node;
    Type   *ty   = sym->type;
    int     size = ty ? ty->size : 2;

    fprintf(out, "_ls%d:", sl->id);

    // String-literal initializer for a char array (e.g. static char s[] = "hello")
    if (ty && istype_array(ty) && init && init->kind == ND_LITERAL && init->u.literal.strval) {
        const char *str = init->u.literal.strval;
        int         len = init->u.literal.strval_len;
        for (int j = 0; j <= len; j++)
            fprintf(out, "\n    byte %d", j < len ? (unsigned char)str[j] : 0);
        fprintf(out, "\n");
        return;
    }

    // Scalar integer initializer
    if (!istype_array(ty) && ty && ty->base != TB_STRUCT && init && init->kind == ND_LITERAL) {
        if (size <= 2)
            fprintf(out, "\n    word %d\n", (int)init->u.literal.ival & 0xffff);
        else
            fprintf(out, "\n    long %d\n", (int)init->u.literal.ival);
        return;
    }

    // Zero-initialized or unsupported: allocb
    fprintf(out, "\n    allocb %d\n", size > 0 ? size : 2);
}

int braun_nstrlits(void) { return g_nstrlits; }

void braun_get_strlit(int i, char label_buf[32], const char **data, int *len) {
    snprintf(label_buf, 32, "_l%d", g_strlits[i].id);
    *data = g_strlits[i].data;
    *len  = g_strlits[i].len;
}

void braun_emit_strlits(FILE *out) {
    for (int i = 0; i < g_nstrlits; i++) {
        fprintf(out, "_l%d:", g_strlits[i].id);
        const char *data = g_strlits[i].data;
        int len = g_strlits[i].len;
        for (int j = 0; j < len; j++)
            fprintf(out, "\n    byte %d", (unsigned char)data[j]);
        fprintf(out, "\n    byte 0\n");
    }
    g_nstrlits = 0;
    for (int i = 0; i < g_nsl; i++)
        emit_static_local_data(out, &g_static_locals[i]);
    g_nsl = 0;
}

// ============================================================
// BraunCtx
// ============================================================

typedef struct {
    Function *f;
    int       tu_index;
    int      *strlit_id;

    // Address-taken local/param symbols (not SSA-promoted)
    Symbol  **addr_taken;
    int       n_addr_taken;
    int       addr_taken_cap;

    // Addr-taken non-variadic param home slots (negative bp offsets)
    Symbol  **param_home_syms;
    int      *param_home_offs;
    int       n_param_homes;
    int       param_home_cap;

    // Variadic param CPU4 bp offsets (4 + idx * 4)
    Symbol  **vparam_syms;
    int      *vparam_offs;
    int       n_vparams;
    int       vparam_cap;

    // Break / continue targets
    Block    *brk;
    Block    *cont;

    // Named label blocks (goto targets)
    char    **label_names;
    Block   **label_blocks;
    int       nlabels;
    int       label_cap;
} BraunCtx;

// ============================================================
// Helper: is sym address-taken in this function?
// ============================================================

static bool is_addr_taken(BraunCtx *ctx, Symbol *sym) {
    for (int i = 0; i < ctx->n_addr_taken; i++)
        if (ctx->addr_taken[i] == sym) return true;
    return false;
}

// ============================================================
// Helper: get home bp offset for an addr-taken param (-N)
// Returns 0 if not found (shouldn't happen)
// ============================================================

static int param_home_off(BraunCtx *ctx, Symbol *sym) {
    for (int i = 0; i < ctx->n_param_homes; i++)
        if (ctx->param_home_syms[i] == sym) return ctx->param_home_offs[i];
    return 0;
}

// ============================================================
// Helper: get variadic param bp offset (returns -1 if not variadic)
// ============================================================

static int vparam_off(BraunCtx *ctx, Symbol *sym) {
    for (int i = 0; i < ctx->n_vparams; i++)
        if (ctx->vparam_syms[i] == sym) return ctx->vparam_offs[i];
    return -1;
}

// ============================================================
// Helper: assembly label for a symbol
// ============================================================

static const char *sym_label(int tu_index, Symbol *sym) {
    static char buf[512];
    switch (sym->kind) {
    case SYM_GLOBAL:
    case SYM_EXTERN:
    case SYM_BUILTIN:
        return sym->name;
    case SYM_STATIC_GLOBAL:
        snprintf(buf, sizeof(buf), "_s%d_%s", sym->tu_index, sym->name);
        return arena_strdup(buf);
    case SYM_STATIC_LOCAL:
        snprintf(buf, sizeof(buf), "_ls%d", sym->offset);
        return arena_strdup(buf);
    default:
        return sym->name;
    }
    (void)tu_index;
}

// ============================================================
// scan_addr_taken — pre-scan function body
// ============================================================

static void scan_addr_taken(BraunCtx *ctx, Node *n) {
    if (!n) return;
    if (n->kind == ND_UNARYOP && n->op_kind == TK_AMPERSAND) {
        Node *operand = n->ch[0];
        if (operand && operand->kind == ND_IDENT && operand->symbol) {
            Symbol *sym = operand->symbol;
            if (sym->kind == SYM_LOCAL || sym->kind == SYM_PARAM) {
                // Add if not already present
                if (!is_addr_taken(ctx, sym)) {
                    if (ctx->n_addr_taken >= ctx->addr_taken_cap) {
                        int nc = ctx->addr_taken_cap ? ctx->addr_taken_cap * 2 : 8;
                        Symbol **nb = arena_alloc(nc * sizeof(Symbol *));
                        memcpy(nb, ctx->addr_taken, ctx->n_addr_taken * sizeof(Symbol *));
                        ctx->addr_taken     = nb;
                        ctx->addr_taken_cap = nc;
                    }
                    ctx->addr_taken[ctx->n_addr_taken++] = sym;
                }
            }
        }
    }
    for (int i = 0; i < 4; i++) scan_addr_taken(ctx, n->ch[i]);
    scan_addr_taken(ctx, n->next);
}

// ============================================================
// max_frame_r — compute frame_size from SYM_LOCAL offsets
// ============================================================

static void max_frame_r(Node *n, int *mx) {
    if (!n) return;
    if ((n->kind == ND_COMPSTMT || n->kind == ND_FORSTMT) && n->symtable) {
        for (Symbol *s = n->symtable->symbols; s; s = s->next) {
            if (s->kind == SYM_LOCAL && s->ns == NS_IDENT && s->offset > *mx)
                *mx = s->offset;
        }
    }
    for (int i = 0; i < 4; i++) max_frame_r(n->ch[i], mx);
    max_frame_r(n->next, mx);
}

// ============================================================
// Named label block lookup / creation
// ============================================================

static Block *get_label_block(BraunCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->nlabels; i++)
        if (strcmp(ctx->label_names[i], name) == 0)
            return ctx->label_blocks[i];
    if (ctx->nlabels >= ctx->label_cap) {
        int nc = ctx->label_cap ? ctx->label_cap * 2 : 8;
        char  **nn = arena_alloc(nc * sizeof(char *));
        Block **nb = arena_alloc(nc * sizeof(Block *));
        memcpy(nn, ctx->label_names,  ctx->nlabels * sizeof(char *));
        memcpy(nb, ctx->label_blocks, ctx->nlabels * sizeof(Block *));
        ctx->label_names  = nn;
        ctx->label_blocks = nb;
        ctx->label_cap    = nc;
    }
    Block *b = new_block(ctx->f);
    b->label = arena_strdup(name);
    ctx->label_names [ctx->nlabels] = (char *)b->label;
    ctx->label_blocks[ctx->nlabels] = b;
    ctx->nlabels++;
    return b;
}

// ============================================================
// Variable maps (Braun algorithm, Symbol* keys)
// ============================================================

static void write_var(Block *b, Symbol *sym, Value *v) {
    for (int i = 0; i < b->ndef; i++) {
        if (b->def_syms[i] == sym) { b->def_vals[i] = v; return; }
    }
    if (b->ndef >= BRAUN_MAP_MAX) return;
    b->def_syms[b->ndef] = sym;
    b->def_vals[b->ndef] = v;
    b->ndef++;
}

static Value *read_var(BraunCtx *ctx, Block *b, Symbol *sym);
static Value *read_var_recursive(BraunCtx *ctx, Block *b, Symbol *sym);
static Value *try_remove_trivial_phi(BraunCtx *ctx, Inst *phi);
static Value *add_phi_operands(BraunCtx *ctx, Symbol *sym, Inst *phi);

static Value *read_var(BraunCtx *ctx, Block *b, Symbol *sym) {
    for (int i = 0; i < b->ndef; i++)
        if (b->def_syms[i] == sym) return val_resolve(b->def_vals[i]);
    return read_var_recursive(ctx, b, sym);
}

static Value *read_var_recursive(BraunCtx *ctx, Block *b, Symbol *sym) {
    Value *v;
    if (!b->sealed) {
        // Incomplete phi
        ValType vt = sym->type ? type_to_valtype(sym->type) : VT_I16;
        if (vt == VT_VOID) vt = VT_I16;
        Value *dst = new_value(ctx->f, VAL_INST, vt);
        Inst  *phi = new_inst(ctx->f, b, IK_PHI, dst);
        // Prepend to front of block so phi stays before all non-phi instructions
        phi->next = b->head;
        phi->prev = NULL;
        if (b->head) b->head->prev = phi;
        else         b->tail = phi;
        b->head = phi;
        phi->block = b;
        write_var(b, sym, dst);
        if (b->niphi >= BRAUN_MAP_MAX) return dst;
        b->iphi_insts[b->niphi] = phi;
        b->iphi_syms [b->niphi] = sym;
        b->niphi++;
        return dst;
    }
    if (b->npreds == 0) {
        ValType vt = sym->type ? type_to_valtype(sym->type) : VT_I16;
        if (vt == VT_VOID) vt = VT_I16;
        Value *u = new_value(ctx->f, VAL_CONST, vt);
        u->iconst = 0;
        write_var(b, sym, u);
        return u;
    }
    if (b->npreds == 1) {
        v = read_var(ctx, b->preds[0], sym);
        write_var(b, sym, v);
        return v;
    }
    ValType vt = sym->type ? type_to_valtype(sym->type) : VT_I16;
    if (vt == VT_VOID) vt = VT_I16;
    Value *dst = new_value(ctx->f, VAL_INST, vt);
    Inst  *phi = new_inst(ctx->f, b, IK_PHI, dst);
    phi->next   = b->head;
    phi->prev   = NULL;
    if (b->head) b->head->prev = phi;
    else         b->tail = phi;
    b->head = phi;
    phi->block = b;
    write_var(b, sym, dst);
    v = add_phi_operands(ctx, sym, phi);
    write_var(b, sym, v);
    return v;
}

static Value *add_phi_operands(BraunCtx *ctx, Symbol *sym, Inst *phi) {
    Block *b = phi->block;
    for (int i = 0; i < b->npreds; i++) {
        Value *pv = read_var(ctx, b->preds[i], sym);
        inst_add_op(phi, pv);
    }
    return try_remove_trivial_phi(ctx, phi);
}

static Value *try_remove_trivial_phi(BraunCtx *ctx, Inst *phi) {
    Value *same = NULL;
    for (int i = 0; i < phi->nops; i++) {
        Value *op = val_resolve(phi->ops[i]);
        if (op == phi->dst || op == same) continue;
        if (same) return phi->dst;
        same = op;
    }
    if (!same) same = new_value(ctx->f, VAL_UNDEF, phi->dst->vtype);
    phi->dst->alias = same;
    phi->is_dead    = 1;
    return same;
}

static void seal_block(BraunCtx *ctx, Block *b) {
    for (int i = 0; i < b->niphi; i++)
        add_phi_operands(ctx, b->iphi_syms[i], b->iphi_insts[i]);
    b->niphi = 0;
    b->sealed = 1;
}

// ============================================================
// Emit helpers (reused patterns)
// ============================================================

static bool can_fold_binop(InstKind kind) {
    switch (kind) {
    case IK_ADD: case IK_SUB: case IK_MUL:
    case IK_AND: case IK_OR:  case IK_XOR:
    case IK_SHL: case IK_SHR: case IK_USHR:
    case IK_EQ:  case IK_NE:
    case IK_LT:  case IK_ULT:
    case IK_LE:  case IK_ULE:
        return true;
    default: return false;
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
    case IK_SHL:  return (int32_t)(ua << (ub & 31));
    case IK_SHR:  return (int32_t)(ua >> (ub & 31));
    case IK_USHR: return (int32_t)(ua >> (ub & 31));
    case IK_EQ:   return a == b ? 1 : 0;
    case IK_NE:   return a != b ? 1 : 0;
    case IK_LT:   return a < b ? 1 : 0;
    case IK_ULT:  return ua < ub ? 1 : 0;
    case IK_LE:   return a <= b ? 1 : 0;
    case IK_ULE:  return ua <= ub ? 1 : 0;
    default: return 0;
    }
}

static Value *emit_binop(BraunCtx *ctx, Block *b, InstKind kind, Value *lhs, Value *rhs, ValType vt) {
    if (lhs && lhs->kind == VAL_CONST && rhs && rhs->kind == VAL_CONST && can_fold_binop(kind)) {
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
    if (v && v->kind == VAL_CONST && kind == IK_NEG)
        return new_const(ctx->f, -v->iconst, vt);
    if (v && v->kind == VAL_CONST && kind == IK_NOT)
        return new_const(ctx->f, !v->iconst, vt);
    Value *dst  = new_value(ctx->f, VAL_INST, vt);
    Inst  *inst = new_inst(ctx->f, b, kind, dst);
    inst_add_op(inst, v);
    inst_append(b, inst);
    return dst;
}

static Value *emit_load(BraunCtx *ctx, Block *b, Value *ptr, int size, int offset, ValType vt) {
    Value *dst  = new_value(ctx->f, VAL_INST, vt);
    Inst  *inst = new_inst(ctx->f, b, IK_LOAD, dst);
    inst_add_op(inst, ptr);
    inst->imm  = offset;
    inst->size = size;
    inst_append(b, inst);
    return dst;
}

static void emit_store(BraunCtx *ctx, Block *b, Value *ptr, Value *val, int size, int offset) {
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

static void emit_jmp(Block *from, Block *to) {
    block_add_succ(from, to);
    block_add_pred(to, from);
    if (from->filled) return;
    Inst *inst = arena_alloc(sizeof(Inst));
    inst->kind   = IK_JMP;
    inst->target = to;
    inst->block  = from;
    inst_append(from, inst);
    from->filled = 1;
}

static void emit_br(Function *f, Block *cond_blk, Value *cond_val,
                    Block *then_blk, Block *else_blk) {
    block_add_succ(cond_blk, then_blk); block_add_pred(then_blk, cond_blk);
    block_add_succ(cond_blk, else_blk); block_add_pred(else_blk, cond_blk);
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

// Map token_kind binary op to InstKind
static InstKind tok_binop_kind(Token_kind tk, ValType vt) {
    bool is_unsigned = (vt == VT_U8 || vt == VT_U16 || vt == VT_U32 || vt == VT_PTR);
    bool is_float    = (vt == VT_F32);
    if (is_float) {
        switch (tk) {
        case TK_PLUS:     return IK_FADD;
        case TK_MINUS:    return IK_FSUB;
        case TK_STAR:     return IK_FMUL;
        case TK_SLASH:    return IK_FDIV;
        case TK_LT:       return IK_FLT;
        case TK_LE:       return IK_FLE;
        case TK_GT:       return IK_FLT;  // operands swapped by caller
        case TK_GE:       return IK_FLE;  // operands swapped by caller
        case TK_EQ:       return IK_FEQ;
        case TK_NE:       return IK_FNE;
        default: break;
        }
    }
    switch (tk) {
    case TK_PLUS:      return IK_ADD;
    case TK_MINUS:     return IK_SUB;
    case TK_STAR:      return IK_MUL;
    case TK_SLASH:     return is_unsigned ? IK_UDIV : IK_DIV;
    case TK_PERCENT:   return is_unsigned ? IK_UMOD : IK_MOD;
    case TK_AMPERSAND: return IK_AND;
    case TK_BITOR:     return IK_OR;
    case TK_BITXOR:    return IK_XOR;
    case TK_SHIFTL:    return IK_SHL;
    case TK_SHIFTR:    return is_unsigned ? IK_USHR : IK_SHR;
    case TK_EQ:        return IK_EQ;
    case TK_NE:        return IK_NE;
    case TK_LT:        return is_unsigned ? IK_ULT : IK_LT;
    case TK_LE:        return is_unsigned ? IK_ULE : IK_LE;
    case TK_GT:        return is_unsigned ? IK_ULT : IK_LT;  // operands swapped
    case TK_GE:        return is_unsigned ? IK_ULE : IK_LE;  // operands swapped
    default:           return IK_ADD;
    }
}

// ============================================================
// Forward declarations
// ============================================================

static Value *cg_expr(BraunCtx *ctx, Block **cur, Node *n);
static Block *cg_stmt(BraunCtx *ctx, Block *b, Node *n);
static Value *cg_addr(BraunCtx *ctx, Block **cur, Node *n);
static Value *cg_logand(BraunCtx *ctx, Block **cur, Node *lhs, Node *rhs, bool is_or);

// ============================================================
// Build CallDesc inline from function Type*
// ============================================================

static CallDesc *make_calldesc(Type *fn_type) {
    if (!fn_type) return NULL;
    if (fn_type->base == TB_POINTER) fn_type = fn_type->u.ptr.pointee;
    if (!fn_type || fn_type->base != TB_FUNCTION) return NULL;
    CallDesc *cd = arena_alloc(sizeof(CallDesc));
    cd->return_type = fn_type->u.fn.ret;
    int n = 0;
    for (Param *p = fn_type->u.fn.params; p; p = p->next) n++;
    cd->nparams     = n;
    cd->param_types = n ? arena_alloc(n * sizeof(Type *)) : NULL;
    int i = 0;
    for (Param *p = fn_type->u.fn.params; p; p = p->next)
        cd->param_types[i++] = p->type;
    cd->is_variadic = fn_type->u.fn.is_variadic;
    cd->hidden_sret = (cd->return_type && cd->return_type->base == TB_STRUCT);
    return cd;
}

// ============================================================
// Helper: emit args list (Node* linked via ->next) into call inst
// ============================================================

static void cg_call_args(BraunCtx *ctx, Block **cur, Inst *call, Node *args_head) {
    for (Node *a = args_head; a; a = a->next) {
        Value *av = cg_expr(ctx, cur, a);
        inst_add_op(call, av);
    }
}

// ============================================================
// Helper: insert pre-colored IK_COPY instructions for register ABI args
// Called before inst_append(b, call) so IRC liveness sees the copies.
// For IK_CALL: ops[0..2] → r1/r2/r3 (first 3 scalar args)
// For IK_ICALL: ops[1..3] → r1/r2/r3, then ops[0] (fp) → r0 (LAST so fp
//   remains live through the arg copies, forcing fp ∉ {r1,r2,r3} via interference)
// Skipped for variadic calls — all args go to the stack.
// ============================================================

static void emit_reg_arg_copies(BraunCtx *ctx, Block *b, Inst *call) {
    bool is_icall = (call->kind == IK_ICALL);
    bool is_var   = call->calldesc && call->calldesc->is_variadic;
    if (is_var) return;

    int arg_base = is_icall ? 1 : 0;
    int nargs    = call->nops - arg_base;
    int nreg     = nargs < 3 ? nargs : 3;

    for (int i = 0; i < nreg; i++) {
        Value *arg = val_resolve(call->ops[arg_base + i]);
        if (!arg) continue;
        Value *v_ri = new_value(ctx->f, VAL_INST, arg->vtype);
        v_ri->phys_reg = i + 1;           // pre-color to r{i+1}
        Inst  *cp = new_inst(ctx->f, b, IK_COPY, v_ri);
        inst_add_op(cp, arg);
        inst_append(b, cp);
        call->ops[arg_base + i] = v_ri;   // redirect call to pre-colored value
    }

    if (is_icall) {
        // fp copy emitted AFTER arg copies so fp is live through them.
        Value *fp = val_resolve(call->ops[0]);
        if (fp) {
            Value *v_r0 = new_value(ctx->f, VAL_INST, fp->vtype);
            v_r0->phys_reg = 0;           // pre-color to r0
            Inst  *fp_cp = new_inst(ctx->f, b, IK_COPY, v_r0);
            inst_add_op(fp_cp, fp);
            inst_append(b, fp_cp);
            call->ops[0] = v_r0;
        }
    }
}

// ============================================================
// Helper: emit a complete call (direct or indirect) with landing+copy
// ============================================================

static Value *emit_call_result(BraunCtx *ctx, Block *b, Inst *call, Type *ret_type) {
    emit_reg_arg_copies(ctx, b, call);
    inst_append(b, call);
    ValType call_vt = ret_type ? type_to_valtype(ret_type) : VT_I16;
    if (call_vt == VT_VOID) call_vt = VT_I16;
    call->dst->vtype = call_vt;
    // Working copy — freely allocated by IRC
    Value *working = new_value(ctx->f, VAL_INST, call_vt);
    Inst  *cp = new_inst(ctx->f, b, IK_COPY, working);
    inst_add_op(cp, call->dst);
    inst_append(b, cp);
    return working;
}

// ============================================================
// cg_addr — compute address of an lvalue
// Returns Value* = address (not the loaded value)
// ============================================================

static Value *cg_addr(BraunCtx *ctx, Block **cur, Node *n) {
    if (!n) return new_value(ctx->f, VAL_UNDEF, VT_PTR);
    Block *b = *cur;

    if (n->kind == ND_IDENT) {
        Symbol *sym = n->symbol;
        if (!sym) return new_value(ctx->f, VAL_UNDEF, VT_PTR);

        // Global / static: emit IK_GADDR
        if (sym->kind == SYM_GLOBAL || sym->kind == SYM_EXTERN ||
            sym->kind == SYM_STATIC_GLOBAL || sym->kind == SYM_STATIC_LOCAL ||
            sym->kind == SYM_BUILTIN) {
            Value *dst = new_value(ctx->f, VAL_INST, VT_PTR);
            Inst  *inst = new_inst(ctx->f, b, IK_GADDR, dst);
            inst->fname = (char *)sym_label(ctx->tu_index, sym);
            inst_append(b, inst);
            return dst;
        }

        // Struct-type non-variadic param: SSA var holds the pointer
        if (sym->kind == SYM_PARAM && sym->type && sym->type->base == TB_STRUCT &&
            vparam_off(ctx, sym) < 0) {
            return val_resolve(read_var(ctx, b, sym));
        }

        // Addr-taken param: use home slot
        if (sym->kind == SYM_PARAM && is_addr_taken(ctx, sym)) {
            int voff = vparam_off(ctx, sym);
            int bpoff = (voff >= 0) ? voff : param_home_off(ctx, sym);
            Value *dst = new_value(ctx->f, VAL_INST, VT_PTR);
            Inst  *inst = new_inst(ctx->f, b, IK_ADDR, dst);
            inst->imm = bpoff;
            inst_append(b, inst);
            return dst;
        }

        // Non-addr-taken param (non-variadic): IK_ADDR with sym->offset
        // (but typically cg_addr on a non-addr-taken param should not happen)
        if (sym->kind == SYM_PARAM) {
            int voff = vparam_off(ctx, sym);
            int bpoff = (voff >= 0) ? voff : sym->offset;
            Value *dst = new_value(ctx->f, VAL_INST, VT_PTR);
            Inst  *inst = new_inst(ctx->f, b, IK_ADDR, dst);
            inst->imm = bpoff;
            inst_append(b, inst);
            return dst;
        }

        // Local: IK_ADDR at negative bp offset
        {
            Value *dst = new_value(ctx->f, VAL_INST, VT_PTR);
            Inst  *inst = new_inst(ctx->f, b, IK_ADDR, dst);
            inst->imm = -(sym->offset);
            inst_append(b, inst);
            return dst;
        }
    }

    if (n->kind == ND_UNARYOP) {
        if (n->op_kind == TK_STAR)
            return cg_expr(ctx, cur, n->ch[0]);  // *p → address is p
        if (n->op_kind == TK_PLUS && n->u.unaryop.is_array_deref)
            return cg_expr(ctx, cur, n->ch[0]);  // non-last array dim → address
    }

    if (n->kind == ND_MEMBER) {
        int offset = n->u.member.offset;
        Value *base;
        if (n->op_kind == TK_ARROW)
            base = cg_expr(ctx, cur, n->ch[0]);
        else
            base = cg_addr(ctx, cur, n->ch[0]);
        b = *cur;
        if (offset == 0) return base;
        Value *off_v = new_const(ctx->f, offset, VT_I16);
        return emit_binop(ctx, b, IK_ADD, base, off_v, VT_PTR);
    }

    // Fallback: evaluate as expression (pointer result)
    return cg_expr(ctx, cur, n);
}

// ============================================================
// cg_expr — generate SSA value for expression Node*
// ============================================================

static Value *cg_expr(BraunCtx *ctx, Block **cur, Node *n) {
    if (!n || !(*cur)) return new_value(ctx->f, VAL_UNDEF, VT_I16);

    Block *b = *cur;
    ValType vt = n->type ? type_to_valtype(n->type) : VT_I16;
    if (vt == VT_VOID) vt = VT_I16;

    switch (n->kind) {

    // ----------------------------------------------------------
    case ND_LITERAL: {
        if (n->u.literal.strval) {
            // String literal → gaddr "_lN"
            int lid = (*ctx->strlit_id)++;
            strlits_push(lid, n->u.literal.strval, n->u.literal.strval_len);
            char buf[32]; snprintf(buf, sizeof(buf), "_l%d", lid);
            Value *dst = new_value(ctx->f, VAL_INST, VT_PTR);
            Inst  *inst = new_inst(ctx->f, b, IK_GADDR, dst);
            inst->fname = arena_strdup(buf);
            inst_append(b, inst);
            return dst;
        }
        if (n->type && (n->type->base == TB_FLOAT || n->type->base == TB_DOUBLE)) {
            float fv = (float)n->u.literal.fval;
            uint32_t bits; memcpy(&bits, &fv, 4);
            return new_const(ctx->f, (int)bits, VT_F32);
        }
        return new_const(ctx->f, (int)n->u.literal.ival, vt);
    }

    // ----------------------------------------------------------
    case ND_IDENT: {
        Symbol *sym = n->symbol;
        if (!sym) return new_value(ctx->f, VAL_UNDEF, vt);

        // Enum constant
        if (sym->kind == SYM_ENUM_CONST)
            return new_const(ctx->f, sym->offset, vt);

        // Direct function call: foo(args)
        if (n->u.ident.is_function) {
            Node *args_head = n->ch[0];
            ValType call_vt = n->type ? type_to_valtype(n->type) : VT_I16;
            if (call_vt == VT_VOID) call_vt = VT_I16;

            // Builtin putchar
            if (sym->kind == SYM_BUILTIN &&
                (strcmp(sym->name, "putchar") == 0 || strcmp(sym->name, "__putchar") == 0)) {
                Value *arg = cg_expr(ctx, cur, args_head); b = *cur;
                Inst *inst = new_inst(ctx->f, b, IK_PUTCHAR, NULL);
                inst_add_op(inst, arg);
                inst_append(b, inst);
                return new_const(ctx->f, 0, VT_I16);
            }

            if (istype_function(sym->type)) {
                // Direct call
                Value *landing = new_value(ctx->f, VAL_INST, call_vt);
                Inst  *call = new_inst(ctx->f, b, IK_CALL, landing);
                call->fname    = (char *)sym_label(ctx->tu_index, sym);
                call->calldesc = make_calldesc(sym->type);
                cg_call_args(ctx, cur, call, args_head); b = *cur;
                return emit_call_result(ctx, b, call, n->type);
            } else if (istype_ptr(sym->type)) {
                // Indirect call via fn-ptr variable
                Value *fp = read_var(ctx, b, sym); fp = val_resolve(fp);
                Value *landing = new_value(ctx->f, VAL_INST, call_vt);
                Inst  *call = new_inst(ctx->f, b, IK_ICALL, landing);
                Type *ftype = sym->type->u.ptr.pointee;
                call->calldesc = make_calldesc(ftype);
                inst_add_op(call, fp);
                cg_call_args(ctx, cur, call, args_head); b = *cur;
                return emit_call_result(ctx, b, call, n->type);
            }
        }

        // Array or struct: yield base address
        if (istype_array(sym->type) || (sym->type && sym->type->base == TB_STRUCT))
            return cg_addr(ctx, cur, n);

        // Function designator used as value (not called): yield address
        if (istype_function(sym->type))
            return cg_addr(ctx, cur, n);

        // Global scalar
        if (sym->kind == SYM_GLOBAL || sym->kind == SYM_EXTERN ||
            sym->kind == SYM_STATIC_GLOBAL || sym->kind == SYM_STATIC_LOCAL) {
            Value *addr = new_value(ctx->f, VAL_INST, VT_PTR);
            Inst  *ga = new_inst(ctx->f, b, IK_GADDR, addr);
            ga->fname = (char *)sym_label(ctx->tu_index, sym);
            inst_append(b, ga);
            int sz = sym->type ? sym->type->size : 2;
            return emit_load(ctx, b, addr, sz, 0, vt);
        }

        // Local or param — addr-taken: load from memory
        if (is_addr_taken(ctx, sym)) {
            int bpoff;
            if (sym->kind == SYM_PARAM) {
                int voff = vparam_off(ctx, sym);
                bpoff = (voff >= 0) ? voff : param_home_off(ctx, sym);
            } else {
                bpoff = -(sym->offset);
            }
            Value *addr = new_value(ctx->f, VAL_INST, VT_PTR);
            Inst  *ai = new_inst(ctx->f, b, IK_ADDR, addr);
            ai->imm = bpoff;
            inst_append(b, ai);
            int sz = sym->type ? sym->type->size : 2;
            return emit_load(ctx, b, addr, sz, 0, vt);
        }

        // SSA variable (local or register param)
        return val_resolve(read_var(ctx, b, sym));
    }

    // ----------------------------------------------------------
    case ND_UNARYOP: {
        Token_kind op = n->op_kind;

        // Indirect call: (*fp)(args) — is_function=true, ch[0]=fp, ch[1]=args
        if (op == TK_STAR && n->u.unaryop.is_function) {
            Node *args_head = n->ch[1];
            ValType call_vt = n->type ? type_to_valtype(n->type) : VT_I16;
            if (call_vt == VT_VOID) call_vt = VT_I16;

            Value *fp = cg_expr(ctx, cur, n->ch[0]); b = *cur;
            // If fp is from a binop (array subscript of fn ptrs), it might need a load
            if (n->ch[0]->kind == ND_BINOP) {
                fp = emit_load(ctx, b, fp, 2, 0, VT_PTR);
            }
            Value *landing = new_value(ctx->f, VAL_INST, call_vt);
            Inst  *call = new_inst(ctx->f, b, IK_ICALL, landing);
            Type *ftype = n->ch[0]->type;
            if (ftype && istype_ptr(ftype)) ftype = ftype->u.ptr.pointee;
            call->calldesc = make_calldesc(ftype);
            inst_add_op(call, fp);
            cg_call_args(ctx, cur, call, args_head); b = *cur;
            return emit_call_result(ctx, b, call, n->type);
        }

        // Array dereference (non-last dim): address only, no load
        if (op == TK_STAR && n->u.unaryop.is_array_deref)
            return cg_expr(ctx, cur, n->ch[0]);

        // Dereference: load from pointer
        if (op == TK_STAR) {
            Value *ptr = cg_expr(ctx, cur, n->ch[0]); b = *cur;
            int sz = n->type ? n->type->size : 2;
            return emit_load(ctx, b, ptr, sz, 0, vt);
        }

        // Address-of
        if (op == TK_AMPERSAND)
            return cg_addr(ctx, cur, n->ch[0]);

        // Unary plus — no-op
        if (op == TK_PLUS)
            return cg_expr(ctx, cur, n->ch[0]);

        // Unary minus
        if (op == TK_MINUS) {
            Value *v = cg_expr(ctx, cur, n->ch[0]); b = *cur;
            if (vt == VT_F32) {
                // -x = 0.0 - x
                Value *zero = new_const(ctx->f, 0, VT_F32);
                return emit_binop(ctx, b, IK_FSUB, zero, v, VT_F32);
            }
            return emit_unop(ctx, b, IK_NEG, v, vt);
        }

        // Bitwise NOT: x ^ -1
        if (op == TK_TILDE) {
            Value *v = cg_expr(ctx, cur, n->ch[0]); b = *cur;
            Value *minus1 = new_const(ctx->f, -1, vt);
            return emit_binop(ctx, b, IK_XOR, v, minus1, vt);
        }

        // Logical NOT: !x = (x == 0)
        if (op == TK_BANG) {
            Value *v = cg_expr(ctx, cur, n->ch[0]); b = *cur;
            Value *zero = new_const(ctx->f, 0, VT_I16);
            return emit_binop(ctx, b, IK_EQ, v, zero, VT_I16);
        }

        // Pre-increment / pre-decrement
        if (op == TK_INC || op == TK_DEC) {
            InstKind bop = (op == TK_INC) ? IK_ADD : IK_SUB;
            int step = 1;
            Type *ot = n->ch[0]->type;
            if (ot && ot->base == TB_POINTER && ot->u.ptr.pointee)
                step = ot->u.ptr.pointee->size;
            else if (ot && ot->base == TB_ARRAY && ot->u.arr.elem)
                step = ot->u.arr.elem->size;
            Value *step_v = new_const(ctx->f, step, vt);

            // Compute: load old, compute new, store back, return new
            if (n->ch[0]->kind == ND_IDENT && n->ch[0]->symbol &&
                (n->ch[0]->symbol->kind == SYM_LOCAL || n->ch[0]->symbol->kind == SYM_PARAM) &&
                !is_addr_taken(ctx, n->ch[0]->symbol)) {
                // SSA path
                Symbol *sym = n->ch[0]->symbol;
                Value *old_v = read_var(ctx, b, sym);
                Value *new_v = emit_binop(ctx, b, bop, old_v, step_v, vt);
                write_var(b, sym, new_v);
                return new_v;
            } else {
                // Memory path
                Value *addr = cg_addr(ctx, cur, n->ch[0]); b = *cur;
                int sz = n->ch[0]->type ? n->ch[0]->type->size : 2;
                Value *old_v = emit_load(ctx, b, addr, sz, 0, vt);
                Value *new_v = emit_binop(ctx, b, bop, old_v, step_v, vt);
                emit_store(ctx, b, addr, new_v, sz, 0);
                return new_v;
            }
        }

        // Post-increment / post-decrement
        if (op == TK_POST_INC || op == TK_POST_DEC) {
            InstKind bop = (op == TK_POST_INC) ? IK_ADD : IK_SUB;
            int step = 1;
            Type *ot = n->ch[0]->type;
            if (ot && ot->base == TB_POINTER && ot->u.ptr.pointee)
                step = ot->u.ptr.pointee->size;
            else if (ot && ot->base == TB_ARRAY && ot->u.arr.elem)
                step = ot->u.arr.elem->size;
            Value *step_v = new_const(ctx->f, step, vt);

            if (n->ch[0]->kind == ND_IDENT && n->ch[0]->symbol &&
                (n->ch[0]->symbol->kind == SYM_LOCAL || n->ch[0]->symbol->kind == SYM_PARAM) &&
                !is_addr_taken(ctx, n->ch[0]->symbol)) {
                // SSA path: save old, write new, return old
                Symbol *sym = n->ch[0]->symbol;
                Value *old_v = read_var(ctx, b, sym);
                Value *new_v = emit_binop(ctx, b, bop, old_v, step_v, vt);
                write_var(b, sym, new_v);
                return old_v;
            } else {
                // Memory path
                Value *addr = cg_addr(ctx, cur, n->ch[0]); b = *cur;
                int sz = n->ch[0]->type ? n->ch[0]->type->size : 2;
                Value *old_v = emit_load(ctx, b, addr, sz, 0, vt);
                Value *new_v = emit_binop(ctx, b, bop, old_v, step_v, vt);
                emit_store(ctx, b, addr, new_v, sz, 0);
                return old_v;
            }
        }

        return new_value(ctx->f, VAL_UNDEF, vt);
    }

    // ----------------------------------------------------------
    case ND_BINOP: {
        Token_kind op = n->op_kind;

        // Comma: eval lhs for side effects, return rhs
        if (op == TK_COMMA) {
            cg_expr(ctx, cur, n->ch[0]);  b = *cur;
            return cg_expr(ctx, cur, n->ch[1]);
        }

        // Short-circuit logical ops
        if (op == TK_LOGAND) return cg_logand(ctx, cur, n->ch[0], n->ch[1], false);
        if (op == TK_LOGOR)  return cg_logand(ctx, cur, n->ch[0], n->ch[1], true);

        Value *lv = cg_expr(ctx, cur, n->ch[0]); b = *cur;
        Value *rv = cg_expr(ctx, cur, n->ch[1]); b = *cur;

        ValType op_vt = lv->vtype != VT_VOID ? lv->vtype : vt;

        // GT/GE: swap operands (use LT/LE with args reversed)
        bool swap = (op == TK_GT || op == TK_GE);
        if (swap) { Value *tmp = lv; lv = rv; rv = tmp; }

        InstKind kind = tok_binop_kind(op, op_vt);
        // Comparison results are always i16
        ValType result_vt = vt;
        if (kind >= IK_LT && kind <= IK_NE) result_vt = VT_I16;
        if (kind >= IK_FLT && kind <= IK_FNE) result_vt = VT_I16;
        if (kind == IK_ULT || kind == IK_ULE) result_vt = VT_I16;
        if (result_vt == VT_VOID) result_vt = VT_I16;

        return emit_binop(ctx, b, kind, lv, rv, result_vt);
    }

    // ----------------------------------------------------------
    case ND_ASSIGN: {
        // Struct assignment: memcpy
        if (n->ch[0]->type && n->ch[0]->type->base == TB_STRUCT) {
            Value *src = cg_expr(ctx, cur, n->ch[1]); b = *cur;
            Value *dst_v = cg_addr(ctx, cur, n->ch[0]); b = *cur;
            Inst *mc = new_inst(ctx->f, b, IK_MEMCPY, NULL);
            mc->imm  = n->ch[0]->type->size;
            mc->size = n->ch[0]->type->size;
            inst_add_op(mc, dst_v);
            inst_add_op(mc, src);
            inst_append(b, mc);
            return src;
        }

        Value *rhs = cg_expr(ctx, cur, n->ch[1]); b = *cur;

        // SSA assign (non-addr-taken local/param scalar)
        if (n->ch[0]->kind == ND_IDENT && n->ch[0]->symbol &&
            (n->ch[0]->symbol->kind == SYM_LOCAL || n->ch[0]->symbol->kind == SYM_PARAM) &&
            !istype_array(n->ch[0]->symbol->type) &&
            !(n->ch[0]->symbol->type && n->ch[0]->symbol->type->base == TB_STRUCT) &&
            !is_addr_taken(ctx, n->ch[0]->symbol)) {
            Symbol *sym = n->ch[0]->symbol;
            write_var(b, sym, rhs);
            return rhs;
        }

        // Memory assign
        Value *addr = cg_addr(ctx, cur, n->ch[0]); b = *cur;
        int sz = n->ch[0]->type ? n->ch[0]->type->size : 2;
        emit_store(ctx, b, addr, rhs, sz, 0);
        return rhs;
    }

    // ----------------------------------------------------------
    case ND_COMPOUND_ASSIGN: {
        Token_kind base_op = n->op_kind;
        ValType lhs_vt = n->ch[0]->type ? type_to_valtype(n->ch[0]->type) : VT_I16;
        if (lhs_vt == VT_VOID) lhs_vt = VT_I16;

        Value *rhs = cg_expr(ctx, cur, n->ch[1]); b = *cur;

        // SSA path for non-addr-taken scalar local/param
        if (n->ch[0]->kind == ND_IDENT && n->ch[0]->symbol &&
            (n->ch[0]->symbol->kind == SYM_LOCAL || n->ch[0]->symbol->kind == SYM_PARAM) &&
            !istype_array(n->ch[0]->symbol->type) &&
            !(n->ch[0]->symbol->type && n->ch[0]->symbol->type->base == TB_STRUCT) &&
            !is_addr_taken(ctx, n->ch[0]->symbol)) {
            Symbol *sym = n->ch[0]->symbol;
            Value *old_v = read_var(ctx, b, sym);
            bool swap = (base_op == TK_GT || base_op == TK_GE);
            Value *lv = swap ? rhs : old_v;
            Value *rv = swap ? old_v : rhs;
            InstKind kind = tok_binop_kind(base_op, lhs_vt);
            Value *new_v = emit_binop(ctx, b, kind, lv, rv, lhs_vt);
            write_var(b, sym, new_v);
            return new_v;
        }

        // Memory path — compute addr once, load, op, store, reload
        Value *addr = cg_addr(ctx, cur, n->ch[0]); b = *cur;
        // Save addr in a fresh SSA value to avoid re-evaluating side effects
        Value *addr_saved = new_value(ctx->f, VAL_INST, VT_PTR);
        Inst  *addr_copy = new_inst(ctx->f, b, IK_COPY, addr_saved);
        inst_add_op(addr_copy, addr);
        inst_append(b, addr_copy);

        int sz = n->ch[0]->type ? n->ch[0]->type->size : 2;
        Value *old_v = emit_load(ctx, b, addr_saved, sz, 0, lhs_vt);
        bool swap = (base_op == TK_GT || base_op == TK_GE);
        Value *lv = swap ? rhs : old_v;
        Value *rv = swap ? old_v : rhs;
        InstKind kind = tok_binop_kind(base_op, lhs_vt);
        Value *new_v = emit_binop(ctx, b, kind, lv, rv, lhs_vt);
        emit_store(ctx, b, addr_saved, new_v, sz, 0);
        return new_v;
    }

    // ----------------------------------------------------------
    case ND_CAST: {
        // ch[1] = expression; ch[0] = type declaration (for target type)
        ValType src_vt;
        Value *v = cg_expr(ctx, cur, n->ch[1]); b = *cur;
        src_vt = v->vtype;

        if (vt == VT_VOID || vt == src_vt) return v;

        InstKind kind = IK_COPY;
        if (vt == VT_F32 && src_vt != VT_F32)                     kind = IK_ITOF;
        else if (vt != VT_F32 && src_vt == VT_F32)                kind = IK_FTOI;
        else if ((vt == VT_I8 || vt == VT_U8) && src_vt != VT_I8
                 && src_vt != VT_U8)                               kind = IK_TRUNC;
        else if ((vt == VT_I16 || vt == VT_U16) &&
                 (src_vt == VT_I32 || src_vt == VT_U32))          kind = IK_TRUNC;
        else if ((vt == VT_U32 && src_vt == VT_I32) ||
                 (vt == VT_I32 && src_vt == VT_U32))              kind = IK_COPY;
        else if (vt == VT_I8  && src_vt == VT_U8)                 kind = IK_SEXT8;
        else if (vt == VT_I16 && src_vt == VT_U16)                kind = IK_SEXT16;
        else if (vt == VT_U8  && src_vt == VT_I8)                 kind = IK_TRUNC;
        else if (vt == VT_U16 && src_vt == VT_I16)                kind = IK_TRUNC;
        else if (src_vt == VT_I8)                                  kind = IK_SEXT8;
        else if (src_vt == VT_I16)                                 kind = IK_SEXT16;

        Value *dst = new_value(ctx->f, VAL_INST, vt);
        Inst  *inst = new_inst(ctx->f, b, kind, dst);
        inst->imm = (vt == VT_I8 || vt == VT_U8) ? 0xff :
                    (vt == VT_I16 || vt == VT_U16) ? 0xffff : 0;
        inst_add_op(inst, v);
        inst_append(b, inst);
        return dst;
    }

    // ----------------------------------------------------------
    case ND_TERNARY: {
        Block *then_blk = new_block(ctx->f);
        Block *else_blk = new_block(ctx->f);
        Block *merge    = new_block(ctx->f);

        Value *cond = cg_expr(ctx, cur, n->ch[0]); b = *cur;
        emit_br(ctx->f, b, cond, then_blk, else_blk);
        seal_block(ctx, then_blk);
        seal_block(ctx, else_blk);

        *cur = then_blk;
        Value *then_v = cg_expr(ctx, cur, n->ch[1]);
        Block *then_exit = *cur;
        if (!then_exit->filled) {
            block_add_succ(then_exit, merge);
            block_add_pred(merge, then_exit);
            Inst *j = arena_alloc(sizeof(Inst));
            j->kind = IK_JMP; j->target = merge; j->block = then_exit;
            inst_append(then_exit, j); then_exit->filled = 1;
        }

        *cur = else_blk;
        Value *else_v = cg_expr(ctx, cur, n->ch[2]);
        Block *else_exit = *cur;
        if (!else_exit->filled) {
            block_add_succ(else_exit, merge);
            block_add_pred(merge, else_exit);
            Inst *j = arena_alloc(sizeof(Inst));
            j->kind = IK_JMP; j->target = merge; j->block = else_exit;
            inst_append(else_exit, j); else_exit->filled = 1;
        }

        seal_block(ctx, merge);
        *cur = merge;

        if (then_v && else_v) {
            ValType phi_vt = vt != VT_VOID ? vt : then_v->vtype;
            Value *phi_dst = new_value(ctx->f, VAL_INST, phi_vt);
            Inst  *phi     = new_inst(ctx->f, merge, IK_PHI, phi_dst);
            inst_add_op(phi, then_v);
            inst_add_op(phi, else_v);
            phi->next = merge->head; phi->prev = NULL;
            if (merge->head) merge->head->prev = phi; else merge->tail = phi;
            merge->head = phi; phi->block = merge;
            return phi_dst;
        }
        return then_v ? then_v : new_const(ctx->f, 0, VT_I16);
    }

    // ----------------------------------------------------------
    case ND_MEMBER: {
        // Function pointer member call: s.fp(args) or s->fp(args)
        if (n->u.member.is_function) {
            Node *args_head = n->ch[1];
            ValType call_vt = n->type ? type_to_valtype(n->type) : VT_I16;
            if (call_vt == VT_VOID) call_vt = VT_I16;
            Value *fp_addr = cg_addr(ctx, cur, n); b = *cur;
            Value *fp = emit_load(ctx, b, fp_addr, 2, 0, VT_PTR);
            Value *landing = new_value(ctx->f, VAL_INST, call_vt);
            Inst  *call = new_inst(ctx->f, b, IK_ICALL, landing);
            // Find function type from member
            Type *struct_t = n->ch[0]->type;
            if (struct_t && istype_ptr(struct_t)) struct_t = struct_t->u.ptr.pointee;
            Type *ftype = NULL;
            if (struct_t) find_offset(struct_t, n->u.member.field_name, &ftype);
            call->calldesc = make_calldesc(ftype);
            inst_add_op(call, fp);
            cg_call_args(ctx, cur, call, args_head); b = *cur;
            return emit_call_result(ctx, b, call, n->type);
        }

        // Struct or array member address → no load for struct/array members
        if (n->type && (n->type->base == TB_STRUCT || n->type->base == TB_ARRAY))
            return cg_addr(ctx, cur, n);

        // Load scalar member
        Value *addr = cg_addr(ctx, cur, n); b = *cur;
        int sz = n->type ? n->type->size : 2;
        return emit_load(ctx, b, addr, sz, 0, vt);
    }

    // ----------------------------------------------------------
    case ND_VA_ARG: {
        // va_arg(ap, T): load from *ap then advance ap by slot size
        // ap is n->ch[0]; requested type is n->type
        int asz = n->type ? n->type->size : 2;
        int slot = asz < 4 ? 4 : asz;  // CPU4: minimum slot = 4 bytes

        // ap is in ch[0]. Handle as an lvalue.
        // Get old ap value, save it, advance ap, load from saved pointer.
        bool ap_is_ssa = (n->ch[0]->kind == ND_IDENT && n->ch[0]->symbol &&
                          (n->ch[0]->symbol->kind == SYM_LOCAL ||
                           n->ch[0]->symbol->kind == SYM_PARAM) &&
                          !is_addr_taken(ctx, n->ch[0]->symbol));
        Symbol *ap_sym = ap_is_ssa ? n->ch[0]->symbol : NULL;

        Value *old_ap;
        if (ap_sym) {
            old_ap = read_var(ctx, b, ap_sym);
        } else {
            Value *ap_addr = cg_addr(ctx, cur, n->ch[0]); b = *cur;
            old_ap = emit_load(ctx, b, ap_addr, 2, 0, VT_PTR);
        }

        // Advance: new_ap = old_ap + slot
        Value *slot_v = new_const(ctx->f, slot, VT_I16);
        Value *new_ap = emit_binop(ctx, b, IK_ADD, old_ap, slot_v, VT_PTR);

        // Write back new ap
        if (ap_sym) {
            write_var(b, ap_sym, new_ap);
        } else {
            Value *ap_addr = cg_addr(ctx, cur, n->ch[0]); b = *cur;
            emit_store(ctx, b, ap_addr, new_ap, 2, 0);
        }

        // Load value from old ap
        return emit_load(ctx, b, old_ap, asz, 0, vt);
    }

    // ----------------------------------------------------------
    case ND_VA_START: {
        // va_start(ap, last): ap = &last + 4
        Value *last_addr = cg_addr(ctx, cur, n->ch[1]); b = *cur;
        Value *four = new_const(ctx->f, 4, VT_I16);
        Value *ap_val = emit_binop(ctx, b, IK_ADD, last_addr, four, VT_PTR);

        // Store into ap (ch[0])
        bool ap_is_ssa = (n->ch[0]->kind == ND_IDENT && n->ch[0]->symbol &&
                          (n->ch[0]->symbol->kind == SYM_LOCAL ||
                           n->ch[0]->symbol->kind == SYM_PARAM) &&
                          !is_addr_taken(ctx, n->ch[0]->symbol));
        if (ap_is_ssa) {
            write_var(b, n->ch[0]->symbol, ap_val);
        } else {
            Value *ap_addr = cg_addr(ctx, cur, n->ch[0]); b = *cur;
            emit_store(ctx, b, ap_addr, ap_val, 2, 0);
        }
        return new_const(ctx->f, 0, VT_I16);
    }

    case ND_VA_END:
        return new_const(ctx->f, 0, VT_I16);

    default:
        break;
    }

    return new_value(ctx->f, VAL_UNDEF, vt);
}

// ============================================================
// Short-circuit logical AND / OR
// ============================================================

static Value *cg_logand(BraunCtx *ctx, Block **cur, Node *lhs, Node *rhs, bool is_or) {
    Block *b = *cur;
    Block *rhs_blk = new_block(ctx->f);
    Block *end_blk = new_block(ctx->f);

    Value *lhs_v = cg_expr(ctx, cur, lhs); b = *cur;
    if (is_or) emit_br(ctx->f, b, lhs_v, end_blk, rhs_blk);
    else        emit_br(ctx->f, b, lhs_v, rhs_blk, end_blk);

    seal_block(ctx, rhs_blk);
    *cur = rhs_blk;
    Value *rhs_v = cg_expr(ctx, cur, rhs);
    Block *rhs_exit = *cur;
    // Normalize rhs to 0/1
    Value *zero = new_const(ctx->f, 0, VT_I16);
    Value *rhs_bool = emit_binop(ctx, rhs_exit, IK_NE, rhs_v, zero, VT_I16);
    if (!rhs_exit->filled) emit_jmp(rhs_exit, end_blk);

    seal_block(ctx, end_blk);
    *cur = end_blk;

    Value *phi_dst = new_value(ctx->f, VAL_INST, VT_I16);
    Inst  *phi     = new_inst(ctx->f, end_blk, IK_PHI, phi_dst);
    Value *lhs_contrib = is_or ? new_const(ctx->f, 1, VT_I16) : new_const(ctx->f, 0, VT_I16);
    inst_add_op(phi, lhs_contrib);
    inst_add_op(phi, rhs_bool);
    phi->next = end_blk->head; phi->prev = NULL;
    if (end_blk->head) end_blk->head->prev = phi; else end_blk->tail = phi;
    end_blk->head = phi; phi->block = end_blk;
    return phi_dst;
}

// ============================================================
// Recursive init helpers (forward declarations)
// ============================================================

static void cg_fill_array(BraunCtx *ctx, Block **cur, Value *base_addr,
                           Type *ty, Node *init_list, int *byte_off);
static void cg_fill_struct(BraunCtx *ctx, Block **cur, Value *base_addr, int base_off,
                            Type *ty, Node *init_list);

// Fill array elements from init_list. *byte_off is the current ABSOLUTE byte offset
// from base_addr. Handles nested ND_INITLIST for multi-dim arrays (rounding up to
// elem_size boundaries as required by C initializer rules).
static void cg_fill_array(BraunCtx *ctx, Block **cur, Value *base_addr,
                           Type *ty, Node *init_list, int *byte_off)
{
    if (!ty || !istype_array(ty) || !init_list) return;
    Type *elem_type = ty->u.arr.elem;
    int   elem_size = elem_type ? elem_type->size : 2;
    Type *leaf_type = array_elem_type(ty);
    int   lsz       = (leaf_type && leaf_type->size > 0) ? leaf_type->size : 2;

    for (Node *item = init_list->ch[0]; item; item = item->next) {
        Node *raw = item;
        while (raw && raw->kind == ND_CAST) raw = raw->ch[1];

        if (raw && raw->kind == ND_INITLIST && elem_type && istype_array(elem_type)) {
            // Inner braces: advance to next elem_size boundary, then recurse
            if (elem_size > 0 && *byte_off % elem_size != 0)
                *byte_off = ((*byte_off / elem_size) + 1) * elem_size;
            cg_fill_array(ctx, cur, base_addr, elem_type, raw, byte_off);
            // Advance to end of this element
            if (elem_size > 0 && *byte_off % elem_size != 0)
                *byte_off = ((*byte_off / elem_size) + 1) * elem_size;
        } else if (raw && raw->kind == ND_INITLIST && elem_type && elem_type->base == TB_STRUCT) {
            // Inner braces for a struct element
            if (elem_size > 0 && *byte_off % elem_size != 0)
                *byte_off = ((*byte_off / elem_size) + 1) * elem_size;
            cg_fill_struct(ctx, cur, base_addr, *byte_off, elem_type, raw);
            *byte_off += elem_size;
        } else if (raw && raw->kind == ND_LITERAL && raw->u.literal.ival == 0 && !raw->u.literal.strval) {
            // Zero: already zero-filled, skip
            *byte_off += lsz;
        } else {
            int off = *byte_off;
            Value *ptr;
            if (off == 0) {
                ptr = base_addr;
            } else {
                Value *ov = new_const(ctx->f, off, VT_I16);
                ptr = emit_binop(ctx, *cur, IK_ADD, base_addr, ov, VT_PTR);
            }
            Value *v = cg_expr(ctx, cur, item);
            emit_store(ctx, *cur, ptr, v, lsz, 0);
            *byte_off += lsz;
        }
    }
}

// Fill struct fields at base_addr+base_off from init_list.
static void cg_fill_struct(BraunCtx *ctx, Block **cur, Value *base_addr, int base_off,
                             Type *ty, Node *init_list)
{
    if (!ty || ty->base != TB_STRUCT || !init_list) return;
    Field *f    = ty->u.composite.members;
    Node  *item = init_list->ch[0];
    for (; f && item; f = f->next, item = item->next) {
        Node *raw = item;
        while (raw && raw->kind == ND_CAST) raw = raw->ch[1];
        int foff = base_off + f->offset;

        if (raw && raw->kind == ND_INITLIST && f->type && f->type->base == TB_STRUCT) {
            cg_fill_struct(ctx, cur, base_addr, foff, f->type, raw);
        } else if (raw && raw->kind == ND_INITLIST && f->type && istype_array(f->type)) {
            int off = foff;
            cg_fill_array(ctx, cur, base_addr, f->type, raw, &off);
        } else {
            Value *v = cg_expr(ctx, cur, item);
            Value *ptr;
            if (foff == 0) {
                ptr = base_addr;
            } else {
                Value *ov = new_const(ctx->f, foff, VT_I16);
                ptr = emit_binop(ctx, *cur, IK_ADD, base_addr, ov, VT_PTR);
            }
            int fsz = f->type ? f->type->size : 2;
            emit_store(ctx, *cur, ptr, v, fsz, 0);
        }
    }
}

// ============================================================
// cg_decl_init — handle local declaration initializer
// ============================================================

static void cg_decl_init(BraunCtx *ctx, Block **cur, Symbol *sym, Node *init) {
    if (!init) return;
    Block *b = *cur;
    Type *ty = sym->type;

    // Scalar (non-array, non-struct)
    if (!istype_array(ty) && ty && ty->base != TB_STRUCT) {
        Value *v = cg_expr(ctx, cur, init); b = *cur;
        if (is_addr_taken(ctx, sym)) {
            int bpoff = -(sym->offset);
            if (sym->kind == SYM_PARAM) bpoff = param_home_off(ctx, sym);
            Value *addr = new_value(ctx->f, VAL_INST, VT_PTR);
            Inst  *ai = new_inst(ctx->f, b, IK_ADDR, addr);
            ai->imm = bpoff;
            inst_append(b, ai);
            emit_store(ctx, b, addr, v, ty->size, 0);
        } else {
            write_var(b, sym, v);
        }
        return;
    }

    // char array from string literal: byte-by-byte stores
    if (istype_array(ty) && init->kind == ND_LITERAL && init->u.literal.strval) {
        int bpoff = -(sym->offset);
        Value *base_addr = new_value(ctx->f, VAL_INST, VT_PTR);
        Inst  *ai = new_inst(ctx->f, b, IK_ADDR, base_addr);
        ai->imm = bpoff;
        inst_append(b, ai);
        const char *str = init->u.literal.strval;
        int slen = init->u.literal.strval_len;
        for (int i = 0; i <= slen; i++) {
            Value *ptr;
            if (i == 0) {
                ptr = base_addr;
            } else {
                Value *off = new_const(ctx->f, i, VT_I16);
                ptr = emit_binop(ctx, b, IK_ADD, base_addr, off, VT_PTR);
            }
            Value *bval = new_const(ctx->f, i < slen ? (unsigned char)str[i] : 0, VT_I8);
            emit_store(ctx, b, ptr, bval, 1, 0);
        }
        return;
    }

    // Array with initializer list: zero-fill then write non-zero elements
    if (istype_array(ty) && init->kind == ND_INITLIST) {
        int bpoff = -(sym->offset);
        Value *base_addr = new_value(ctx->f, VAL_INST, VT_PTR);
        Inst  *ai = new_inst(ctx->f, b, IK_ADDR, base_addr);
        ai->imm = bpoff;
        inst_append(b, ai);
        // Zero-fill entire array
        Type *leaf = array_elem_type(ty);
        int lsz = (leaf && leaf->size > 0) ? leaf->size : 2;
        int total = ty->size;
        for (int off = 0; off < total; off += lsz) {
            Value *ptr;
            if (off == 0) {
                ptr = base_addr;
            } else {
                Value *off_v = new_const(ctx->f, off, VT_I16);
                ptr = emit_binop(ctx, b, IK_ADD, base_addr, off_v, VT_PTR);
            }
            emit_store(ctx, b, ptr, new_const(ctx->f, 0, VT_I16), lsz, 0);
        }
        // Write non-zero initializers (recursive for multi-dim arrays)
        int off = 0;
        cg_fill_array(ctx, cur, base_addr, ty, init, &off);
        return;
    }

    // Struct with initializer list: handled as memcpy from a temp
    // (for simplicity: zero addr then store each field is complex; use memcpy approach)
    if (ty && ty->base == TB_STRUCT) {
        int bpoff = -(sym->offset);
        Value *dst_addr = new_value(ctx->f, VAL_INST, VT_PTR);
        Inst  *ai = new_inst(ctx->f, b, IK_ADDR, dst_addr);
        ai->imm = bpoff;
        inst_append(b, ai);

        if (init->kind == ND_INITLIST) {
            // Zero-fill struct
            int total = ty->size;
            for (int off = 0; off < total; off += 2) {
                Value *ptr;
                if (off == 0) {
                    ptr = dst_addr;
                } else {
                    Value *ov = new_const(ctx->f, off, VT_I16);
                    ptr = emit_binop(ctx, b, IK_ADD, dst_addr, ov, VT_PTR);
                }
                emit_store(ctx, b, ptr, new_const(ctx->f, 0, VT_I16), 2, 0);
            }
            // Store each field (recursive for nested struct/array fields)
            cg_fill_struct(ctx, cur, dst_addr, 0, ty, init);
        } else {
            // Struct from expression: memcpy
            Value *src_addr = cg_expr(ctx, cur, init); b = *cur;
            Inst *mc = new_inst(ctx->f, b, IK_MEMCPY, NULL);
            mc->imm  = ty->size;
            mc->size = ty->size;
            inst_add_op(mc, dst_addr);
            inst_add_op(mc, src_addr);
            inst_append(b, mc);
        }
        return;
    }
}

// ============================================================
// cg_stmt — generate SSA for a statement Node*
// Returns the current block after the statement
// ============================================================

static Block *cg_stmt(BraunCtx *ctx, Block *b, Node *n) {
    if (!b || !n) return b;

    // Labels must be processed even from a filled block
    if (n->kind == ND_LABELSTMT) {
        Block *target = get_label_block(ctx, n->u.labelstmt.name);
        if (!b->filled) emit_jmp(b, target);
        // Do NOT seal yet — backward gotos may add predecessors
        return cg_stmt(ctx, target, n->ch[0]);
    }

    if (b->filled) return b;

    switch (n->kind) {

    case ND_COMPSTMT: {
        Node *stmt = n->ch[0];
        while (stmt) {
            b = cg_stmt(ctx, b, stmt);
            stmt = stmt->next;
        }
        return b;
    }

    case ND_EXPRSTMT: {
        if (n->ch[0]) { Block *cur = b; cg_expr(ctx, &cur, n->ch[0]); b = cur; }
        return b;
    }

    case ND_STMT:
        return cg_stmt(ctx, b, n->ch[0]);

    case ND_EMPTY:
        return b;

    case ND_DECLARATION: {
        // Local variable declarations with initializers
        for (Node *d = n->ch[1]; d; d = d->next) {
            if (d->kind != ND_DECLARATOR) continue;
            Symbol *sym = d->symbol;
            if (!sym) continue;
            if (sym->kind == SYM_ENUM_CONST) continue;
            if (sym->ns == NS_TYPEDEF) continue;
            if (istype_function(sym->type)) continue;
            // Static locals: emit data section entry
            if (sym->kind == SYM_STATIC_LOCAL) {
                Node *init = d->ch[1];
                static_locals_push(sym->offset, sym, init);
                continue;
            }
            Node *init = d->ch[1];
            if (init) {
                Block *cur = b;
                cg_decl_init(ctx, &cur, sym, init);
                b = cur;
            }
        }
        return b;
    }

    case ND_IFSTMT: {
        Block *cur = b;
        Value *cond = cg_expr(ctx, &cur, n->ch[0]); b = cur;
        Block *then_blk = new_block(ctx->f);
        Block *else_blk = new_block(ctx->f);
        Block *merge    = new_block(ctx->f);
        emit_br(ctx->f, b, cond, then_blk, else_blk);
        seal_block(ctx, then_blk);
        Block *then_exit = cg_stmt(ctx, then_blk, n->ch[1]);
        if (!then_exit->filled) emit_jmp(then_exit, merge);
        seal_block(ctx, else_blk);
        if (n->ch[2]) {
            Block *else_exit = cg_stmt(ctx, else_blk, n->ch[2]);
            if (!else_exit->filled) emit_jmp(else_exit, merge);
        } else {
            emit_jmp(else_blk, merge);
        }
        seal_block(ctx, merge);
        return merge;
    }

    case ND_WHILESTMT: {
        Block *header = new_block(ctx->f);  // unsealed (back-edge from body)
        Block *body   = new_block(ctx->f);
        Block *exit   = new_block(ctx->f);
        emit_jmp(b, header);
        Block *cur = header;
        Value *cond = cg_expr(ctx, &cur, n->ch[0]);
        emit_br(ctx->f, cur, cond, body, exit);
        seal_block(ctx, body);
        Block *saved_brk  = ctx->brk;
        Block *saved_cont = ctx->cont;
        ctx->brk  = exit;
        ctx->cont = header;
        Block *body_exit = cg_stmt(ctx, body, n->ch[1]);
        if (!body_exit->filled) emit_jmp(body_exit, header);
        seal_block(ctx, header);
        ctx->brk  = saved_brk;
        ctx->cont = saved_cont;
        seal_block(ctx, exit);
        return exit;
    }

    case ND_FORSTMT: {
        // ch[0]=init, ch[1]=cond, ch[2]=step, ch[3]=body (ND_EMPTY if absent)
        b = cg_stmt(ctx, b, n->ch[0]);
        Block *cond_blk = new_block(ctx->f);  // unsealed
        Block *body_blk = new_block(ctx->f);
        Block *step_blk = new_block(ctx->f);
        Block *exit_blk = new_block(ctx->f);
        emit_jmp(b, cond_blk);
        Block *cur = cond_blk;
        if (!n->ch[1] || n->ch[1]->kind == ND_EMPTY) {
            // Infinite loop: jump straight to body
            seal_block(ctx, body_blk);
            seal_block(ctx, cond_blk);
            Inst *j = arena_alloc(sizeof(Inst));
            j->kind = IK_JMP; j->target = body_blk; j->block = cond_blk;
            inst_append(cond_blk, j); cond_blk->filled = 1;
            block_add_succ(cond_blk, body_blk); block_add_pred(body_blk, cond_blk);
        } else {
            Value *cond = cg_expr(ctx, &cur, n->ch[1]);
            emit_br(ctx->f, cur, cond, body_blk, exit_blk);
            seal_block(ctx, body_blk);
        }
        Block *saved_brk  = ctx->brk;
        Block *saved_cont = ctx->cont;
        ctx->brk  = exit_blk;
        ctx->cont = step_blk;
        Block *body_exit = cg_stmt(ctx, body_blk, n->ch[3]);
        if (!body_exit->filled) emit_jmp(body_exit, step_blk);
        ctx->brk  = saved_brk;
        ctx->cont = saved_cont;
        seal_block(ctx, step_blk);
        if (n->ch[2] && n->ch[2]->kind != ND_EMPTY) {
            Block *step_exit = cg_stmt(ctx, step_blk, n->ch[2]);
            if (!step_exit->filled) emit_jmp(step_exit, cond_blk);
        } else {
            emit_jmp(step_blk, cond_blk);
        }
        seal_block(ctx, cond_blk);
        seal_block(ctx, exit_blk);
        return exit_blk;
    }

    case ND_DOWHILESTMT: {
        Block *body_blk = new_block(ctx->f);  // unsealed (back-edge from cond)
        Block *cond_blk = new_block(ctx->f);
        Block *exit_blk = new_block(ctx->f);
        emit_jmp(b, body_blk);
        Block *saved_brk  = ctx->brk;
        Block *saved_cont = ctx->cont;
        ctx->brk  = exit_blk;
        ctx->cont = cond_blk;
        Block *body_exit = cg_stmt(ctx, body_blk, n->ch[0]);
        ctx->brk  = saved_brk;
        ctx->cont = saved_cont;
        if (!body_exit->filled) emit_jmp(body_exit, cond_blk);
        seal_block(ctx, cond_blk);
        Block *cur = cond_blk;
        Value *cond = cg_expr(ctx, &cur, n->ch[1]);
        emit_br(ctx->f, cur, cond, body_blk, exit_blk);
        seal_block(ctx, body_blk);
        seal_block(ctx, exit_blk);
        return exit_blk;
    }

    case ND_RETURNSTMT: {
        Inst *ret = arena_alloc(sizeof(Inst));
        ret->kind  = IK_RET;
        ret->block = b;
        if (n->ch[0]) {
            Block *cur = b;
            Value *v = cg_expr(ctx, &cur, n->ch[0]);
            b = cur; ret->block = b;
            inst_add_op(ret, v);
        }
        inst_append(b, ret);
        b->filled = 1;
        return b;
    }

    case ND_BREAKSTMT:
        if (ctx->brk) emit_jmp(b, ctx->brk);
        b->filled = 1;
        return b;

    case ND_CONTINUESTMT:
        if (ctx->cont) emit_jmp(b, ctx->cont);
        b->filled = 1;
        return b;

    case ND_GOTOSTMT: {
        Block *target = get_label_block(ctx, n->u.label);
        emit_jmp(b, target);
        b->filled = 1;
        return b;
    }

    case ND_SWITCHSTMT: {
        // ch[0]=selector, ch[1]=body (ND_COMPSTMT)
        Block *cur = b;
        Value *sel = cg_expr(ctx, &cur, n->ch[0]); b = cur;
        Block *exit_blk = new_block(ctx->f);

        // Collect case labels from body (flat list of stmts, some are ND_CASESTMT/ND_DEFAULTSTMT)
        enum { MAX_CASES = 256 };
        int    case_vals[MAX_CASES];
        Block *case_blks[MAX_CASES];
        int    ncases = 0;
        Block *default_blk = NULL;

        Node *body = n->ch[1];  // ND_COMPSTMT
        Node *stmts = body ? body->ch[0] : NULL;
        for (Node *s = stmts; s; s = s->next) {
            if (s->kind == ND_CASESTMT && ncases < MAX_CASES) {
                case_vals[ncases] = (int)s->u.casestmt.value;
                case_blks[ncases] = new_block(ctx->f);
                ncases++;
            } else if (s->kind == ND_DEFAULTSTMT) {
                default_blk = new_block(ctx->f);
            }
        }

        Block *dispatch_default = default_blk ? default_blk : exit_blk;

        // Emit comparison chain
        for (int ci = 0; ci < ncases; ci++) {
            Value *kv  = new_const(ctx->f, case_vals[ci], VT_I16);
            Value *cmp = emit_binop(ctx, b, IK_EQ, sel, kv, VT_I16);
            Block *nc  = new_block(ctx->f);
            seal_block(ctx, nc);
            emit_br(ctx->f, b, cmp, case_blks[ci], nc);
            b = nc;
        }
        emit_jmp(b, dispatch_default);

        // Walk body statements
        Block *saved_brk = ctx->brk;
        ctx->brk = exit_blk;

        // Unreachable prelude block (before first case label)
        Block *body_cur = new_block(ctx->f);
        seal_block(ctx, body_cur);

        int case_idx = 0;
        for (Node *s = stmts; s; s = s->next) {
            if (s->kind == ND_CASESTMT) {
                Block *cb = case_blks[case_idx++];
                if (!body_cur->filled) emit_jmp(body_cur, cb);
                seal_block(ctx, cb);
                body_cur = cb;
                // ch[0] of ND_CASESTMT = ND_EMPTY (label body is siblings)
            } else if (s->kind == ND_DEFAULTSTMT) {
                if (!body_cur->filled) emit_jmp(body_cur, default_blk);
                seal_block(ctx, default_blk);
                body_cur = default_blk;
                // ch[0] of ND_DEFAULTSTMT = ND_EMPTY
            } else {
                body_cur = cg_stmt(ctx, body_cur, s);
            }
        }
        if (!body_cur->filled) emit_jmp(body_cur, exit_blk);

        ctx->brk = saved_brk;
        seal_block(ctx, exit_blk);
        return exit_blk;
    }

    case ND_CASESTMT:
    case ND_DEFAULTSTMT:
        // Should only appear as children of ND_SWITCHSTMT.
        // If encountered standalone, process child statement.
        return cg_stmt(ctx, b, n->ch[0]);

    case ND_VA_START: {
        Block *cur = b;
        cg_expr(ctx, &cur, n);  // va_start handled in cg_expr
        return cur;
    }

    case ND_VA_END:
        return b;  // no-op

    default:
        // Expression statement fallback
        if (n->is_expr) {
            Block *cur = b;
            cg_expr(ctx, &cur, n);
            b = cur;
        }
        return b;
    }
}

// ============================================================
// braun_function — entry point
// ============================================================

Function *braun_function(Node *func_decl, int tu_index, int *strlit_id) {
    if (!func_decl) return NULL;

    // Locate: func_decl is ND_DECLARATION (is_func_defn=true)
    // ch[1] = ND_DECLARATOR chain (first has the symbol)
    // ch[2] = ND_COMPSTMT (body)
    Node *func_decl_node = func_decl->ch[1];
    Node *body_node      = func_decl->ch[2];
    if (!func_decl_node || !body_node) return NULL;
    Symbol *fsym = func_decl_node->symbol;
    if (!fsym) return NULL;

    const char *fname = sym_label(tu_index, fsym);

    Type *fn_type = fsym->type;
    if (!fn_type || fn_type->base != TB_FUNCTION) return NULL;
    bool is_variadic = fn_type->u.fn.is_variadic;

    // Compute frame size
    int frame_size = 0;
    max_frame_r(body_node, &frame_size);

    Function *f = new_function(fname);
    f->frame_size  = frame_size;
    f->is_variadic = is_variadic;

    BraunCtx ctx = {0};
    ctx.f         = f;
    ctx.tu_index  = tu_index;
    ctx.strlit_id = strlit_id;

    // Pre-scan for address-taken locals/params
    scan_addr_taken(&ctx, body_node);

    // Collect param symbols from body's scope, sorted by offset (ascending)
    Symbol *param_syms[64]; int nparams = 0;
    if (body_node->symtable) {
        for (Symbol *s = body_node->symtable->symbols; s; s = s->next)
            if (s->kind == SYM_PARAM && s->ns == NS_IDENT && nparams < 64)
                param_syms[nparams++] = s;
        // Sort by offset ascending
        for (int i = 0; i < nparams - 1; i++)
            for (int j = i + 1; j < nparams; j++)
                if (param_syms[i]->offset > param_syms[j]->offset) {
                    Symbol *tmp = param_syms[i]; param_syms[i] = param_syms[j]; param_syms[j] = tmp;
                }
    }

    // For variadic: build vparam offset map (CPU4: 4 + idx * 4)
    if (is_variadic) {
        for (int i = 0; i < nparams; i++) {
            int cpu4_off = 4 + i * 4;
            if (ctx.n_vparams >= ctx.vparam_cap) {
                int nc = ctx.vparam_cap ? ctx.vparam_cap * 2 : 8;
                Symbol **ns = arena_alloc(nc * sizeof(Symbol *));
                int     *no = arena_alloc(nc * sizeof(int));
                memcpy(ns, ctx.vparam_syms, ctx.n_vparams * sizeof(Symbol *));
                memcpy(no, ctx.vparam_offs, ctx.n_vparams * sizeof(int));
                ctx.vparam_syms = ns;
                ctx.vparam_offs = no;
                ctx.vparam_cap  = nc;
            }
            ctx.vparam_syms[ctx.n_vparams] = param_syms[i];
            ctx.vparam_offs[ctx.n_vparams] = cpu4_off;
            ctx.n_vparams++;
        }
    }

    // For non-variadic: build param home slots for addr-taken params
    enum { NREG_PARAMS = 3 };
    if (!is_variadic) {
        Symbol *at_params[64]; int n_at = 0;
        for (int i = 0; i < nparams; i++)
            if (i < NREG_PARAMS && is_addr_taken(&ctx, param_syms[i]))
                at_params[n_at++] = param_syms[i];
        // Sort for deterministic layout
        for (int i = 0; i < n_at - 1; i++)
            for (int j = i + 1; j < n_at; j++)
                if (at_params[i]->offset > at_params[j]->offset) {
                    Symbol *tmp = at_params[i]; at_params[i] = at_params[j]; at_params[j] = tmp;
                }
        for (int i = 0; i < n_at; i++) {
            Symbol *ps = at_params[i];
            int psz = ps->type ? ps->type->size : 2;
            if (psz == 4 && (f->frame_size % 4) != 0) f->frame_size += 2;
            f->frame_size += psz;
            int home_off = -f->frame_size;
            if (ctx.n_param_homes >= ctx.param_home_cap) {
                int nc = ctx.param_home_cap ? ctx.param_home_cap * 2 : 8;
                Symbol **ns = arena_alloc(nc * sizeof(Symbol *));
                int     *no = arena_alloc(nc * sizeof(int));
                memcpy(ns, ctx.param_home_syms, ctx.n_param_homes * sizeof(Symbol *));
                memcpy(no, ctx.param_home_offs, ctx.n_param_homes * sizeof(int));
                ctx.param_home_syms = ns;
                ctx.param_home_offs = no;
                ctx.param_home_cap  = nc;
            }
            ctx.param_home_syms[ctx.n_param_homes] = ps;
            ctx.param_home_offs[ctx.n_param_homes] = home_off;
            ctx.n_param_homes++;
        }
    }

    // Create entry block (sealed immediately — no predecessors)
    Block *entry = new_block(f);
    entry->sealed = 1;

    // Emit parameter setup
    int stack_slot = 0;  // count of stack params (beyond NREG_PARAMS)
    for (int idx = 0; idx < nparams; idx++) {
        Symbol *ps = param_syms[idx];
        ValType pvt = ps->type ? type_to_valtype(ps->type) : VT_I16;
        if (pvt == VT_VOID) pvt = VT_I16;

        if (is_variadic) {
            // All params from stack (4-byte slots)
            int bp_off = 4 + idx * 4;
            int sz = ps->type ? ps->type->size : 2;
            Value *pv = new_value(f, VAL_INST, pvt);
            Inst *li = new_inst(f, entry, IK_LOAD, pv);
            li->imm  = bp_off;
            li->size = sz;
            inst_add_op(li, NULL);  // NULL base = bp-relative
            inst_append(entry, li);
            write_var(entry, ps, pv);
        } else if (idx < NREG_PARAMS) {
            // Register param: landing pre-colored to r{idx+1}, copy freely allocated
            Value *landing = new_value(f, VAL_INST, pvt);
            landing->phys_reg = idx + 1;
            Inst *pi = new_inst(f, entry, IK_PARAM, landing);
            pi->param_idx = idx + 1;
            inst_append(entry, pi);
            if (f->nparams >= f->param_cap) {
                int nc = f->param_cap ? f->param_cap * 2 : 8;
                Value **np = arena_alloc(nc * sizeof(Value *));
                memcpy(np, f->params, f->nparams * sizeof(Value *));
                f->params    = np;
                f->param_cap = nc;
            }
            f->params[f->nparams++] = landing;

            if (is_addr_taken(&ctx, ps)) {
                // Home the param: store to frame slot
                int home_off = param_home_off(&ctx, ps);
                int sz = ps->type ? ps->type->size : 2;
                // Emit addr + store
                Value *addr = new_value(f, VAL_INST, VT_PTR);
                Inst  *ai = new_inst(f, entry, IK_ADDR, addr);
                ai->imm = home_off;
                inst_append(entry, ai);
                emit_store(&ctx, entry, addr, landing, sz, 0);
                // For subsequent reads: the variable maps to memory, not an SSA value.
                // We do NOT write_var here — cg_expr will emit a load from the home slot.
            } else {
                // Free copy for IRC to allocate
                Value *pv = new_value(f, VAL_INST, pvt);
                Inst *copy = new_inst(f, entry, IK_COPY, pv);
                inst_add_op(copy, landing);
                inst_append(entry, copy);
                write_var(entry, ps, pv);
            }
        } else {
            // Stack param: bp + 4 + stack_slot * 4
            int bp_off = 4 + stack_slot * 4;
            int sz = ps->type ? ps->type->size : 2;
            Value *pv = new_value(f, VAL_INST, pvt);
            Inst *li = new_inst(f, entry, IK_LOAD, pv);
            li->imm  = bp_off;
            li->size = sz;
            inst_add_op(li, NULL);  // NULL base = bp-relative
            inst_append(entry, li);
            write_var(entry, ps, pv);
            stack_slot++;
        }
    }

    // Generate body
    Block *exit_blk = cg_stmt(&ctx, entry, body_node);
    if (exit_blk && !exit_blk->filled) {
        Inst *ret = arena_alloc(sizeof(Inst));
        ret->kind  = IK_RET;
        ret->block = exit_blk;
        inst_append(exit_blk, ret);
        exit_blk->filled = 1;
    }

    // Seal all named label blocks (handles backward gotos)
    for (int i = 0; i < ctx.nlabels; i++) {
        Block *lb = ctx.label_blocks[i];
        if (!lb->sealed) seal_block(&ctx, lb);
    }

    return f;
}
