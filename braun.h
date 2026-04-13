#ifndef BRAUN_H
#define BRAUN_H

#include <stdio.h>
#include "ssa.h"
#include "smallcc.h"

/*
 * braun.h — Braun SSA construction directly from Node* AST
 *
 * Entry point:
 *   braun_function(func_decl, tu_index, strlit_id) → Function*
 *
 * func_decl: ND_DECLARATION node with is_func_defn=true
 *
 * String literals encountered in function bodies are accumulated
 * internally; call braun_emit_strlits(out) after each function
 * (or once after all functions) to flush them to the data section.
 */

Function *braun_function(Node *func_decl, int tu_index, int *strlit_id);
void      braun_emit_strlits(FILE *out);
void      braun_register_inline_candidate(Node *func_decl, int tu_index);

/* Access pending strlits for callers (e.g. irsim) that need the raw bytes.
 * Must be called before braun_emit_strlits, which clears the list. */
int       braun_nstrlits(void);
void      braun_get_strlit(int i, char label_buf[32], const char **data, int *len);

#endif // BRAUN_H
