# 0006 — Type derivation is wrong for shifts, pointer difference, ternary and compound assignment

Status: OPEN (found 2026-09-26 by the compiler review, `docs/review-2026-09.md`; reproduced on `sim_c` and, where marked, at `-O0` as well).

Reproducers (all `// XFAIL: issue 0006`):

- `tests/cases/ops/ptr_diff_scaled.c` — `p - q` for `int *` three elements apart is 12
- `tests/cases/ops/shr_by_unsigned_count.c` — `-8 >> 1u` is `0x7ffffffc`
- `tests/cases/ops/compound_assign_float_rhs.c` — `int i = 2; i *= 1.5f;` leaves 2
- `tests/cases/ops/ternary_mixed_branch_types.c` — `c ? 1 : 2.5f` with `c == 0` is not 2.5f

Root causes, all in `parser.c`:

- Shifts go through `usual_arith_type` (`parser.c:1831-1841, 1750-1770`); C says the
  result type is the promoted left operand only. The R2C fold in `braun.c:850` and the
  emit-time P7 fold (`emit.c:427`) also shift `IK_SHR` logically, so the constant case
  is wrong at every `-O` level.
- `ptr - ptr` is typed as the pointer (`parser.c:1838-1839`) and never divided by the
  element size (`parser.c:1980-1983`).
- The ternary takes the then-branch type (`parser.c:1893-1894`) and
  `insert_coercions_step` never converts the branches.
- Compound assignment casts the RHS to the LHS type before the operation
  (`parser.c:1995-2003`); C computes in the common type and converts the result.
- Also observed, not a correctness bug: `&&`/`||` convert their operands to a common
  type (`itof` on an int operand when the other is float) instead of testing each
  against zero in its own type; one wasted instruction per mixed condition.

Fix: per-operator rules — shift: `promote(lhs)`; pointer difference: `t_int` and an
`IK_DIV` (or shift) by the element size; ternary: `usual_arith_type` of both branches
and a cast on each; compound assignment: cast LHS up, operate, cast back (co-change
with `ND_COMPOUND_ASSIGN` in `braun.c`); logical operators: no coercion. Move
`fold_binop` into a shared `fold.c` and make P7 call it, with `a >> (ub & 31)`
arithmetic for `IK_SHR`.
