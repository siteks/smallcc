#ifndef EMIT_H
#define EMIT_H

#include <stdio.h>
#include "ssa.h"
#include "sx.h"

/*
 * emit.h — Instruction selection: SSA IR → CPU4 assembly
 */

void emit_function(Function *f, FILE *out);
void emit_globals(Sx *program, FILE *out);

#endif // EMIT_H
