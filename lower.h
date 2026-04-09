#ifndef LOWER_H
#define LOWER_H

#include "sx.h"
#include "smallcc.h"

/*
 * lower.h — Lowering pass: Node* parse tree → Sexp AST for globals/strlits
 *
 * Entry point:
 *   lower_globals(root, tu_index, strlit_id) → (program gvar... strlit...)
 *
 * Only emits global variable and string literal data-section nodes.
 * Function bodies are handled directly by braun_function() in braun.c.
 */

Sx *lower_globals(Node *root, int tu_index, int *strlit_id);

#endif // LOWER_H
