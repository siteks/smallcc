# Cross-Repo Coordination

CPU4 spans three closely-coupled but independently-versioned repos:

| Repo | Owns | Lives at |
|---|---|---|
| `smallcc` | Compiler, reference simulator (`sim_c`, `cpu4/cpu.py`), ISA spec, ABI spec, test corpus | this repo |
| `cpu4_hardware` | RTL implementation of the ISA, FPGA build, hardware tests | `../cpu4_hardware` |
| `coremark_single_file` | Benchmark sources tuned for this target | `../coremark_single_file` |

Each repo has its own Claude Code context. This file is the contract for how
those contexts coordinate without stepping on each other.

## Read freely, write your own column

Each Claude **may read** any of the other repos at will. Reading is
zero-cost and avoids round-tripping facts through the human. The hardware
Claude consulting `sim_c.c` to confirm semantics, or the compiler Claude
checking RTL timing, is exactly the right thing.

Each Claude **must only write** to its own repo. No cross-repo edits — even
"obvious" fixes — without the human ratifying first. This keeps version
ownership clear and prevents two contexts from racing on the same source of
truth.

## Canonical artifacts

Three documents are the contract between repos. They live in `smallcc`
because that's where the compiler keeps its references; the hardware repo
should symlink (or git-submodule) them, not copy:

- **`docs/isa/cpu4.md`** — the ISA spec. Encoding, semantics, mnemonics.
  When the spec changes, both compiler and hardware update against it. The
  spec wins all ties.
- **`docs/abi.md`** — the ABI. Calling convention, frame layout, types.
  Visible to hardware mainly for tooling (debuggers, profilers); compiler
  enforces it.
- **`tests/cases/`** — the test corpus. Each `.c` file with `EXPECT_R0`
  metadata is a reproducible behavioural check. Adding to the corpus is the
  primary way bugs get pinned down across repos: the discovering Claude
  writes a test, the responsible Claude makes it pass.

  **The smallcc copy is canonical.** The hardware repo should consume the
  same files (symlink or submodule), not maintain a parallel copy — historical
  duplication has already drifted (smallcc has 3 files the hardware copy
  doesn't, e.g. `tests/cases/multifile/main.c`). When either Claude writes a
  new reproducer it lands in `smallcc/tests/cases/`; the hardware test runner
  picks it up via the symlink. New tests follow the existing subdirectory
  convention (`coremark/`, `array/`, etc.).

`sim_c` is the de-facto executable spec — when the ISA doc is ambiguous,
`sim_c.c` is what the compiler targets and what the hardware should match.
Discrepancies between the doc and `sim_c` are spec bugs to be filed.

## When to involve the human

- **ISA changes** (new instruction, encoding tweak, semantics change). The
  proposing Claude writes a short delta doc covering opcode, encoding,
  semantics, motivation, expected impact. Human ratifies; both Claudes then
  implement against the amended spec.
- **ABI changes** (calling convention, struct layout, type sizes). Same
  workflow as ISA changes — the changes are visible across the boundary.
- **Cross-repo bug investigations.** Once the discovering Claude has a
  minimal reproducer, the human relays it to the right side. The reproducer
  belongs in the test corpus.

## What not to do

- **Don't sync MEMORY.md across Claudes.** Each session's memory is local
  context. Anything that should outlive the session goes in a checked-in doc.
- **Don't have one Claude run another's tooling indirectly.** If the
  hardware Claude needs a smallcc-compiled assembly, it asks the human (or
  reads a checked-in `.s` from the test corpus). Avoids version-skew.
- **Don't duplicate ISA / ABI text into each repo.** Symlink or submodule.
  Two physical copies will drift.

## Lightweight handoff format

When a finding does need to cross repos, a one-paragraph spec-delta note is
the format:

> **Subject:** widen F0c imm7 → imm8 for cbeq/cbne
> **Why:** profile shows 8% of dynamic loads have constants in [128, 255],
> blocked from compact form by the imm7 cap.
> **Spec change:** F0c bit layout shifts from `001 o ddd iiiiiiiiiiiiiiiii`
> to `001 o ddd ii iiiiiiiiiiiiiiii` (steal one bit from disp10 → disp9).
> **Compiler impact:** P17 cap goes from 127 to 255; range from ±511 to ±255.
> **Hardware impact:** decoder bit-slice change.

Either Claude can write that. Larger changes graduate to a `docs/proposals/`
document. The human ratifies and both implement.
