#ifndef SX_H
#define SX_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "smallcc.h"  // for Type*

// ============================================================
// Sexp AST — the data-section interchange format
//
// lower.c encodes global variables and string literals as small
// sexp trees ((gvar ...), (strlit ...)); emit.c and irsim.c decode
// them. Function bodies never touch this representation — they go
// directly from Node* to SSA in braun.c.
// ============================================================

typedef enum { SX_PAIR, SX_SYM, SX_STR, SX_INT } SxKind;

typedef struct Sx {
    SxKind   kind;
    union {
        struct { struct Sx *car, *cdr; };   // SX_PAIR
        char    *s;                          // SX_SYM / SX_STR
        int      i;                          // SX_INT
    };
} Sx;

// Constructors — allocate from the permanent arena
Sx *sx_sym(const char *s);          // SX_SYM
Sx *sx_str(const char *s);          // SX_STR
Sx *sx_int(int i);                  // SX_INT
Sx *sx_cons(Sx *car, Sx *cdr);      // SX_PAIR

// Build a proper list: sx_list(3, a, b, c) → (a b c)
Sx *sx_list(int n, ...);

// Accessors
Sx *sx_car(Sx *s);          // car of a pair; NULL if not a pair
Sx *sx_nth(Sx *s, int n);   // 0-indexed element (car = 0); NULL if out of range
const char *sx_car_sym(Sx *s);  // car->s if car is SX_SYM, else NULL

#endif // SX_H
