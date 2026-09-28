# 0006 — Type derivation is wrong for shifts, pointer difference, ternary and compound assignment

Status: FIXED 2026-09-28. Shifts take the promoted left operand's type and
each operand is promoted alone; the constant folds in `braun.c` and `emit.c`
(P7) shift `IK_SHR` arithmetically. Pointer difference is `int`, the byte
difference shifted right (power-of-two element) or divided. `?:` converts both
branches to the common type (same-typed branches keep their type, which is
as-if). Compound assignment runs in the common type through
`cg_rmw_as` in `braun.c`, except for integer `+ - * & | ^ << >>`, whose low
bits do not depend on the width. Cast folds of constants use the target's
`itof`/`ftoi`. Found on the way and fixed: `+=`/`++` on an unsigned char or
short held in a register did not wrap (250 + 10 stayed 260); braun now wraps
the result where the operation can overflow, and `opt_narrow_wrap_range`
drops the wrap where a dominating branch bounds the operand (loop counters),
so CoreMark stays within 0.2%. Not done: `&&`/`||` still convert operands to a
common type (one wasted `itof` on a mixed condition, not a correctness bug).
Tests: `ops/operator_typing_rules.c`, `ops/narrow_unsigned_wrap.c`.

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
