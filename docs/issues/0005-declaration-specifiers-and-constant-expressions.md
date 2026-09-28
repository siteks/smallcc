# 0005 — Specifier combinations default to int; constant expressions are five partial evaluators

Status: FIXED 2026-09-28. Specifiers are canonicalised before lookup (`int`
implied by short/long, `long double` is double); array sizes, enum values and
case labels are parsed as expressions and folded by `const_int` (`const.c`,
shared with issue 0004), with a compile error when not constant; a second
declaration of an identifier in the same block scope is an error. `long long`
is still `long` (C89 has no `long long`). Tests: `ops/const_expr_contexts.c`,
`errors/case_label_nonconst.c`.

Reproducers (all `// XFAIL: issue 0005`):

- `tests/cases/typedef/short_int_size.c` — `sizeof(short int)` is 4
- `tests/cases/ops/unsigned_short_int_cast.c` — `(unsigned short int)-1 != 65535`
- `tests/cases/array/size_shift_expr.c` — `int buf[1 << 4]` has `sizeof == 0`
- `tests/cases/braun/case_label_expr.c` — `case 1 + 2:` is a parse error
- `tests/cases/errors/duplicate_local.c` — `int f(int a) { int a; }` compiles

Root causes:

- `typespec_to_base` (`types.c:394-431`) has no rows for `DS_SHORT|DS_INT`,
  `DS_UNSIGNED|DS_SHORT|DS_INT`, `DS_LONG|DS_INT`, `DS_UNSIGNED|DS_LONG|DS_INT`,
  `DS_LONG|DS_DOUBLE`; `default:` returns `t_int`. `long long` collapses to `long`
  because the bitmask OR loses the second `long` (`parser.c:920`).
- Five constant-expression evaluators, none complete: `eval_const_expr`
  (`types.c:301-336`, no shifts/bitwise/ternary/casts, returns 0), case labels
  (`parser.c:1396-1423`), enum values (`parser.c:1195-1202`), the preprocessor's own
  (`preprocess.c:434-638`, legitimately separate), and the cast/negate peelers in
  `lower.c` and `braun.c:99-110`.
- `insert_local_ident` (`types.c:1178`) appends without a same-scope lookup.

Fix: canonicalise the specifier mask first (drop `DS_INT` when SHORT/LONG is present,
`LONG|DOUBLE -> DOUBLE`, count `long`s) and make the `default` a `src_error`; parse
case labels, enum values and array sizes with `cond_expr()` and fold them with the one
`const_eval` from issue 0004; look up before insert in `insert_local_ident`. No effect
on generated code for programs that compile today.
