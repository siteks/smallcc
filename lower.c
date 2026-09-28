/*
 * lower.c — Lowering pass: Node* → Sexp AST for global variables and string literals
 *
 * Walks the type-annotated Node* tree and produces a (program gvar... strlit...)
 * sexp for the data section. Function bodies are compiled directly to SSA by braun.c.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "lower.h"
#include "sx.h"
#include "smallcc.h"
#include "braun.h"   // strlit_lookup / strlit_register (cross-TU dedup)
#include "const.h"
#include "cpu4/fpu_model.h"

// ============================================================
// String literal accumulator
// ============================================================

typedef struct { int id; const char *data; int len; } LStrLit;
static LStrLit *g_strlits;
static int      g_nstrlits, g_strlit_cap;
static int      g_strlit_id;  // current strlit counter (shared with caller via pointer)

// Push a new string literal entry (caller has already incremented g_strlit_id).
static void push_strlit(int id, const char *data, int len) {
    if (g_nstrlits >= g_strlit_cap) {
        int nc = g_strlit_cap ? g_strlit_cap * 2 : 16;
        LStrLit *nb = arena_alloc(nc * sizeof(LStrLit));
        memcpy(nb, g_strlits, g_nstrlits * sizeof(LStrLit));
        g_strlits    = nb;
        g_strlit_cap = nc;
    }
    g_strlits[g_nstrlits].id   = id;
    g_strlits[g_nstrlits].data = data;
    g_strlits[g_nstrlits].len  = len;
    g_nstrlits++;
}

// Return the label "_lN" for a string literal, reusing an existing label
// when an identical literal was already assigned (cross-TU dedup); otherwise
// assign the next strlit ID and record the data so it can be emitted later.
static const char *assign_strlit(const char *data, int len) {
    int id = strlit_lookup(data, len);
    if (id < 0) {
        id = g_strlit_id++;
        strlit_register(data, len, id);
        push_strlit(id, data, len);
    }
    char buf[32]; snprintf(buf, sizeof(buf), "_l%d", id);
    return arena_strdup(buf);
}

// ============================================================
// Static data layout
// ============================================================
//
// One walk, driven by the object's Type (array elements, struct fields, the
// first member of a union), lays out every static initialiser: globals here
// and static locals from braun.c. Values come from const_eval; anything it
// cannot evaluate is a compile error, never a zero (docs/issues/0004).
//
// The result is the data-section form emit.c and irsim.c consume:
//   (gvar label size)             zero-filled
//   (gvar label size v)           scalar integer / float bits
//   (gvar label size (strref L))  scalar pointer
//   (gvar label size (gfields item...))  with items (sz v), (0 n) padding,
//                                  (strref L) a pointer-sized address

typedef struct {
    Sx     *head;
    Sx    **tail;
    int     off;          // bytes laid out so far
    StrlitFn strlit;
} Layout;

static void lay_item(Layout *L, int off, Sx *item, int size) {
    if (off > L->off) {
        *L->tail = sx_cons(sx_list(2, sx_int(0), sx_int(off - L->off)), NULL);
        L->tail = &(*L->tail)->cdr;
    }
    if (item) {
        *L->tail = sx_cons(item, NULL);
        L->tail = &(*L->tail)->cdr;
    }
    L->off = off + size;
}

static bool is_aggregate(Type *t) {
    return t && (t->base == TB_ARRAY || t->base == TB_STRUCT);
}

static Node *strip_casts(Node *n) {
    while (n && n->kind == ND_CAST) n = n->ch[1];
    return n;
}

// A string literal initialising a char array (possibly braced: {"abc"}).
static Node *char_array_string(Type *t, Node *init) {
    if (!t || t->base != TB_ARRAY || !t->u.arr.elem || t->u.arr.elem->size != 1) return NULL;
    Node *n = init;
    if (n && n->kind == ND_INITLIST && n->ch[0] && !n->ch[0]->next) n = n->ch[0];
    n = strip_casts(n);
    return (n && n->kind == ND_LITERAL && n->u.literal.strval) ? n : NULL;
}

static uint32_t uint_to_fbits(uint32_t u) {
    float f = (float)u; uint32_t b; memcpy(&b, &f, 4); return b;
}

static void lay_scalar(Layout *L, Type *t, int off, Node *e) {
    if (e && e->kind == ND_INITLIST) {                   // int x = { 5 };
        if (e->ch[0] && e->ch[0]->next)
            src_error(e->line, e->col, "too many initialisers for a scalar");
        e = e->ch[0];
        if (!e) return;
    }
    CVal v;
    if (!const_eval(e, &v, L->strlit))
        src_error(e->line, e->col, "initialiser is not a constant expression");
    int size = t->size;
    bool fp = (t->base == TB_FLOAT || t->base == TB_DOUBLE);
    if (v.kind == CV_ADDR) {
        if (fp || size != PTR_SIZE)
            src_error(e->line, e->col, "an address does not fit a %d-byte initialiser", size);
        if (v.i != 0)
            src_error(e->line, e->col, "address-plus-offset initialisers are not supported");
        lay_item(L, off, sx_list(2, sx_sym("strref"), sx_str(v.label)), PTR_SIZE);
        return;
    }
    uint32_t bits;
    if (fp) {
        if (v.kind == CV_INT)
            bits = v.uns ? uint_to_fbits((uint32_t)v.i) : cpu4_itof((uint32_t)v.i);
        else
            bits = v.fbits;
    } else {
        bits = (v.kind == CV_FLT) ? cpu4_ftoi(v.fbits) : (uint32_t)v.i;
    }
    int32_t val = (int32_t)bits;
    if (size == 1) val = (int8_t)val;                    // the directive masks; keep the
    else if (size == 2) val = (int16_t)val;              // printed value in range
    lay_item(L, off, sx_list(2, sx_int(size), sx_int(val)), size);
}

static void lay_object(Layout *L, Type *t, int off, Node *init);

// Fill aggregate t from a run of initialisers (the contents of a brace list,
// or, under brace elision, the continuation of the enclosing one).
static void lay_members(Layout *L, Type *t, int off, Node **cur) {
    if (t->base == TB_ARRAY) {
        Type *et = t->u.arr.elem;
        int n = et && et->size ? t->size / et->size : 0;
        for (int i = 0; i < n && *cur; i++) {
            Node *e = *cur;
            if (is_aggregate(et) && e->kind != ND_INITLIST && !char_array_string(et, e))
                lay_members(L, et, off + i * et->size, cur);       // brace elision
            else { lay_object(L, et, off + i * et->size, e); *cur = e->next; }
        }
        return;
    }
    for (Field *f = t->u.composite.members; f && *cur; f = f->next) {
        Node *e = *cur;
        if (is_aggregate(f->type) && e->kind != ND_INITLIST && !char_array_string(f->type, e))
            lay_members(L, f->type, off + f->offset, cur);
        else { lay_object(L, f->type, off + f->offset, e); *cur = e->next; }
        if (t->u.composite.is_union) break;              // only the first member
    }
}

static void lay_object(Layout *L, Type *t, int off, Node *init) {
    if (!init) return;
    Node *s = char_array_string(t, init);
    if (s) {
        int n = s->u.literal.strval_len + 1;
        if (n > t->size) n = t->size;                    // char c[3] = "abc": no NUL
        for (int j = 0; j < n; j++) {
            int b = j < s->u.literal.strval_len ? (unsigned char)s->u.literal.strval[j] : 0;
            lay_item(L, off + j, sx_list(2, sx_int(1), sx_int(b)), 1);
        }
        return;
    }
    if (is_aggregate(t)) {
        if (init->kind != ND_INITLIST)
            src_error(init->line, init->col, "an aggregate needs a brace-enclosed initialiser");
        Node *cur = init->ch[0];
        lay_members(L, t, off, &cur);
        if (cur)
            src_error(cur->line, cur->col, "too many initialisers");
        return;
    }
    lay_scalar(L, t, off, init);
}

Sx *lower_static_data(const char *label, Type *ty, Node *init, StrlitFn strlit) {
    int size = ty ? ty->size : INT_SIZE;
    Sx *gv = sx_list(3, sx_sym("gvar"), sx_str(label), sx_int(size));
    if (!init) return gv;                                // zero-filled

    Layout L = { NULL, NULL, 0, strlit };
    L.head = sx_list(1, sx_sym("gfields"));
    L.tail = &L.head->cdr;
    lay_object(&L, ty, 0, init);

    Sx *val = L.head;
    // A scalar is one item at offset 0: keep the plain forms (and the BSS
    // test on a zero value) that emit.c and irsim.c use for scalars.
    Sx *first = L.head->cdr ? L.head->cdr->car : NULL;
    if (!is_aggregate(ty) && first && !L.head->cdr->cdr) {
        if (first->car->kind == SX_INT && first->car->i == size) val = first->cdr->car;
        else if (first->car->kind == SX_SYM) val = first;          // (strref L)
    } else if (L.off < size) {
        lay_item(&L, size, NULL, 0);                     // tail padding
    }
    Sx **tail = &gv->cdr->cdr->cdr;
    *tail = sx_cons(val, NULL);
    return gv;
}

static const char *lower_strlit(const char *data, int len) { return assign_strlit(data, len); }

static Sx *lower_global(Node *decl, Symbol *sym) {
    Node *init = NULL;
    for (Node *d = decl->ch[1]; d; d = d->next) {
        if (d->kind == ND_DECLARATOR && d->symbol == sym && d->ch[1]) {
            init = d->ch[1]; break;
        }
    }
    return lower_static_data(sym_label(sym), sym->type, init, lower_strlit);
}

// ============================================================
// lower_globals — entry point
// ============================================================

Sx *lower_globals(Node *root, int *strlit_id) {
    if (!root) return NULL;

    g_strlit_id = *strlit_id;
    g_nstrlits  = 0;

    Sx *program = sx_list(1, sx_sym("program"));
    Sx **tail   = &program->cdr;

    Node *decls = (root->kind == ND_PROGRAM) ? root->ch[0] : root;
    for (Node *d = decls; d; d = d->next) {
        if (d->kind != ND_DECLARATION) continue;
        if (d->u.declaration.is_func_defn) continue;  // handled by braun_function()
        if (d->u.declaration.sclass == SC_TYPEDEF) continue;

        for (Node *decl = d->ch[1]; decl; decl = decl->next) {
            if (decl->kind != ND_DECLARATOR) continue;
            Symbol *sym = decl->symbol;
            if (!sym) continue;
            if (sym->kind == SYM_EXTERN || sym->kind == SYM_ENUM_CONST) continue;
            if (istype_function(sym->type)) continue;

            Sx *gv = lower_global(d, sym);
            *tail = sx_cons(gv, NULL); tail = &(*tail)->cdr;
        }
    }

    // Append string literals collected during global lowering
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

    // Write back final strlit counter so braun.c continues from here
    *strlit_id = g_strlit_id;

    return program;
}
