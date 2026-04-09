/*
 * lower.c — Lowering pass: Node* → Sexp AST + TypeMap
 *
 * Walks the type-annotated Node* tree (after resolve_symbols, derive_types,
 * insert_coercions) and produces a Sx* program sexp + populates TypeMap.
 *
 * Alpha-renaming:
 *   Locals/params: "name@N"    (N = per-symbol sequential id)
 *   Globals:       "name"      (or "_s{tu}_{name}" for statics)
 *   Static locals: "_ls{id}"
 *   Temporaries:   "_t@{N}"
 *
 * Sexp grammar (abbreviated):
 *   expr:  (int v) | (flt bits) | (var "x") | (addr "x") | (gaddr "name")
 *          (load size e) | (store size addr val)
 *          (assign "x" e)
 *          (binop "op" lhs rhs) | (unop "op" e)
 *          (call "f" args...) | (icall fp args...)
 *          (putchar e)
 *          (if cond then else)        -- ternary
 *          (seq stmt... expr)         -- side effects + value
 *          (cast e)                   -- type cast wrapper
 *          (memcpy dst src size)      -- struct assignment
 *   stmt:  (skip) | (block s...) | (expr e)
 *          (if cond then) | (if cond then else)
 *          (while cond body) | (for init cond step body) | (do body cond)
 *          (return) | (return e) | (break) | (continue)
 *          (goto "L") | (label "L" stmt)
 *          (switch e case_block) | (case v s) | (default s)
 *          (va_start ap last)
 *   top:   (func "name" frame_size (params "p"...) body)
 *          (gvar "name" size val...)
 *          (strlit "_lN" bytes...)
 *   program: (program top...)
 */

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lower.h"
#include "sx.h"
#include "smallcc.h"

// ============================================================
// LowerCtx
// ============================================================

typedef struct {
    int      tu_index;
    TypeMap *tm;
    // Alpha-renaming: Symbol* → string  (linear scan, bounded by function size)
    Symbol **sym_keys;
    char   **sym_vals;
    int      nsyms;
    int      sym_cap;
    int      sym_counter;
    // Temp variable counter
    int      temp_counter;
    // Shared string literal counter (advances per TU)
    int     *strlit_count;
    // Side-effect statement accumulator (for ++ / compound-assign)
    Sx     **pre_stmts;
    int      nstmts;
    int      stmt_cap;
    // Address-taken symbols: variables that must live in memory (not SSA-promoted)
    Symbol **addr_taken;
    int      n_addr_taken;
    int      addr_taken_cap;
    // Static local gvars: accumulated during function lowering, appended to program
    Sx     **static_gvars;
    int      n_static_gvars;
    int      static_gvars_cap;
    // Param homing: address-taken SYM_PARAMs in non-variadic functions need local
    // stack slots because register-ABI params are NOT on the stack.
    // Maps Symbol* → negative bp offset of the home slot.
    Symbol **param_home_syms;
    int     *param_home_offs;  // negative bp offsets for home slots
    int      n_param_homes;
    int      param_home_cap;
    // CPU4 stack-ABI param offsets for variadic functions.
    // sym->offset from types.c uses CPU3 frame layout (FRAME_OVERHEAD=8, WORD_SIZE=2 slots).
    // CPU4 uses 4-byte slots and 4-byte frame overhead.
    // Maps Symbol* → positive bp offset using CPU4 layout (4 + idx*4).
    Symbol **vparam_syms;
    int     *vparam_offsets;
    int      n_vparams;
    int      vparam_cap;
} LowerCtx;

// ============================================================
// stmt accumulator helpers
// ============================================================

static void ctx_push_static_gvar(LowerCtx *ctx, Sx *gv) {
    if (ctx->n_static_gvars >= ctx->static_gvars_cap) {
        ctx->static_gvars_cap = ctx->static_gvars_cap ? ctx->static_gvars_cap * 2 : 8;
        ctx->static_gvars = realloc(ctx->static_gvars,
                                    ctx->static_gvars_cap * sizeof(Sx *));
    }
    ctx->static_gvars[ctx->n_static_gvars++] = gv;
}

static void ctx_push_stmt(LowerCtx *ctx, Sx *s) {
    if (ctx->nstmts >= ctx->stmt_cap) {
        ctx->stmt_cap = ctx->stmt_cap ? ctx->stmt_cap * 2 : 16;
        ctx->pre_stmts = realloc(ctx->pre_stmts, ctx->stmt_cap * sizeof(Sx *));
    }
    ctx->pre_stmts[ctx->nstmts++] = s;
}

static int ctx_mark(LowerCtx *ctx)  { return ctx->nstmts; }

// Wrap accumulated stmts since mark + result_expr into (seq s... result)
// or just return result_expr if no stmts were pushed.
static Sx *ctx_drain(LowerCtx *ctx, int mark, Sx *result_expr) {
    int count = ctx->nstmts - mark;
    if (count == 0) return result_expr;
    Sx *list = sx_cons(result_expr, NULL);
    for (int i = ctx->nstmts - 1; i >= mark; i--)
        list = sx_cons(ctx->pre_stmts[i], list);
    ctx->nstmts = mark;
    return sx_cons(sx_sym("seq"), list);
}

// Drain stmts since mark as a block + final expr statement
static Sx *ctx_drain_stmt(LowerCtx *ctx, int mark, Sx *stmt) {
    int count = ctx->nstmts - mark;
    if (count == 0) return stmt;
    Sx *block = sx_list(1, sx_sym("block"));
    Sx **tail = &block->cdr;
    for (int i = mark; i < ctx->nstmts; i++) {
        *tail = sx_cons(ctx->pre_stmts[i], NULL);
        tail = &(*tail)->cdr;
    }
    *tail = sx_cons(stmt, NULL);
    ctx->nstmts = mark;
    if (!block->cdr->cdr) return block->cdr->car;
    return block;
}

// ============================================================
// Alpha-renaming
// ============================================================

static bool sym_is_global(Symbol *sym) {
    return sym->kind == SYM_GLOBAL || sym->kind == SYM_STATIC_GLOBAL ||
           sym->kind == SYM_EXTERN || sym->kind == SYM_BUILTIN;
}

static const char *sym_alpha(LowerCtx *ctx, Symbol *sym) {
    if (!sym) return "_nil";
    for (int i = 0; i < ctx->nsyms; i++)
        if (ctx->sym_keys[i] == sym) return ctx->sym_vals[i];
    if (ctx->nsyms >= ctx->sym_cap) {
        ctx->sym_cap = ctx->sym_cap ? ctx->sym_cap * 2 : 64;
        ctx->sym_keys = realloc(ctx->sym_keys, ctx->sym_cap * sizeof(Symbol *));
        ctx->sym_vals = realloc(ctx->sym_vals, ctx->sym_cap * sizeof(char *));
    }
    char buf[256];
    switch (sym->kind) {
    case SYM_LOCAL: case SYM_PARAM:
        snprintf(buf, sizeof(buf), "%s@%d", sym->name, ++ctx->sym_counter);
        break;
    case SYM_GLOBAL:  snprintf(buf, sizeof(buf), "%s", sym->name); break;
    case SYM_STATIC_GLOBAL:
        snprintf(buf, sizeof(buf), "_s%d_%s", sym->tu_index, sym->name); break;
    case SYM_STATIC_LOCAL:
        snprintf(buf, sizeof(buf), "_ls%d", sym->offset); break;
    case SYM_EXTERN: case SYM_BUILTIN:
        snprintf(buf, sizeof(buf), "%s", sym->name); break;
    case SYM_ENUM_CONST:
        snprintf(buf, sizeof(buf), "_ec%d", sym->offset); break;
    }
    const char *name = arena_strdup(buf);
    ctx->sym_keys[ctx->nsyms] = sym;
    ctx->sym_vals[ctx->nsyms] = (char *)name;
    ctx->nsyms++;
    return name;
}

static const char *fresh_temp(LowerCtx *ctx) {
    char buf[64];
    snprintf(buf, sizeof(buf), "_t@%d", ++ctx->temp_counter);
    return arena_strdup(buf);
}

// ============================================================
// Address-taken tracking
// ============================================================

static void mark_addr_taken(LowerCtx *ctx, Symbol *sym) {
    if (!sym) return;
    for (int i = 0; i < ctx->n_addr_taken; i++)
        if (ctx->addr_taken[i] == sym) return;
    if (ctx->n_addr_taken >= ctx->addr_taken_cap) {
        ctx->addr_taken_cap = ctx->addr_taken_cap ? ctx->addr_taken_cap * 2 : 16;
        ctx->addr_taken = realloc(ctx->addr_taken, ctx->addr_taken_cap * sizeof(Symbol *));
    }
    ctx->addr_taken[ctx->n_addr_taken++] = sym;
}

static bool sym_is_addr_taken(LowerCtx *ctx, Symbol *sym) {
    if (!sym) return false;
    for (int i = 0; i < ctx->n_addr_taken; i++)
        if (ctx->addr_taken[i] == sym) return true;
    return false;
}

// Return the CPU4 bp offset for a variadic-function param (positive).
// Returns -1 if not found (non-variadic param or sym not in vparam table).
static int get_vparam_offset(LowerCtx *ctx, Symbol *sym) {
    for (int i = 0; i < ctx->n_vparams; i++)
        if (ctx->vparam_syms[i] == sym) return ctx->vparam_offsets[i];
    return -1;
}

// Return the negative bp offset of the local home slot for an address-taken param.
// Falls back to CPU4 variadic offset if available, else sym->offset.
static int get_param_home_off(LowerCtx *ctx, Symbol *sym) {
    for (int i = 0; i < ctx->n_param_homes; i++)
        if (ctx->param_home_syms[i] == sym) return ctx->param_home_offs[i];
    // For variadic params, use precomputed CPU4 offset
    int voff = get_vparam_offset(ctx, sym);
    if (voff >= 0) return voff;
    return sym->offset;  // fallback
}

// Pre-scan a function body to find all variables whose address is taken (&x).
static void scan_addr_taken(LowerCtx *ctx, Node *node) {
    if (!node) return;
    if (node->kind == ND_UNARYOP && node->op_kind == TK_AMPERSAND) {
        // &x — the operand's address is taken
        Node *operand = node->ch[0];
        if (operand && operand->kind == ND_IDENT && operand->symbol) {
            Symbol *sym = operand->symbol;
            if (sym->kind == SYM_LOCAL || sym->kind == SYM_PARAM)
                mark_addr_taken(ctx, sym);
        }
    }
    // Recurse into all children
    for (int i = 0; i < 4; i++) scan_addr_taken(ctx, node->ch[i]);
    scan_addr_taken(ctx, node->next);
}

// ============================================================
// Forward declarations
// ============================================================

static Sx *lower_expr(LowerCtx *ctx, Node *node);
static Sx *lower_lval_addr(LowerCtx *ctx, Node *node);
static Sx *lower_stmt(LowerCtx *ctx, Node *node);
static Sx *lower_block(LowerCtx *ctx, Node *first);
static Sx *lower_global(LowerCtx *ctx, Node *decl, Symbol *sym);

static Sx *reg_type(LowerCtx *ctx, Sx *sx, Type *t) {
    if (sx && t) typemap_set_vtype(ctx->tm, sx->id, type_to_valtype(t));
    return sx;
}

static int sz(Type *t) { return (t && t->size > 0) ? t->size : 2; }

// ============================================================
// Lvalue address
// ============================================================

// Return the address sexp of an lvalue (no load).
static Sx *lower_lval_addr(LowerCtx *ctx, Node *node) {
    if (!node) return sx_int(0);
    switch (node->kind) {
    case ND_IDENT: {
        Symbol *sym = node->symbol;
        if (!sym) return sx_int(0);
        if (sym_is_global(sym) || sym->kind == SYM_STATIC_LOCAL) {
            Sx *s = sx_list(2, sx_sym("gaddr"), sx_str(sym_alpha(ctx, sym)));
            reg_type(ctx, s, get_pointer_type(node->type));
            return s;
        }
        // Include the bp-relative offset (negative for SYM_LOCAL) for IK_ADDR.
        // For address-taken SYM_PARAM in non-variadic functions, use the home slot
        // (negative) rather than the stack-ABI offset (positive) because register-ABI
        // params are not on the stack.
        // For variadic params, use the precomputed CPU4 offset (4 + idx*4).
        //
        // Special case: struct-type params in non-variadic register ABI.
        // The caller passes a pointer to the struct in r1 (not the struct itself).
        // The SSA variable "p@N" holds that pointer value — so (var "p@N") IS the
        // struct address. Return it directly rather than (addr "p@N" offset) which
        // would incorrectly compute bp+offset pointing at an uninitialized frame slot.
        if (sym->kind == SYM_PARAM &&
            sym->type && sym->type->base == TB_STRUCT) {
            int voff = get_vparam_offset(ctx, sym);
            if (voff < 0) {
                // Non-variadic: param holds the pointer; return SSA var as the address.
                Sx *s = sx_list(2, sx_sym("var"), sx_str(sym_alpha(ctx, sym)));
                reg_type(ctx, s, get_pointer_type(node->type));
                return s;
            }
            // Variadic: struct pushed on stack at known bp offset.
            Sx *s = sx_list(3, sx_sym("addr"), sx_str(sym_alpha(ctx, sym)), sx_int(voff));
            reg_type(ctx, s, get_pointer_type(node->type));
            return s;
        }
        int bpoff;
        if (sym->kind == SYM_PARAM) {
            if (sym_is_addr_taken(ctx, sym)) {
                bpoff = get_param_home_off(ctx, sym);
            } else {
                int voff = get_vparam_offset(ctx, sym);
                bpoff = (voff >= 0) ? voff : sym->offset;
            }
        } else {
            bpoff = -(sym->offset);
        }
        Sx *s = sx_list(3, sx_sym("addr"), sx_str(sym_alpha(ctx, sym)), sx_int(bpoff));
        reg_type(ctx, s, get_pointer_type(node->type));
        return s;
    }
    case ND_UNARYOP:
        if (node->op_kind == TK_STAR)
            return lower_expr(ctx, node->ch[0]);   // *p → address is p
        if (node->op_kind == TK_PLUS && node->u.unaryop.is_array_deref)
            return lower_expr(ctx, node->ch[0]);
        break;
    case ND_MEMBER: {
        int offset = node->u.member.offset;
        Sx *base;
        if (node->op_kind == TK_ARROW)
            base = lower_expr(ctx, node->ch[0]);
        else
            base = lower_lval_addr(ctx, node->ch[0]);
        if (offset == 0) { reg_type(ctx, base, get_pointer_type(node->type)); return base; }
        Sx *s = sx_list(4, sx_sym("binop"), sx_str("+"), base, sx_int(offset));
        reg_type(ctx, s, get_pointer_type(node->type));
        return s;
    }
    default: break;
    }
    return lower_expr(ctx, node);  // fallback: result is a pointer
}

// ============================================================
// Lvalue representation: is it an SSA var or a memory slot?
// ============================================================

typedef struct { bool is_var; const char *name; Sx *addr; } LVal;

static LVal lower_lval(LowerCtx *ctx, Node *node) {
    LVal lv = {false, NULL, NULL};
    if (!node) return lv;
    if (node->kind == ND_IDENT) {
        Symbol *sym = node->symbol;
        if (sym && (sym->kind == SYM_LOCAL || sym->kind == SYM_PARAM)) {
            Type *t = node->type;
            if (t && t->base != TB_ARRAY && t->base != TB_STRUCT
                && !sym_is_addr_taken(ctx, sym)) {
                lv.is_var = true;
                lv.name   = sym_alpha(ctx, sym);
                return lv;
            }
        }
    }
    lv.addr = lower_lval_addr(ctx, node);
    return lv;
}

static Sx *lval_load(LowerCtx *ctx, LVal lv, Type *t) {
    if (lv.is_var) {
        Sx *s = sx_list(2, sx_sym("var"), sx_str(lv.name));
        return reg_type(ctx, s, t);
    }
    Sx *s = sx_list(3, sx_sym("load"), sx_int(sz(t)), lv.addr);
    return reg_type(ctx, s, t);
}

static Sx *lval_store(LowerCtx *ctx, LVal lv, Sx *val, Type *t) {
    if (lv.is_var)
        return sx_list(3, sx_sym("assign"), sx_str(lv.name), val);
    return sx_list(4, sx_sym("store"), sx_int(sz(t)), lv.addr, val);
}

// ============================================================
// Call lowering helper
// ============================================================

static Sx *lower_call_args(LowerCtx *ctx, Node *args_head) {
    Sx *list = NULL, **tail = &list;
    for (Node *a = args_head; a; a = a->next) {
        int m = ctx_mark(ctx);
        Sx *av = lower_expr(ctx, a);
        av = ctx_drain(ctx, m, av);
        *tail = sx_cons(av, NULL);
        tail = &(*tail)->cdr;
    }
    return list;
}

static void populate_calldesc(LowerCtx *ctx, Sx *call, Type *fn_type) {
    if (!fn_type) return;
    if (fn_type->base == TB_POINTER) fn_type = fn_type->u.ptr.pointee;
    if (!fn_type || fn_type->base != TB_FUNCTION) return;
    CallDesc *cd   = arena_alloc(sizeof(CallDesc));
    cd->return_type = fn_type->u.fn.ret;
    int n = 0;
    for (Param *p = fn_type->u.fn.params; p; p = p->next) n++;
    cd->nparams    = n;
    cd->param_types = n ? arena_alloc(n * sizeof(Type *)) : NULL;
    int i = 0;
    for (Param *p = fn_type->u.fn.params; p; p = p->next)
        cd->param_types[i++] = p->type;
    cd->is_variadic = fn_type->u.fn.is_variadic;
    cd->hidden_sret = (cd->return_type && cd->return_type->base == TB_STRUCT);
    typemap_set_calldesc(ctx->tm, call->id, cd);
}

// ============================================================
// Main expression lowering
// ============================================================

static Sx *lower_expr(LowerCtx *ctx, Node *node) {
    if (!node) return sx_list(2, sx_sym("int"), sx_int(0));

    switch (node->kind) {

    // --------------------------------------------------------
    case ND_LITERAL: {
        Sx *s;
        if (node->u.literal.strval) {
            // String literal → gaddr "_lN"
            int lid = (*ctx->strlit_count)++;
            char buf[32]; snprintf(buf, sizeof(buf), "_l%d", lid);
            s = sx_list(2, sx_sym("gaddr"), sx_str(arena_strdup(buf)));
            reg_type(ctx, s, node->type ? node->type : get_pointer_type(get_basic_type(TB_CHAR)));
        } else if (node->type && (node->type->base == TB_FLOAT ||
                                   node->type->base == TB_DOUBLE)) {
            float fv = (float)node->u.literal.fval;
            uint32_t bits; memcpy(&bits, &fv, 4);
            s = sx_list(2, sx_sym("flt"), sx_int((int)bits));
            reg_type(ctx, s, node->type);
        } else {
            s = sx_list(2, sx_sym("int"), sx_int((int)node->u.literal.ival));
            reg_type(ctx, s, node->type);
        }
        return s;
    }

    // --------------------------------------------------------
    case ND_IDENT: {
        Symbol *sym = node->symbol;
        if (!sym) return sx_list(2, sx_sym("int"), sx_int(0));

        // Enum constant: inline integer value
        if (sym->kind == SYM_ENUM_CONST) {
            Sx *s = sx_list(2, sx_sym("int"), sx_int(sym->offset));
            return reg_type(ctx, s, node->type);
        }

        // Function call: ND_IDENT with is_function=true, args in ch[0]->next
        if (node->u.ident.is_function) {
            Node *args_head = node->ch[0];
            Sx *arg_list = lower_call_args(ctx, args_head);
            // Builtin putchar (__putchar is the compiler-builtin opcode emitter)
            if (sym->kind == SYM_BUILTIN &&
                (strcmp(sym->name, "putchar") == 0 || strcmp(sym->name, "__putchar") == 0)) {
                Sx *s = sx_cons(sx_sym("putchar"), arg_list);
                return reg_type(ctx, s, node->type);
            }
            if (istype_function(sym->type)) {
                // Direct call
                const char *fname = sym_alpha(ctx, sym);
                Sx *call = sx_cons(sx_sym("call"), sx_cons(sx_str(fname), arg_list));
                populate_calldesc(ctx, call, sym->type);
                return reg_type(ctx, call, node->type);
            } else if (istype_ptr(sym->type)) {
                // Indirect call via function pointer variable
                Sx *fp = lval_load(ctx, lower_lval(ctx, node), sym->type);
                // Strip the is_function flag to prevent recursion: we just need the ptr val
                Sx *call = sx_cons(sx_sym("icall"), sx_cons(fp, arg_list));
                populate_calldesc(ctx, call, sym->type);
                return reg_type(ctx, call, node->type);
            }
        }

        // Array or struct: yield base address
        if (istype_array(sym->type) || sym->type->base == TB_STRUCT) {
            return lower_lval_addr(ctx, node);
        }
        // Function designator used as value (not a call)
        if (istype_function(sym->type)) {
            return lower_lval_addr(ctx, node);
        }
        // Scalar variable
        if (sym_is_global(sym) || sym->kind == SYM_STATIC_LOCAL) {
            Sx *ga = sx_list(2, sx_sym("gaddr"), sx_str(sym_alpha(ctx, sym)));
            Sx *s  = sx_list(3, sx_sym("load"), sx_int(sz(sym->type)), ga);
            return reg_type(ctx, s, node->type);
        }
        // Local or param: use SSA var unless address is taken
        if (!sym_is_addr_taken(ctx, sym)) {
            Sx *s = sx_list(2, sx_sym("var"), sx_str(sym_alpha(ctx, sym)));
            return reg_type(ctx, s, node->type);
        }
        // Address-taken: must load from memory via home slot (for non-variadic params)
        // or via stack-ABI offset (for variadic params / locals).
        {
            int bpoff;
            if (sym->kind == SYM_PARAM)
                bpoff = get_param_home_off(ctx, sym);
            else
                bpoff = -(sym->offset);
            Sx *addr = sx_list(3, sx_sym("addr"), sx_str(sym_alpha(ctx, sym)), sx_int(bpoff));
            Sx *s    = sx_list(3, sx_sym("load"), sx_int(sz(sym->type)), addr);
            return reg_type(ctx, s, node->type);
        }
    }

    // --------------------------------------------------------
    case ND_UNARYOP: {
        Token_kind op = node->op_kind;

        // Indirect call: (*fp)(args) — ND_UNARYOP "*" with is_function=true
        if (op == TK_STAR && node->u.unaryop.is_function) {
            Node *args_head = node->ch[1]; // args linked via ->next
            Sx *arg_list = lower_call_args(ctx, args_head);
            int m = ctx_mark(ctx);
            Sx *fp_expr = lower_expr(ctx, node->ch[0]);
            // If the callee is a binop (array subscript of fn ptrs), load it
            if (node->ch[0]->kind == ND_BINOP)
                fp_expr = sx_list(3, sx_sym("load"), sx_int(2), fp_expr);
            fp_expr = ctx_drain(ctx, m, fp_expr);
            Sx *call = sx_cons(sx_sym("icall"), sx_cons(fp_expr, arg_list));
            Type *ftype = node->ch[0]->type;
            if (ftype && istype_ptr(ftype)) ftype = ftype->u.ptr.pointee;
            populate_calldesc(ctx, call, ftype);
            return reg_type(ctx, call, node->type);
        }

        if (op == TK_STAR) {
            // Dereference
            if (node->u.unaryop.is_array_deref)
                return lower_expr(ctx, node->ch[0]); // array non-last dim → address
            int m = ctx_mark(ctx);
            Sx *ptr = lower_expr(ctx, node->ch[0]);
            ptr = ctx_drain(ctx, m, ptr);
            Sx *s = sx_list(3, sx_sym("load"), sx_int(sz(node->type)), ptr);
            return reg_type(ctx, s, node->type);
        }
        if (op == TK_AMPERSAND)
            return lower_lval_addr(ctx, node->ch[0]);
        if (op == TK_PLUS)
            return lower_expr(ctx, node->ch[0]);
        if (op == TK_MINUS || op == TK_TILDE || op == TK_BANG) {
            const char *op_s = op==TK_MINUS?"-": op==TK_TILDE?"~":"!";
            int m = ctx_mark(ctx);
            Sx *e = lower_expr(ctx, node->ch[0]);
            e = ctx_drain(ctx, m, e);
            Sx *s = sx_list(3, sx_sym("unop"), sx_str(op_s), e);
            return reg_type(ctx, s, node->type);
        }
        // Pre-increment / pre-decrement
        if (op == TK_INC || op == TK_DEC) {
            const char *bop = (op == TK_INC) ? "+" : "-";
            LVal lv = lower_lval(ctx, node->ch[0]);
            // Step size: 1 for scalars, sizeof(pointee) for pointers/arrays.
            int step = 1;
            Type *ot = node->ch[0]->type;
            if (ot && ot->base == TB_POINTER && ot->u.ptr.pointee)
                step = ot->u.ptr.pointee->size;
            else if (ot && ot->base == TB_ARRAY && ot->u.arr.elem)
                step = ot->u.arr.elem->size;
            Sx *one = sx_list(2, sx_sym("int"), sx_int(step));
            reg_type(ctx, one, node->type);
            Sx *old = lval_load(ctx, lv, node->type);
            Sx *nw  = sx_list(4, sx_sym("binop"), sx_str(bop), old, one);
            reg_type(ctx, nw, node->type);
            ctx_push_stmt(ctx, lval_store(ctx, lv, nw, node->type));
            // Return fresh read of new value
            return lval_load(ctx, lv, node->type);
        }
        // Post-increment / post-decrement
        if (op == TK_POST_INC || op == TK_POST_DEC) {
            const char *bop = (op == TK_POST_INC) ? "+" : "-";
            LVal lv = lower_lval(ctx, node->ch[0]);
            Sx *old = lval_load(ctx, lv, node->type);
            const char *tmp = fresh_temp(ctx);
            // save old value
            ctx_push_stmt(ctx, sx_list(3, sx_sym("assign"), sx_str(tmp), old));
            Sx *tmp_var = sx_list(2, sx_sym("var"), sx_str(tmp));
            reg_type(ctx, tmp_var, node->type);
            // Step size: 1 for scalars, sizeof(pointee) for pointers/arrays.
            int step = 1;
            Type *ot = node->ch[0]->type;
            if (ot && ot->base == TB_POINTER && ot->u.ptr.pointee)
                step = ot->u.ptr.pointee->size;
            else if (ot && ot->base == TB_ARRAY && ot->u.arr.elem)
                step = ot->u.arr.elem->size;
            Sx *one = sx_list(2, sx_sym("int"), sx_int(step));
            reg_type(ctx, one, node->type);
            Sx *nw = sx_list(4, sx_sym("binop"), sx_str(bop), tmp_var, one);
            reg_type(ctx, nw, node->type);
            ctx_push_stmt(ctx, lval_store(ctx, lv, nw, node->type));
            // Return old value
            Sx *result = sx_list(2, sx_sym("var"), sx_str(tmp));
            return reg_type(ctx, result, node->type);
        }
        break;
    }

    // --------------------------------------------------------
    case ND_BINOP: {
        // Comma operator: evaluate lhs for side effects, return rhs
        if (node->op_kind == TK_COMMA) {
            int m = ctx_mark(ctx);
            Sx *lhs = lower_expr(ctx, node->ch[0]);
            lhs = ctx_drain(ctx, m, lhs);
            ctx_push_stmt(ctx, sx_list(2, sx_sym("expr"), lhs));
            return lower_expr(ctx, node->ch[1]);
        }
        static const struct { Token_kind k; const char *s; }
        ops[] = {
            {TK_PLUS,"+"},   {TK_MINUS,"-"},  {TK_STAR,"*"},
            {TK_SLASH,"/"},  {TK_PERCENT,"%"},{TK_AMPERSAND,"&"},
            {TK_BITOR,"|"},  {TK_BITXOR,"^"}, {TK_SHIFTL,"<<"},
            {TK_SHIFTR,">>"},{TK_EQ,"=="},    {TK_NE,"!="},
            {TK_LT,"<"},     {TK_LE,"<="},    {TK_GT,">"},
            {TK_GE,">="},    {TK_LOGAND,"&&"},{TK_LOGOR,"||"},
        };
        const char *op_s = "?";
        for (size_t i = 0; i < sizeof(ops)/sizeof(ops[0]); i++)
            if (ops[i].k == node->op_kind) { op_s = ops[i].s; break; }
        int m = ctx_mark(ctx);
        Sx *lhs = lower_expr(ctx, node->ch[0]);
        lhs = ctx_drain(ctx, m, lhs);
        m = ctx_mark(ctx);
        Sx *rhs = lower_expr(ctx, node->ch[1]);
        rhs = ctx_drain(ctx, m, rhs);
        Sx *s = sx_list(4, sx_sym("binop"), sx_str(op_s), lhs, rhs);
        return reg_type(ctx, s, node->type);
    }

    // --------------------------------------------------------
    case ND_ASSIGN: {
        int m = ctx_mark(ctx);
        Sx *rhs = lower_expr(ctx, node->ch[1]);
        rhs = ctx_drain(ctx, m, rhs);
        // Struct assignment: memcpy
        if (node->ch[0]->type && node->ch[0]->type->base == TB_STRUCT) {
            Sx *dst = lower_lval_addr(ctx, node->ch[0]);
            // rhs already evaluated above (line 545-546).
            // For struct variables: rhs = lower_lval_addr result = address.
            // For calls returning struct: rhs = call sexp whose result (r0) is
            //   the address of the returned struct.
            // Either way, rhs IS the source address.
            Sx *src = rhs;
            Sx *st  = sx_list(4, sx_sym("memcpy"), dst, src,
                              sx_int(node->ch[0]->type->size));
            ctx_push_stmt(ctx, st);
            return src;
        }
        LVal lv = lower_lval(ctx, node->ch[0]);
        // For memory-based lvals (e.g. buf[len++]), lv.addr may contain a (seq ...)
        // expression with side effects. Evaluating lv.addr multiple times re-runs
        // those side effects (e.g. len++ fires again on each use). Save the address
        // to a temp variable once so both the store and the reload use the stable pointer.
        if (!lv.is_var && lv.addr) {
            const char *addr_tmp = fresh_temp(ctx);
            ctx_push_stmt(ctx, sx_list(3, sx_sym("assign"), sx_str(addr_tmp), lv.addr));
            lv.addr = sx_list(2, sx_sym("var"), sx_str(addr_tmp));
            typemap_set_vtype(ctx->tm, lv.addr->id, VT_PTR);
        }
        ctx_push_stmt(ctx, lval_store(ctx, lv, rhs, node->ch[0]->type));
        return lval_load(ctx, lv, node->type);
    }

    // --------------------------------------------------------
    case ND_COMPOUND_ASSIGN: {
        // The parser maps compound tokens to base tokens:
        // TK_AMP_ASSIGN → TK_AMPERSAND, TK_PLUS_ASSIGN → TK_PLUS, etc.
        // So node->op_kind holds the BASE operator token here.
        static const struct { Token_kind k; const char *s; }
        cops[] = {
            {TK_PLUS,"+"},      {TK_MINUS,"-"},
            {TK_STAR,"*"},      {TK_SLASH,"/"},
            {TK_PERCENT,"%"},   {TK_AMPERSAND,"&"},
            {TK_BITOR,"|"},     {TK_BITXOR,"^"},
            {TK_SHIFTL,"<<"},   {TK_SHIFTR,">>"},
        };
        const char *op_s = "+";
        for (size_t i = 0; i < sizeof(cops)/sizeof(cops[0]); i++)
            if (cops[i].k == node->op_kind) { op_s = cops[i].s; break; }
        int m = ctx_mark(ctx);
        Sx *rhs = lower_expr(ctx, node->ch[1]);
        rhs = ctx_drain(ctx, m, rhs);
        LVal lv = lower_lval(ctx, node->ch[0]);
        // For memory-based lvals (e.g. a[i++]), lv.addr may contain a (seq ...)
        // expression with side effects. Evaluating lv.addr multiple times re-runs
        // those side effects (e.g. i++ fires again on each use). Save the address
        // to a temp variable once so subsequent load and store use the stable pointer.
        if (!lv.is_var && lv.addr) {
            const char *addr_tmp = fresh_temp(ctx);
            ctx_push_stmt(ctx, sx_list(3, sx_sym("assign"), sx_str(addr_tmp), lv.addr));
            lv.addr = sx_list(2, sx_sym("var"), sx_str(addr_tmp));
            typemap_set_vtype(ctx->tm, lv.addr->id, VT_PTR);
        }
        Sx *old = lval_load(ctx, lv, node->ch[0]->type);
        Sx *nw  = sx_list(4, sx_sym("binop"), sx_str(op_s), old, rhs);
        reg_type(ctx, nw, node->type);
        ctx_push_stmt(ctx, lval_store(ctx, lv, nw, node->ch[0]->type));
        return lval_load(ctx, lv, node->type);
    }

    // --------------------------------------------------------
    case ND_CAST: {
        int m = ctx_mark(ctx);
        Sx *child = lower_expr(ctx, node->ch[1]);
        child = ctx_drain(ctx, m, child);
        Sx *s = sx_list(2, sx_sym("cast"), child);
        return reg_type(ctx, s, node->type);
    }

    // --------------------------------------------------------
    case ND_MEMBER: {
        // s.field or s->field
        // member function call: ND_MEMBER with is_function=true, args in ch[1]
        if (node->u.member.is_function) {
            Node *args_head = node->ch[1];
            Sx *arg_list = lower_call_args(ctx, args_head);
            Sx *fp_addr  = lower_lval_addr(ctx, node);
            Sx *fp       = sx_list(3, sx_sym("load"), sx_int(2), fp_addr);
            Sx *call     = sx_cons(sx_sym("icall"), sx_cons(fp, arg_list));
            Type *struct_t = node->ch[0]->type;
            if (struct_t && istype_ptr(struct_t)) struct_t = struct_t->u.ptr.pointee;
            Type *ftype = NULL;
            if (struct_t) find_offset(struct_t, node->u.member.field_name, &ftype);
            populate_calldesc(ctx, call, ftype);
            return reg_type(ctx, call, node->type);
        }
        Sx *addr = lower_lval_addr(ctx, node);
        if (node->type && (node->type->base == TB_STRUCT || node->type->base == TB_ARRAY))
            return addr; // struct/array member address, no load (arrays decay to pointer)
        Sx *s = sx_list(3, sx_sym("load"), sx_int(sz(node->type)), addr);
        return reg_type(ctx, s, node->type);
    }

    // --------------------------------------------------------
    case ND_TERNARY: {
        int m = ctx_mark(ctx);
        Sx *cond = lower_expr(ctx, node->ch[0]);
        cond = ctx_drain(ctx, m, cond);
        m = ctx_mark(ctx);
        Sx *then_e = lower_expr(ctx, node->ch[1]);
        then_e = ctx_drain(ctx, m, then_e);
        m = ctx_mark(ctx);
        Sx *else_e = lower_expr(ctx, node->ch[2]);
        else_e = ctx_drain(ctx, m, else_e);
        Sx *s = sx_list(4, sx_sym("if"), cond, then_e, else_e);
        return reg_type(ctx, s, node->type);
    }

    // --------------------------------------------------------
    case ND_VA_ARG: {
        // va_arg(ap, T): load from *ap then advance ap
        // ap is node->ch[0], type is node->type
        // CPU4 stack slot = 4 bytes (push is 32-bit); advance by slot size
        LVal ap_lv = lower_lval(ctx, node->ch[0]);
        int asz = sz(node->type);
        int slot = asz < 4 ? 4 : asz;  // CPU4: minimum slot size is 4 bytes
        // Save old ap to a temp before advancing, so the load uses the pre-advance
        // pointer. Without this, an SSA-promoted ap would be re-read after the
        // advance statement updates it, producing a load from the wrong address.
        const char *tmp = fresh_temp(ctx);
        Sx *ap_val = lval_load(ctx, ap_lv, node->ch[0]->type);
        ctx_push_stmt(ctx, sx_list(3, sx_sym("assign"), sx_str(tmp), ap_val));
        Sx *tmp_var = sx_list(2, sx_sym("var"), sx_str(tmp));
        reg_type(ctx, tmp_var, node->ch[0]->type);  // ptr type for old ap
        // advance ap: ap = old_ap + slot
        Sx *ap_new = sx_list(4, sx_sym("binop"), sx_str("+"),
                             lval_load(ctx, ap_lv, node->ch[0]->type),
                             sx_int(slot));
        ctx_push_stmt(ctx, lval_store(ctx, ap_lv, ap_new, node->ch[0]->type));
        // load value from saved old ap pointer
        Sx *loaded = sx_list(3, sx_sym("load"), sx_int(asz), tmp_var);
        return reg_type(ctx, loaded, node->type);
    }

    case ND_VA_START: {
        // va_start called as expression (wrapped in ND_EXPRSTMT)
        LVal ap_lv = lower_lval(ctx, node->ch[0]);
        int m = ctx_mark(ctx);
        Sx *last = lower_lval_addr(ctx, node->ch[1]);
        last = ctx_drain(ctx, m, last);
        Sx *ap_val = sx_list(4, sx_sym("binop"), sx_str("+"), last, sx_int(4));
        ctx_push_stmt(ctx, lval_store(ctx, ap_lv, ap_val, node->ch[0]->type));
        return sx_list(2, sx_sym("int"), sx_int(0));
    }

    case ND_VA_END:
        return sx_list(2, sx_sym("int"), sx_int(0));

    default: break;
    }

    // Fallback
    return sx_list(2, sx_sym("int"), sx_int(0));
}

// ============================================================
// Statement lowering
// ============================================================

static Sx *lower_expr_as_stmt(LowerCtx *ctx, Node *expr) {
    if (!expr) return sx_list(1, sx_sym("skip"));
    int m = ctx_mark(ctx);
    Sx *e = lower_expr(ctx, expr);
    return ctx_drain_stmt(ctx, m, sx_list(2, sx_sym("expr"), e));
}

// Helper: emit a single scalar store at byte_offset, return offset+size.
static int emit_scalar_store(Sx *base, int byte_offset, int esize, int val,
                              Sx **block_tail_ptr) {
    Sx *ptr = (byte_offset == 0) ? base
              : sx_list(4, sx_sym("binop"), sx_str("+"), base, sx_int(byte_offset));
    Sx *vsx = sx_list(2, sx_sym("int"), sx_int(val));
    Sx *st  = sx_list(4, sx_sym("store"), sx_int(esize), ptr, vsx);
    *block_tail_ptr = sx_cons(st, NULL);
    *block_tail_ptr = (*block_tail_ptr)->cdr;  // won't work — caller advances tail
    return byte_offset + esize;
}

// Pre-zero an entire local array (all leaf scalars set to 0).
static void emit_zero_fill_array(Sx *base, Type *type, Sx **tail) {
    if (!type || type->base != TB_ARRAY) return;
    Type *leaf = array_elem_type(type);
    int lsz = (leaf && leaf->size > 0) ? leaf->size : 2;
    int total = type->size;
    for (int off = 0; off < total; off += lsz) {
        Sx *ptr_sx = (off == 0) ? base
                     : sx_list(4, sx_sym("binop"), sx_str("+"), base, sx_int(off));
        Sx *st = sx_list(4, sx_sym("store"), sx_int(lsz), ptr_sx,
                         sx_list(2, sx_sym("int"), sx_int(0)));
        *tail = sx_cons(st, NULL); tail = &(*tail)->cdr;
    }
}

// Mirrors CPU3 gen_inits: fills non-zero values into a pre-zeroed array.
// root_type: the outermost array type (used for row_stride computation).
// offset: current row-start byte offset.
// depth: current nesting level (0 = outermost list).
static void emit_array_init_stmts(Sx *base, Type *root_type,
                                   Node *init_list, int offset, int depth,
                                   Sx **tail) {
    if (!root_type || root_type->base != TB_ARRAY) return;
    Type *leaf = array_elem_type(root_type);
    int elem_sz = (leaf && leaf->size > 0) ? leaf->size : 2;

    // row_stride = size of one element at current depth
    Type *arr_at = root_type;
    for (int d = 0; d < depth && arr_at->base == TB_ARRAY; d++)
        arr_at = arr_at->u.arr.elem;
    int row_stride = (arr_at->base == TB_ARRAY && arr_at->u.arr.elem)
                     ? arr_at->u.arr.elem->size : elem_sz;

    int ptr = offset;
    for (Node *item = init_list; item; item = item->next) {
        Node *raw = item;
        while (raw && raw->kind == ND_CAST) raw = raw->ch[1];

        if (raw && raw->kind == ND_INITLIST) {
            // Brace: jump to next row if we've written any scalars in this row
            int new_offset = (ptr != offset) ? offset + row_stride : offset;
            emit_array_init_stmts(base, root_type, raw->ch[0], new_offset, depth + 1, tail);
            while (*tail) tail = &(*tail)->cdr;  // advance past recursively-appended stores
            offset = new_offset + row_stride;
            ptr = offset;
        } else {
            // Scalar: get value and emit store if non-zero
            int v = 0;
            if (raw && raw->kind == ND_LITERAL && !raw->u.literal.strval) {
                if (leaf && (leaf->base == TB_FLOAT || leaf->base == TB_DOUBLE)) {
                    float fv = (float)raw->u.literal.fval;
                    uint32_t bits; memcpy(&bits, &fv, 4); v = (int)bits;
                } else {
                    v = (int)raw->u.literal.ival;
                }
            } else if (raw && raw->kind == ND_UNARYOP && raw->op_kind == TK_MINUS) {
                Node *inner = raw->ch[0];
                while (inner && inner->kind == ND_CAST) inner = inner->ch[1];
                if (inner && inner->kind == ND_LITERAL && !inner->u.literal.strval) {
                    if (leaf && (leaf->base == TB_FLOAT || leaf->base == TB_DOUBLE)) {
                        float fv = -(float)inner->u.literal.fval;
                        uint32_t bits; memcpy(&bits, &fv, 4); v = (int)bits;
                    } else {
                        v = -(int)inner->u.literal.ival;
                    }
                }
            }
            if (v != 0) {
                Sx *ptr_sx = (ptr == 0) ? base
                             : sx_list(4, sx_sym("binop"), sx_str("+"), base, sx_int(ptr));
                Sx *st = sx_list(4, sx_sym("store"), sx_int(elem_sz), ptr_sx,
                                 sx_list(2, sx_sym("int"), sx_int(v)));
                *tail = sx_cons(st, NULL); tail = &(*tail)->cdr;
            }
            ptr += elem_sz;
        }
    }
}

// Emit zero-fill stores for 'size' bytes at (base + byte_offset).
static void emit_zero_fill_bytes(Sx *base, int byte_offset, int size, int scalar_size,
                                  Sx **tail) {
    for (int off = 0; off < size; off += scalar_size) {
        int abs_off = byte_offset + off;
        Sx *ptr_sx = (abs_off == 0) ? base
                     : sx_list(4, sx_sym("binop"), sx_str("+"), base, sx_int(abs_off));
        Sx *st = sx_list(4, sx_sym("store"), sx_int(scalar_size), ptr_sx,
                         sx_list(2, sx_sym("int"), sx_int(0)));
        *tail = sx_cons(st, NULL); tail = &(*tail)->cdr;
    }
}

// Emit stores for a local struct initializer (recursive for nested structs).
static void emit_struct_init_stmts(Sx *base, Type *type, Node *init_list_head,
                                    int byte_offset, Sx **tail) {
    if (!type || type->base != TB_STRUCT) return;
    Field *f = type->u.composite.members;
    Node *el = init_list_head;
    while (f) {
        int foff = byte_offset + f->offset;
        if (el) {
            Node *r = el;
            while (r && r->kind == ND_CAST) r = r->ch[1];
            if (r && r->kind == ND_INITLIST && f->type->base == TB_STRUCT) {
                // Nested struct with brace initializer: recurse
                emit_struct_init_stmts(base, f->type, r->ch[0], foff, tail);
                while (*tail) tail = &(*tail)->cdr;
            } else if (r && r->kind == ND_INITLIST && f->type->base == TB_ARRAY) {
                // Array field with brace initializer
                Sx *fbase = (foff == 0) ? base
                            : sx_list(4, sx_sym("binop"), sx_str("+"), base, sx_int(foff));
                emit_zero_fill_array(fbase, f->type, tail);
                while (*tail) tail = &(*tail)->cdr;
                emit_array_init_stmts(fbase, f->type, r->ch[0], 0, 0, tail);
                while (*tail) tail = &(*tail)->cdr;
            } else {
                // Scalar (or unhandled): get integer value
                int v = 0;
                if (r && r->kind == ND_LITERAL && !r->u.literal.strval) {
                    if (f->type->base == TB_FLOAT || f->type->base == TB_DOUBLE) {
                        float fv = (float)r->u.literal.fval;
                        uint32_t bits; memcpy(&bits, &fv, 4); v = (int)bits;
                    } else {
                        v = (int)r->u.literal.ival;
                    }
                } else if (r && r->kind == ND_UNARYOP && r->op_kind == TK_MINUS) {
                    Node *inner = r->ch[0];
                    while (inner && inner->kind == ND_CAST) inner = inner->ch[1];
                    if (inner && inner->kind == ND_LITERAL && !inner->u.literal.strval)
                        v = -(int)inner->u.literal.ival;
                }
                // Store the scalar; use leaf size for struct/array fields
                int fsize = f->type->size;
                int lsize = (f->type->base == TB_STRUCT || f->type->base == TB_ARRAY) ? 2 : fsize;
                if (lsize <= 0) lsize = 2;
                Sx *ptr_sx = (foff == 0) ? base
                             : sx_list(4, sx_sym("binop"), sx_str("+"), base, sx_int(foff));
                Sx *st = sx_list(4, sx_sym("store"), sx_int(lsize), ptr_sx,
                                 sx_list(2, sx_sym("int"), sx_int(v)));
                *tail = sx_cons(st, NULL); tail = &(*tail)->cdr;
                // Zero-fill remainder of field if composite
                if (fsize > lsize) {
                    emit_zero_fill_bytes(base, foff + lsize, fsize - lsize, lsize, tail);
                    while (*tail) tail = &(*tail)->cdr;
                }
            }
            el = el->next;
        } else {
            // No initializer for this field: zero-fill
            int fsize = f->type->size;
            int lsize = (f->type->base == TB_ARRAY && array_elem_type(f->type))
                        ? array_elem_type(f->type)->size : 2;
            if (lsize <= 0 || lsize > fsize) lsize = 2;
            emit_zero_fill_bytes(base, foff, fsize, lsize, tail);
            while (*tail) tail = &(*tail)->cdr;
        }
        f = f->next;
    }
}

static Sx *lower_stmt(LowerCtx *ctx, Node *node) {
    if (!node) return sx_list(1, sx_sym("skip"));
    switch (node->kind) {

    case ND_EXPRSTMT:
        return lower_expr_as_stmt(ctx, node->ch[0]);

    case ND_COMPSTMT:
        return lower_block(ctx, node->ch[0]);

    case ND_IFSTMT: {
        int m = ctx_mark(ctx);
        Sx *cond = lower_expr(ctx, node->ch[0]);
        Sx *body_cond = ctx_drain_stmt(ctx, m, sx_list(1, sx_sym("_cond_placeholder_")));
        (void)body_cond;
        // Actually drain into the cond expression directly
        m = ctx_mark(ctx);
        cond = lower_expr(ctx, node->ch[0]);
        cond = ctx_drain(ctx, m, cond);
        Sx *then_s = lower_stmt(ctx, node->ch[1]);
        if (node->ch[2]) {
            Sx *else_s = lower_stmt(ctx, node->ch[2]);
            return sx_list(4, sx_sym("if"), cond, then_s, else_s);
        }
        return sx_list(3, sx_sym("if"), cond, then_s);
    }

    case ND_WHILESTMT: {
        int m = ctx_mark(ctx);
        Sx *cond = lower_expr(ctx, node->ch[0]);
        cond = ctx_drain(ctx, m, cond);
        Sx *body = lower_stmt(ctx, node->ch[1]);
        return sx_list(3, sx_sym("while"), cond, body);
    }

    case ND_FORSTMT: {
        Sx *init, *cond, *step, *body;
        init = (node->ch[0] && node->ch[0]->kind != ND_EMPTY)
               ? lower_stmt(ctx, node->ch[0]) : sx_list(1, sx_sym("skip"));
        if (node->ch[1] && node->ch[1]->kind != ND_EMPTY) {
            int m = ctx_mark(ctx);
            cond = lower_expr(ctx, node->ch[1]);
            cond = ctx_drain(ctx, m, cond);
        } else {
            cond = sx_list(2, sx_sym("int"), sx_int(1));
            typemap_set_vtype(ctx->tm, cond->id, VT_I16);
        }
        step = (node->ch[2] && node->ch[2]->kind != ND_EMPTY)
               ? lower_expr_as_stmt(ctx, node->ch[2]) : sx_list(1, sx_sym("skip"));
        body = lower_stmt(ctx, node->ch[3]);
        return sx_list(5, sx_sym("for"), init, cond, step, body);
    }

    case ND_DOWHILESTMT: {
        Sx *body = lower_stmt(ctx, node->ch[0]);
        int m = ctx_mark(ctx);
        Sx *cond = lower_expr(ctx, node->ch[1]);
        cond = ctx_drain(ctx, m, cond);
        return sx_list(3, sx_sym("do"), body, cond);
    }

    case ND_RETURNSTMT: {
        if (!node->ch[0]) return sx_list(1, sx_sym("return"));
        int m = ctx_mark(ctx);
        Sx *val = lower_expr(ctx, node->ch[0]);
        return ctx_drain_stmt(ctx, m, sx_list(2, sx_sym("return"), val));
    }

    case ND_BREAKSTMT:    return sx_list(1, sx_sym("break"));
    case ND_CONTINUESTMT: return sx_list(1, sx_sym("continue"));
    case ND_GOTOSTMT:     return sx_list(2, sx_sym("goto"), sx_str(node->u.label));
    case ND_EMPTY:        return sx_list(1, sx_sym("skip"));
    case ND_STMT:         return lower_stmt(ctx, node->ch[0]);

    case ND_LABELSTMT: {
        Sx *s = lower_stmt(ctx, node->ch[0]);
        return sx_list(3, sx_sym("label"), sx_str(node->u.labelstmt.name), s);
    }

    case ND_SWITCHSTMT: {
        int m = ctx_mark(ctx);
        Sx *sel = lower_expr(ctx, node->ch[0]);
        sel = ctx_drain(ctx, m, sel);
        Sx *body = lower_stmt(ctx, node->ch[1]);
        return sx_list(3, sx_sym("switch"), sel, body);
    }

    case ND_CASESTMT:
        return sx_list(3, sx_sym("case"),
                       sx_list(2, sx_sym("int"), sx_int((int)node->u.casestmt.value)),
                       lower_stmt(ctx, node->ch[0]));

    case ND_DEFAULTSTMT:
        return sx_list(2, sx_sym("default"), lower_stmt(ctx, node->ch[0]));

    case ND_DECLARATION: {
        // Local variable declaration with optional initializer
        Sx *block = sx_list(1, sx_sym("block"));
        Sx **tail = &block->cdr;
        for (Node *d = node->ch[1]; d; d = d->next) {
            if (d->kind != ND_DECLARATOR) continue;
            Symbol *sym = d->symbol;
            if (!sym) continue;
            if (sym->kind == SYM_ENUM_CONST) continue;
            if (sym->ns == NS_TYPEDEF) continue;
            if (istype_function(sym->type)) continue;
            // Static locals: emit data-section gvar; no runtime init code.
            if (sym->kind == SYM_STATIC_LOCAL) {
                Sx *gv = lower_global(ctx, node, sym);
                ctx_push_static_gvar(ctx, gv);
                continue;
            }
            Node *init = d->ch[1]; // initializer
            if (!init) continue;
            // scalar
            if (!istype_array(sym->type) && sym->type->base != TB_STRUCT) {
                const char *name = sym_alpha(ctx, sym);
                int m = ctx_mark(ctx);
                Sx *ival = lower_expr(ctx, init);
                Sx *st;
                if (sym_is_addr_taken(ctx, sym)) {
                    // Variable is address-taken: must store to memory
                    int bpoff = (sym->kind == SYM_PARAM) ? sym->offset : -(sym->offset);
                    Sx *addr = sx_list(3, sx_sym("addr"), sx_str(name), sx_int(bpoff));
                    st = ctx_drain_stmt(ctx, m,
                             sx_list(4, sx_sym("store"), sx_int(sz(sym->type)), addr, ival));
                } else {
                    st = ctx_drain_stmt(ctx, m,
                             sx_list(3, sx_sym("assign"), sx_str(name), ival));
                }
                *tail = sx_cons(st, NULL); tail = &(*tail)->cdr;
            } else if (istype_array(sym->type) && init->kind == ND_LITERAL
                       && init->u.literal.strval) {
                // char s[] = "str": initialise each byte
                const char *str = init->u.literal.strval;
                int slen        = init->u.literal.strval_len;
                int bpoff = (sym->kind == SYM_PARAM) ? sym->offset : -(sym->offset);
                Sx *base = sx_list(3, sx_sym("addr"), sx_str(sym_alpha(ctx, sym)),
                                   sx_int(bpoff));
                for (int i = 0; i <= slen; i++) {
                    Sx *ptr  = sx_list(4, sx_sym("binop"), sx_str("+"), base, sx_int(i));
                    Sx *bval = sx_list(2, sx_sym("int"),
                                      sx_int(i < slen ? (unsigned char)str[i] : 0));
                    Sx *st   = sx_list(4, sx_sym("store"), sx_int(1), ptr, bval);
                    *tail = sx_cons(st, NULL); tail = &(*tail)->cdr;
                }
            } else if (istype_array(sym->type) && init->kind == ND_INITLIST) {
                // Local array initializer: pre-zero then fill non-zero values.
                int bpoff = (sym->kind == SYM_PARAM) ? sym->offset : -(sym->offset);
                Sx *base = sx_list(3, sx_sym("addr"), sx_str(sym_alpha(ctx, sym)),
                                   sx_int(bpoff));
                emit_zero_fill_array(base, sym->type, tail);
                while (*tail) tail = &(*tail)->cdr;
                emit_array_init_stmts(base, sym->type, init->ch[0], 0, 0, tail);
                while (*tail) tail = &(*tail)->cdr;
            } else if (sym->type->base == TB_STRUCT && init->kind == ND_INITLIST) {
                // Local struct initializer.
                int bpoff = (sym->kind == SYM_PARAM) ? sym->offset : -(sym->offset);
                Sx *base = sx_list(3, sx_sym("addr"), sx_str(sym_alpha(ctx, sym)),
                                   sx_int(bpoff));
                emit_struct_init_stmts(base, sym->type, init->ch[0], 0, tail);
                while (*tail) tail = &(*tail)->cdr;
            } else if (sym->type->base == TB_STRUCT) {
                // Struct init from expression (function call returning struct, or
                // struct-to-struct copy). Lower the RHS expression — for calls, the
                // callee returns a pointer to the struct (address of its local) in r0.
                // Emit memcpy(dst, rhs_addr, size).
                int bpoff = (sym->kind == SYM_PARAM) ? sym->offset : -(sym->offset);
                Sx *base = sx_list(3, sx_sym("addr"), sx_str(sym_alpha(ctx, sym)),
                                   sx_int(bpoff));
                int m = ctx_mark(ctx);
                Sx *src;
                if (init->type && init->type->base == TB_STRUCT &&
                    init->kind != ND_IDENT && init->kind != ND_MEMBER) {
                    // Function call: lower_expr evaluates the call; callee returns &ret in r0
                    src = lower_expr(ctx, init);
                } else {
                    // Struct variable: get its address
                    src = lower_lval_addr(ctx, init);
                }
                Sx *st = ctx_drain_stmt(ctx, m,
                             sx_list(4, sx_sym("memcpy"), base, src,
                                     sx_int(sym->type->size)));
                *tail = sx_cons(st, NULL); tail = &(*tail)->cdr;
            }
        }
        if (!block->cdr) return sx_list(1, sx_sym("skip"));
        if (!block->cdr->cdr) return block->cdr->car;
        return block;
    }

    case ND_VA_START: {
        // va_start(ap, last): set *ap = address-just-past-last-param.
        // Use lval_load/store so ap uses the same SSA/memory path as va_arg.
        // CPU4: 4-byte stack slots, so offset is 4.
        LVal ap_lv = lower_lval(ctx, node->ch[0]);
        int m = ctx_mark(ctx);
        Sx *last = lower_lval_addr(ctx, node->ch[1]);  // address of last named param
        last = ctx_drain(ctx, m, last);
        // ap_val = &last + 4 (first variadic arg is 4 bytes past last named param)
        Sx *ap_val = sx_list(4, sx_sym("binop"), sx_str("+"), last, sx_int(4));
        return lval_store(ctx, ap_lv, ap_val, node->ch[0]->type);
    }

    case ND_VA_END:
        return sx_list(1, sx_sym("skip"));

    default:
        if (node->is_expr)
            return lower_expr_as_stmt(ctx, node);
        return sx_list(1, sx_sym("skip"));
    }
}

// Lower linked stmt list into (block s0 s1 ...) or single stmt
static Sx *lower_block(LowerCtx *ctx, Node *first) {
    Sx *block = sx_list(1, sx_sym("block"));
    Sx **tail = &block->cdr;
    int count = 0;
    for (Node *n = first; n; n = n->next) {
        Sx *s = lower_stmt(ctx, n);
        if (!s) continue;
        const char *tag = sx_car_sym(s);
        if (tag && strcmp(tag, "skip") == 0) continue;
        *tail = sx_cons(s, NULL); tail = &(*tail)->cdr;
        count++;
    }
    if (count == 0) return sx_list(1, sx_sym("skip"));
    if (count == 1) return block->cdr->car;
    return block;
}

// ============================================================
// String literal collection (pre-pass to assign IDs)
// ============================================================

static int g_strlit_id;

typedef struct {
    int id; const char *data; int len;
} LStrLit;
static LStrLit *g_strlits;
static int g_nstrlits, g_strlit_cap;

static void collect_strlits(Node *n) {
    if (!n) return;
    if (n->kind == ND_LITERAL && n->u.literal.strval) {
        if (g_nstrlits >= g_strlit_cap) {
            g_strlit_cap = g_strlit_cap ? g_strlit_cap * 2 : 16;
            g_strlits = realloc(g_strlits, g_strlit_cap * sizeof(LStrLit));
        }
        g_strlits[g_nstrlits].id   = g_strlit_id++;
        g_strlits[g_nstrlits].data = n->u.literal.strval;
        g_strlits[g_nstrlits].len  = n->u.literal.strval_len;
        g_nstrlits++;
    }
    for (int i = 0; i < 4; i++) collect_strlits(n->ch[i]);
    collect_strlits(n->next);
}

// ============================================================
// Frame size computation
// ============================================================

// After finalize_local_offsets, sym->offset for SYM_LOCAL is the total
// bp-relative byte offset (accumulated from all enclosing scope sizes).
// The frame size needed is the maximum sym->offset across all locals.
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
// Function lowering
// ============================================================

static Sx *lower_function(LowerCtx *ctx, Node *decl) {
    // decl: ND_DECLARATION (is_func_defn=true)
    //   ch[1]: ND_DECLARATOR (symbol is the function)
    //   ch[2]: ND_COMPSTMT   (function body)
    Node *func_decl = decl->ch[1];
    Node *body_node = decl->ch[2];
    if (!func_decl || !body_node) return NULL;
    Symbol *fsym = func_decl->symbol;
    if (!fsym) return NULL;

    const char *fname = sym_alpha(ctx, fsym);

    // Collect parameters from body's symbol table (SYM_PARAM, sorted by offset)
    Sx *params = sx_list(1, sx_sym("params"));
    Sx **ptail = &params->cdr;

    if (body_node->symtable) {
        // Two-pass: collect all SYM_PARAM symbols
        // Parameters are in order of ascending offset (FRAME_OVERHEAD + 0, +2, ...)
        // Find max param offset to know how many there are, then sort
        Symbol *param_syms[64]; int nparams = 0;
        for (Symbol *s = body_node->symtable->symbols; s; s = s->next) {
            if (s->kind == SYM_PARAM && s->ns == NS_IDENT && nparams < 64)
                param_syms[nparams++] = s;
        }
        // Sort by offset ascending
        for (int i = 0; i < nparams-1; i++)
            for (int j = i+1; j < nparams; j++)
                if (param_syms[i]->offset > param_syms[j]->offset) {
                    Symbol *tmp = param_syms[i];
                    param_syms[i] = param_syms[j];
                    param_syms[j] = tmp;
                }
        for (int i = 0; i < nparams; i++) {
            const char *pname = sym_alpha(ctx, param_syms[i]);
            *ptail = sx_cons(sx_str(pname), NULL);
            ptail = &(*ptail)->cdr;
        }
    }

    int frame_size = 0;
    max_frame_r(body_node, &frame_size);

    int is_variadic = (fsym->type && fsym->type->base == TB_FUNCTION &&
                       fsym->type->u.fn.is_variadic) ? 1 : 0;

    // Pre-scan for address-taken locals/params so they use memory (not SSA) paths
    ctx->n_addr_taken = 0;
    scan_addr_taken(ctx, body_node);

    // For variadic functions, build a mapping from Symbol* → CPU4 bp offset.
    // CPU4 uses 4-byte push slots and 4-byte frame overhead (vs CPU3's 2-byte slots
    // and 8-byte overhead). sym->offset reflects CPU3 layout; we correct it here.
    ctx->n_vparams = 0;
    if (is_variadic && body_node->symtable) {
        // param_syms[] is already sorted by offset above; re-collect here
        Symbol *vps[64]; int nvps = 0;
        for (Symbol *s = body_node->symtable->symbols; s; s = s->next)
            if (s->kind == SYM_PARAM && s->ns == NS_IDENT && nvps < 64)
                vps[nvps++] = s;
        // Sort by offset ascending (same order as param_syms[])
        for (int i = 0; i < nvps-1; i++)
            for (int j = i+1; j < nvps; j++)
                if (vps[i]->offset > vps[j]->offset) {
                    Symbol *tmp = vps[i]; vps[i] = vps[j]; vps[j] = tmp;
                }
        for (int i = 0; i < nvps; i++) {
            int cpu4_off = 4 + i * 4;  // 4-byte frame overhead + 4-byte slot * idx
            if (ctx->n_vparams >= ctx->vparam_cap) {
                ctx->vparam_cap = ctx->vparam_cap ? ctx->vparam_cap * 2 : 8;
                ctx->vparam_syms    = realloc(ctx->vparam_syms,
                                              ctx->vparam_cap * sizeof(Symbol *));
                ctx->vparam_offsets = realloc(ctx->vparam_offsets,
                                              ctx->vparam_cap * sizeof(int));
            }
            ctx->vparam_syms[ctx->n_vparams]    = vps[i];
            ctx->vparam_offsets[ctx->n_vparams]  = cpu4_off;
            ctx->n_vparams++;
        }
    }

    // For non-variadic functions, address-taken SYM_PARAMs must be homed to local
    // stack slots because register-ABI params are not on the stack.
    // Collect them, allocate frame space, and record home offsets in ctx.
    ctx->n_param_homes = 0;
    if (!is_variadic && body_node->symtable) {
        // Collect address-taken param symbols
        Symbol *at_params[64]; int n_at = 0;
        for (Symbol *s = body_node->symtable->symbols; s && n_at < 64; s = s->next)
            if (s->kind == SYM_PARAM && s->ns == NS_IDENT && sym_is_addr_taken(ctx, s))
                at_params[n_at++] = s;
        // Sort by offset for deterministic layout
        for (int i = 0; i < n_at - 1; i++)
            for (int j = i + 1; j < n_at; j++)
                if (at_params[i]->offset > at_params[j]->offset) {
                    Symbol *tmp = at_params[i]; at_params[i] = at_params[j]; at_params[j] = tmp;
                }
        for (int i = 0; i < n_at; i++) {
            Symbol *ps = at_params[i];
            int psz = ps->type ? ps->type->size : 2;
            if (psz == 4 && (frame_size % 4) != 0) frame_size += 2; // align to 4
            frame_size += psz;
            int home_off = -frame_size;  // negative = below bp
            if (ctx->n_param_homes >= ctx->param_home_cap) {
                ctx->param_home_cap = ctx->param_home_cap ? ctx->param_home_cap * 2 : 8;
                ctx->param_home_syms = realloc(ctx->param_home_syms,
                                               ctx->param_home_cap * sizeof(Symbol *));
                ctx->param_home_offs = realloc(ctx->param_home_offs,
                                               ctx->param_home_cap * sizeof(int));
            }
            ctx->param_home_syms[ctx->n_param_homes] = ps;
            ctx->param_home_offs[ctx->n_param_homes] = home_off;
            ctx->n_param_homes++;
        }
    }

    Sx *body = lower_stmt(ctx, body_node);

    // If any params were homed, prepend stores to copy register values to home slots.
    // The (var "p@N") on the RHS resolves to the IK_PARAM value in Braun SSA.
    if (ctx->n_param_homes > 0) {
        Sx *block = sx_list(1, sx_sym("block"));
        Sx **tail = &block->cdr;
        for (int i = 0; i < ctx->n_param_homes; i++) {
            Symbol *ps = ctx->param_home_syms[i];
            int home_off = ctx->param_home_offs[i];
            int psz = ps->type ? ps->type->size : 2;
            const char *pname = sym_alpha(ctx, ps);  // already registered above
            Sx *addr = sx_list(3, sx_sym("addr"), sx_str(pname), sx_int(home_off));
            Sx *var  = sx_list(2, sx_sym("var"),  sx_str(pname));
            Sx *st   = sx_list(4, sx_sym("store"), sx_int(psz), addr, var);
            *tail = sx_cons(st, NULL);
            tail = &(*tail)->cdr;
        }
        *tail = sx_cons(body, NULL);
        body = block;
    }

    // (func "name" frame_size is_variadic (params ...) body)
    return sx_list(6, sx_sym("func"), sx_str(fname), sx_int(frame_size),
                   sx_int(is_variadic), params, body);
}

// ============================================================
// Global variable lowering
// ============================================================

static Sx *lower_global(LowerCtx *ctx, Node *decl, Symbol *sym) {
    const char *name = sym_alpha(ctx, sym);
    // Find initializer
    Node *init = NULL;
    for (Node *d = decl->ch[1]; d; d = d->next) {
        if (d->kind == ND_DECLARATOR && d->symbol == sym && d->ch[1]) {
            init = d->ch[1]; break;
        }
    }

    int size = sym->type ? sym->type->size : 2;
    Sx *gv = sx_list(2, sx_sym("gvar"), sx_str(name));
    Sx **tail = &gv->cdr->cdr;
    *tail = sx_cons(sx_int(size), NULL); tail = &(*tail)->cdr;

    if (!init) return gv; // zero-filled

    // Peel casts inserted by insert_coercions (e.g. int→long for global long x=12)
    // to expose the underlying literal for compile-time constant evaluation.
    // Also handle unary minus wrapping a literal (e.g. long g = -5).
    {
        int neg = 1;
        Node *r = init;
        while (r && r->kind == ND_CAST) r = r->ch[1];
        if (r && r->kind == ND_UNARYOP && r->op_kind == TK_MINUS) {
            neg = -1;
            r = r->ch[0];
        }
        while (r && r->kind == ND_CAST) r = r->ch[1];
        if (r && r->kind == ND_LITERAL && !r->u.literal.strval) {
            if (neg != 1) {
                // Rebuild literal with negated value
                // (safe only for integer literals — float literals use fval)
                Node *tmp = r;
                r = (Node *)arena_alloc(sizeof(Node));
                *r = *tmp;
                r->u.literal.ival = -tmp->u.literal.ival;
                r->u.literal.fval = -tmp->u.literal.fval;
            }
            init = r;
        }
    }

    if (init->kind == ND_LITERAL && !init->u.literal.strval) {
        if (sym->type && (sym->type->base == TB_FLOAT ||
                          sym->type->base == TB_DOUBLE)) {
            float fv = (float)init->u.literal.fval;
            uint32_t bits; memcpy(&bits, &fv, 4);
            *tail = sx_cons(sx_int((int)bits), NULL);
        } else {
            *tail = sx_cons(sx_int((int)init->u.literal.ival), NULL);
        }
        return gv;
    }
    if (init->kind == ND_LITERAL && init->u.literal.strval) {
        if (sym->type && sym->type->base == TB_ARRAY) {
            // char s[] = "hello" → embed bytes directly; don't emit a separate strlit
            // Find the pre-collected strlit and remove it from g_strlits (it was eagerly collected).
            // Emit as (strbytes byte0 byte1 ... byteN).
            const char *bytes = init->u.literal.strval;
            int len = init->u.literal.strval_len;
            Sx *sb = sx_list(1, sx_sym("strbytes"));
            Sx **bt = &sb->cdr;
            for (int j = 0; j <= len; j++) {
                *bt = sx_cons(sx_int(j < len ? (unsigned char)bytes[j] : 0), NULL);
                bt = &(*bt)->cdr;
            }
            *tail = sx_cons(sb, NULL);
            // Remove from g_strlits (it was collected by collect_strlits earlier)
            // and advance g_strlit_id to keep the counter in sync with lower_expr.
            for (int k = 0; k < g_nstrlits; k++) {
                if (g_strlits[k].data == bytes) {
                    // Remove by shifting
                    for (int m = k; m < g_nstrlits - 1; m++)
                        g_strlits[m] = g_strlits[m+1];
                    g_nstrlits--;
                    g_strlit_id++;  // consume the pre-assigned ID slot
                    break;
                }
            }
        } else {
            // char *p = "hello" → pointer reference to string literal label
            char buf[32]; snprintf(buf, sizeof(buf), "_l%d", g_strlit_id++);
            *tail = sx_cons(sx_list(2, sx_sym("strref"), sx_str(arena_strdup(buf))), NULL);
        }
        return gv;
    }
    // Array / struct initializer list
    if (init->kind == ND_INITLIST) {
        // Determine element size (for flat integer arrays; structs use per-field sizes)
        Type *etype = (sym->type && sym->type->base == TB_ARRAY)
                      ? sym->type->u.arr.elem : NULL;
        int esize = etype ? etype->size : 2;
        // Build (ginit elem_size v0 v1 ...) list
        Sx *gi = sx_list(2, sx_sym("ginit"), sx_int(esize));
        Sx **gt = &gi->cdr->cdr;
        // Flatten initializer list elements (handle nested ND_INITLIST for multi-dim)
        // For simplicity: flatten recursively up to 3 levels
        for (Node *el = init->ch[0]; el; el = el->next) {
            Node *raw_el = el;
            while (raw_el && raw_el->kind == ND_CAST) raw_el = raw_el->ch[1];
            if (raw_el && raw_el->kind == ND_INITLIST) {
                // Nested initializer (e.g. inner dimension of 2D array)
                for (Node *nel = raw_el->ch[0]; nel; nel = nel->next) {
                    Node *r = nel;
                    while (r && r->kind == ND_CAST) r = r->ch[1];
                    if (r && r->kind == ND_INITLIST) {
                        for (Node *nnel = r->ch[0]; nnel; nnel = nnel->next) {
                            Node *rr = nnel;
                            while (rr && rr->kind == ND_CAST) rr = rr->ch[1];
                            int v = (rr && rr->kind == ND_LITERAL) ? (int)rr->u.literal.ival : 0;
                            *gt = sx_cons(sx_int(v), NULL); gt = &(*gt)->cdr;
                        }
                    } else {
                        int v = (r && r->kind == ND_LITERAL) ? (int)r->u.literal.ival : 0;
                        *gt = sx_cons(sx_int(v), NULL); gt = &(*gt)->cdr;
                    }
                }
            } else {
                int neg = 1;
                Node *r = raw_el;
                if (r && r->kind == ND_UNARYOP && r->op_kind == TK_MINUS) {
                    neg = -1; r = r->ch[0];
                }
                while (r && r->kind == ND_CAST) r = r->ch[1];
                // String literal element in pointer array: char *arr[] = {"a", "b"}
                if (r && r->kind == ND_LITERAL && r->u.literal.strval &&
                    etype && etype->base == TB_POINTER) {
                    char buf[32]; snprintf(buf, sizeof(buf), "_l%d", g_strlit_id++);
                    *gt = sx_cons(sx_list(2, sx_sym("strref"), sx_str(arena_strdup(buf))), NULL);
                    gt = &(*gt)->cdr;
                } else {
                    int v = (r && r->kind == ND_LITERAL) ? (int)r->u.literal.ival * neg : 0;
                    // For float elements
                    if (etype && (etype->base == TB_FLOAT || etype->base == TB_DOUBLE)) {
                        double fv = (r && r->kind == ND_LITERAL) ? r->u.literal.fval * neg : 0.0;
                        float fv32 = (float)fv;
                        uint32_t bits; memcpy(&bits, &fv32, 4); v = (int)bits;
                    }
                    *gt = sx_cons(sx_int(v), NULL); gt = &(*gt)->cdr;
                }
            }
        }
        *tail = sx_cons(gi, NULL);
        return gv;
    }
    return gv;
}

// ============================================================
// lower_program — entry point
// ============================================================

Sx *lower_program(Node *root, TypeMap *tm, int tu_index, int *strlit_id) {
    if (!root) return NULL;

    // Pre-collect string literals to assign IDs starting from *strlit_id
    g_strlit_id  = *strlit_id;
    g_nstrlits   = 0;
    collect_strlits(root);
    g_strlit_id  = *strlit_id; // reset to start; lower_expr will reassign same IDs

    LowerCtx ctx = {0};
    ctx.tu_index     = tu_index;
    ctx.tm           = tm;
    ctx.strlit_count = &g_strlit_id;

    Sx *program = sx_list(1, sx_sym("program"));
    Sx **tail   = &program->cdr;

    Node *decls = (root->kind == ND_PROGRAM) ? root->ch[0] : root;
    for (Node *d = decls; d; d = d->next) {
        if (d->kind != ND_DECLARATION) continue;
        if (d->u.declaration.is_func_defn) {
            Sx *f = lower_function(&ctx, d);
            if (f) { *tail = sx_cons(f, NULL); tail = &(*tail)->cdr; }
            // Flush static locals collected during function lowering
            for (int i = 0; i < ctx.n_static_gvars; i++) {
                *tail = sx_cons(ctx.static_gvars[i], NULL); tail = &(*tail)->cdr;
            }
            ctx.n_static_gvars = 0;
        } else {
            for (Node *decl = d->ch[1]; decl; decl = decl->next) {
                if (decl->kind != ND_DECLARATOR) continue;
                Symbol *sym = decl->symbol;
                if (!sym) continue;
                if (sym->kind == SYM_EXTERN || sym->kind == SYM_ENUM_CONST) continue;
                if (istype_function(sym->type)) continue;
                // Skip typedef declarations — they have no runtime storage.
                if (d->u.declaration.sclass == SC_TYPEDEF) continue;
                Sx *gv = lower_global(&ctx, d, sym);
                *tail = sx_cons(gv, NULL); tail = &(*tail)->cdr;
            }
        }
    }

    // Append string literals
    for (int i = 0; i < g_nstrlits; i++) {
        char buf[32]; snprintf(buf, sizeof(buf), "_l%d", g_strlits[i].id);
        Sx *sl = sx_list(2, sx_sym("strlit"), sx_str(arena_strdup(buf)));
        Sx **stail = &sl->cdr->cdr;
        const char *bytes = g_strlits[i].data;
        int len = g_strlits[i].len;
        for (int j = 0; j <= len; j++) {
            *stail = sx_cons(sx_int(j < len ? (unsigned char)bytes[j] : 0), NULL);
            stail = &(*stail)->cdr;
        }
        *tail = sx_cons(sl, NULL); tail = &(*tail)->cdr;
    }

    // Write back the final strlit counter so the next TU continues from here
    *strlit_id = g_strlit_id;

    return program;
}
