#ifndef LOWER_H
#define LOWER_H

#include "sx.h"
#include "smallcc.h"

/*
 * lower.h — Lowering pass: Node* parse tree → Sexp AST for globals/strlits
 *
 * Entry point:
 *   lower_globals(root, strlit_id) → (program gvar... strlit...)
 *
 * Only emits global variable and string literal data-section nodes.
 * Function bodies are handled directly by braun_function() in braun.c.
 */

Sx *lower_globals(Node *root, int *strlit_id);

/* The data-section form (gvar label size [init]) of one static object with
 * initialiser `init` (NULL: zero-filled); braun.c uses it for static locals.
 * `strlit` returns the label of a string literal's storage. */
#include "const.h"
Sx *lower_static_data(const char *label, Type *ty, Node *init, StrlitFn strlit);

#endif // LOWER_H
