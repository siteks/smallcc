# Documentation Index

This directory contains the authoritative technical documentation for smallcc.

## Getting Started

- **Compiler users**: See [architecture.md](architecture.md) for how the compiler works, then [../CLAUDE.md](../CLAUDE.md) for quick build/test commands.
- **Backend developers**: See [compiler-pipeline.md](compiler-pipeline.md) for the nanopass CPU4 backend and [optimization-passes.md](optimization-passes.md) for the pass catalog.
- **ISA reference**: See [isa/cpu4.md](isa/cpu4.md) for instruction set details.

## Documentation Files

| File | Purpose |
|---|---|
| [architecture.md](architecture.md) | Compiler frontend and middle-end: preprocessor, tokeniser, parser, type system, per-TU compilation |
| [compiler-pipeline.md](compiler-pipeline.md) | CPU4 nanopass backend: lowering, Braun SSA, out-of-SSA, legalize, IRC register allocation, emission |
| [optimization-passes.md](optimization-passes.md) | Pass catalog, bitmask system, LICM/CSE tuning constants, emission peepholes |
| [abi.md](abi.md) | Calling convention, frame layout, type sizes, symbol naming, MMIO map |
| [c89-status.md](c89-status.md) | C89 compliance status: what's implemented, deliberate deviations, limitations |
| [testing.md](testing.md) | Test systems, debugging tips, simulator usage |
| [isa/cpu4.md](isa/cpu4.md) | CPU4 instruction set architecture (RISC-like with registers) |

## Architecture Overview

```
Frontend:     preprocessor → tokeniser → parser → AST
Type system:  resolve_symbols → derive_types → insert_coercions
Backend:      lower_globals + braun_function (Node* → SSA)
              → pre-OOS opts → out_of_ssa → post-OOS opts
              → legalize → irc_allocate → emit_function
```

The CPU3 stack-machine backend and its documentation were removed; the CPU4
nanopass pipeline is the compiler's sole backend. See git history for the
CPU3-era material.
