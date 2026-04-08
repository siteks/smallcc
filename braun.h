#ifndef BRAUN_H
#define BRAUN_H

#include "ssa.h"
#include "sx.h"

/*
 * braun.h — Braun SSA construction from Sexp AST
 *
 * Entry point:
 *   braun_function(func_sexp, tm) → Function*
 *
 * func_sexp: (func "name" (params "p"...) body)
 */

Function *braun_function(Sx *func_sexp, TypeMap *tm);

#endif // BRAUN_H
