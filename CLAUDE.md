# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

```bash
make mycc           # Build the compiler
make test_all       # Run all test suites
make test_struct    # Run a specific test suite (also: test_init, test_ops, test_logops, test_func, test_longs, test_array, test_loops, test_goto, test_struct_init, test_floats, test_compound, test_remaining, test_typedef, test_strings, test_enum, test_variadic, test_funcptr)
make clean          # Remove binaries and temp files
```

To run the compiler directly (input is a C source string, output is assembly to stdout):
```bash
./mycc "int main(){return 5+3;}"
```

To compile multiple `.c` files as separate translation units and link them in-memory:
```bash
./mycc file1.c file2.c > out.s && ./cpu3/sim.py out.s
```

To run the full pipeline (compiler + simulator) with a string:
```bash
./mycc "int main(){return 5+3;}" > tmp.s && ./cpu3/sim.py tmp.s
```

Tests use `cpu3/sim.py` to execute generated assembly and check the value left in register `r0`. On test failure, verbose output is written to `error.log` using `cpu2/sim.py`. The test harness is in `test.sh`; individual suites are in `tests/`.

---

## Compiler Architecture

This is a single-pass C89 subset compiler. Input is either a C source string (command-line argument) or one or more `.c` files. Assembly is written to stdout.

### Compilation Pipeline

For each translation unit (single TU when given a string; one per `.c` file when given files):

```
./mycc "C code"            (string mode: one TU)
./mycc a.c b.c ...         (file mode: one TU per file, in order)

Per-TU loop [mycc.c]:
  reset_codegen()           [codegen.c]     Clear per-TU codegen state
  reset_parser()            [parser.c]      Clear per-TU parser state
  reset_types_state()       [types.c]       Fresh symbol/scope tables (type list preserved)
  make_basic_types()        [types.c]       Populate/reuse global type table
  prepopulate_extern_syms() [mycc.c]        Inject globals from previous TUs
  tokenise()                [tokeniser.c]   Token linked list
  program()                 [parser.c]      AST (Node tree)
  propagate_types()         [parser.c]      Annotate AST nodes with resolved Type2*
  gen_code(node, tu_index)  [codegen.c]     Emit assembly to stdout
  harvest_globals()         [mycc.c]        Collect non-static globals for next TU

Preamble (ssp / jl main / halt) is emitted once before the loop.
```

Every phase prints debug information to stderr (token list, AST before/after type propagation, symbol table, type table).

### Source Files

| File | Role |
|---|---|
| `mycc.h` | All shared structs, enums, and function prototypes |
| `mycc.c` | Entry point; per-TU loop; `ExternSym` table; `harvest_globals`/`prepopulate_extern_syms` |
| `tokeniser.c` | Lexer — produces a `Token` linked list |
| `parser.c` | Recursive-descent parser — builds AST; also contains `propagate_types` and `check_operands` |
| `types.c` | Type table, symbol table, struct layout, `add_types_and_symbols`, `reset_types_state`, `insert_extern_sym` |
| `codegen.c` | AST walk; emits assembly pseudoinstructions; `reset_codegen`; static label mangling |

---

## Tokeniser (`tokeniser.c`)

Converts the input string to a singly-linked list of `Token` structs.

### Token Struct

```c
struct Token {
    Token_kind  kind;   // enum — see below
    Token       *next;
    char        *val;   // text of the token
    double      fval;   // parsed float value
    long long   ival;   // parsed integer value (also string literal length for TK_STRING)
    int         loc;    // character position in source
};
```

### Token Kinds

- **Delimiters**: `TK_LPAREN TK_RPAREN TK_LBRACE TK_RBRACE TK_LBRACKET TK_RBRACKET TK_COMMA TK_SEMICOLON TK_COLON TK_DOT TK_ARROW`
- **Operators** (2-char checked before 1-char): `TK_EQ(==) TK_NE(!=) TK_GE(>=) TK_LE(<=) TK_INC(++) TK_DEC(--) TK_LOGAND(&&) TK_LOGOR(||) TK_SHIFTR(>>) TK_SHIFTL(<<) TK_ARROW(->)` then single-char: `TK_ASSIGN(=) TK_PLUS TK_MINUS TK_STAR TK_SLASH TK_AMPERSAND TK_TWIDDLE(~) TK_BANG(!) TK_BITOR(|) TK_BITXOR(^) TK_LT TK_GT TK_PERCENT TK_QUESTION`
- **Compound assignment**: `TK_PLUS_ASSIGN TK_MINUS_ASSIGN TK_STAR_ASSIGN TK_SLASH_ASSIGN TK_AMP_ASSIGN TK_BITOR_ASSIGN TK_BITXOR_ASSIGN TK_SHIFTL_ASSIGN TK_SHIFTR_ASSIGN TK_PERCENT_ASSIGN`
- **Literals**: `TK_CONSTINT`, `TK_CONSTFLT`, `TK_CHARACTER`, `TK_STRING`, `TK_IDENT`
- **Keywords** (32): `auto break case char const continue default do double else enum extern float for goto if int long register return short signed sizeof static struct switch typedef union unsigned void volatile while`
- **Other**: `TK_ELLIPSIS` (`...` — variadic parameter marker), `TK_EOF`

### Lexing Rules

- Whitespace is skipped.
- Two-character tokens are checked before single-character tokens.
- Identifiers/keywords: `[a-zA-Z_][a-zA-Z0-9_]*`, then matched against `keywords[]` table.
- Integer constants: `strtol()` with optional `u/U`, `l/L`, or `ul/UL` suffix; hex (`0x…`) and octal (`0…`) recognised.
- Float constants: `strtod()` with optional `f/F` suffix.
- Character constants: `'x'` with escape sequences `\a \b \f \n \r \t \v \\ \? \' \" \xhh \ooo`.
- String literals: `"…"` decoded into `val` (with C escape sequences); `ival` holds the decoded byte count (excluding null terminator). Adjacent string literals are concatenated in the tokeniser loop. The `val` buffer is heap-allocated.

---

## Parser (`parser.c`)

A hand-written recursive-descent parser. Also owns `propagate_types` and arithmetic-conversion helpers.

### C89 Grammar Supported

```
translation-unit    → external-declaration*
external-declaration → function-definition | declaration

declaration         → decl-specifier+ init-declarator-list? ";"
decl-specifier      → storage-class | type-specifier | type-qualifier
storage-class       → auto | register | static | extern | typedef
type-specifier      → void | char | short | int | long | float | double
                    | signed | unsigned | struct-or-union-specifier | enum-specifier
                    | typedef-name
type-qualifier      → const | volatile

struct-or-union-specifier
                    → (struct|union) ident? "{" struct-declaration+ "}"
                    | (struct|union) ident

declarator          → pointer? direct-declarator
pointer             → "*" type-qualifier* pointer?
direct-declarator   → ident direct-decl-suffix*
                    | "(" declarator ")" direct-decl-suffix*
direct-decl-suffix  → "[" constant-expr? "]"
                    | "(" param-type-list ")"

param-declaration   → decl-specifier+ declarator?   (abstract declarators accepted)

init-declarator     → declarator ("=" initializer)?
initializer         → assign-expr | "{" initializer-list "}"

statement           → expr-stmt | compound-stmt | if-stmt | while-stmt | for-stmt
                    | do-stmt | switch-stmt | break-stmt | continue-stmt | return-stmt
compound-stmt       → "{" (declaration | statement)* "}"
if-stmt             → "if" "(" expr ")" stmt ("else" stmt)?
while-stmt          → "while" "(" expr ")" stmt
for-stmt            → "for" "(" (declaration | expr?) ";" expr? ";" expr? ")" stmt
do-stmt             → "do" stmt "while" "(" expr ")" ";"
switch-stmt         → "switch" "(" expr ")" compound-stmt
case-label          → "case" integer-constant ":"    (parsed as stmt inside compound)
default-label       → "default" ":"                  (parsed as stmt inside compound)
break-stmt          → "break" ";"
continue-stmt       → "continue" ";"
goto-stmt           → "goto" ident ";"
label-stmt          → ident ":" stmt
return-stmt         → "return" expr? ";"

expr                → assign-expr ("," assign-expr)*
assign-expr         → unary-expr "=" assign-expr | cond-expr
cond-expr           → logor-expr ("?" expr ":" cond-expr)?
logor-expr          → logand-expr ("||" logand-expr)*
logand-expr         → bitor-expr ("&&" bitor-expr)*
bitor-expr          → bitxor-expr ("|" bitxor-expr)*
bitxor-expr         → bitand-expr ("^" bitand-expr)*
bitand-expr         → equal-expr ("&" equal-expr)*
equal-expr          → rel-expr (("==" | "!=") rel-expr)*
rel-expr            → shift-expr (("<" | ">" | "<=" | ">=") shift-expr)*
shift-expr          → add-expr (("<<" | ">>") add-expr)*
add-expr            → mult-expr (("+" | "-") mult-expr)*
mult-expr           → cast-expr (("*" | "/" | "%") cast-expr)*
cast-expr           → "(" type-name ")" cast-expr | unary-expr
unary-expr          → postfix-expr
                    | ("++" | "--") unary-expr
                    | ("+" | "-" | "*" | "&" | "~" | "!") cast-expr
postfix-expr        → primary-expr postfix-suffix*
postfix-suffix      → "[" expr "]"
                    | "(" arg-list? ")"
                    | "." ident
                    | "->" ident
                    | "++" | "--"
primary-expr        → ident | constant | string-literal | "(" expr ")"
```

**Not yet implemented:** `register` semantics (no register allocator). `sizeof(complex_expr)` not supported (only type names and simple idents). Calling through a function-pointer stored in an array element is not implemented.

**Abstract declarators** in parameter lists are fully supported: `int foo(int, int)` (unnamed params) and `int (*fp)(int, int)` (function-pointer params with inner unnamed params) are both accepted.

### AST Node Kinds

```c
ND_PROGRAM       // root; children are top-level declarations
ND_FUNCTION      // not used (functions are ND_DECLARATION with is_func_defn)
ND_DECLARATION   // variable or function declaration
ND_DECLARATOR    // declarator part of a declaration
ND_DIRECT_DECL   // direct declarator (name + suffix)
ND_ARRAY_DECL    // "[N]" suffix
ND_FUNC_DECL     // "(params)" suffix
ND_PTYPE_LIST    // parameter type list
ND_TYPE_NAME     // type name in a cast
ND_STRUCT        // struct/union specifier
ND_UNION         // (unused; structs cover both)
ND_MEMBER        // "." or "->" member access — children: [struct_expr, field_ident]
ND_COMPSTMT      // "{}" block — children: declarations and statements
ND_EXPRSTMT      // expression statement
ND_IFSTMT        // if — children: [cond, then] or [cond, then, else]
ND_WHILESTMT     // while — children: [cond, body]
ND_FORSTMT       // for — children: [init, cond, inc, body] (ND_EMPTY for absent parts)
                 //   if init is a declaration, node->symtable holds its implicit scope
ND_DOWHILESTMT   // do-while — children: [body, cond]
ND_SWITCHSTMT    // switch — children: [selector, body_compstmt]
ND_CASESTMT      // case N: — ival holds the constant; no children
ND_DEFAULTSTMT   // default: — no children
ND_BREAKSTMT     // break; — no children
ND_CONTINUESTMT  // continue; — no children
ND_GOTOSTMT      // goto ident; — val = target label name
ND_LABELSTMT     // ident: stmt — val = label name; child[0] = statement
ND_EMPTY         // absent for-loop part (init/cond/inc omitted)
ND_RETURNSTMT    // return — children: [expr] or empty
ND_STMT          // generic wrapper
ND_ASSIGN        // "=" — children: [lhs, rhs]
ND_BINOP         // binary op (val = "+", "-", "*", "/", "==", etc.)
ND_UNARYOP       // unary op (val = "+", "-", "*", "&", "~", "!", "++", "--", "post++", "post--")
ND_CAST          // explicit cast — children: [ND_DECLARATION type placeholder, expr]
ND_TERNARY       // "?:" — children: [cond, then_expr, else_expr]
ND_IDENT         // variable or function reference (val = name)
ND_LITERAL       // integer, float, or string constant
ND_INITLIST      // initializer list "{…}"
ND_CONSTEXPR     // constant expression
ND_VA_START      // va_start(ap, last) — children: [ap, last]
ND_VA_ARG        // va_arg(ap, type) — child: [ap]; node->type = requested type
ND_VA_END        // va_end(ap) — no-op
ND_PARAM_LIST    // (unused directly)
ND_DECLSTMT      // (unused directly)
ND_EXPR          // (unused directly)
ND_UNDEFINED
```

### Node Struct (key fields)

```c
struct Node {
    Node_kind    kind;
    char         val[64];       // operator or name string
    long long    ival;          // integer constant value
    double       fval;          // float constant value
    char         *strval;       // heap-allocated decoded string literal content (NULL if not a string)
    int          strval_len;    // byte length of strval (excluding null terminator)
    Node       **children;
    int          child_count;
    int          offset;        // struct member byte offset (ND_MEMBER)
    int          pointer_level; // number of pointer stars in declarator
    int          struct_depth;  // nesting depth for struct definitions
    bool         is_expr;       // node participates in type propagation
    bool         is_func_defn;  // true for function definitions (not just decls)
    bool         is_function;   // true when ident/unaryop has postfix "(args)" applied —
                                //   marks this node as a call site (or dereference-call)
    bool         is_array_deref;// true for array subscript UNARYOP "+" (no load)
    bool         is_array;      // true for ND_ARRAY_DECL nodes
    bool         is_variadic;   // true for variadic function/param-type-list
    int          array_size;    // size of array declared
    Type_base    typespec;      // parser-internal bitmask (TB_INT | TB_UNSIGNED etc.)
    Token_kind   sclass;        // storage class specifier
    Token_kind   typequal;      // type qualifier
    Scope        scope;         // which scope this node lives in
    Symbol_table *symtable;     // symbol table for compound statements
    Symbol       *symbol;       // resolved symbol (set during add_types_and_symbols)
    Type2        *type;         // resolved type (set during propagate_types)
                                // NOTE: new_node() initialises type = t_void (not NULL)
};
```

### Array Subscript Rewriting

`E1[E2]` is rewritten in the parser (not in a separate pass) as pointer arithmetic:

- **Pointer subscript** (base has pointer type): creates `ND_UNARYOP "*"` wrapping `ND_BINOP "+"(ptr, idx)`. `propagate_types` inserts stride scaling.
- **Not the last array dimension** (e.g., `a[i]` where `a` is `int[3][4]`): creates `ND_UNARYOP "+"` with `is_array_deref = true` and `node->type` set to the element-at-that-depth type. No load is generated; the address is passed through.
- **Last array dimension** (e.g., `a[i]` where `a` is `int[4]`): creates `ND_UNARYOP "*"` with `node->type` set to the leaf element type. A load is generated.

For array subscripts the child is `ND_BINOP "+"` with children `[base_addr, ND_BINOP "*" [stride_constant, index_expr]]`.

### Type Propagation (`propagate_types`)

Post-order (children first) tree walk. For each node:

- `ND_IDENT` (not struct member, `is_expr == true` only): `node->type = find_symbol(…)->type`
- `ND_MEMBER`: looks up field in `lhs->type->u.composite.members`, sets `node->offset` and `node->type`; for `"->"` the lhs pointer is automatically dereferenced; when `is_function=true` (call through member fn-ptr), `node->type` is resolved to the function's return type
- `ND_BINOP` (is_expr): calls `check_operands()` → inserts arithmetic-conversion casts, returns lhs type; then comparison/logical operators (`< <= > >= == != && ||`) force `node->type = t_int` regardless of operand type
- `ND_UNARYOP` (is_expr): calls `check_unary_operand()` → for `*` returns `elem_type(child->type)`; preserves parser-set types (guards with `if (node->type == t_void)`)
- `ND_RETURNSTMT`: inserts a cast if return type differs from expression type

### Scope Tracking and `last_symbol_table`

The global `last_symbol_table` always points to the most recently created scope from `enter_new_scope(false)`. `comp_stmt(true)` uses it to re-enter the scope created by `param_type_list` (so function parameters and body variables share a scope).

**Important:** `param_type_list` explicitly restores `last_symbol_table = node->symtable` after `leave_scope()`. This prevents inner `param_type_list` calls (from function-pointer declarators such as `int (*fp)(int)`) from overwriting `last_symbol_table` and corrupting the outer function's scope chain.

---

## Type System (`types.c` + `mycc.h`)

### Type Representation: `Type2`

All types are represented as `Type2` structs stored in a global linked list (`types`). Factory functions intern types by structural equality.

```c
typedef enum {
    TB2_VOID, TB2_CHAR, TB2_UCHAR, TB2_SHORT, TB2_USHORT,
    TB2_INT,  TB2_UINT, TB2_LONG,  TB2_ULONG,
    TB2_FLOAT, TB2_DOUBLE,
    TB2_POINTER, TB2_ARRAY, TB2_FUNCTION, TB2_STRUCT, TB2_ENUM
} Type2_base;

typedef int Type2_qual;   // TQ_CONST = 0x1, TQ_VOLATILE = 0x2

struct Type2 {
    Type2_base  base;
    Type2_qual  qual;
    int         size;   // bytes
    int         align;  // alignment
    union {
        struct { Type2 *pointee; }                       ptr;
        struct { Type2 *elem; int count; }               arr;
        struct { Type2 *ret; Param *params; bool is_variadic; } fn;
        struct { Symbol *tag; Field *members; bool is_union; } composite;
        struct { Symbol *tag; }                          enu;
    } u;
    Type2 *next;        // linked list of all types
};

struct Field {
    char   *name;       // member name (strdup'd)
    Type2  *type;
    int     offset;     // byte offset within struct/union
    Field  *next;
};
```

### Basic Type Sizes

| Type | Size | Align | Global pointer |
|---|---|---|---|
| void | 0 | 1 | `t_void` |
| char | 1 | 1 | `t_char` |
| uchar | 1 | 1 | `t_uchar` |
| short | 2 | 2 | `t_short` |
| ushort | 2 | 2 | `t_ushort` |
| int | 2 | 2 | `t_int` |
| uint | 2 | 2 | `t_uint` |
| long | 4 | 4 | `t_long` |
| ulong | 4 | 4 | `t_ulong` |
| float | 4 | 4 | `t_float` |
| double | 4 | 4 | `t_double` |
| pointer | 2 | 2 | — |
| function | 0 | — | — |

**Note:** The target has a 16-bit address space. `int` and pointers are 2 bytes. Function types have size 0; when a function designator is used as a value (e.g. passed as an argument), it decays to a pointer-sized value (2 bytes).

### Factory Functions

```c
Type2 *get_basic_type(Type2_base base);            // returns cached singleton
Type2 *get_pointer_type(Type2 *pointee);           // dedup by pointee pointer
Type2 *get_array_type(Type2 *elem, int count);     // size = elem->size * count
Type2 *get_function_type(Type2 *ret, Param *params, bool is_variadic);
Type2 *get_struct_type(Symbol *tag, Field *members, bool is_union);
Type2 *get_enum_type(Symbol *tag);
```

### Type Derivation

`tstr_compact(Node *declarator)` returns a derivation string from a declarator node:
- `""` — plain scalar
- `"*"` — pointer
- `"*()"` — pointer to function (e.g. `int (*fp)(int)`)
- `"[N]"` — array of N elements
- `"[N][M]"` — 2-D array (outermost first)
- `"()"` — function
- `"(...)"` — variadic function

`apply_derivation(Type2 *base, const char *ts)` builds the Type2 chain left-to-right (outermost first), so `"[3][4]"` → `array(3, array(4, base))` and `"*()"` → `pointer(function(base))`.

`type2_from_ts(Node *node, char *ts)` is the public entry point used by the parser. When `node->typespec & TB_TYPEDEF`, the base is taken from `node->type` directly (already resolved during parsing).

### Struct Layout

`calc_struct_layout(Type2 *st)` iterates the `Field` linked list:

- **Struct**: for each field, align current offset to `field->type->align`, record `field->offset`, advance by `field->size`. Final `st->size = do_align(total, max_align)`.
- **Union**: all fields at `offset = 0`; `st->size = max(field sizes)`.

`do_align(val, size)` rounds `val` up to the next multiple of `size` (handles 2 and 4).

### Symbol Table

The symbol table is a **tree of `Symbol_table` nodes** mirroring the lexical scope hierarchy.

```c
struct Symbol_table {
    Scope           scope;          // depth + indices identifying this scope
    Symbol         *idents;         // NS_IDENT linked list
    Symbol         *tags;           // NS_TAG linked list (struct/union tags)
    Symbol         *members;        // NS_MEMBER (unused; struct members are in Type2)
    Symbol         *labels;         // NS_LABEL
    Symbol         *typedefs;       // NS_TYPEDEF linked list (typedef names)
    int             size;           // bytes of locals in this scope
    int             global_offset;  // accumulated offset from parent scopes (for nested locals)
    int             child_count;
    Symbol_table  **children;
    Symbol_table   *parent;
};

struct Symbol {
    char   *name;
    Type2  *type;
    int     offset;        // see addressing below
    bool    is_param;
    bool    is_enum_const; // true for enum constants (use offset as integer value, no load)
    bool    is_static;     // file-scope static — label mangled to _s{tu_index}_{name}
    int     tu_index;      // TU that defined this static (set by insert_ident)
    bool    is_extern_decl;// extern declaration — no data allocation; upgraded on definition
    Symbol *next;
};
```

**Scope addressing:** A scope is identified by `(depth, indices[])`. E.g., depth=2, indices=[3,1] means: the 1st child of the 3rd child of the root. Scopes are created by `enter_new_scope()` and released by `leave_scope()`.

**Symbol offsets:**
- **Global** (`depth == 0`): `offset` is the byte position in the global data area (used as a label name in assembly).
- **Local** (`is_param == false`): `offset` is the accumulated size of locals declared before this one in the current (and enclosing) scopes. Used as `lea -(global_offset + offset)`.
- **Parameter** (`is_param == true`): `offset` starts at 8 (above saved lr and bp) and increases for each parameter. Used as `lea (global_offset + offset)`.
- **Enum constant** (`is_enum_const == true`): `offset` holds the integer value; no memory address.

`find_local_addr(node, name)` searches from the node's scope upward, returning `global_offset + symbol->offset`, with bit 30 set for parameters. Returns -1 for globals (depth 0).

### Key Helper Functions

```c
Type2 *elem_type(Type2 *t)          // strip one array/pointer layer; return self for scalars
Type2 *array_elem_type(Type2 *t)    // walk to innermost element type (all array layers)
int    array_dimensions(Type2 *t)   // count array nesting depth
int    find_offset(Type2 *t, char *field, Type2 **out_type)  // field byte offset, -1 if not found
Symbol *find_symbol(Node *node, char *name, Namespace ns)    // search up scope tree
bool   is_typedef_name(char *name)   // search curr_scope_st chain for typedef name
Type2 *find_typedef_type(char *name) // return typedef's resolved Type2*

bool istype_int(Type2 *t)      // t->base == TB2_INT
bool istype_ptr(Type2 *t)      // t->base == TB2_POINTER
bool istype_array(Type2 *t)    // t->base == TB2_ARRAY
bool istype_function(Type2 *t) // t->base == TB2_FUNCTION
bool istype_intlike(Type2 *t)  // any integer or pointer type (for pointer arithmetic)
// … etc.
```

---

## Code Generator (`codegen.c`)

Walks the annotated AST and prints assembly instructions to stdout.

### Stack Frame Layout

```
  bp+8 + 2*(n-1)   param n   (last param, 2-byte int/pointer)
       ...
  bp+8             param 0   (first param)   ← sp immediately before jl
  bp+4             return address (lr)
  bp+0             saved bp                  ← bp set here by enter
  bp-2             local 0
  bp-4             local 1
       ...
  bp-2k            local k                   ← sp after all adj in nested scopes
```

`enter word` executes: `mem[sp-4] = lr; mem[sp-8] = bp; bp = sp-8; sp -= word + 8`.

`ret` executes: `sp = bp; bp = mem[sp]; pc = mem[sp+4]; sp += 8`.

Locals within a compound statement are allocated with `adj -size` on entry and reclaimed with `adj +size` on exit. Nested compound statements chain their adj pairs.

### Variable Addressing

`gen_varaddr_from_ident(node, name)`:
- **Global (non-static)**: `immw label_name` (symbolic address)
- **Global (static)**: `immw _s{tu_index}_{name}` (mangled to be TU-private)
- **Local static**: `immw _ls{id}` (persistent data-section label; `id` from `local_static_counter`)
- **Parameter**: `lea positive_offset` (above bp)
- **Local**: `lea -offset` (below bp)

After `gen_varaddr`, r0 holds the **address** of the variable. For non-array, non-pointer variables, `gen_ld(size)` is immediately called to load the value; arrays and pointers leave r0 as the address.

### Expression Idioms

**Binary operation** (`ND_BINOP`):
```asm
; gen_expr(lhs) → r0 = lhs
push                ; stack[sp] = lhs
; gen_expr(rhs) → r0 = rhs
op                  ; r0 = stack[sp] op r0; sp += 4
```

**Assignment** (`ND_ASSIGN`):
```asm
; gen_addr(lhs) → r0 = address of lhs
push
; gen_expr(rhs) → r0 = value
sb/sw/sl            ; mem[stack[sp]] = r0; sp += 4
```

**Pointer/array dereference** (`ND_UNARYOP "*"`, non-call):
```asm
; gen_expr(child) → r0 = address to load from
lb/lw/ll            ; r0 = mem[r0]  (size from elem_type(node->type)->size)
```

**Struct member access** (`ND_MEMBER`, `is_function=false`):
```asm
; gen_addr(struct_expr) → r0 = struct base address
push
immw member_offset
add
lb/lw/ll            ; load member value
```

**Call through struct member function pointer** (`ND_MEMBER`, `is_function=true`; covers `s.fp(args)` and `s->fp(args)`):
```asm
; push arguments right-to-left (children[2..])
; gen_expr(arg[n-1]) → pushw/push
; ...
; gen_expr(arg[0]) → pushw/push
; gen_addr(node) → r0 = address of fn-ptr field (struct_base + member_offset)
lw                  ; load function pointer value (2 bytes)
jli                 ; indirect call
adj   param_total_bytes
```

**Direct function call** (`ND_IDENT` with `is_function=true` and function type):
```asm
; push arguments right-to-left (last arg first)
; gen_expr(arg[n-1]) → pushw/push
; ...
; gen_expr(arg[0]) → pushw/push
jl    func_name
adj   param_total_bytes   ; caller cleans up parameters
```
Function designators used as values (e.g. `fp = myfunc`) emit only `immw func_name` (no call).

**Indirect call through function pointer variable** (`fp(args)` where `fp` has `ptr(fn)` type):
```asm
; push arguments right-to-left
; gen_expr(arg[n-1]) → pushw/push
; ...
; gen_expr(arg[0]) → pushw/push
lea   fp_offset     ; or immw fp for globals
lw                  ; load function pointer value into r0
jli                 ; indirect call: lr = pc; pc = r0
adj   param_total_bytes
```

**Indirect call through dereferenced pointer** (`(*fp)(args)`):
```asm
; push arguments right-to-left (children[1..n-1])
; gen_expr(children[0]) → r0 = function address (evaluating the pointer expr)
jli
adj   param_total_bytes
```

**`putchar(c)`** — special CPU builtin, handled directly in `gen_callfunction`:
```asm
; gen_expr(c) → r0
putchar             ; opcode 0x1e; writes r0 & 0xff to stdout; does not modify r0
```

**Logical OR** (`||`):
```asm
; gen_expr(lhs)
jnz  L1         ; short-circuit: if lhs != 0, result is 1
; gen_expr(rhs)
jnz  L1
immw 0
j    L2
L1: immw 1
L2:
```

**Logical AND** (`&&`):
```asm
; gen_expr(lhs)
jz   L1         ; short-circuit: if lhs == 0, result is 0
; gen_expr(rhs)
jz   L1
immw 1
j    L2
L1: immw 0
L2:
```

**Break/continue label stacks** (`codegen.c` file scope):
```c
static int break_labels[64];   // target label for break at each loop nesting depth
static int cont_labels[64];    // target label for continue at each loop nesting depth
static int loop_depth = 0;     // current nesting depth (incremented on loop entry)
```
`gen_whilestmt`, `gen_forstmt`, `gen_dowhilestmt`, and `gen_switchstmt` push their break/continue targets before generating the body and pop them after. `gen_breakstmt`/`gen_continuestmt` emit `j break_labels[loop_depth-1]` / `j cont_labels[loop_depth-1]`.

**Switch dispatch** (`gen_switchstmt`): two-phase — phase 1 re-evaluates the selector expression for each `case` label, emitting `push; eq; jnz lcase` comparisons; phase 2 emits the compound-statement body verbatim, inserting `gen_label` at each `ND_CASESTMT`/`ND_DEFAULTSTMT` node.

**Goto/label resolution** (`gen_function`): before emitting a function body, `collect_labels()` walks the AST and calls `new_label()` for every `ND_LABELSTMT`, storing `(name, id)` pairs in `label_table[64]`. Both `gen_gotostmt` and `gen_labelstmt` look up the name in this table, so forward jumps resolve correctly.

### Cast Generation (`gen_cast`)

Casts are no-ops when src and dst have the same size. Otherwise:

| From → To | Instructions |
|---|---|
| char → int (sign-extend) | `sxb` |
| short → int (sign-extend) | `sxw` |
| long/ulong → int (truncate) | `push; immw 0xffff; and` |
| long/ulong → char (truncate) | `push; immw 0xff; and` |
| int → char (truncate) | `push; immw 0xff; and` |
| uchar/ushort → larger | zero-extend (no-op, top bits assumed 0) |
| int/ptr → long (zero-extend) | `push; immw 0; push; …` (builds 32-bit) |
| int-like → float/double | `sxb` or `sxw` (sign-extend to 32 bits) then `itof` |
| float/double → int-like | `ftoi` then `push; immw mask; and` if size < 4 |

### Global Variable Initialization

**Scalars**: emits `word value` (or `long` for 4-byte integer types) in the data section. Float scalars emit 4 bytes of IEEE 754 representation via `gen_bytes`.

**Arrays** (global): `gen_mem_inits()` recursively fills a `char data[]` byte buffer, then `gen_bytes()` emits one `byte` directive per byte.

**Arrays** (local): `gen_fill(vaddr, size)` zeroes all bytes, then `gen_inits()` recursively emits `lea; push; immw; sw/sl` sequences for non-zero initializers.

**Structs**: `gen_struct_inits` (local) and `gen_struct_mem_inits` (global) write each field; nested structs recurse. Partial initializers leave remaining fields zeroed.

### Three-Pass Code Generation

`gen_code(node, tu_index)` sets `current_tu` then makes three passes over the top-level declaration list for the current TU:

1. **Pass 1**: emit function definitions (follows the preamble inline in the text area).
2. **Pass 2**: emit global variable declarations (data area after the code). `extern` declarations and bare function prototypes are skipped (no data emitted). Static globals use the mangled label `_s{tu_index}_{name}`.
3. **Pass 2b**: emit local static variable data collected during pass 1 (`_ls0:`, `_ls1:` etc.). `local_static_counter` is monotonically increasing across TUs. Labels are assigned at symbol-table build time; data is emitted after all global vars.
4. **Pass 3**: emit deferred string literal data (`_l0:`, `_l1:` etc.), each as a sequence of `byte` directives followed by a null terminator. The `labels` counter and string literal IDs are monotonically increasing across TUs to prevent collisions.

---

## Per-Translation-Unit Compilation (`mycc.c` + `types.c` + `codegen.c`)

The compiler supports true per-TU compilation when invoked with `.c` file arguments. Each file is compiled independently with its own symbol table; the combined assembly is emitted as a single stream to stdout. The assembler resolves all label references, so cross-TU calls work without object files.

### Input Modes

- **String mode** (`./mycc "C code"`): single TU; `tu_index = 0`. Identical to previous behaviour.
- **File mode** (`./mycc a.c b.c ...`): detected by `fopen(argv[1])` succeeding. One TU per file, compiled in argument order. The preamble is emitted once before the loop.

### Per-TU State Reset

At the start of each TU, three reset functions are called:

```c
reset_codegen()      // clears strlit_count, loop_depth, label_table_size
                     // does NOT reset 'labels' — monotonically increasing across TUs
reset_parser()       // clears current_function
                     // does NOT reset anon_index — monotonically increasing across TUs
reset_types_state()  // allocates a fresh symbol_table / curr_scope_st / last_symbol_table
                     // resets scope_depth and scope_indices
                     // does NOT touch the 'types' linked list or t_int/t_void singletons
                     // (Type2* pointers from previous TUs remain valid)
```

After reset, `make_basic_types()` re-populates the basic type singletons (they are found in the existing `types` list and reused, not duplicated).

### Extern Symbol Table (`mycc.c`)

A flat `ExternSym[]` array accumulates non-static global symbols after each TU:

```c
typedef struct { char *name; Type2 *type; } ExternSym;
static ExternSym extern_syms[1024];
static int       extern_sym_count = 0;
```

**`harvest_globals()`** (called after `gen_code`): walks `symbol_table->idents` of the just-compiled TU and appends any symbol that is not `is_static`, not `is_extern_decl`, and not `putchar`. Deduplication prevents double-adding.

**`prepopulate_extern_syms()`** (called before each TU's `program()`): calls `insert_extern_sym()` for each accumulated symbol. This makes previously-defined globals visible to the new TU without requiring explicit `extern` declarations.

**`insert_extern_sym()`** (in `types.c`): inserts a symbol into the fresh global scope with `is_extern_decl = true` and no data allocation (`st->size` is not incremented). Skipped if the name is already present (e.g. `putchar`).

### Static Symbol Mangling

File-scope `static` symbols are TU-private. The label name in emitted assembly is mangled from `foo` to `_s{tu_index}_foo` in three sites in `codegen.c`:

| Site | Code |
|---|---|
| `gen_function` (function label) | `printf("_s%d_%s:\n", fsym->tu_index, fsym->name)` |
| `gen_callfunction` (direct call) | `gen_jl("_s{tu_index}_{name}")` |
| `gen_varaddr_from_ident` (global address) | `printf("    immw    _s%d_%s\n", sym->tu_index, sym->name)` |

`is_static` and `tu_index` are set on the `Symbol` by `insert_ident()` in `types.c` when `node->sclass == TK_STATIC` at global scope.

### Extern Declaration Handling

When `insert_ident()` sees `node->sclass == TK_EXTERN` at global scope it:
- Does **not** increment `st->size` (no data allocated).
- Sets `sym->is_extern_decl = true`.

When a real definition arrives for the same name (in the same or a later TU), `insert_ident()` finds the existing entry and upgrades it: clears `is_extern_decl`, updates the type, and allocates data offset.

`gen_decl()` skips entire `extern` declarations (`node->sclass == TK_EXTERN → return`) and bare function prototypes (`istype_function(n->symbol->type) → continue`) so no spurious labels or data are emitted.

---

## Target ISA: CPU3

A custom 32-bit accumulator/stack hybrid machine simulated by `cpu3/sim.py` (`cpu3/assembler.py` + `cpu3/cpu.py`).

### Registers

| Register | Width | Role |
|---|---|---|
| `r0` | 32-bit | Accumulator and return value |
| `sp` | 16-bit | Stack pointer (grows downward) |
| `bp` | 16-bit | Base/frame pointer |
| `lr` | 16-bit | Link register (return address) |
| `pc` | 16-bit | Program counter |
| `H` | 1-bit | Halt flag |

Registers are masked to their width after each instruction. Memory is 65536 bytes. The stack starts at `sp = 0x1000` (set by `ssp`). Data is little-endian.

### Instruction Encoding

Three formats:
- **Format 0** (1 byte): opcode only
- **Format 1** (2 bytes): opcode + signed 8-bit immediate
- **Format 2** (3 bytes): opcode + unsigned 16-bit immediate (little-endian)

### Instruction Set

#### Format 0 — No Operand

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0x00 | `halt` | H = 1 |
| 0x01 | `ret` | sp = bp; bp = mem32[sp]; pc = mem32[sp+4]; sp += 8 |
| 0x02 | `push` | sp -= 4; mem32[sp] = r0 |
| 0x03 | `pushw` | sp -= 2; mem16[sp] = r0 |
| 0x04 | `pop` | r0 = mem32[sp]; sp += 4 |
| 0x05 | `popw` | r0 = mem16[sp]; sp += 2 |
| 0x06 | `lb` | r0 = mem8[r0] |
| 0x07 | `lw` | r0 = mem16[r0] |
| 0x08 | `ll` | r0 = mem32[r0] |
| 0x09 | `sb` | addr = mem32[sp]; mem8[addr] = r0; sp += 4 |
| 0x0a | `sw` | addr = mem32[sp]; mem16[addr] = r0; sp += 4 |
| 0x0b | `sl` | addr = mem32[sp]; mem32[addr] = r0; sp += 4 |
| 0x0c | `add` | r0 = mem32[sp] + r0; sp += 4 |
| 0x0d | `sub` | r0 = mem32[sp] - r0; sp += 4 |
| 0x0e | `mul` | r0 = mem32[sp] * r0; sp += 4 |
| 0x0f | `div` | r0 = mem32[sp] / r0; sp += 4 |
| 0x10 | `mod` | r0 = mem32[sp] % r0; sp += 4 |
| 0x11 | `shl` | r0 = mem32[sp] << r0; sp += 4 |
| 0x12 | `shr` | r0 = mem32[sp] >> r0; sp += 4 |
| 0x13 | `lt` | r0 = (mem32[sp] < r0) ? 1 : 0; sp += 4 |
| 0x14 | `le` | r0 = (mem32[sp] <= r0) ? 1 : 0; sp += 4 |
| 0x15 | `gt` | r0 = (mem32[sp] > r0) ? 1 : 0; sp += 4 |
| 0x16 | `ge` | r0 = (mem32[sp] >= r0) ? 1 : 0; sp += 4 |
| 0x17 | `eq` | r0 = (mem32[sp] == r0) ? 1 : 0; sp += 4 |
| 0x18 | `ne` | r0 = (mem32[sp] != r0) ? 1 : 0; sp += 4 |
| 0x19 | `and` | r0 = mem32[sp] & r0; sp += 4 |
| 0x1a | `or` | r0 = mem32[sp] \| r0; sp += 4 |
| 0x1b | `xor` | r0 = mem32[sp] ^ r0; sp += 4 |
| 0x1c | `sxb` | r0 = sign_extend_8(r0) |
| 0x1d | `sxw` | r0 = sign_extend_16(r0) |
| 0x1e | `putchar` | sys.stdout.write(chr(r0 & 0xff)); r0 unchanged |
| 0x1f | `jli` | lr = pc; pc = r0 & 0xffff  *(indirect call via r0)* |
| 0x20 | `fadd` | r0 = float_bits(float(stack) + float(r0)); sp += 4 |
| 0x21 | `fsub` | r0 = float_bits(float(stack) - float(r0)); sp += 4 |
| 0x22 | `fmul` | r0 = float_bits(float(stack) * float(r0)); sp += 4 |
| 0x23 | `fdiv` | r0 = float_bits(float(stack) / float(r0)); sp += 4 |
| 0x24 | `flt` | r0 = (float(stack) < float(r0)) ? 1 : 0; sp += 4 |
| 0x25 | `fle` | r0 = (float(stack) <= float(r0)) ? 1 : 0; sp += 4 |
| 0x26 | `fgt` | r0 = (float(stack) > float(r0)) ? 1 : 0; sp += 4 |
| 0x27 | `fge` | r0 = (float(stack) >= float(r0)) ? 1 : 0; sp += 4 |
| 0x28 | `itof` | r0 = float_bits(float(signed32(r0))) |
| 0x29 | `ftoi` | r0 = int(float(r0)) truncated toward zero |

Float operands are 32-bit IEEE 754 single-precision values stored as raw bit patterns in r0 and on the stack. `float_bits(f)` means the IEEE 754 bit pattern of `f`. `float(x)` means interpret the bit pattern `x` as IEEE 754.

**Stack-binary convention**: all `add`, `sub`, `mul`, `div`, `mod`, `shl`, `shr`, comparisons, and bitwise ops pop their left operand from the stack (`mem32[sp]`) and use r0 as right operand, writing the result to r0 and incrementing sp by 4.

**Store convention**: `sb`, `sw`, `sl` pop a 32-bit address from the stack and write r0 to that address.

**CPU builtin**: `putchar` (0x1e) is pre-declared as a global builtin symbol by `make_basic_types()` via `insert_builtin()` in `types.c`; the compiler emits the opcode directly without a call frame.

#### Format 1 — Signed 8-bit Operand

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0x40 | `immb imm8` | r0 = sign_extend(imm8) |
| 0x41 | `adj imm8` | sp += imm8 |

`adj` is used to allocate (`adj -N`) or free (`adj +N`) local variable space within a scope.

#### Format 2 — Unsigned 16-bit Operand

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0x80 | `immw imm16` | r0 = imm16 |
| 0x81 | `immwh imm16` | r0 = (r0 & 0xffff) \| (imm16 << 16) |
| 0x82 | `j imm16` | pc = imm16 |
| 0x83 | `jl imm16` | lr = pc; pc = imm16 |
| 0x84 | `jz imm16` | if (r0 == 0) pc = imm16 |
| 0x85 | `jnz imm16` | if (r0 != 0) pc = imm16 |
| 0x86 | `enter imm16` | mem32[sp-4] = lr; mem32[sp-8] = bp; bp = sp-8; sp -= imm16+8 |
| 0x87 | `lea imm16` | r0 = bp + sign_extend(imm16) |
| 0x88 | `ssp imm16` | sp = imm16 |

**Loading a 32-bit constant**: use `immw` (lower 16 bits) followed by `immwh` (upper 16 bits). The compiler's `gen_imm(val)` emits this pair when `val > 0xffff`.

**Direct function call**: `jl` saves the return address in `lr`, then jumps. `enter` saves `lr` and `bp` on the stack and sets `bp` for the new frame. On return, `ret` restores the frame.

**Indirect function call**: load the function pointer into r0, then `jli` (same save/jump semantics as `jl` but target is r0 not an immediate).

### Assembly Syntax

```
.text=0             ; set text section origin to address 0
.data=N             ; set data section origin

label:              ; defines a label at current address
    mnemonic        ; zero-operand instruction
    mnemonic  val   ; instruction with operand (decimal or 0x hex)
    mnemonic  label ; instruction with label operand (resolved by assembler)

; Directives:
    byte  v1 v2 ...    ; emit one byte per value
    word  v1 v2 ...    ; emit one 16-bit word per value (labels allowed as values)
    long  v1 v2 ...    ; emit one 32-bit long per value
    allocb N           ; reserve N zero bytes
    allocw N           ; reserve N zero words (word-aligned)
    align              ; align to next word boundary
```

Comments start with `;`. Labels and instruction names are case-sensitive. The assembler resolves labels in `word` and `byte` directives (not only in instruction operands).

### Typical Assembly Output

```asm
.text=0
    ssp     0x1000    ; set stack pointer
    jl      main      ; call main
    halt

main:
    enter   0         ; save lr, bp; no extra locals at function level
    adj     -4        ; allocate 2 locals (int a, int b each 2 bytes)
    lea     -2
    push
    immw    0x000a    ; a = 10
    sw
    lea     -4
    push
    immw    0x000b    ; b = 11
    sw
    lea     -2
    lw
    push
    lea     -4
    lw
    add               ; a + b
    ret
    adj     4         ; reclaim locals
    ret               ; function return
```

---

## C89 Compliance Status

Preprocessor excluded. Features are assessed against ANSI C89/ISO C90.

**Deliberate deviations from C89:**
- **K&R (old-style) function definitions are not supported** — only ANSI prototype-style definitions are accepted.
- **`//` line comments are supported** — a C99/C++ extension not present in C89.
- **Implicit `int` return type is not allowed** — every function must explicitly state its return type.
- **Implicit function declarations are not allowed** — a function must be declared or defined before it is called.
- **Bit fields in structs are not supported** — the syntax `type name : width;` is not parsed. Bit fields are rarely used in practice and add significant complexity (platform-specific packing, masking, alignment) for minimal benefit on this target.
- **Declarations may appear anywhere in a block** — C99 extension. In C89, all declarations within a `{}` block must precede all statements. This compiler accepts declarations interleaved with statements (e.g. `int a=1; a++; int b=a*2;`). Note: all locals are still allocated upfront at scope entry (single `adj -N`), so a variable's storage exists from the start of the block even if its declaration appears later in the source.
- **`for`-init declarations** — C99 extension. `for (int i = 0; i < n; i++)` is supported. The loop variable is scoped to the for statement (condition, increment, and body). Implemented by creating an implicit hidden scope for the init declaration; `gen_forstmt` wraps the loop in `adj -N`/`adj +N`.

### Statements

| Feature | Status | Notes |
|---|---|---|
| `if` / `if-else` | ✅ | |
| `while` | ✅ | |
| `do-while` | ✅ | |
| `for` | ✅ | init/cond/inc all optional (ND_EMPTY for absent parts); init may be a declaration (C99) |
| `switch` / `case` / `default` | ✅ | fall-through supported; selector re-evaluated per case |
| `break` | ✅ | works inside `while`, `for`, `do-while`, `switch` |
| `continue` | ✅ | works inside `while`, `for`, `do-while` |
| `goto` | ✅ | function-scoped labels; forward and backward jumps |
| Labeled statements (`lbl:`) | ✅ | pre-scan assigns numeric label IDs before codegen |
| `return` | ✅ | Auto-casts to declared return type |
| Compound statement `{ }` | ✅ | |

### Expressions and Operators

| Feature | Status | Notes |
|---|---|---|
| Arithmetic `+ - * /` | ✅ | |
| Modulo `%` | ✅ | |
| Compound assignment `+= -= *= /= %= &= \|= ^= <<= >>=` | ✅ | Desugared to `lhs = lhs op rhs` in parser |
| Ternary `? :` | ✅ | ND_TERNARY node; right-associative via `cond_expr()` |
| Comma operator `,` (in expressions) | ✅ | `expr()` loops on commas; function args use `assign_expr()` |
| `sizeof` | ✅ | `sizeof(type-name)` and `sizeof(ident)`; complex exprs not supported |
| Pre/post `++ --` | ✅ | |
| Unary `+ - ~ !` | ✅ | |
| Address-of `&` | ✅ | |
| Dereference `*` | ✅ | |
| Relational `< <= > >= == !=` | ✅ | |
| Logical `&& \|\|` (short-circuit) | ✅ | |
| Bitwise `& \| ^ << >>` | ✅ | |
| Cast `(type)expr` | ✅ | |
| Array subscript `a[i]` | ✅ | Rewritten to pointer arithmetic |
| Struct member `.` | ✅ | Including nested structs |
| Pointer member `->` | ✅ | ND_MEMBER with val `"->"`, pointer dereferenced in gen_addr |
| Function call | ✅ | Including recursion; multiple args supported |
| Assignment `=` | ✅ | |

### Types

| Feature | Status | Notes |
|---|---|---|
| `char` / `unsigned char` | ✅ | 1 byte |
| `short` / `unsigned short` | ✅ | 2 bytes |
| `int` / `unsigned int` | ✅ | 2 bytes on target |
| `long` / `unsigned long` | ✅ | 4 bytes |
| `float` | ✅ | 4-byte IEEE 754; arithmetic, comparisons, and int↔float casts work |
| `double` | ✅ | Treated identically to `float` (no 64-bit FP distinction on target) |
| `void` | ✅ | |
| Pointers | ✅ | Including pointer arithmetic |
| Arrays (1-D and N-D) | ✅ | |
| `struct` | ✅ | Including nested structs and correct member offsets |
| `union` | ✅ | All members at offset 0; `is_union` flag in Type2 sets size = max member size |
| `enum` | ✅ | Tagged and anonymous enums; implicit/explicit/negative values; enum variables; sizeof(enum); case labels with enum constants |
| `typedef` | ✅ | Scalar, pointer, struct/union aliases; lexical scoping with shadowing |
| Function pointers | ✅ | Declare, assign, call via `fp(args)`, `(*fp)(args)`, `s.fp(args)`, `s->fp(args)`; pass as arguments; use as parameters (`int (*f)(int)`) |
| Bit fields in structs | N/A | Deliberately not supported — see deviations |
| `const` / `volatile` qualifiers | ⚠️ | Parsed and stored; semantics not enforced |

### Declarations and Linkage

| Feature | Status | Notes |
|---|---|---|
| Multiple declarators (`int a, b;`) | ✅ | |
| Scalar initializer (`int x = 5;`) | ✅ | |
| Array initializer (`int a[] = {1,2,3};`) | ✅ | Including multi-dim |
| Struct/union initializer | ✅ | Flat and nested; partial init zero-fills remainder |
| `typedef` declarations | ✅ | Stored in `NS_TYPEDEF` per scope; `gen_decl` skips typedef nodes |
| `auto` | ⚠️ | Parsed; no semantic difference from default local |
| `register` | ⚠️ | Parsed; ignored (no register allocator) |
| `static` (file scope) | ✅ | Internal linkage: label mangled to `_s{tu_index}_{name}`; invisible to other TUs |
| `static` (local) | ✅ | Persistent storage in data section; label `_ls{id}`; compile-time-constant initializers only |
| `extern` | ✅ | Suppresses data allocation; real definition upgrades the symbol; cross-TU globals pre-populated automatically |
| Forward declarations | ✅ | Functions declared before their definition resolve correctly via label references in assembly |
| K&R (old-style) function definitions | N/A | Deliberately not supported; ANSI prototype style only |

### Literals and Constants

| Feature | Status | Notes |
|---|---|---|
| Decimal integer constants | ✅ | |
| Hex constants (`0x…`) | ✅ | |
| Octal constants (`0…`) | ✅ | |
| Integer suffixes `u/U`, `l/L`, `ul/UL` | ✅ | |
| Floating-point constants | ✅ | Parsed and emitted as IEEE 754 via `immw`/`immwh` pair |
| Character constants (`'a'`, escape sequences) | ✅ | |
| String literals (`"…"`) | ✅ | `TK_STRING` token; escape decoding; adjacent concatenation; `char*` type; deferred data emission in codegen pass 3; `char s[]`/`char s[N]` init; assembler extended to resolve labels in `word` directives |

### Functions and Calling Convention

| Feature | Status | Notes |
|---|---|---|
| Function definitions with prototypes | ✅ | |
| Parameter passing (value) | ✅ | |
| Return values | ✅ | |
| Recursion | ✅ | |
| Variadic functions (`...`) | ✅ | `va_list` (typedef'd to `int`), `va_start`, `va_arg`, `va_end`; `putchar` CPU builtin (opcode 0x1e) |

### Type Conversion and Promotion

| Feature | Status | Notes |
|---|---|---|
| Integer promotions (char/short → int in expressions) | ✅ | Applied in `check_operands` and `check_unary_operand` for all binary and most unary ops |
| Usual arithmetic conversions (mixed-type operands) | ✅ | Full C89 rank hierarchy: float > ulong > long > uint > int |
| Pointer ↔ integer conversions | ✅ | Used in tests (e.g. `a = 0x2000`) |
| Floating-point conversions | ✅ | `itof`/`ftoi` opcodes; int→float sign-extends first |
| Function-to-pointer decay | ✅ | Function designators used as values or passed as arguments decay to pointer-sized (2 bytes) |

#### Target-Dependent Arithmetic Notes (revisit if porting to a wider target)

These behaviors are correct for the current 16-bit target (`sizeof(int) == sizeof(short) == 2`) but would need changes for a 32-bit target (`sizeof(int) == 4`, `sizeof(short) == 2`):

- **`unsigned short` promotes to `unsigned int`** — on this target `int` is 2 bytes and cannot represent unsigned short values above 32767, so C89 §3.2.1.1 requires promotion to `unsigned int`. On a 32-bit target `int` can represent all `unsigned short` values, so the promotion would be to `int` instead of `unsigned int`. Both are size-2 here, so no code is generated either way.
- **`short → int` is a no-op** — both are 2 bytes on this target; no code is generated. On a 32-bit target this would emit `sxw` (sign-extend to 32 bits).
- **`unsigned char → int` is a no-op** — zero-extension from 8 to 16 bits is assumed already done; on a 32-bit target it would zero-extend to 32 bits.
- **Function argument promotion is not applied** — C89 §3.3.2.2 says each argument is converted to the declared parameter type. No casts are inserted at call sites for narrow types (`char`, `short`). Harmless here because all integer types ≤ 2 bytes and the calling convention pushes 2-byte slots regardless. On a 32-bit target where arguments occupy 4-byte slots, narrow arguments would need widening casts before the `push`.
- **`int` vs `unsigned int` arithmetic** — both are 2 bytes; only sign-interpretation differs (comparisons, right-shift, division). Mixed `int`/`uint` arithmetic converts to `uint` per the conversions, but the bit patterns are identical for `+`, `-`, `*`.

### Summary

**Implemented and working**: basic scalar types, pointers, 1-D/N-D arrays, structs, unions, `float`/`double` (IEEE 754 arithmetic, comparisons, int↔float casts), `if`/`while`/`for`/`do-while`, `switch`/`case`/`default`, `break`, `continue`, `goto`, labeled statements, all arithmetic and bitwise operators including `%`, all comparison and logical operators, unary `+ - ~ ! &` and dereference `*`, pre/post-increment/decrement, struct/union member access (`.` and `->`), explicit casts, array subscripting, function definitions and calls (multi-arg, recursive), integer constants (decimal/hex/octal), floating-point constants, string literals, compound assignment (`+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`), ternary `?:`, comma operator, `sizeof(type)`/`sizeof(ident)`, `typedef` (scalar/pointer/struct aliases with lexical scoping), `enum` (tagged/anonymous, implicit/explicit/negative values, enum variables, enum constants in expressions and case labels), variadic functions (`va_list`/`va_start`/`va_arg`/`va_end`), function pointers (declare, assign, call via `fp(args)`, `(*fp)(args)`, `s.fp(args)`, `s->fp(args)`, pass as arguments and parameters), per-TU compilation with `static` internal linkage and cross-TU `extern` resolution, `static` local variables (persistent storage in data section, compile-time-constant initializers, independent between functions).

**Partially working**: `const`/`volatile` (stored, not enforced), `auto`/`register` (parsed, ignored), integer promotions in function arguments (not applied at call sites; harmless on this target — see Target-Dependent Arithmetic Notes).

**Not yet implemented**: bit fields (deliberate), `sizeof(complex_expr)`, calling through a function-pointer stored in an array element.

**Extensions beyond C89**: `//` line comments; declarations anywhere in a block (C99); `for`-init declarations (C99).
