/*
 * lower.c — Lowering pass: Node* → Sexp AST for global variables and string literals
 *
 * Walks the type-annotated Node* tree and produces a (program gvar... strlit...)
 * sexp for the data section. Function bodies are compiled directly to SSA by braun.c.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lower.h"
#include "sx.h"
#include "smallcc.h"

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
        g_strlit_cap = g_strlit_cap ? g_strlit_cap * 2 : 16;
        g_strlits = realloc(g_strlits, g_strlit_cap * sizeof(LStrLit));
    }
    g_strlits[g_nstrlits].id   = id;
    g_strlits[g_nstrlits].data = data;
    g_strlits[g_nstrlits].len  = len;
    g_nstrlits++;
}

// Assign the next strlit ID and return its label string "_lN".
// Also records the data so it can be emitted later.
static const char *assign_strlit(const char *data, int len) {
    int id = g_strlit_id++;
    push_strlit(id, data, len);
    char buf[32]; snprintf(buf, sizeof(buf), "_l%d", id);
    return arena_strdup(buf);
}

// ============================================================
// Symbol name helper
// ============================================================

// Return the assembly label for a global/static symbol.
static const char *sym_label(int tu_index, Symbol *sym) {
    if (!sym) return "_nil";
    char buf[256];
    switch (sym->kind) {
    case SYM_GLOBAL:        snprintf(buf, sizeof(buf), "%s", sym->name); break;
    case SYM_STATIC_GLOBAL: snprintf(buf, sizeof(buf), "_s%d_%s", tu_index, sym->name); break;
    case SYM_STATIC_LOCAL:  snprintf(buf, sizeof(buf), "_ls%d", sym->offset); break;
    case SYM_EXTERN:
    case SYM_BUILTIN:       snprintf(buf, sizeof(buf), "%s", sym->name); break;
    default:                snprintf(buf, sizeof(buf), "%s", sym->name); break;
    }
    return arena_strdup(buf);
}

// ============================================================
// Global variable lowering
// ============================================================

static Sx *lower_global(int tu_index, Node *decl, Symbol *sym) {
    const char *name = sym_label(tu_index, sym);

    // Find initializer node
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

    // Peel casts and unary minus for compile-time constant evaluation.
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
        const char *bytes = init->u.literal.strval;
        int len = init->u.literal.strval_len;
        if (sym->type && sym->type->base == TB_ARRAY) {
            // char s[] = "hello" → embed bytes directly (no separate strlit label)
            Sx *sb = sx_list(1, sx_sym("strbytes"));
            Sx **bt = &sb->cdr;
            for (int j = 0; j <= len; j++) {
                *bt = sx_cons(sx_int(j < len ? (unsigned char)bytes[j] : 0), NULL);
                bt = &(*bt)->cdr;
            }
            *tail = sx_cons(sb, NULL);
        } else {
            // char *p = "hello" → pointer to string literal label
            const char *lbl = assign_strlit(bytes, len);
            *tail = sx_cons(sx_list(2, sx_sym("strref"), sx_str(lbl)), NULL);
        }
        return gv;
    }

    // Array / struct initializer list
    if (init->kind == ND_INITLIST) {
        Type *etype = (sym->type && sym->type->base == TB_ARRAY)
                      ? sym->type->u.arr.elem : NULL;
        int esize = etype ? etype->size : 2;
        Sx *gi = sx_list(2, sx_sym("ginit"), sx_int(esize));
        Sx **gt = &gi->cdr->cdr;

        for (Node *el = init->ch[0]; el; el = el->next) {
            // Flatten nested init list (multi-dim arrays)
            if (el->kind == ND_INITLIST) {
                for (Node *el2 = el->ch[0]; el2; el2 = el2->next) {
                    Node *raw_el2 = el2;
                    while (raw_el2 && raw_el2->kind == ND_CAST) raw_el2 = raw_el2->ch[1];
                    int v = (raw_el2 && raw_el2->kind == ND_LITERAL) ? (int)raw_el2->u.literal.ival : 0;
                    *gt = sx_cons(sx_int(v), NULL); gt = &(*gt)->cdr;
                }
                continue;
            }
            Node *raw_el = el;
            while (raw_el && raw_el->kind == ND_CAST) raw_el = raw_el->ch[1];
            if (raw_el && raw_el->kind == ND_UNARYOP && raw_el->op_kind == TK_AMPERSAND) {
                // &global_var element in pointer array
                Node *operand = raw_el->ch[0];
                Symbol *osym = (operand && operand->kind == ND_IDENT) ? operand->symbol : NULL;
                const char *olabel = osym ? sym_label(tu_index, osym) : "_nil";
                *gt = sx_cons(sx_list(2, sx_sym("strref"), sx_str(olabel)), NULL);
                gt = &(*gt)->cdr;
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
                    const char *lbl = assign_strlit(r->u.literal.strval, r->u.literal.strval_len);
                    *gt = sx_cons(sx_list(2, sx_sym("strref"), sx_str(lbl)), NULL);
                    gt = &(*gt)->cdr;
                } else {
                    int v = (r && r->kind == ND_LITERAL) ? (int)r->u.literal.ival * neg : 0;
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
// lower_globals — entry point
// ============================================================

Sx *lower_globals(Node *root, int tu_index, int *strlit_id) {
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

            Sx *gv = lower_global(tu_index, d, sym);
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
