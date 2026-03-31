# Compiler Architecture

## Preprocessor (`preprocess.c`)

Runs before the tokeniser on each translation unit. Transforms raw source text into preprocessed text that is fed to `tokenise()`.

### Features

| Directive | Behaviour |
|---|---|
| `#define NAME body` | Object-like macro; body expanded on use |
| `#define NAME(p, …) body` | Function-like macro; parameters substituted |
| `#undef NAME` | Remove macro |
| `#ifdef NAME` / `#ifndef NAME` | Conditional compilation (nestable) |
| `#else` / `#endif` | Complement / close conditional block |
| `#include "file"` | Resolved relative to the including file's directory |
| `#include <file>` | Resolved in the system include directory (`set_include_dir()`) |

Unknown directives are silently ignored. Adjacent string literals are **not** concatenated here; that is done by the tokeniser.

### API

```c
char *preprocess(const char *src, const char *filename);
  // Returns a malloc'd, null-terminated preprocessed string.
  // filename is used for relative #include resolution and error messages.

void reset_preprocessor(void);
  // Clears the macro table and resets the include-depth counter.
  // Called once per TU before preprocessing begins.

void set_include_dir(const char *dir);
  // Sets the directory searched for #include <file>.
  // Called once at startup from smallcc.c using the compiler binary's directory.
```

### System Include Directory

`smallcc.c` calls `get_compiler_dir(argv[0])` to find the directory containing the compiler binary, then passes `$bindir/include` to `set_include_dir()`. This lets `#include <stdio.h>` resolve to `include/stdio.h` next to the binary regardless of the working directory.

---

## Tokeniser (`tokeniser.c`)

Converts the preprocessed string to a singly-linked list of `Token` structs.

### Token Struct

```c
struct Token {
    Token_kind  kind;   // enum — see below
    Token       *next;
    char        *val;   // text of the token
    double      fval;   // parsed float value
    long long   ival;   // parsed integer value (also string literal length for TK_STRING)
    int         loc;    // character position in source (legacy; prefer line/col)
    int         line;   // 1-based line number
    int         col;    // 1-based column number
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

A hand-written recursive-descent parser. Also owns the three-pass type resolution: `resolve_symbols`, `derive_types`, and `insert_coercions`.

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
ND_PROGRAM       // root; ch[0] is head of top-level declaration linked list (->next)
ND_DECLARATION   // variable or function declaration; u.declaration holds specifiers
ND_DECLARATOR    // declarator; u.declarator.pointer_level = star count; ch[1] = initializer
ND_DIRECT_DECL   // direct declarator (name + suffix list via ->next)
ND_ARRAY_DECL    // "[N]" suffix; ch[0] = size expr (NULL if unspecified)
ND_FUNC_DECL     // "(params)" suffix; ch[0] = ND_PTYPE_LIST
ND_PTYPE_LIST    // parameter type list; u.ptype_list.is_variadic; params via ->next
ND_TYPE_NAME     // type name in a cast or sizeof
ND_STRUCT        // struct/union specifier; u.struct_spec.is_union; node->symtable = member scope
ND_MEMBER        // "." or "->" access; ch[0] = struct expr; u.member.{field_name,offset,is_function}
ND_COMPSTMT      // "{}" block; node->symtable = scope; statements via ch[0] linked list
ND_EXPRSTMT      // expression statement; ch[0] = expr
ND_IFSTMT        // if; ch[0]=cond, ch[1]=then, ch[2]=else (NULL if absent)
ND_WHILESTMT     // while; ch[0]=cond, ch[1]=body
ND_FORSTMT       // for; ch[0]=init, ch[1]=cond, ch[2]=inc, ch[3]=body (ND_EMPTY if absent)
                 //   node->symtable = hidden for-init scope (non-NULL only when init is a decl)
ND_DOWHILESTMT   // do-while; ch[0]=body, ch[1]=cond
ND_SWITCHSTMT    // switch; ch[0]=selector, ch[1]=body compstmt
ND_CASESTMT      // case N:; u.casestmt.{value, label_id}
ND_DEFAULTSTMT   // default:; u.defaultstmt.label_id
ND_BREAKSTMT     // break;
ND_CONTINUESTMT  // continue;
ND_GOTOSTMT      // goto ident; u.label = target label name
ND_LABELSTMT     // ident: stmt; u.labelstmt.name; ch[0] = statement
ND_EMPTY         // absent for-loop init/cond/inc
ND_RETURNSTMT    // return; ch[0] = expr (NULL for void return)
ND_STMT          // generic statement wrapper
ND_ASSIGN        // "="; ch[0]=lhs, ch[1]=rhs
ND_COMPOUND_ASSIGN // +=, -=, etc.; ch[0]=lhs, ch[1]=rhs; op_kind = base operator token
ND_BINOP         // binary op; op_kind = operator token; ch[0]=lhs, ch[1]=rhs
ND_UNARYOP       // unary op; op_kind = operator token; ch[0]=operand
                 //   u.unaryop.is_function: call through dereferenced fn-ptr
                 //   u.unaryop.is_array_deref: address-only (no load) for non-last array dim
ND_CAST          // explicit cast; ch[0]=ND_DECLARATION (type), ch[1]=expr
ND_TERNARY       // ?:; ch[0]=cond, ch[1]=then, ch[2]=else
ND_IDENT         // name; u.ident.name; u.ident.is_function = has postfix call
ND_LITERAL       // constant; u.literal.{ival, fval, strval, strval_len}
ND_INITLIST      // initializer list; items via ch[0] linked list
ND_VA_START      // va_start(ap, last); ch[0]=ap, ch[1]=last_param
ND_VA_ARG        // va_arg(ap, type); ch[0]=ap; node->type = requested type
ND_VA_END        // va_end(ap); no-op
ND_UNDEFINED
```

### Node Struct

Nodes use a tagged union (`u`) for kind-specific payload and a fixed 4-slot child array (`ch[4]`) for positional children. A `next` pointer links siblings in list contexts (e.g. top-level declarations, statement lists inside a compound).

```c
struct Node {
    Node_kind    kind;

    // Positional child slots. ch[0..3] default to NULL (arena zeroes memory).
    Node        *ch[4];

    // Kind-specific payload — only the matching union member is valid.
    union {
        struct { char *name; bool is_function; }                  ident;       // ND_IDENT
        char                                                      *label;      // ND_GOTOSTMT
        struct { bool is_function; bool is_array_deref; }         unaryop;     // ND_UNARYOP
        struct { char *field_name; bool is_function; int offset; } member;     // ND_MEMBER
        struct { char *name; }                                    labelstmt;   // ND_LABELSTMT
        struct { int pointer_level; }                             declarator;  // ND_DECLARATOR
        struct { bool is_variadic; }                              ptype_list;  // ND_PTYPE_LIST
        struct { Decl_spec typespec; StorageClass sclass;
                 bool is_func_defn; Type *typedef_type; }         declaration; // ND_DECLARATION
        struct { bool is_union; }                                 struct_spec; // ND_STRUCT
        struct { long long ival; double fval;
                 char *strval; int strval_len; }                  literal;     // ND_LITERAL
        struct { long long value; int label_id; }                 casestmt;    // ND_CASESTMT
        struct { int label_id; }                                  defaultstmt; // ND_DEFAULTSTMT
    } u;

    Node        *next;       // sibling link (linked-list children, e.g. stmts in compstmt)
    bool         is_expr;    // participates in type propagation
    Token_kind   op_kind;    // ND_BINOP/UNARYOP/MEMBER: identifies the operator
    Symbol_table *st;        // innermost scope at this node (set during parsing)
    Symbol_table *symtable;  // ND_COMPSTMT: its own scope
                             // ND_FORSTMT: hidden for-init scope (NULL if init is not a decl)
                             // ND_STRUCT: the struct-member parsing scope
    Symbol      *symbol;     // resolved symbol (set during resolve_symbols / add_types_and_symbols)
    Type        *type;       // resolved type (set during resolve_symbols / derive_types)
                             // NOTE: new_node() initialises type = t_void (not NULL)
};
```

### Array Subscript Rewriting

`E1[E2]` is rewritten in the parser (not in a separate pass) as pointer arithmetic:

- **Pointer subscript** (base has pointer type): creates `ND_UNARYOP "*"` wrapping `ND_BINOP "+"(ptr, idx)`. `insert_coercions` inserts stride scaling.
- **Not the last array dimension** (e.g., `a[i]` where `a` is `int[3][4]`): creates `ND_UNARYOP "+"` with `is_array_deref = true` and `node->type` set to the element-at-that-depth type. No load is generated; the address is passed through.
- **Last array dimension** (e.g., `a[i]` where `a` is `int[4]`): creates `ND_UNARYOP "*"` with `node->type` set to the leaf element type. A load is generated.

For array subscripts the child is `ND_BINOP "+"` with children `[base_addr, ND_BINOP "*" [stride_constant, index_expr]]`.

### Three-Pass Type Resolution

Type annotation is split across three sequential passes called from `smallcc.c` after parsing:

**Pass 1 — `resolve_symbols(root)`** (post-order)
- `ND_IDENT` (`is_expr == true`): `node->type = find_symbol_st(node->st, name, NS_IDENT)->type`; `node->symbol` is also set.
- `ND_MEMBER`: looks up the field in `lhs->type->u.composite.members`; sets `u.member.offset` and `node->type`; for `"->"` the lhs pointer is automatically dereferenced; when `u.member.is_function` (call through member fn-ptr), `node->type` is resolved to the function's return type.

**Pass 2 — `derive_types(root)`** (post-order, pure — no AST mutations)
- `ND_BINOP` (`is_expr`): `binop_result_type(lhs, rhs, op)` returns the usual-arithmetic-conversion result type; comparison/logical operators force result to `t_int`.
- `ND_UNARYOP` (`is_expr`): `unary_result_type(operand, op)` — for `*` returns `elem_type(child->type)`; preserves parser-set types when `node->type != t_void`.
- `ND_CAST`: result type taken from the type-declaration child.

**Pass 3 — `insert_coercions(root)`** (post-order, mutates AST)
- Inserts `ND_CAST` nodes for arithmetic conversions (e.g. `char → int`, `int → long`) and for `ND_RETURNSTMT` when the expression type differs from the declared return type.
- Inserts stride-scale multiplications for pointer arithmetic (`ND_BINOP "+"` where one operand is a pointer/array — multiplies the integer operand by `elem_type->size`).

### Scope Tracking and `last_symbol_table`

`type_ctx.last_symbol_table` always points to the most recently created scope from `enter_new_scope()`. `comp_stmt(true)` uses it to re-enter the scope created by `param_type_list`, so function parameters and body variables share a scope.

**Important:** `param_type_list` explicitly restores `type_ctx.last_symbol_table = node->symtable` after `leave_scope()`. This prevents inner `param_type_list` calls (e.g. from function-pointer declarators such as `int (*fp)(int)`) from overwriting it and corrupting the outer function's scope chain.

---

## Type System (`types.c` + `smallcc.h`)

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

Declaration specifiers accumulated during parsing are stored in a `DeclParseState`:

```c
typedef struct {
    Decl_spec    typespec;      // accumulated keyword bitmask (DS_INT | DS_UNSIGNED etc.)
    StorageClass sclass;        // SC_NONE / SC_STATIC / SC_EXTERN / SC_TYPEDEF etc.
    Type        *typedef_type;  // resolved Type* when DS_TYPEDEF is set
} DeclParseState;
```

Type construction from AST nodes is done by two static helpers in `types.c`:

`type_from_declarator(Node *decl, Type *base)` walks an `ND_DECLARATOR` node: applies `u.declarator.pointer_level` pointer stars to `base`, then calls `type_from_direct_decl` for array/function suffixes.

`type_from_direct_decl(Node *dd, Type *base)` walks an `ND_DIRECT_DECL` node: applies array (`ND_ARRAY_DECL`) and function suffixes right-to-left (rightmost = innermost), then recurses for grouped inner declarators (e.g. `(*fp)`).

`type2_from_decl_node(Node *node, DeclParseState ds)` is the public entry point (used by the parser for casts and `sizeof`). It determines the base type from `ds.typespec` (or `ds.typedef_type` when `DS_TYPEDEF` is set) and applies the first declarator child if present.

**`sizeof(struct tag)` / cast to struct type**: `type_name()` in `parser.c` calls `struct_decl(&ds, 0)` after `parse_decl_specifiers` when `DS_STRUCT | DS_UNION` is set. This populates `node->ch[0]` with an `ND_STRUCT` node that carries the tag name. `type2_from_decl_node` then looks up the tag via `find_symbol_st(node->st, tagname, NS_TAG)` to obtain the already-defined struct `Type*`. Without the `struct_decl` call the tag name would be silently consumed by the declarator loop, and without the tag lookup the result would be `t_void` regardless.

`generate_struct_type` in `types.c` builds the `Field` list for a struct type by iterating the member `ND_DECLARATION` nodes stored in the AST. It reads the base type from `d->u.declaration.typespec`; when `DS_TYPEDEF` is set it uses `d->u.declaration.typedef_type` (the resolved `Type*` copied from `DeclParseState.typedef_type` during `add_types_and_symbols`). This `typedef_type` field is necessary because `typespec_to_base` has no way to recover a typedef's underlying type from the bitmask alone.

### Struct Layout

`calc_struct_layout(Type *st)` iterates the `Field` linked list:

- **Struct**: for each field, align current offset to `field->type->align`, record `field->offset`, advance by `field->size`. Final `st->size = do_align(total, max_align)`.
- **Union**: all fields at `offset = 0`; `st->size = max(field sizes)`.

`do_align(val, size)` rounds `val` up to the next multiple of `size` (handles 2 and 4).

### Symbol Table

Symbols and scopes use three supporting enums:

```c
// Which C name space a symbol belongs to
typedef enum { NS_IDENT, NS_TAG, NS_TYPEDEF } Namespace;

// Distinguishes compound-statement scopes from struct-member scopes
typedef enum { ST_COMPSTMT, ST_STRUCT } Scope_type;

// Symbol kind — encodes storage/addressing class
typedef enum {
    SYM_LOCAL,         // stack-allocated local  (lea -offset)
    SYM_PARAM,         // function parameter     (lea +offset)
    SYM_GLOBAL,        // non-static global      (immw name)
    SYM_STATIC_GLOBAL, // file-scope static      (immw _s{tu}_{name})
    SYM_STATIC_LOCAL,  // local-scope static     (immw _ls{id})
    SYM_EXTERN,        // extern decl            (no data allocated)
    SYM_ENUM_CONST,    // enum constant          (offset = integer value, no load)
    SYM_BUILTIN,       // compiler-pre-declared  (putchar etc.)
} SymbolKind;
```

`Symbol_table` nodes form a chain via `parent` pointers. Each scope holds a single unified symbol list (identifiers, tags, and typedefs are distinguished by the `ns` field):

```c
struct Symbol_table {
    int          depth;        // nesting depth (0 = global)
    Scope_type   scope_type;   // ST_COMPSTMT or ST_STRUCT
    int          scope_id;     // monotonic ID assigned at creation (for debug)
    Symbol      *symbols;      // unified list: all namespaces, filtered by ns field
    Symbol      *symbols_tail; // tail pointer (O(1) append)
    int          size;         // total bytes of SYM_LOCAL symbols in this scope
    int          local_offset; // running byte offset for the next SYM_LOCAL
    int          param_offset; // running byte offset for the next SYM_PARAM (init: FRAME_OVERHEAD)
    Symbol_table *parent;
};

struct Symbol {
    const char  *name;
    Type        *type;
    int          offset;    // meaning depends on kind — see below
    SymbolKind   kind;
    Namespace    ns;        // NS_IDENT | NS_TAG | NS_TYPEDEF
    int          tu_index;  // TU index for SYM_STATIC_GLOBAL / SYM_STATIC_LOCAL
    Symbol      *next;
};
```

**Symbol offset semantics (by `kind`):**
- `SYM_GLOBAL`: byte position in the global data area (label index in assembly).
- `SYM_LOCAL`: bp-relative offset below frame base; address = `lea -(local_offset)`.
- `SYM_PARAM`: bp-relative offset above frame base; address = `lea +(param_offset)`.
- `SYM_ENUM_CONST`: the integer constant value; no memory address involved.
- `SYM_STATIC_LOCAL`: the `local_static_counter` ID; label = `_ls{offset}`.
- `SYM_STATIC_GLOBAL`, `SYM_EXTERN`, `SYM_BUILTIN`: addressed by label name.

`find_local_addr(node, name)` in `codegen.c` searches from `node->st` upward, returning `LocalAddr { int offset; bool is_param; }`. Returns `offset == -1` for globals and local statics (use label instead).

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

## Per-Translation-Unit Compilation (`smallcc.c` + `types.c` + `codegen.c` + `backend.c`)

The compiler supports true per-TU compilation. Each file is compiled independently with its own symbol table; the combined assembly is emitted as a single stream. The assembler resolves all label references, so cross-TU calls work without object files.

### Invocation

```
smallcc [-o outfile] [-stats] <source.c> [source2.c ...]
```

Assembly is written to `outfile` when `-o` is given; otherwise to stdout. `-stats` prints per-TU arena allocation sizes and total arena usage to stderr.

#### Standard Library Auto-Prepend

At startup, `smallcc.c` locates the directory containing the compiler binary (`get_compiler_dir(argv[0])`), then:

1. Calls `set_include_dir("$bindir/include")` so `#include <>` headers resolve correctly.
2. Scans `$bindir/lib/*.c` alphabetically (`scan_lib_files()`). Each `.c` found there is compiled as an implicit TU **before** the user's files.

This means `lib/stdio.c`, `lib/stdlib.c`, and `lib/string.c` are always compiled first (TUs 0–2). Their global symbols are propagated to later TUs via the `SYM_EXTERN` carry-forward mechanism in `reset_types_state()`. User code can call `printf`, `strlen`, etc. without any explicit `extern` declaration.

If `lib/` does not exist next to the binary, the scan silently returns 0 files and no lib TUs are added.

Each `.c` argument is one user translation unit, compiled in argument order after the lib TUs. The preamble is emitted once before the per-TU loop.

### Per-TU State Reset

At the start of each TU, four reset functions are called:

```c
reset_codegen()        // clears strlit_count, loop_depth, label_table_size
                       // does NOT reset 'labels' — monotonically increasing across TUs
reset_parser()         // clears current_function
                       // does NOT reset anon_index — monotonically increasing across TUs
reset_types_state()    // carries SYM_EXTERN symbols from previous TU's global scope into
                       // a fresh symbol_table / curr_scope_st / last_symbol_table;
                       // resets scope_depth and scope_id counter;
                       // does NOT touch the 'types' linked list or t_int/t_void singletons
                       // (Type* pointers from previous TUs remain valid)
reset_preprocessor()   // clears macro table; resets include-depth counter
```

After reset, `make_basic_types()` re-populates the basic type singletons (found in the existing `types` list and reused, not duplicated).

### Cross-TU Global Propagation

There is no separate `ExternSym[]` array. The mechanism is simpler:

**`harvest_globals()`** (called after `backend_emit_asm`): walks the just-compiled TU's global `symbol_table->symbols` and calls `insert_extern_sym()` for each `NS_IDENT` symbol that is not `SYM_STATIC_GLOBAL`, `SYM_STATIC_LOCAL`, `SYM_EXTERN`, or `SYM_BUILTIN`.

**`insert_extern_sym()`** (in `types.c`): marks the symbol `SYM_EXTERN` in the current global scope (or inserts a new `SYM_EXTERN` entry if not already present).

**`reset_types_state()`**: when creating the next TU's fresh symbol table, it walks the previous TU's global scope and copies all `SYM_EXTERN` entries into the new table. This carry-forward is the actual propagation mechanism — no separate data structure is needed.

The net effect: globals defined in TU N are visible as `SYM_EXTERN` symbols in TU N+1, N+2, …, without requiring the user to write `extern` declarations. This is how lib TUs make `printf`, `strlen`, etc. available to user code.

### Static Symbol Mangling

File-scope `static` symbols are TU-private. The label name is mangled from `foo` to `_s{tu_index}_foo` at three IR-append sites in `codegen.c`:

| Site | IR emitted |
|---|---|
| `gen_function` (function label) | `ir_append(IR_SYMLABEL, 0, "_s{tu_index}_{name}")` |
| `gen_callfunction` (direct call) | `ir_append(IR_JL, 0, "_s{tu_index}_{name}")` |
| `gen_varaddr_from_ident` (global address) | `ir_append(IR_IMM, 0, "_s{tu_index}_{name}")` |

`backend_emit_asm` in `backend.c` turns these IR nodes into assembly text. `kind = SYM_STATIC_GLOBAL` and `tu_index` are set on the `Symbol` by `insert_ident()` in `types.c` when `sclass == SC_STATIC` at global scope.

### Extern Declaration Handling

When `insert_ident()` sees `sclass == SC_EXTERN` at global scope it:
- Does **not** increment `st->size` (no data allocated).
- Sets `sym->kind = SYM_EXTERN`.

When a real definition arrives for the same name (in the same or a later TU), `insert_ident()` finds the existing entry and upgrades it: sets `kind` to `SYM_GLOBAL`, updates the type, and allocates data offset.

`gen_decl()` skips entire `extern` declarations (`sym->kind == SYM_EXTERN → return`) and bare function prototypes (`istype_function(sym->type) → continue`) so no spurious labels or data are emitted.

---

## Variadic Functions

The compiler supports C89-style variadic functions using `...` in the parameter list and the standard `<stdarg.h>` macros.

### Parsing and Type Representation

- The `...` token is recognized as `TK_ELLIPSIS` in the tokeniser
- `ND_PTYPE_LIST` has an `is_variadic` flag set when `...` appears in the parameter list
- Function types store `is_variadic` in `Type.u.fn.is_variadic`
- `va_list` is pre-registered by the compiler as a typedef for `int` (a pointer-sized type)
- `va_start`, `va_arg`, and `va_end` are built-in keywords handled by the parser (not macros)

### Code Generation

**va_start(ap, last_param)** (`codegen.c`):
- Calculates the offset immediately after the last named parameter
- Emits: `ap = bp + last_param.offset + last_param.size`
- This points `ap` to the first variadic argument on the stack

**va_arg(ap, T)** (`codegen.c`):
1. Saves the current `ap` value (old pointer)
2. Advances `ap` by `sizeof(T)` bytes
3. Loads the value from `*old_ap` with proper type size and sign extension
- Works for `int`, `long`, `float`, `double`, and pointer types
- **Not supported**: struct types (incompatible with struct-by-value ABI)

**va_end(ap)** (`codegen.c`):
- No-op — the compiler generates no code

### Stack Layout for Variadic Calls

Arguments are pushed right-to-left by the caller. Named parameters are accessed at fixed `bp+offset` locations. Variadic arguments are accessed through the `ap` pointer which iterates forward through the argument stack area.

```
bp+8+2*(n-1)   param n (first variadic, accessed via *ap)
    ...
bp+8           param 0 (last named, accessed via bp+offset)
bp+4           return address (lr)
bp+0           saved bp
```

### Usage Example

```c
#include <stdarg.h>

int sum(int count, ...) {
    va_list ap;
    va_start(ap, count);
    int total = 0;
    for (int i = 0; i < count; i++) {
        total += va_arg(ap, int);
    }
    va_end(ap);
    return total;
}
```

### Limitations

- `va_arg(ap, struct S)` is not supported. Struct arguments are passed via hidden-copy pointers, but `va_arg` reads raw bytes based on `sizeof(type)`. Pass a `struct S *` instead.
- Variadic functions cannot be inlined by the compiler (no inline assembly generation for varargs).

---

## Backend Files

After `gen_ir` and `peephole`, the pipeline splits by target architecture. All backend files
are independent of the parser and type system; they consume only `IRInst` lists and emit
assembly text.

### `codegen.c` — IR Generation

Walks the annotated AST and builds the flat `IRInst` (stack IR) list. Also runs
`mark_basic_blocks()` to insert `IR_BB_START` sentinels after IR construction. The IR is
identical for both CPU3 and CPU4 targets.

### `optimise.c` — Stack-IR Peephole

Rules 1–9: constant folding, dead branch elimination, `adj` merging, multiply strength-reduction,
and store/reload elimination. Runs on the stack IR before the target split. Rule 9 (store/reload
elim) is currently gated on CPU3 only. See `docs/backend.md` for rule details.

### `backend.c` — CPU3 Emitter

`backend_emit_asm(ir_head)` walks the stack IR list and writes CPU3 assembly text directly.
One IR node → one or a few assembly instructions. Also owns the `-ann` source-comment
infrastructure (`set_ann_source`, `emit_src_comment`).

### `braun.c` — Stack IR → IR3 (Braun SSA Construction, CPU4)

`braun_ssa(ir_head)` converts the stack IR to a 3-address `IR3Inst` list with fresh virtual
registers. Uses the Braun et al. (2013) SSA construction algorithm with
`readVariable`/`writeVariable`/`readVariableRecursive` to place phi nodes on demand. Models
the stack machine's expression stack as a virtual register stack, peeks ahead to collapse
`LEA+LOAD`/`LEA+STORE` pairs into bp-relative IR3 ops, and handles call-site
argument/temporary partitioning via `flush_for_call_n`. SSA promotion of scalar locals is
enabled for leaf functions unconditionally and for non-leaf functions with ≤8 promotable
variables.

### `ir3.c` / `ir3.h` — IR3 Infrastructure (CPU4)

Defines the `IR3Inst` struct and `IR3Op` enum. `build_cfg()` constructs a basic-block CFG
from the stack IR for the Braun SSA algorithm. `ir3_new_vreg()` allocates fresh virtual
register IDs.

### `irc.c` — Iterated Register Coalescing (CPU4)

`irc_regalloc(head)` implements Iterated Register Coalescing (Appel & George 1996). Runs
per function. Phases: liveness analysis → interference graph build → Simplify/Coalesce/
Freeze/SelectSpill loop → AssignColors → RewriteProgram (if spills), then repeats. Maps
virtual registers to physical r1–r7 (r0 reserved for ACCUM). At each call site, adds
interference edges from all live vregs to r1–r7, forcing call-spanning vregs to spill —
no separate call-spill pass needed. Post-coloring MOVs with same-colored operands are
eliminated. Frame expansion mirrors the linscan approach: ENTER imm is patched and
bp-relative offsets are shifted down.

### `risc_backend.c` — CPU4 Emitter

`risc_backend_emit(head)` walks the post-regalloc `IR3Inst` list and emits CPU4 assembly text.
Selects F2 bp-relative instructions when offsets are in range, falls back to F3b
register-relative otherwise. Expands `le`/`ge`/`les`/`ges`/`fle`/`fge` into 3-instruction
sequences (the CPU4 ISA omits these as direct encodings). Skips nodes marked dead by
the sign-extend peephole (`op < 0`). Also supports `-ann` source comments via the shared
`ann_lines[]` index.
