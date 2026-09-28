/*
 * const.c — the compiler's one constant-expression evaluator (see const.h).
 *
 * Replaces the partial evaluators that used to live in types.c (array
 * sizes), parser.c (enum values, case labels), lower.c and braun.c (static
 * initialisers), each of which silently produced 0 for what it did not
 * understand (docs/issues/0004, 0005).
 */
#include <string.h>
#include "const.h"
#include "cpu4/fpu_model.h"

static long long norm(long long v, bool uns) {
    return uns ? (long long)(uint32_t)v : (long long)(int32_t)v;
}

static bool type_unsigned(Type *t) {
    return t && (t->base == TB_UCHAR || t->base == TB_USHORT ||
                 t->base == TB_UINT  || t->base == TB_ULONG);
}

static bool type_float(Type *t) {
    return t && (t->base == TB_FLOAT || t->base == TB_DOUBLE);
}

static uint32_t to_fbits(double d) {
    float f = (float)d; uint32_t b; memcpy(&b, &f, 4); return b;
}

static Symbol *ident_symbol(Node *n) {
    if (n->symbol) return n->symbol;
    return n->st ? find_symbol_st(n->st, n->u.ident.name, NS_IDENT) : NULL;
}

static bool is_static_storage(Symbol *s) {
    return s && (s->kind == SYM_GLOBAL || s->kind == SYM_STATIC_GLOBAL ||
                 s->kind == SYM_STATIC_LOCAL || s->kind == SYM_EXTERN ||
                 s->kind == SYM_BUILTIN);
}

// Convert v to an int (for int contexts: shifts, bitwise, sizes).
static bool as_int(CVal *v) {
    if (v->kind == CV_INT) return true;
    if (v->kind == CV_FLT) {
        v->kind = CV_INT; v->uns = false;
        v->i = (int32_t)cpu4_ftoi(v->fbits);
        return true;
    }
    return false;
}

static void as_flt(CVal *v) {
    if (v->kind == CV_INT) {
        v->fbits = v->uns ? to_fbits((double)(uint32_t)v->i) : cpu4_itof((uint32_t)v->i);
        v->kind = CV_FLT;
    }
}

static bool truthy(CVal *v) {
    if (v->kind == CV_INT) return v->i != 0;
    if (v->kind == CV_FLT) return (v->fbits & 0x7fffffffu) != 0;
    return true;                      // an address constant is never null
}

static void set_int(CVal *out, long long v, bool uns) {
    out->kind = CV_INT; out->uns = uns; out->i = norm(v, uns); out->label = NULL;
}

// The address of an lvalue with static storage duration.
static bool addr_of(Node *n, CVal *out, StrlitFn strlit) {
    if (!n) return false;
    if (n->kind == ND_IDENT) {
        Symbol *s = ident_symbol(n);
        if (!is_static_storage(s)) return false;
        out->kind = CV_ADDR; out->label = sym_label(s); out->i = 0;
        return true;
    }
    if (n->kind == ND_UNARYOP && n->op_kind == TK_STAR)          // &*p == p
        return const_eval(n->ch[0], out, strlit) && out->kind == CV_ADDR;
    if (n->kind == ND_UNARYOP && n->op_kind == TK_PLUS && n->u.unaryop.is_array_deref)
        return const_eval(n->ch[0], out, strlit) && out->kind == CV_ADDR;
    if (n->kind == ND_MEMBER && n->ch[0]) {
        bool ok = (n->op_kind == TK_ARROW) ? const_eval(n->ch[0], out, strlit)
                                           : addr_of(n->ch[0], out, strlit);
        if (!ok || out->kind != CV_ADDR) return false;
        out->i += n->u.member.offset;
        return true;
    }
    if (n->kind == ND_LITERAL && n->u.literal.strval && strlit) {
        out->kind = CV_ADDR; out->i = 0;
        out->label = strlit(n->u.literal.strval, n->u.literal.strval_len);
        return true;
    }
    return false;
}

static bool eval_cast(Type *t, CVal *v) {
    if (!t || t->base == TB_VOID) return false;
    if (type_float(t)) {
        if (v->kind == CV_ADDR) return false;
        as_flt(v);
        return true;
    }
    if (t->base == TB_POINTER || t->base == TB_ARRAY || t->base == TB_FUNCTION) {
        if (v->kind == CV_FLT) return false;
        if (v->kind == CV_INT) v->uns = true;
        return true;
    }
    if (v->kind == CV_ADDR)                     // (int)&x: still an address, if it fits
        return t->size >= PTR_SIZE;
    if (!as_int(v)) return false;
    int bits = t->size * 8;
    long long x = v->i;
    if (bits < 32) {
        x &= (1LL << bits) - 1;
        if (!type_unsigned(t) && (x >> (bits - 1))) x -= 1LL << bits;
    }
    set_int(v, x, type_unsigned(t) && bits >= 32);
    return true;
}

static bool eval_binop(Token_kind op, CVal *l, CVal *r, CVal *out) {
    // Address arithmetic: offsets are already scaled by insert_coercions.
    if (l->kind == CV_ADDR || r->kind == CV_ADDR) {
        if (op == TK_PLUS && l->kind == CV_ADDR && r->kind == CV_INT) {
            *out = *l; out->i += r->i; return true;
        }
        if (op == TK_PLUS && r->kind == CV_ADDR && l->kind == CV_INT) {
            *out = *r; out->i += l->i; return true;
        }
        if (op == TK_MINUS && l->kind == CV_ADDR && r->kind == CV_INT) {
            *out = *l; out->i -= r->i; return true;
        }
        return false;
    }
    bool shift = (op == TK_SHIFTL || op == TK_SHIFTR);
    bool intop = shift || op == TK_PERCENT || op == TK_AMPERSAND ||
                 op == TK_BITOR || op == TK_BITXOR;
    if (!intop && (l->kind == CV_FLT || r->kind == CV_FLT)) {
        as_flt(l); as_flt(r);
        uint32_t a = l->fbits, b = r->fbits;
        switch (op) {
        case TK_PLUS:  out->kind = CV_FLT; out->fbits = cpu4_fadd(a, b); return true;
        case TK_MINUS: out->kind = CV_FLT; out->fbits = cpu4_fsub(a, b); return true;
        case TK_STAR:  out->kind = CV_FLT; out->fbits = cpu4_fmul(a, b); return true;
        case TK_SLASH: out->kind = CV_FLT; out->fbits = cpu4_fdiv(a, b); return true;
        case TK_LT:    set_int(out, cpu4_flt(a, b), false); return true;
        case TK_LE:    set_int(out, cpu4_fle(a, b), false); return true;
        case TK_GT:    set_int(out, cpu4_flt(b, a), false); return true;
        case TK_GE:    set_int(out, cpu4_fle(b, a), false); return true;
        case TK_EQ:    set_int(out, a == b, false); return true;
        case TK_NE:    set_int(out, a != b, false); return true;
        default: return false;
        }
    }
    if (!as_int(l) || !as_int(r)) return false;
    bool uns = shift ? l->uns : (l->uns || r->uns);
    long long a = l->i, b = r->i;
    uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
    int32_t  sa = (int32_t)a,  sb = (int32_t)b;
    switch (op) {
    case TK_PLUS:    set_int(out, (long long)ua + ub, uns); return true;
    case TK_MINUS:   set_int(out, (long long)ua - ub, uns); return true;
    case TK_STAR:    set_int(out, (long long)((uint64_t)ua * ub), uns); return true;
    case TK_SLASH:
    case TK_PERCENT:
        if (ub == 0) return false;
        if (uns) set_int(out, op == TK_SLASH ? ua / ub : ua % ub, true);
        else if (sa == INT32_MIN && sb == -1) set_int(out, op == TK_SLASH ? sa : 0, false);
        else set_int(out, op == TK_SLASH ? sa / sb : sa % sb, false);
        return true;
    case TK_SHIFTL:  set_int(out, ub >= 32 ? 0 : (long long)((uint64_t)ua << ub), uns); return true;
    case TK_SHIFTR:
        if (uns) set_int(out, ub >= 32 ? 0 : ua >> ub, true);
        else     set_int(out, ub >= 32 ? (sa < 0 ? -1 : 0) : sa >> ub, false);
        return true;
    case TK_AMPERSAND: set_int(out, ua & ub, uns); return true;
    case TK_BITOR:     set_int(out, ua | ub, uns); return true;
    case TK_BITXOR:    set_int(out, ua ^ ub, uns); return true;
    case TK_LT: set_int(out, uns ? ua <  ub : sa <  sb, false); return true;
    case TK_LE: set_int(out, uns ? ua <= ub : sa <= sb, false); return true;
    case TK_GT: set_int(out, uns ? ua >  ub : sa >  sb, false); return true;
    case TK_GE: set_int(out, uns ? ua >= ub : sa >= sb, false); return true;
    case TK_EQ: set_int(out, ua == ub, false); return true;
    case TK_NE: set_int(out, ua != ub, false); return true;
    default: return false;
    }
}

bool const_eval(Node *n, CVal *out, StrlitFn strlit) {
    if (!n) return false;
    memset(out, 0, sizeof *out);
    switch (n->kind) {
    case ND_LITERAL:
        if (n->u.literal.strval)
            return addr_of(n, out, strlit);
        if (type_float(n->type)) {
            out->kind = CV_FLT; out->fbits = to_fbits(n->u.literal.fval);
            return true;
        }
        set_int(out, n->u.literal.ival, type_unsigned(n->type));
        return true;

    case ND_IDENT: {
        Symbol *s = ident_symbol(n);
        if (!s) return false;
        if (s->kind == SYM_ENUM_CONST) { set_int(out, s->offset, false); return true; }
        // An array or function designator decays to its address.
        if (s->type && (s->type->base == TB_ARRAY || s->type->base == TB_FUNCTION))
            return addr_of(n, out, strlit);
        return false;
    }

    case ND_CAST: {
        if (!const_eval(n->ch[1], out, strlit)) return false;
        return eval_cast(n->type, out);
    }

    case ND_UNARYOP: {
        if (n->op_kind == TK_AMPERSAND)
            return addr_of(n->ch[0], out, strlit);
        if (n->u.unaryop.is_array_deref)          // a[i] of a non-last dimension: an address
            return addr_of(n, out, strlit);
        if (n->op_kind == TK_STAR) {
            // arr[i] of the last dimension is *(arr + i); only its address
            // (under &) is constant, and addr_of handles that.
            return false;
        }
        if (!const_eval(n->ch[0], out, strlit)) return false;
        switch (n->op_kind) {
        case TK_PLUS:  return out->kind != CV_ADDR;
        case TK_MINUS:
            if (out->kind == CV_FLT) { out->fbits ^= 0x80000000u; return true; }
            if (out->kind != CV_INT) return false;
            set_int(out, -out->i, out->uns); return true;
        case TK_TILDE:
            if (!as_int(out)) return false;
            set_int(out, ~out->i, out->uns); return true;
        case TK_BANG: {
            bool t = truthy(out);
            set_int(out, !t, false); return true;
        }
        default: return false;
        }
    }

    case ND_BINOP: {
        CVal l, r;
        if (n->op_kind == TK_LOGAND || n->op_kind == TK_LOGOR) {
            if (!const_eval(n->ch[0], &l, strlit)) return false;
            bool lt = truthy(&l);
            if (n->op_kind == TK_LOGAND && !lt) { set_int(out, 0, false); return true; }
            if (n->op_kind == TK_LOGOR  &&  lt) { set_int(out, 1, false); return true; }
            if (!const_eval(n->ch[1], &r, strlit)) return false;
            set_int(out, truthy(&r), false); return true;
        }
        if (!const_eval(n->ch[0], &l, strlit) || !const_eval(n->ch[1], &r, strlit)) return false;
        return eval_binop(n->op_kind, &l, &r, out);
    }

    case ND_TERNARY: {
        CVal c;
        if (!const_eval(n->ch[0], &c, strlit)) return false;
        return const_eval(truthy(&c) ? n->ch[1] : n->ch[2], out, strlit);
    }

    case ND_MEMBER:
        return false;                 // a member's value is not constant; &s.m is (addr_of)

    default:
        return false;
    }
}

long long const_int(Node *n, const char *what) {
    CVal v;
    if (!const_eval(n, &v, NULL) || !as_int(&v))
        src_error(n ? n->line : 0, n ? n->col : 0, "%s is not an integer constant expression", what);
    return v.i;
}
