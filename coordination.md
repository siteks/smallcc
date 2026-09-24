# Cross-Repo Coordination

smallcc is a standalone repo and also the toolchain submodule of the CPU4
sea-of-processors project. This file is the contract between the two.

| Where | Owns |
|---|---|
| `smallcc` (this repo) | Compiler, `sim_c` / `irsim` / `cpu4/cpu.py`, **ISA spec** (`docs/isa/cpu4.md`), **ABI spec** (`docs/abi.md`), the test corpus (`tests/cases/`), fuzzer, CoreMark bench, compiler intrinsics and the simulator's device models (MMIO, multi-core `-cores`, SoC tile `-soc`) |
| processor monorepo (`../..` when this checkout is its `toolchain/smallcc` submodule) | RTL and FPGA boards (`hw/`), host tools, runtime libraries (CSP channels, BIOS), applications (ray tracer), hardware-only tests, system-level docs |

## Canonical artifacts

- **`docs/isa/cpu4.md`** wins all ties. **`sim_c.c`** is the executable spec:
  when the doc is ambiguous, what `sim_c` does is what the compiler targets and
  what the hardware must match. A doc/`sim_c` discrepancy is a spec bug to fix
  here.
- **`docs/abi.md`** is the software contract; hardware sees it only through the
  instructions the compiler emits.
- **`tests/cases/`** is the single corpus. Downstream runs these same files
  through the submodule path; it never keeps a copy. Hardware-only cases
  (device model, SoC) live downstream.

## The ISA/ABI change rule

An ISA or ABI change is **one commit in smallcc** that touches, together:
`docs/isa/cpu4.md` (or `docs/abi.md`), `sim_c.c`, `cpu4/cpu.py`, the compiler
(emit/legalize/assembler tables) and at least one corpus test that exercises
the change. It is followed by **one commit downstream** that updates the RTL
and bumps the submodule pointer. There is never a downstream-only change to
ISA semantics, and never a "temporary" local copy of the RTL or the spec.

Submodule discipline: commit and push here before bumping the pointer
downstream, so the recorded commit is always resolvable.

## Proposals

Anyone (either side, either Claude session) can propose a change with a short
spec-delta note:

> **Subject:** widen F0c imm7 → imm8 for cbeq/cbne
> **Why:** profile shows 8% of dynamic loads have constants in [128, 255],
> blocked from compact form by the imm7 cap.
> **Spec change:** F0c bit layout shifts from `001 o ddd iiiiiiiiiiiiiiiii`
> to `001 o ddd ii iiiiiiiiiiiiiiii` (steal one bit from disp10 → disp9).
> **Compiler impact:** P17 cap goes from 127 to 255; range from ±511 to ±255.
> **Hardware impact:** decoder bit-slice change.

Larger proposals are documents under `docs/proposals/` in the monorepo. When
a proposal is accepted the ISA-facing part lands here with the change, per the
rule above. `docs/handoffs/` holds the historical cross-repo handoffs from
when the hardware was a separate repo; new ones are not needed.

## Working across the boundary

- A Claude Code session at the monorepo root may edit both trees; the commits
  are still two, in the order above.
- Cross-boundary bugs are pinned down as corpus entries: the discovering side
  writes a minimal `.c` reproducer under `tests/cases/`, the responsible side
  makes it pass.
- Don't rely on session memory to carry facts across sessions or repos.
  Anything that should outlive a session goes in a checked-in doc.
