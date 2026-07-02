#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "sx.h"
#include "smallcc.h"

// ============================================================
// Sexp allocation
// ============================================================

static Sx *sx_alloc(SxKind kind) {
    Sx *s = arena_alloc(sizeof(Sx));
    s->kind = kind;
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

Sx *sx_nth(Sx *s, int n) {
    for (int i = 0; i < n; i++) {
        if (!s || s->kind != SX_PAIR) return NULL;
        s = s->cdr;
    }
    if (!s || s->kind != SX_PAIR) return NULL;
    return s->car;
}

const char *sx_car_sym(Sx *s) {
    if (!s || s->kind != SX_PAIR) return NULL;
    Sx *c = s->car;
    if (!c || c->kind != SX_SYM) return NULL;
    return c->s;
}
