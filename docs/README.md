# Documentation Index

This directory contains the authoritative technical documentation for smallcc.

## Getting Started

- **Compiler users**: See [architecture.md](architecture.md) for how the compiler works, then [../CLAUDE.md](../CLAUDE.md) for quick build/test commands.
- **Backend developers**: See [backend.md](backend.md) for IR details and both target backends.
- **ISA reference**: See [isa/cpu3.md](isa/cpu3.md) and [isa/cpu4.md](isa/cpu4.md) for instruction set details.

## Documentation Files

| File | Purpose |
|---|---|
| [architecture.md](architecture.md) | Compiler frontend and middle-end: preprocessor, tokeniser, parser, type system, per-TU compilation |
| [backend.md](backend.md) | Backend documentation: stack IR, CPU3 backend, CPU4 SSA backend with IRC register allocation, peephole optimisations |
| [c89-status.md](c89-status.md) | C89 compliance status: what's implemented, deliberate deviations, limitations |
| [testing.md](testing.md) | Test systems, debugging tips, simulator usage |
| [isa/cpu3.md](isa/cpu3.md) | CPU3 instruction set architecture (stack machine) |
| [isa/cpu4.md](isa/cpu4.md) | CPU4 instruction set architecture (RISC-like with registers) |

## Architecture Overview

```
Frontend:     preprocessor → tokeniser → parser → AST
Type system:  resolve_symbols → derive_types → insert_coercions
IR gen:       gen_ir → mark_basic_blocks → peephole

CPU3 path:    backend_emit_asm (stack IR → CPU3 assembly)
CPU4 path:    braun_ssa → ir3_optimize → irc_regalloc → risc_backend_emit
```
