# 0004 — Global initialisers that are not a bare literal are silently zero-filled

Status: FIXED 2026-09-28. `const.c` is the one constant evaluator (`const_eval`:
integer, float with the target's arithmetic, address constants), and
`lower_static_data` in `lower.c` lays out every static initialiser by walking
the Type (nested arrays and structs, brace elision, unions, char arrays with
padding). Static locals use the same routine (previously a struct or pointer
initialiser on a static local was also zero-filled). A non-constant initialiser
is a compile error. Remaining limit: an address plus an offset (`&a[2]`,
`&s.f` past offset 0) is a compile error, because the assemblers' data
directives take a bare label. Tests: `init/global_nested_aggregates.c`,
`init/static_local_aggregates.c`, `errors/global_nonconst_init.c`.

Reproducers (all `// XFAIL: issue 0004`):

- `tests/cases/init/global_const_expr.c` — `int g = 1 + 2;` reads back 0
- `tests/cases/init/global_addr_of.c` — `int *p = &x;` at top level is 0
- `tests/cases/init/global_enum_const.c` — `int g = E;` is 0

Root cause: `lower_global` (`lower.c:78-267`) understands a literal, a string, and a
one-level init list, and returns zero bytes for anything else without a diagnostic.
`&` is handled only inside an init list (`lower.c:179-184, 234-240`), flattening is one
level deep (`lower.c:223-231`, so `int m[2][2][2] = {...}` is also wrong), and the
nested-struct path (`lower.c:154-178`) loses the innermost field.
`docs/c89-status.md` claims these work.

Fix: one `const_eval(Node*)` returning INT / FLOAT / SYMREF(label, offset) / STRLIT
(shared with issue 0005), and a layout walk that recurses on the **Type** (struct
fields, array elements) rather than on the shape of the init list. Any initialiser
`const_eval` cannot evaluate is a compile error, never a zero. No effect on generated
code (data section only).
