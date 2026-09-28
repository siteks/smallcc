# 0009 — Scalar promotion promotes a location without proving it is private (always-on pass)

Status: FIXED 2026-09-28: every other load in the loop must be provably
disjoint from the promoted location (same base with non-overlapping offsets,
different globals, global vs frame slot, or C's aliasing rule: accesses of
different sizes, neither a byte, are different objects), and the store must
dominate the latch. CoreMark's matrix promotion (int accumulator, short
elements) is kept by the aliasing rule. Test: `opt/scalar_promote_alias_ptrs.c`.

Reproducer: `tests/cases/opt/scalar_promote_alias_load.c` (`// XFAIL: issue 0009`).
`*p += 1; s += g;` in a loop with `p == &g` returns 0 instead of 15, at every `-O`
level (the pass is not bitmask-gated).

Root cause: `opt_scalar_promote` (`opt.c:2875-3045`) matches one `load; ...; store`
pair on the same SSA address (`opt.c:2944-2951`) and checks only that there are no
*other stores* in the loop (`opt.c:2961-2971`). Other *loads* of the same memory are
ignored, and a second `IK_GADDR` of the same global in another block is a different
`Value*` (pre-OOS GVN refuses to merge zero-operand instructions, `opt.c:573-580`),
so the loop reads stale memory while the promoted copy lives in a register.

Fix: reject the loop if any other `IK_LOAD`/`IK_STORE` in the body is not provably
disjoint (same base `Value` or same `IK_GADDR` name with non-overlapping `(imm, size)`);
treat everything else as may-alias. Also require `dominates(store->block, latch)` for
every back-edge predecessor before feeding the accumulator phi (`opt.c:2988-2991`);
that second hole was reported but not reproduced. Removes only promotions that were
wrong.
