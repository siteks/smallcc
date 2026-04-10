#ifndef EMIT_H
#define EMIT_H

#include <stdio.h>
#include "ssa.h"
#include "sx.h"

/*
 * emit.h — Instruction selection: SSA IR → CPU4 assembly
 */

// Source annotation (-ann)
extern int flag_annotate;
void set_ann_source(const char *preprocessed_src);

void emit_function(Function *f, FILE *out);
void emit_globals(Sx *program, FILE *out);

#endif // EMIT_H
