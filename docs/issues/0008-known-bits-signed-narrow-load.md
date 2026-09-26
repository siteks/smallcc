# 0008 — Known-bits treats every narrow load as zero-extending

Status: OPEN (found 2026-09-26 by the compiler review, `docs/review-2026-09.md`; reproduced on `sim_c` and, where marked, at `-O0` as well).

Reproducer: `tests/cases/opt/known_bits_signed_byte_load.c` (`// XFAIL: issue 0008`).
`char g = -1; return *p & 0xff;` gives -1 at `-O2` (255 at `-O0`).

Root cause: `opt_known_bits` (`opt.c:962-966`) sets the known-zero mask to `~0xFF` for
any 1-byte load, but signed loads are emitted as `llbx`/`lbx` (R2H at `opt.c:776-800`
and emit P10 depend on that). R2H first turns `SEXT8(load)` into a copy, then R2K
"proves" the `AND` redundant (`opt.c:1147-1153`) and removes it.

Fix: guard the narrow-load case with `!vt_signed(inst->dst->vtype)` (the same rule
`irsim.c:504` applies). No effect on generated code for unsigned loads.
