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

// Source-line map emission (-g). When set, emit_function prepends each
// instruction whose source line changes with a "; @src FILE LINE" directive.
// sim_c (the assembler) picks these up and writes a PC→source JSON map.
extern int flag_linemap;

void emit_function(Function *f, FILE *out);
void emit_globals(Sx *program, FILE *init_out, FILE *bss_out);

#endif // EMIT_H
