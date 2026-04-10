#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "sx.h"
#include "smallcc.h"

// ============================================================
// Sexp allocation
// ============================================================

static uint32_t sx_next_id = 1;  // 0 is reserved for "no id"

static Sx *sx_alloc(SxKind kind) {
    Sx *s = arena_alloc(sizeof(Sx));
    s->kind = kind;
    s->id   = sx_next_id++;
    return s;
}

Sx *sx_sym(const char *str) {
    Sx *s = sx_alloc(SX_SYM);
    s->s = arena_strdup(str);
    return s;
}

Sx *sx_str(const char *str) {
    Sx *s = sx_alloc(SX_STR);
    s->s = arena_strdup(str);
    return s;
}

Sx *sx_int(int i) {
    Sx *s = sx_alloc(SX_INT);
    s->i = i;
    return s;
}

Sx *sx_cons(Sx *car, Sx *cdr) {
    Sx *s = sx_alloc(SX_PAIR);
    s->car = car;
    s->cdr = cdr;
    return s;
}

Sx *sx_list(int n, ...) {
    va_list ap;
    va_start(ap, n);
    // Collect elements
    Sx *elems[256];
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++)
        elems[i] = va_arg(ap, Sx *);
    va_end(ap);
    // Build list right-to-left
    Sx *tail = NULL;
    for (int i = n - 1; i >= 0; i--)
        tail = sx_cons(elems[i], tail);
    return tail;
}

// ============================================================
// Accessors
// ============================================================

Sx *sx_car(Sx *s) {
    if (!s || s->kind != SX_PAIR) return NULL;
    return s->car;
}

Sx *sx_cdr(Sx *s) {
    if (!s || s->kind != SX_PAIR) return NULL;
    return s->cdr;
}

Sx *sx_nth(Sx *s, int n) {
    for (int i = 0; i < n; i++) {
        if (!s || s->kind != SX_PAIR) return NULL;
        s = s->cdr;
    }
    if (!s || s->kind != SX_PAIR) return NULL;
    return s->car;
}

int sx_len(Sx *s) {
    int n = 0;
    while (s && s->kind == SX_PAIR) {
        n++;
        s = s->cdr;
    }
    return n;
}

const char *sx_car_sym(Sx *s) {
    if (!s || s->kind != SX_PAIR) return NULL;
    Sx *c = s->car;
    if (!c || c->kind != SX_SYM) return NULL;
    return c->s;
}

bool sx_is_sym(Sx *s, const char *name) {
    return s && s->kind == SX_SYM && strcmp(s->s, name) == 0;
}

// ============================================================
// Printer
// ============================================================

void sx_print(Sx *s, FILE *out) {
    if (!s) { fprintf(out, "()"); return; }
    switch (s->kind) {
    case SX_SYM:
        fprintf(out, "%s", s->s);
        break;
    case SX_STR:
        fputc('"', out);
        for (const char *p = s->s; *p; p++) {
            if (*p == '"' || *p == '\\') fputc('\\', out);
            fputc(*p, out);
        }
        fputc('"', out);
        break;
    case SX_INT:
        fprintf(out, "%d", s->i);
        break;
    case SX_PAIR:
        fputc('(', out);
        sx_print(s->car, out);
        s = s->cdr;
        while (s && s->kind == SX_PAIR) {
            fputc(' ', out);
            sx_print(s->car, out);
            s = s->cdr;
        }
        if (s) {
            fprintf(out, " . ");
            sx_print(s, out);
        }
        fputc(')', out);
        break;
    }
}

void sx_println(Sx *s, FILE *out) {
    sx_print(s, out);
    fputc('\n', out);
}

// ============================================================
// Node reconstruction helpers
// ============================================================

Sx *sx_with_car(Sx *pair, Sx *new_car) {
    Sx *s = sx_alloc(SX_PAIR);
    s->id  = pair->id;          // inherit stable id
    s->car = new_car;
    s->cdr = pair->cdr;
    return s;
}

Sx *sx_with_cdr(Sx *pair, Sx *new_cdr) {
    Sx *s = sx_alloc(SX_PAIR);
    s->id  = pair->id;
    s->car = pair->car;
    s->cdr = new_cdr;
    return s;
}

Sx *sx_clone_id(Sx *original_template, Sx *car, Sx *cdr) {
    Sx *s = sx_alloc(SX_PAIR);
    s->id  = original_template->id;
    s->car = car;
    s->cdr = cdr;
    return s;
}

// ============================================================
// TypeMap  (open-addressing hash table, id → TypeEntry)
// ============================================================

typedef struct {
    uint32_t  id;
    ValType   vt;
    CallDesc *cd;
} TypeEntry;

#define TM_INIT_SIZE 64

struct TypeMap {
    TypeEntry *buckets;
    int        cap;
    int        count;
};

TypeMap *typemap_new(void) {
    TypeMap *tm = arena_alloc(sizeof(TypeMap));
    tm->cap     = TM_INIT_SIZE;
    tm->count   = 0;
    tm->buckets = arena_alloc(tm->cap * sizeof(TypeEntry));
    return tm;
}

void typemap_free(TypeMap *tm) {
    (void)tm; // arena-allocated; nothing to free
}

static void tm_grow(TypeMap *tm);

static TypeEntry *tm_find(TypeMap *tm, uint32_t id, bool insert) {
    int mask = tm->cap - 1;
    int h    = (int)(id * 2654435761u) & mask;
    for (int i = 0; i < tm->cap; i++) {
        TypeEntry *e = &tm->buckets[(h + i) & mask];
        if (e->id == id) return e;
        if (e->id == 0) {
            if (!insert) return NULL;
            if (tm->count * 2 >= tm->cap) {
                tm_grow(tm);
                return tm_find(tm, id, insert);
            }
            e->id = id;
            tm->count++;
            return e;
        }
    }
    if (insert) { tm_grow(tm); return tm_find(tm, id, insert); }
    return NULL;
}

static void tm_grow(TypeMap *tm) {
    int old_cap        = tm->cap;
    TypeEntry *old_bkt = tm->buckets;
    tm->cap    *= 2;
    tm->count   = 0;
    tm->buckets = arena_alloc(tm->cap * sizeof(TypeEntry));
    for (int i = 0; i < old_cap; i++) {
        if (old_bkt[i].id) {
            TypeEntry *e = tm_find(tm, old_bkt[i].id, true);
            e->vt = old_bkt[i].vt;
            e->cd = old_bkt[i].cd;
        }
    }
}

void typemap_set_vtype(TypeMap *tm, uint32_t id, ValType vt) {
    TypeEntry *e = tm_find(tm, id, true);
    e->vt = vt;
}

void typemap_set_calldesc(TypeMap *tm, uint32_t id, CallDesc *cd) {
    TypeEntry *e = tm_find(tm, id, true);
    e->cd = cd;
}

ValType typemap_vtype(TypeMap *tm, uint32_t id) {
    TypeEntry *e = tm_find(tm, id, false);
    return e ? e->vt : VT_VOID;
}

CallDesc *typemap_calldesc(TypeMap *tm, uint32_t id) {
    TypeEntry *e = tm_find(tm, id, false);
    return e ? e->cd : NULL;
}

// ============================================================
// type_to_valtype
// ============================================================

ValType type_to_valtype(Type *t) {
    if (!t) return VT_VOID;
    switch (t->base) {
    case TB_VOID:                       return VT_VOID;
    case TB_CHAR:                       return VT_I8;
    case TB_UCHAR:                      return VT_U8;
    case TB_SHORT: case TB_INT:         return VT_I16;
    case TB_USHORT: case TB_UINT:       return VT_U16;
    case TB_LONG:                       return VT_I32;
    case TB_ULONG:                      return VT_U32;
    case TB_FLOAT: case TB_DOUBLE:      return VT_F32;
    case TB_POINTER: case TB_FUNCTION:  return VT_PTR;
    case TB_ARRAY:                      return VT_PTR;   // array decays to pointer
    case TB_STRUCT: case TB_ENUM:       return VT_I16;   // struct size varies; enum is int
    default:                            return VT_VOID;
    }
}
