/* Types for the tables isatool/gen.py generates from an ISA definition
 * (<arch>/isa_table_c.h): what sim_c's assembler and disassembler need to
 * encode and decode any instruction of any ISA from its bit pattern. */
#ifndef ISATOOL_ISA_TYPES_H
#define ISATOOL_ISA_TYPES_H
#include <stdint.h>

#define ISA_MAX_GPR 32

enum { ISA_REG = 0, ISA_SIMM, ISA_UIMM, ISA_INDEX, ISA_BYTES, ISA_RAW, ISA_ABS, ISA_PCREL };

typedef struct {
    uint8_t kind;          /* ISA_REG or an immediate kind */
    uint8_t bits;          /* field width */
    uint8_t scale;         /* ISA_BYTES: the source value is divided by this */
    uint8_t nruns;         /* the field's contiguous pieces, most significant first */
    uint8_t shift[4];      /* bit position of each piece's lsb in the instruction word */
    uint8_t width[4];
} IsaOp;

typedef struct {
    const char *name;
    uint8_t     len;       /* bytes */
    uint8_t     nops;
    uint32_t    match;     /* (word & mask) == match identifies the instruction */
    uint32_t    mask;
    IsaOp       ops[4];    /* in assembly order */
} IsaInstr;

typedef struct {
    const char *name, *real;
    uint8_t     n;         /* operands of the real instruction */
    uint8_t     src[4];    /* source operand for each */
} IsaPseudo;
#endif
