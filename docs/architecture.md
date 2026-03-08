# Compiler Architecture

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
ND_COMPOUND_ASSIGN // compound assignment (+=, -=, *=, /=, etc.) — children: [lhs, rhs]; op_kind = base op (e.g. TK_PLUS for +=); avoids double-eval of lhs
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
    bool         is_expr;       // node participates in type propagation
    bool         is_func_defn;  // true for function definitions (not just decls)
    bool         is_function;   // true when ident/unaryop has postfix "(args)" applied —
                                //   marks this node as a call site (or dereference-call)
    bool         is_array_deref;// true for array subscript UNARYOP "+" (no load)
    bool         is_variadic;   // true for variadic function/param-type-list
    Decl_spec    typespec;      // parser-internal bitmask (DS_INT | DS_UNSIGNED etc.)
    Token_kind   sclass;        // storage class specifier
    Token_kind   op_kind;       // for ND_BINOP/UNARYOP/MEMBER: identifies the operator
    Symbol_table *st;           // scope this node belongs to (set during parsing)
    Symbol_table *symtable;     // symbol table for compound statements
    Symbol       *symbol;       // resolved symbol (set during add_types_and_symbols)
    Type        *type;         // resolved type (set during propagate_types)
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

### Type Representation: `Type`

All types are represented as `Type` structs stored in a global linked list (`types`). Factory functions intern types by structural equality.

```c
typedef enum {
    TB_VOID, TB_CHAR, TB_UCHAR, TB_SHORT, TB_USHORT,
    TB_INT,  TB_UINT, TB_LONG,  TB_ULONG,
    TB_FLOAT, TB_DOUBLE,
    TB_POINTER, TB_ARRAY, TB_FUNCTION, TB_STRUCT, TB_ENUM
} Type_base;

typedef int Type_qual;   // TQ_CONST = 0x1, TQ_VOLATILE = 0x2

struct Type {
    Type_base  base;
    Type_qual  qual;
    int         size;   // bytes
    int         align;  // alignment
    union {
        struct { Type *pointee; }                       ptr;
        struct { Type *elem; int count; }               arr;
        struct { Type *ret; Param *params; bool is_variadic; } fn;
        struct { Symbol *tag; Field *members; bool is_union; } composite;
        struct { Symbol *tag; }                          enu;
    } u;
    Type *next;        // linked list of all types
};

struct Field {
    char   *name;       // member name (strdup'd)
    Type  *type;
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
| function | 2 | 2 | — |

**Note:** The target has a 16-bit address space. `int` and pointers are 2 bytes. Function types have size 2 (same as pointer); function designators used as values (e.g. passed as arguments) decay to a function-pointer value (2 bytes).

### Factory Functions

```c
Type *get_basic_type(Type_base base);            // returns cached singleton
Type *get_pointer_type(Type *pointee);           // dedup by pointee pointer
Type *get_array_type(Type *elem, int count);     // size = elem->size * count
Type *get_function_type(Type *ret, Param *params, bool is_variadic);
Type *get_struct_type(Symbol *tag, Field *members, bool is_union);
Type *get_enum_type(Symbol *tag);
```

### Type Derivation

Type construction from AST nodes is done directly by two static helpers in `types.c` (no intermediate derivation string):

`type_from_declarator(Node *decl, Type *base)` walks an `ND_DECLARATOR` node: applies pointer stars to `base` (innermost), then calls `type_from_direct_decl` for array/function suffixes.

`type_from_direct_decl(Node *dd, Type *base)` walks an `ND_DIRECT_DECL` node: applies array (`ND_ARRAY_DECL`) and function suffixes right-to-left (rightmost = innermost), then recurses for grouped inner declarators (e.g. `(*fp)`).

`type2_from_decl_node(Node *node)` is the public entry point (used by the parser for casts and `sizeof`). It determines the base type from `node->typespec` and applies the first declarator child if present. When `node->typespec & DS_TYPEDEF`, the base is taken from `node->type` directly.

### Struct Layout

`calc_struct_layout(Type *st)` iterates the `Field` linked list:

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
    Symbol         *members;        // NS_MEMBER (unused; struct members are in Type)
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
    Type  *type;
    int     offset;        // see addressing below
    bool    is_param;
    bool    is_enum_const; // true for enum constants (use offset as integer value, no load)
    bool    is_static;     // file-scope static — label mangled to _s{tu_index}_{name}
    int     tu_index;      // TU that defined this static (set by insert_ident)
    bool    is_extern_decl; // extern declaration — no data allocation; upgraded on definition
    bool    is_local_static;// local-scope static: persistent data section, label = _ls{offset}
    Symbol *next;
};
```

**Scope addressing:** A scope is identified by `(depth, indices[])`. E.g., depth=2, indices=[3,1] means: the 1st child of the 3rd child of the root. Scopes are created by `enter_new_scope()` and released by `leave_scope()`.

**Symbol offsets:**
- **Global** (`depth == 0`): `offset` is the byte position in the global data area (used as a label name in assembly).
- **Local** (`is_param == false`): `offset` is the accumulated size of locals declared before this one in the current (and enclosing) scopes. Used as `lea -(global_offset + offset)`.
- **Parameter** (`is_param == true`): `offset` starts at 8 (above saved lr and bp) and increases for each parameter. Used as `lea (global_offset + offset)`.
- **Enum constant** (`is_enum_const == true`): `offset` holds the integer value; no memory address.

`find_local_addr(node, name)` (codegen.c, static) searches from `node->st` upward, returning `LocalAddr { int offset; bool is_param; }`. `offset` is `global_offset + symbol->offset` for locals/params; `-1` for globals (depth 0) and local statics (which use their `_ls{id}` label instead). The `is_param` flag distinguishes parameters from locals for `lea` sign direction.

### Key Helper Functions

```c
Type *elem_type(Type *t)          // strip one array/pointer layer; return self for scalars
Type *array_elem_type(Type *t)    // walk to innermost element type (all array layers)
int    array_dimensions(Type *t)   // count array nesting depth
int    find_offset(Type *t, char *field, Type **out_type)  // field byte offset, -1 if not found
Symbol *find_symbol(Node *node, char *name, Namespace ns)    // search up scope tree
bool   is_typedef_name(char *name)   // search curr_scope_st chain for typedef name
Type *find_typedef_type(char *name) // return typedef's resolved Type*

bool istype_int(Type *t)      // t->base == TB_INT
bool istype_ptr(Type *t)      // t->base == TB_POINTER
bool istype_array(Type *t)    // t->base == TB_ARRAY
bool istype_function(Type *t) // t->base == TB_FUNCTION
bool istype_intlike(Type *t)  // any integer or pointer type (for pointer arithmetic)
// … etc.
```

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
                     // (Type* pointers from previous TUs remain valid)
```

After reset, `make_basic_types()` re-populates the basic type singletons (they are found in the existing `types` list and reused, not duplicated).

### Extern Symbol Table (`mycc.c`)

A flat `ExternSym[]` array accumulates non-static global symbols after each TU:

```c
typedef struct { char *name; Type *type; } ExternSym;
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
