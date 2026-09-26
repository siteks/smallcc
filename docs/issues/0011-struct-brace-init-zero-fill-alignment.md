# 0011 — Struct brace-initialiser zero-fill stores halfwords regardless of alignment and size

Status: OPEN (found 2026-09-26 by the compiler review, `docs/review-2026-09.md`; reproduced on `sim_c` and, where marked, at `-O0` as well).

Reproducer: `tests/cases/struct_init/char3_brace_init.c` (`// XFAIL: issue 0011`).
`struct { char a, b, c; } s = {1, 2, 3};` aborts `sim_c` with an alignment error at
every `-O` level; on the RTL the misaligned store silently corrupts (see
`docs/abi.md`, "Enforcement is currently asymmetric").

Root cause: `braun.c:2192-2195` zero-fills a brace-initialised struct with 2-byte
stores at every even offset up to `ty->size`, so a size-3, align-1 object gets a
misaligned 16-bit store and one byte written past the object. It then stores every
initialised field again (`cg_fill_struct`), so `{1, 2, 3}` costs 2 + 3 stores.
Arrays (`braun.c:2174-2177`) use the leaf size and are fine.

Fix: fill with the largest unit that is at most `ty->align` and stays inside
`ty->size`, and skip the zero-fill for bytes the initialiser covers (or zero only the
uncovered tail and holes). Fewer stores per struct initialisation.
