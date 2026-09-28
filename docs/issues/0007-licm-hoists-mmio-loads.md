# 0007 — LICM hoists loads out of store-free loops regardless of address; volatile is not implemented

Status: FIXED 2026-09-28 (the wrong code). `volatile` is implemented: the
parser keeps const/volatile on types (`get_qualified_type`), braun marks every
load and store through a volatile lvalue (`Inst.is_volatile`: a volatile
object, a member of a volatile struct, a pointee of a pointer to volatile),
a volatile local lives in its frame slot, and LICM, load CSE, load narrowing,
scalar promotion, IRC's dead-code pass and legalize Pass H leave marked
accesses alone. Every MMIO access in `sw/` is already through `volatile`.
Still open, as an optimisation rather than a correctness item: the loop passes
use "dominated by the header" as the loop body, which over-approximates
(making them conservative); switching the thirteen sites to the natural loop
body needs the LICM budgets retuned against CoreMark and the ray tracer.
Not implemented: qualifiers on the pointer itself (`int *volatile p`) and
volatile parameters. Tests: `opt/licm_mmio_spin.c`, `hw/volatile_accesses.c`.

Reproducer: `tests/cases/opt/licm_mmio_spin.c` (`// XFAIL: issue 0007`,
`SIM_ARGS: -maxsteps 200000`). `while (*t - t0 < 100u) {}` on the cycle counter at
`0xFF00` never terminates at `-O2`.

Root cause: `opt_licm` (`opt.c:1816-1831, 1847-1848`) hoists any `IK_LOAD` whose loop
is store-free, with no regard for the address. `opt_load_cse` already refuses constant
addresses for exactly this reason (`opt.c:1293`); LICM has no such guard, and the
`volatile` qualifier is parsed and dropped (`Type.qual` is never written,
`parser.c:1064-1069`).

Why it has not bitten on hardware: every LICM/LSR/downcount path defines "in the loop"
as "dominated by the header" (`opt.c:228, 1577, 1608, 1634, 1689, 1751, 1798, 1820,
1841, 1889, 2618, 2749, 2765`), which includes the post-loop code and its stores, so
`loop_store_free` is almost always false. The KU5P probes' `while (*ctrl & 1u) {}`
loops (`sw/ku5p/*.c`, `sw/raytracer/ku5p/rt_anim.c:319`) survive only by that accident.
**Fixing the loop-body definition alone would break them**; the two changes must land
together.

Fix: hoist an `IK_LOAD` only when its base is an `IK_ADDR` (a frame slot) — anything
else may be MMIO or another core's channel — or implement `volatile` as a flag on
`IK_LOAD`/`IK_STORE` that every pass treats as a clobber (preferable; it is also what
the CSP runtime needs). Then compute the real loop body once in `find_loops` (the
existing `loop_body_marks`, `opt.c:193-215`), use it at all thirteen sites, and retune
the LICM budgets against CoreMark and the ray-tracer frame count, since they were
tuned against the inflated counts.
