/*
 * const.h — the compiler's one constant-expression evaluator.
 *
 * Used for array sizes, enum values and case labels (at parse time, before
 * types are derived) and for static initialisers (global and static local,
 * after insert_coercions). Integer arithmetic is C's on 32-bit int/unsigned;
 * float arithmetic is the target's (cpu4/fpu_model.h), so a folded constant
 * has exactly the bits the run-time operation would produce.
 */
#ifndef CONST_H
#define CONST_H

#include <stdbool.h>
#include <stdint.h>
#include "smallcc.h"

typedef enum { CV_INT, CV_FLT, CV_ADDR } CvKind;

typedef struct {
    CvKind      kind;
    bool        uns;     // CV_INT: unsigned int (else int)
    long long   i;       // CV_INT: value, normalised to 32 bits; CV_ADDR: byte offset
    uint32_t    fbits;   // CV_FLT: IEEE single bits
    const char *label;   // CV_ADDR: symbol or string-literal label
} CVal;

// Returns the label for a string literal's storage, registering it if new.
// NULL where string literals are not constant (integer contexts).
typedef const char *(*StrlitFn)(const char *data, int len);

// Evaluate n; false if it is not a constant expression.
bool const_eval(Node *n, CVal *out, StrlitFn strlit);

// An integer constant expression, or a compile error at n naming `what`.
long long const_int(Node *n, const char *what);

#endif
