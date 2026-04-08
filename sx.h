#ifndef SX_H
#define SX_H

#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include "smallcc.h"  // for Type*

// ============================================================
// Sexp AST  (Regime 1 tree representation)
// ============================================================

typedef enum { SX_PAIR, SX_SYM, SX_STR, SX_INT } SxKind;

typedef struct Sx {
    SxKind   kind;
    uint32_t id;    // stable id for TypeMap keying; set at allocation time
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
Sx *sx_cdr(Sx *s);          // cdr of a pair; NULL if not a pair
Sx *sx_nth(Sx *s, int n);   // 0-indexed element (car = 0); NULL if out of range
int sx_len(Sx *s);          // length of a proper list

// Predicate helpers
const char *sx_car_sym(Sx *s);  // car->s if car is SX_SYM, else NULL
bool sx_is_sym(Sx *s, const char *name);  // s->kind==SX_SYM && strcmp(s->s,name)==0

// Printer
void sx_print(Sx *s, FILE *out);
void sx_println(Sx *s, FILE *out);  // print followed by newline

// Rebuild a pair with a new car (inherits id from original)
Sx *sx_with_car(Sx *pair, Sx *new_car);
// Rebuild a pair with a new cdr
Sx *sx_with_cdr(Sx *pair, Sx *new_cdr);

// Clone a node (same id — use when replacing with equivalent)
Sx *sx_clone_id(Sx *original_template, Sx *car, Sx *cdr);

// ============================================================
// TypeMap  (Regime 1 → Regime 2 type artifact)
// ============================================================

// ValType: the types that survive through the SSA IR.
typedef enum {
    VT_VOID,
    VT_I8,
    VT_I16,
    VT_I32,
    VT_U8,
    VT_U16,
    VT_U32,
    VT_PTR,
    VT_F32,
} ValType;

// CallDesc: ABI descriptor for call sites; attached to (call ...) / (call_indirect ...)
typedef struct {
    Type  *return_type;     // C return type (for sret detection, widening)
    int    nparams;
    Type **param_types;     // array of C param types
    bool   is_variadic;
    bool   hidden_sret;     // struct-returning: first arg is hidden sret pointer
} CallDesc;

// TypeMap: stable id → {ValType, CallDesc*}
typedef struct TypeMap TypeMap;

TypeMap  *typemap_new(void);
void      typemap_free(TypeMap *tm);

void      typemap_set_vtype(TypeMap *tm, uint32_t id, ValType vt);
void      typemap_set_calldesc(TypeMap *tm, uint32_t id, CallDesc *cd);

ValType   typemap_vtype(TypeMap *tm, uint32_t id);      // VT_VOID if absent
CallDesc *typemap_calldesc(TypeMap *tm, uint32_t id);   // NULL if absent

// Convert a C Type* to the corresponding ValType
ValType type_to_valtype(Type *t);

#endif // SX_H
