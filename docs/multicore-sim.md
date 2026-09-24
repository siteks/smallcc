# sim_c multi-core mode (`-cores N`)

Added 2026-07-12 to support the sea-of-processors ray-tracer demo
(`../ray_tracer`); designed to be the behavioural reference for the future
multi-core RTL. Single-core behaviour (`-cores 1`, the default) is exactly
the historical simulator — same state line, same cycle counts, byte-identical
outputs; the whole existing test corpus passes unchanged.

## Model

- **N cores (1..1024), SPMD.** Every core boots the same assembled image at
  `pc=0` with a **private 64 KB BRAM** (code + globals + stack). Software
  reads `CORE_ID` and branches by role. There is no fork/dispatch primitive —
  this deliberately matches "many small independent cores", not the
  8-context barrel (contexts share BRAM; cores do not).
- **Shared:** the 32 MB SDRAM (≥0x10000 via plain pointers, plus the MMIO
  keyhole), `DISP_MODE`/LUT, and the bitmap framebuffer (`-fb` dump).
  The 80×30 text framebuffer / `-dumpfb` shows **core 0's** BRAM.
- **Scheduling: deterministic round-robin.** One tick = each live (non-halted)
  core executes one instruction, in core order. Same command line → same
  execution, always. A spinning core cannot starve the others.
- **Timing:** `g_cycles`/the µs counter (0xFF00) count **ticks**. `-maxsteps`
  caps ticks. Per-core executed-instruction counts are reported at exit.
  With `-cores 1`, ticks == instructions == the historical `cycles:` value.

## New MMIO registers (reserved range 0xFF24/0xFF28)

| Addr | Name | Access | Value |
|---|---|---|---|
| `0xFF24` | `CORE_ID` | RO | index of the reading core (0..N−1); 0 when `-cores` not given |
| `0xFF28` | `NCORES` | RO | N; 1 when `-cores` not given |

(Not yet in `docs/abi.md` §6 — that file had pending edits when this landed;
fold these two rows in on the next abi.md touch. The RTL should implement
both when multi-core hardware exists.)

## Output format

`-cores 1`: unchanged single state line.

`-cores N` (N>1): one line per core, prefixed `coreK `, where the `cycles:`
field is **that core's executed instruction count**, followed by a
`ticks:<T>` summary line (wall-clock ticks for the whole run):

```
core0 r0:00000000 ... H:1 cycles:11603
core1 r0:00000000 ... H:1 cycles:9111
...
ticks:11603
```

`-trace` lines gain a `cK ` prefix when N>1. `-profile` aggregates
instruction counts across all cores (per-PC, and all cores share one image).

## Software conventions (no atomics exist)

Inter-core communication is shared-SDRAM with **single-writer /
single-reader** words only: one core writes a slot, one core reads it; a
non-zero value doubles as the full flag; the reader clears it to ack.
`putchar` goes straight to stderr from any core — in practice only core 0
should print. Tests: `tests/cases/multicore/` (`SIM_ARGS:` magic comment
passes extra sim flags; the multicore dir is skipped under `--irsim`).

## Implementation notes (sim_c.c)

Core state lives in `Core {r[8], sp, bp, lr, pc, H, insns, bram[64K]}`;
the executor body is untouched — `r`/`sp`/`bp`/`lr`/`pc`/`H` are macro
aliases onto the current core (`cc`), and the BRAM router indirects through
`g_bram`, both swapped per instruction by the round-robin loop. The
assembler image stays in the global `mem[]` and is copied into every core's
BRAM before the run. `sim_c` is now built `-O2` (~3× faster; ~70M
instr/s — a 4-core ray-tracer frame simulates in ~7 s).
