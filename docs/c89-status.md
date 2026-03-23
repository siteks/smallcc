# C89 Compliance Status

Preprocessor excluded. Features are assessed against ANSI C89/ISO C90.

## Deliberate Deviations from C89

- **K&R (old-style) function definitions are not supported** — only ANSI prototype-style definitions are accepted.
- **`//` line comments are supported** — a C99/C++ extension not present in C89.
- **Implicit `int` return type is not allowed** — every function must explicitly state its return type.
- **Implicit function declarations are not allowed** — a function must be declared or defined before it is called.
- **Bit fields in structs are not supported** — the syntax `type name : width;` is not parsed. Bit fields are rarely used in practice and add significant complexity (platform-specific packing, masking, alignment) for minimal benefit on this target.
- **Declarations may appear anywhere in a block** — C99 extension. In C89, all declarations within a `{}` block must precede all statements. This compiler accepts declarations interleaved with statements (e.g. `int a=1; a++; int b=a*2;`). Note: all locals are still allocated upfront at scope entry (single `adj -N`), so a variable's storage exists from the start of the block even if its declaration appears later in the source.
- **`for`-init declarations** — C99 extension. `for (int i = 0; i < n; i++)` is supported. The loop variable is scoped to the for statement (condition, increment, and body). Implemented by creating an implicit hidden scope for the init declaration; `gen_forstmt` wraps the loop in `adj -N`/`adj +N`.

## Statements

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

## Expressions and Operators

| Feature | Status | Notes |
|---|---|---|
| Arithmetic `+ - * /` | ✅ | |
| Modulo `%` | ✅ | |
| Compound assignment `+= -= *= /= %= &= \|= ^= <<= >>=` | ✅ | `ND_COMPOUND_ASSIGN` node in codegen (not desugared); avoids double-evaluation of LHS (e.g. `a[i++] += 1`) |
| Ternary `? :` | ✅ | ND_TERNARY node; right-associative via `cond_expr()` |
| Comma operator `,` (in expressions) | ✅ | `expr()` loops on commas; function args use `assign_expr()` |
| `sizeof` | ⚠️ | `sizeof(type-name)` and `sizeof(ident)` work, including `sizeof(struct tag)` and `sizeof(enum tag)`; `sizeof` applied to any other expression (e.g. `sizeof(*p)`, `sizeof(a[0])`, `sizeof(s.x)`, `sizeof(a+b)`) is not supported |
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

## Types

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
| `struct` | ✅ | Including nested structs, correct member offsets, assignment (`s1 = s2`), and return by value (hidden-pointer ABI) |
| `union` | ✅ | All members at offset 0; `is_union` flag in Type sets size = max member size |
| `enum` | ✅ | Tagged and anonymous enums; implicit/explicit/negative values; enum variables; sizeof(enum); case labels with enum constants |
| `typedef` | ✅ | Scalar, pointer, struct/union aliases; lexical scoping with shadowing |
| Function pointers | ✅ | Declare, assign, call via `fp(args)`, `(*fp)(args)`, `arr[i](args)`, `s.fp(args)`, `s->fp(args)`; pass as arguments; use as parameters (`int (*f)(int)`) |
| Bit fields in structs | N/A | Deliberately not supported — see deviations |
| `const` / `volatile` qualifiers | ⚠️ | Parsed and stored; semantics not enforced |

## Declarations and Linkage

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
| `static` (local) | ✅ | Persistent storage in data section; label `_ls{id}`; compile-time-constant initializers only (no function calls, no other variables — violations produce wrong code silently rather than an error) |
| `extern` | ✅ | Suppresses data allocation; real definition upgrades the symbol; cross-TU globals pre-populated automatically |
| Forward declarations | ✅ | Functions declared before their definition resolve correctly via label references in assembly |
| K&R (old-style) function definitions | N/A | Deliberately not supported; ANSI prototype style only |

## Literals and Constants

| Feature | Status | Notes |
|---|---|---|
| Decimal integer constants | ✅ | |
| Hex constants (`0x…`) | ✅ | |
| Octal constants (`0…`) | ✅ | |
| Integer suffixes `u/U`, `l/L`, `ul/UL` | ✅ | |
| Floating-point constants | ✅ | Parsed and emitted as IEEE 754 via `immw`/`immwh` pair |
| Character constants (`'a'`, escape sequences) | ✅ | |
| String literals (`"…"`) | ✅ | `TK_STRING` token; escape decoding; adjacent concatenation; `char*` type; deferred data emission in codegen pass 3; `char s[]`/`char s[N]` init; assembler extended to resolve labels in `word` directives; global arrays of string-literal pointers (including cast wrappers like `(unsigned char*)"str"`) emit `word label` per element |

## Functions and Calling Convention

| Feature | Status | Notes |
|---|---|---|
| Function definitions with prototypes | ✅ | `int f(void)` (no params) and `int f()` (treated as no params) both accepted; the distinction between "zero params" and "unspecified params" is not enforced |
| Parameter passing (value) — scalars | ✅ | |
| Parameter passing (value) — structs | ✅ | Hidden-copy ABI: caller allocates a copy buffer, copies the struct, passes a pointer; callee accesses transparently. `va_arg(ap, struct P)` not supported — see variadic row. |
| Return values | ✅ | |
| Recursion | ✅ | |
| Variadic functions (`...`) | ✅ | `va_list` (typedef'd to `int`), `va_start`, `va_arg`, `va_end`; `putchar` CPU builtin (opcode 0x1e) |
| `va_arg` with struct type | ❌ | `va_arg(ap, struct P)` is not supported; the `va_arg` implementation reads a fixed-size slot sized by `sizeof(type)` but struct args are passed as hidden pointers under the struct-by-value ABI, so the two mechanisms are incompatible. Pass a `struct P *` and dereference instead. |

## Type Conversion and Promotion

| Feature | Status | Notes |
|---|---|---|
| Integer promotions (char/short → int in expressions) | ✅ | Applied in `check_operands` and `check_unary_operand` for all binary and most unary ops |
| Usual arithmetic conversions (mixed-type operands) | ✅ | Full C89 rank hierarchy: float > ulong > long > uint > int |
| Pointer ↔ integer conversions | ✅ | Used in tests (e.g. `a = 0x2000`) |
| Floating-point conversions | ✅ | `itof`/`ftoi` opcodes; int→float sign-extends first |
| Function-to-pointer decay | ✅ | Function designators used as values or passed as arguments decay to pointer-sized (2 bytes) |

### Target-Dependent Arithmetic Notes (revisit if porting to a wider target)

These behaviors are correct for the current 16-bit target (`sizeof(int) == sizeof(short) == 2`) but would need changes for a 32-bit target (`sizeof(int) == 4`, `sizeof(short) == 2`):

- **`unsigned short` promotes to `unsigned int`** — on this target `int` is 2 bytes and cannot represent unsigned short values above 32767, so C89 §3.2.1.1 requires promotion to `unsigned int`. On a 32-bit target `int` can represent all `unsigned short` values, so the promotion would be to `int` instead of `unsigned int`. Both are size-2 here, so no code is generated either way.
- **`short → int` is a no-op** — both are 2 bytes on this target; no code is generated. On a 32-bit target this would emit `sxw` (sign-extend to 32 bits).
- **`unsigned char → int` is a no-op** — zero-extension from 8 to 16 bits is assumed already done; on a 32-bit target it would zero-extend to 32 bits.
- **Function argument slot widening is applied** — `push_args_list` compares each argument's natural slot size against the declared parameter's slot size. When widening is needed (e.g. `int` arg to `unsigned long` param), the caller sign-extends (signed types) or zero-extends before pushing. When narrowing is needed (e.g. `long` arg to `int` param), the caller truncates with a mask. This ensures the callee's load matches the pushed slot size.
- **`int` vs `unsigned int` arithmetic** — both are 2 bytes; only sign-interpretation differs (comparisons, right-shift, division). Mixed `int`/`uint` arithmetic converts to `uint` per the conversions, but the bit patterns are identical for `+`, `-`, `*`.

## Summary

**Implemented and working**: basic scalar types, pointers, 1-D/N-D arrays, structs (including assignment, pass by value, return by value via hidden-pointer ABI, and global initialization with pointer-to-global fields), unions, `float`/`double` (IEEE 754 arithmetic, comparisons, int↔float casts), `if`/`while`/`for`/`do-while`, `switch`/`case`/`default`, `break`, `continue`, `goto`, labeled statements, all arithmetic and bitwise operators including `%`, all comparison and logical operators, unary `+ - ~ ! &` and dereference `*`, pre/post-increment/decrement, struct/union member access (`.` and `->`), explicit casts, array subscripting, function definitions and calls (multi-arg, recursive, argument slot widening/narrowing at call sites), integer constants (decimal/hex/octal), floating-point constants, string literals, compound assignment (`+=` `-=` `*=` `/=` `%=` `&=` `|=` `^=` `<<=` `>>=`), ternary `?:`, comma operator, `sizeof(type)`/`sizeof(ident)`/`sizeof(struct tag)`/`sizeof(enum tag)`, `typedef` (scalar/pointer/struct aliases with lexical scoping), `enum` (tagged/anonymous, implicit/explicit/negative values, enum variables, enum constants in expressions and case labels), variadic functions (`va_list`/`va_start`/`va_arg`/`va_end`), function pointers (declare, assign, call via `fp(args)`, `(*fp)(args)`, `arr[i](args)`, `s.fp(args)`, `s->fp(args)`, pass as arguments and parameters), per-TU compilation with `static` internal linkage and cross-TU `extern` resolution, `static` local variables (persistent storage in data section, compile-time-constant initializers, independent between functions), global arrays of pointer-to-string-literal (e.g. `char *names[] = {"a", "b"}` and with casts like `(unsigned char *)"str"`).

**Partially working**: `const`/`volatile` (stored, not enforced), `auto`/`register` (parsed, ignored).

**Not yet implemented**: bit fields (deliberate), `sizeof` on non-trivial expressions, predefined macros (`__FILE__`, `__LINE__`, `__DATE__`, `__TIME__`, `__STDC__`), `#error` directive (silently ignored instead of halting compilation), `va_arg` with struct type (use `struct P *` instead).

**Extensions beyond C89**: `//` line comments; declarations anywhere in a block (C99); `for`-init declarations (C99).
