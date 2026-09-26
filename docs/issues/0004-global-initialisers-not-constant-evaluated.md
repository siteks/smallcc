# 0004 — Global initialisers that are not a bare literal are silently zero-filled

Status: OPEN (found 2026-09-26 by the compiler review, `docs/review-2026-09.md`; reproduced on `sim_c` and, where marked, at `-O0` as well).

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
