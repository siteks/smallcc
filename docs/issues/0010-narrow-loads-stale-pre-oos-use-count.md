# 0010 — Pre-OOS use_count is approximate, yet opt_narrow_loads mutates a shared load on use_count == 1

Status: OPEN (found 2026-09-26 by the compiler review, `docs/review-2026-09.md`; reproduced on `sim_c` and, where marked, at `-O0` as well).

Reproducer: `tests/cases/opt/narrow_loads_stale_use_count.c` (`// XFAIL: issue 0010`).
A loop that tests `x & 0xff` and also compares `i < x` returns 1 instead of 20 at `-O2`.

Root cause: `try_remove_trivial_phi` (`braun.c:775-787`) redirects uses through
`alias` without transferring counts, so a value can be under-counted before OOS.
`opt_narrow_loads` (`opt.c:755-772`) then narrows a 4-byte load to 1 byte and retypes
it on `use_count == 1` while another user still reads the full value. `verify.c:165-169`
documents the approximation and assumes every consumer runs after a `recount_uses`,
but `opt_redundant_bool` recounts only when it fires (`opt.c:728`); `opt_range_check`
(`opt.c:1399-1400`) and `opt_bitwise_dist` (`opt.c:1481`) also read pre-OOS counts.

Fix: one unconditional `recount_uses(f)` immediately after `split_critical_edges`
(O(n)), and let `verify.c` enforce exact counts from that point on. Slightly positive
for generated code: exact counts let R2H and range-check fire in cases they currently
miss, never the reverse.
