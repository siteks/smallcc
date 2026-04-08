#ifndef LOWER_H
#define LOWER_H

#include "sx.h"
#include "smallcc.h"

/*
 * lower.h — Lowering pass: Node* parse tree → Sexp AST + TypeMap
 *
 * Entry point:
 *   lower_program(root, tm, tu_index) → (program gvar... strlit... func...)
 *
 * After this call the TypeMap is populated with ValType and CallDesc for
 * every expression-valued sexp node.
 */

Sx *lower_program(Node *root, TypeMap *tm, int tu_index, int *strlit_id);

#endif // LOWER_H
