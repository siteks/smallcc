# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

```bash
make mycc           # Build the compiler
make test_all       # Run all test suites
make test_struct    # Run a specific test suite (also: test_init, test_ops, test_logops, test_func, test_longs, test_array, test_loops)
make clean          # Remove binaries and temp files
```

To run the compiler directly (input is a C source string, output is assembly to stdout):
```bash
./mycc "int main(){return 5+3;}"
```

To run the full pipeline (compiler + simulator):
```bash
./mycc "int main(){return 5+3;}" > tmp.s && ./cpu3/sim.py tmp.s
```

Tests use `cpu3/sim.py` to execute generated assembly and check the value left in register `r0`. On test failure, verbose output is written to `error.log` using `cpu2/sim.py`. The test harness is in `test.sh`; individual suites are in `tests/`.

---

## Compiler Architecture

This is a single-pass C89 subset compiler. Source code is taken as a command-line string argument and assembly is written to stdout.

### Compilation Pipeline

```
./mycc "C code"
  → tokenise()              [tokeniser.c]   Token linked list
  → program()               [parser.c]      AST (Node tree)
  → make_basic_types()      [types.c]       Populate global type table
  → add_types_and_symbols() [types.c]       Build symbol table tree + assign types
  → propagate_types()       [parser.c]      Annotate AST nodes with resolved Type2*
  → gen_code()              [codegen.c]     Emit assembly to stdout
```

Orchestrated by `mycc.c`. Every phase prints debug information to stderr (token list, AST before/after type propagation, symbol table, type table). The fixed preamble (`ssp 0x1000 / jl main / halt`) is printed before `gen_code` is called.

### Source Files

| File | Role |
|---|---|
| `mycc.h` | All shared structs, enums, and function prototypes |
| `mycc.c` | Entry point; orchestrates the pipeline |
| `tokeniser.c` | Lexer — produces a `Token` linked list |
| `parser.c` | Recursive-descent parser — builds AST; also contains `propagate_types` and `check_operands` |
| `types.c` | Type table, symbol table, struct layout, `add_types_and_symbols`, `generate_struct_type2` |
| `codegen.c` | AST walk; emits assembly pseudoinstructions |

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
    long long   ival;   // parsed integer value
    int         loc;    // character position in source
};
```

### Token Kinds

- **Delimiters**: `TK_LPAREN ( ) RPAREN LBRACE RBRACE LBRACKET RBRACKET COMMA SEMICOLON COLON DOT ARROW`
- **Operators** (2-char first, then 1-char): `== != >= <= ++ -- && || >> <<  ->` then `= + - * / & ~ ! | ^ < >`
- **Literals**: `TK_CONSTINT`, `TK_CONSTFLT`, `TK_CHARACTER`, `TK_IDENT`
- **Keywords** (32): `auto break case char const continue default do double else enum extern float for goto if int long register return short signed sizeof static struct switch typedef union unsigned void volatile while`
- `TK_EOF` — end of input

### Lexing Rules

- Whitespace is skipped.
- Two-character tokens are checked before single-character tokens.
- Identifiers/keywords: `[a-zA-Z_][a-zA-Z0-9_]*`, then matched against `keywords[]` table.
- Integer constants: `strtol()` with optional `u/U`, `l/L`, or `ul/UL` suffix; hex (`0x…`) recognised.
- Float constants: `strtod()` with optional `f/F` suffix.
- Character constants: `'x'` with escape sequences `\a \b \f \n \r \t \v \\ \? \' \" \xhh \ooo`.

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

init-declarator     → declarator ("=" initializer)?
initializer         → assign-expr | "{" initializer-list "}"

statement           → expr-stmt | compound-stmt | if-stmt | while-stmt | for-stmt
                    | do-stmt | switch-stmt | break-stmt | continue-stmt | return-stmt
compound-stmt       → "{" declaration* statement* "}"
if-stmt             → "if" "(" expr ")" stmt ("else" stmt)?
while-stmt          → "while" "(" expr ")" stmt
for-stmt            → "for" "(" expr? ";" expr? ";" expr? ")" stmt
do-stmt             → "do" stmt "while" "(" expr ")" ";"
switch-stmt         → "switch" "(" expr ")" compound-stmt
case-label          → "case" integer-constant ":"    (parsed as stmt inside compound)
default-label       → "default" ":"                  (parsed as stmt inside compound)
break-stmt          → "break" ";"
continue-stmt       → "continue" ";"
return-stmt         → "return" expr? ";"

expr                → assign-expr ("," assign-expr)*
assign-expr         → unary-expr "=" assign-expr | logor-expr
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
primary-expr        → ident | constant | "(" expr ")"
```

**Not yet implemented:** `goto`, `sizeof`, `typedef`, `enum` bodies, `register`/`extern`/`static` semantics, string literals.

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
ND_MEMBER        // "." member access — children: [struct_expr, field_ident]
ND_COMPSTMT      // "{}" block — children: declarations and statements
ND_EXPRSTMT      // expression statement
ND_IFSTMT        // if — children: [cond, then] or [cond, then, else]
ND_WHILESTMT     // while — children: [cond, body]
ND_FORSTMT       // for — children: [init, cond, inc, body] (ND_EMPTY for absent parts)
ND_DOWHILESTMT   // do-while — children: [body, cond]
ND_SWITCHSTMT    // switch — children: [selector, body_compstmt]
ND_CASESTMT      // case N: — ival holds the constant; no children
ND_DEFAULTSTMT   // default: — no children
ND_BREAKSTMT     // break; — no children
ND_CONTINUESTMT  // continue; — no children
ND_EMPTY         // absent for-loop part (init/cond/inc omitted)
ND_RETURNSTMT    // return — children: [expr] or empty
ND_STMT          // generic wrapper
ND_ASSIGN        // "=" — children: [lhs, rhs]
ND_BINOP         // binary op (val = "+", "-", "*", "/", "==", etc.)
ND_UNARYOP       // unary op (val = "+", "-", "*") — child: [operand]
ND_CAST          // explicit cast — children: [ND_DECLARATION type placeholder, expr]
ND_IDENT         // variable or function reference (val = name)
ND_LITERAL       // integer or float constant
ND_INITLIST      // initializer list "{…}"
ND_CONSTEXPR     // constant expression
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
    Node       **children;
    int          child_count;
    int          offset;        // struct member byte offset (ND_MEMBER)
    int          struct_depth;  // nesting depth for struct definitions
    bool         is_expr;       // node participates in type propagation
    bool         is_func_defn;  // true for function definitions (not just decls)
    bool         is_array_deref;// true for array subscript UNARYOP "+" (no load)
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

- **Not the last dimension** (e.g., `a[i]` where `a` is `int[3][4]`): creates `ND_UNARYOP "+"` with `is_array_deref = true` and `node->type` set to the element-at-that-depth type. No load is generated; the address is passed through.
- **Last dimension** (e.g., `a[i]` where `a` is `int[4]`, or `a[i][j]` for the `j` part): creates `ND_UNARYOP "*"` with `node->type` set to the leaf element type. A load is generated.

The child is always `ND_BINOP "+"` with children `[base_addr, ND_BINOP "*" [stride_constant, index_expr]]`.

### Type Propagation (`propagate_types`)

Post-order (children first) tree walk. For each node:

- `ND_IDENT` (not struct member): `node->type = find_symbol(…)->type`
- `ND_MEMBER`: looks up field in `lhs->type->u.composite.members`, sets `node->offset` and `node->type`
- `ND_BINOP` (is_expr): calls `check_operands()` → inserts arithmetic-conversion casts, returns lhs type
- `ND_UNARYOP` (is_expr): calls `check_unary_operand()` → for `*` returns `elem_type(child->type)`; preserves parser-set types (guards with `if (node->type == t_void)`)
- `ND_RETURNSTMT`: inserts a cast if return type differs from expression type

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

**Note:** The target has a 16-bit address space. `int` and pointers are 2 bytes.

### Factory Functions

```c
Type2 *get_basic_type(Type2_base base);            // returns cached singleton
Type2 *get_pointer_type(Type2 *pointee);           // dedup by pointee pointer
Type2 *get_array_type(Type2 *elem, int count);     // size = elem->size * count
Type2 *get_function_type(Type2 *ret, Param *params, bool is_variadic);
Type2 *get_struct_type(Symbol *tag, Field *members, bool is_union);
```

### Type Derivation

`tstr_compact(Node *declarator)` returns a derivation string from a declarator node:
- `""` — plain scalar
- `"*"` — pointer
- `"[N]"` — array of N elements
- `"[N][M]"` — 2-D array (outermost first)
- `"()"` — function

`apply_derivation(Type2 *base, const char *ts)` builds the Type2 chain left-to-right (outermost first), so `"[3][4]"` → `array(3, array(4, base))`.

`type2_from_ts(Node *node, char *ts)` is the public entry point used by the parser.

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
    int             size;           // bytes of locals in this scope
    int             global_offset;  // accumulated offset from parent scopes (for nested locals)
    int             child_count;
    Symbol_table  **children;
    Symbol_table   *parent;
};

struct Symbol {
    char   *name;
    Type2  *type;
    int     offset;     // see addressing below
    bool    is_param;
    Symbol *next;
};
```

**Scope addressing:** A scope is identified by `(depth, indices[])`. E.g., depth=2, indices=[3,1] means: the 1st child of the 3rd child of the root. Scopes are created by `enter_new_scope()` and released by `leave_scope()`.

**Symbol offsets:**
- **Global** (`depth == 0`): `offset` is the byte position in the global data area (used as a label name in assembly).
- **Local** (`is_param == false`): `offset` is the accumulated size of locals declared before this one in the current (and enclosing) scopes. Used as `lea -(global_offset + offset)`.
- **Parameter** (`is_param == true`): `offset` starts at 8 (above saved lr and bp) and increases for each parameter. Used as `lea (global_offset + offset)`.

`find_local_addr(node, name)` searches from the node's scope upward, returning `global_offset + symbol->offset`, with bit 30 set for parameters.

### Key Helper Functions

```c
Type2 *elem_type(Type2 *t)          // strip one array/pointer layer; return self for scalars
Type2 *array_elem_type(Type2 *t)    // walk to innermost element type (all array layers)
int    array_dimensions(Type2 *t)   // count array nesting depth
int    find_offset(Type2 *t, char *field, Type2 **out_type)  // field byte offset, -1 if not found
Symbol *find_symbol(Node *node, char *name, Namespace ns)    // search up scope tree

bool istype_int(Type2 *t)      // t->base == TB2_INT
bool istype_ptr(Type2 *t)      // t->base == TB2_POINTER
bool istype_array(Type2 *t)    // t->base == TB2_ARRAY
bool istype_intlike(Type2 *t)  // any integer or pointer type (for pointer arithmetic)
// … etc.
```

---

## Code Generator (`codegen.c`)

Walks the annotated AST and prints assembly instructions to stdout.

### Stack Frame Layout

```
  bp+8 + 4*(n-1)   param n   (last param)
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
- **Global**: `immw label_name` (symbolic address)
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

**Pointer/array dereference** (`ND_UNARYOP "*"`):
```asm
; gen_expr(child) → r0 = address to load from
lb/lw/ll            ; r0 = mem[r0]  (size from elem_type(node->type)->size)
```

**Struct member access** (`ND_MEMBER`):
```asm
; gen_addr(struct_expr) → r0 = struct base address
push
immw member_offset
add
lb/lw/ll            ; load member value
```

**Function call** (`ND_IDENT` + `ND_FUNC_DECL`):
```asm
; push arguments right-to-left (last arg first)
; gen_expr(arg[n-1]) → push
; ...
; gen_expr(arg[0]) → pushw/push
jl    func_name
adj   param_total_bytes   ; caller cleans up parameters
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

### Global Variable Initialization

**Scalars**: emits `word value` (or `long` for 4-byte types) in the data section.

**Arrays** (global): `gen_mem_inits()` recursively fills a `char data[]` byte buffer, then `gen_bytes()` emits one `byte` directive per byte.

**Arrays** (local): `gen_fill(vaddr, size)` zeroes all bytes, then `gen_inits()` recursively emits `lea; push; immw; sw/sl` sequences for non-zero initializers.

Struct initializers are not yet supported.

### Two-Pass Code Generation

`gen_code()` makes two passes over the top-level declaration list:

1. **Pass 1**: emit function definitions (`.text` section — though no explicit section directive is emitted; functions follow the preamble inline).
2. **Pass 2**: emit global variable declarations (implicitly in a data area after the code).

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

**Stack-binary convention**: all `add`, `sub`, `mul`, `div`, `mod`, `shl`, `shr`, comparisons, and bitwise ops pop their left operand from the stack (`mem32[sp]`) and use r0 as right operand, writing the result to r0 and incrementing sp by 4.

**Store convention**: `sb`, `sw`, `sl` pop a 32-bit address from the stack and write r0 to that address.

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

**Function call**: `jl` saves the return address in `lr`, then jumps. `enter` saves `lr` and `bp` on the stack and sets `bp` for the new frame. On return, `ret` restores the frame.

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
    word  v1 v2 ...    ; emit one 16-bit word per value
    long  v1 v2 ...    ; emit one 32-bit long per value
    allocb N           ; reserve N zero bytes
    allocw N           ; reserve N zero words (word-aligned)
    align              ; align to next word boundary
```

Comments start with `;`. Labels and instruction names are case-sensitive.

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

### Statements

| Feature | Status | Notes |
|---|---|---|
| `if` / `if-else` | ✅ | |
| `while` | ✅ | |
| `do-while` | ✅ | |
| `for` | ✅ | init/cond/inc all optional (ND_EMPTY for absent parts) |
| `switch` / `case` / `default` | ✅ | fall-through supported; selector re-evaluated per case |
| `break` | ✅ | works inside `while`, `for`, `do-while`, `switch` |
| `continue` | ✅ | works inside `while`, `for`, `do-while` |
| `goto` | ❌ | Token exists; no parser or codegen |
| Labeled statements (`lbl:`) | ❌ | |
| `return` | ✅ | Auto-casts to declared return type |
| Compound statement `{ }` | ✅ | |

### Expressions and Operators

| Feature | Status | Notes |
|---|---|---|
| Arithmetic `+ - * /` | ✅ | |
| Modulo `%` | ❌ | Not tokenised, parsed, or in codegen |
| Compound assignment `+= -= *= /= %= &= \|= ^= <<= >>=` | ❌ | Not tokenised or parsed |
| Ternary `? :` | ❌ | Not tokenised or parsed |
| Comma operator `,` (in expressions) | ❌ | Comma is only parsed as a separator |
| `sizeof` | ❌ | `TK_SIZEOF` tokenised; not parsed |
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
| Pointer member `->` | ❌ | `TK_ARROW` tokenised; not parsed |
| Function call | ✅ | Including recursion |
| Assignment `=` | ✅ | |

### Types

| Feature | Status | Notes |
|---|---|---|
| `char` / `unsigned char` | ✅ | 1 byte |
| `short` / `unsigned short` | ✅ | 2 bytes |
| `int` / `unsigned int` | ✅ | 2 bytes on target |
| `long` / `unsigned long` | ✅ | 4 bytes |
| `float` | ⚠️ | Parsed and typed; codegen emits integer ops (no FP arithmetic) |
| `double` | ⚠️ | Same as float |
| `void` | ✅ | |
| Pointers | ✅ | Including pointer arithmetic |
| Arrays (1-D and N-D) | ✅ | |
| `struct` | ✅ | Including nested structs and correct member offsets |
| `union` | ⚠️ | Parsed; member offsets set to 0, but member access codegen not distinct from struct |
| `enum` | ❌ | Keyword tokenised; body not parsed; constants not supported |
| `typedef` | ❌ | Keyword tokenised; not functional |
| Function pointers | ⚠️ | Can be declared; calling through a function-pointer variable not supported |
| Bit fields in structs | ❌ | |
| `const` / `volatile` qualifiers | ⚠️ | Parsed and stored; semantics not enforced |

### Declarations and Linkage

| Feature | Status | Notes |
|---|---|---|
| Multiple declarators (`int a, b;`) | ✅ | |
| Scalar initializer (`int x = 5;`) | ✅ | |
| Array initializer (`int a[] = {1,2,3};`) | ✅ | Including multi-dim |
| Struct/union initializer | ❌ | |
| `auto` | ⚠️ | Parsed; no semantic difference from default local |
| `register` | ⚠️ | Parsed; ignored (no register allocator) |
| `static` | ⚠️ | Parsed; internal-linkage and persistent-storage semantics not enforced |
| `extern` | ⚠️ | Parsed; external-linkage semantics not enforced |
| Forward declarations | ⚠️ | Functions can be declared before definition, but semantic checking is minimal |
| K&R (old-style) function definitions | N/A | Deliberately not supported; ANSI prototype style only |

### Literals and Constants

| Feature | Status | Notes |
|---|---|---|
| Decimal integer constants | ✅ | |
| Hex constants (`0x…`) | ✅ | |
| Octal constants (`0…`) | ✅ | |
| Integer suffixes `u/U`, `l/L`, `ul/UL` | ✅ | |
| Floating-point constants | ✅ | Parsed; no FP codegen |
| Character constants (`'a'`, escape sequences) | ✅ | |
| String literals (`"…"`) | ❌ | Not tokenised or parsed |

### Functions and Calling Convention

| Feature | Status | Notes |
|---|---|---|
| Function definitions with prototypes | ✅ | |
| Parameter passing (value) | ✅ | |
| Return values | ✅ | |
| Recursion | ✅ | |
| Variadic functions (`...`) | ❌ | `is_variadic` flag exists in type; no `va_list` / `va_start` / `va_arg` / `va_end` |
| Implicit `int` return type | ❌ | Return type must be stated explicitly |
| Implicit function declaration | ❌ | Callee must be declared before call |

### Type Conversion and Promotion

| Feature | Status | Notes |
|---|---|---|
| Integer promotions (char/short → int in expressions) | ⚠️ | Partial; not uniformly applied |
| Usual arithmetic conversions (mixed-type operands) | ⚠️ | Basic widening casts emitted; full C89 hierarchy not guaranteed |
| Pointer ↔ integer conversions | ✅ | Used in tests (e.g. `a = 0x2000`) |
| Floating-point conversions | ❌ | No FP arithmetic in codegen |

### Summary

**Implemented and working**: basic scalar types, pointers, 1-D/N-D arrays, structs, `if`/`while`/`for`/`do-while`, `switch`/`case`/`default`, `break`, `continue`, all arithmetic and bitwise operators except `%`, all comparison and logical operators, pre/post increment, address-of, dereference, struct member access (`.`), explicit casts, array subscripting, function definitions and calls, integer constants (decimal/hex/octal).

**Partially working**: unions (layout correct, but not semantically distinct from struct in codegen), `const`/`volatile` (stored, not enforced), storage classes (parsed, not enforced), `float`/`double` (types only, no FP arithmetic).

**Not yet implemented**: `goto`, labels, `%`, all compound assignments (`+=` etc.), ternary `?:`, `sizeof`, `->`, `enum`, `typedef`, string literals, struct/union initializers, variadic functions, bit fields.

**Extensions beyond C89**: `//` line comments.
