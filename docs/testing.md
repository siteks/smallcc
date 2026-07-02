# Testing and Simulation

## Overview

The test corpus is a set of standalone `.c` files under `tests/cases/`, each
carrying its expectations in magic comments. One pytest harness
(`tests/conftest.py`) runs the corpus in two independent execution modes:

- **`make test`** — compile with `smallcc`, assemble and execute with `sim_c`
  (the C simulator), check the value left in `r0` and any `putchar` output.
- **`make test_irsim`** — compile with `smallcc -runoos` and `-runirc`, which
  interpret the post-OOS and post-IRC IR in-process (`irsim.c`) without ever
  generating assembly.

Together these form a **three-stage differential oracle**: a divergence
between `-runoos` and `-runirc` localizes a bug to legalize/IRC; a divergence
between `-runirc` and `sim_c` localizes it to emission or the assembler.

Tests under `tests/cases/hw/` exercise `sim_c`'s MMIO device model
(framebuffer, `DISP_MODE`, the SDRAM keyhole) and are skipped in irsim mode —
the IR interpreter has no device model.

---

## The C Simulator (`sim_c`)

`sim_c` is a native C binary that assembles and runs CPU4 assembly files. It
is the de-facto executable ISA spec (see `coordination.md`).

### Build

```bash
make sim_c
```

This compiles `sim_c.c` (self-contained) against the system libc with `-lm`.

### Usage

```
./sim_c [options] file.s
  -trace FILE        per-instruction execution trace
  -arch cpu4         target architecture (only cpu4)
  -maxsteps N        override the instruction-step cap
  -dump FILE         assemble + write bytecode dump; no execution
  -dumpfb            dump the 80x30 text framebuffer at 0xF000 after running
  -fb FILE           dump the bitmap framebuffer to FILE.ppm (honors DISP_MODE)
  -profile           per-source-line execution profile
  -linemap FILE      assemble + write PC->source JSON map; no execution
  -hex FILE          assemble + write hex bytes; no execution
```

### Output format

After the program halts, `sim_c` prints one line to **stdout**:

```
r0:0000002a r1:.. r2:.. r3:.. r4:.. r5:.. r6:.. r7:.. sp:f000 bp:0000 lr:0006 pc:0007 H:1 cycles:N
```

All values are hex. `H:1` means halted normally. `putchar` output goes to
**stderr**, keeping stdout clean for register parsing.

### Debug facilities

| Feature | Description |
|---|---|
| Register dump | Always printed on halt (see above) |
| `-trace FILE` | Writes every instruction as it executes |
| Write watchpoints | Writes to addresses below `0x5000` print to stderr — useful for catching stray stores into the code/data area |
| Crash trace | On unknown opcode, dumps the last 32 executed instructions |
| Immediate range checks | The assembler rejects out-of-range immediates (e.g. F2 imm7) instead of silently masking them |
| MMIO cycle counter | 32-bit read-only counter at `0xFF00`, incremented once per instruction |

### Memory model

64 KB BRAM (`sp`/`bp`/`pc` are 16-bit) plus a lazily allocated 32 MB SDRAM
for data addresses ≥ `0x10000`, reached through ordinary C pointers under the
ILP32 model. `irsim.c` mirrors this model.

---

## The IR Interpreter (`irsim.c`)

`smallcc -runoos file.c` and `smallcc -runirc file.c` interpret the IR
directly (post-OOS and post-IRC respectively) and print the same
`r0:XXXXXXXX` line as `sim_c`. Because the same `run_function` handles both
forms (values are indexed by id, not phys_reg), this checks the pipeline's
semantics independently of emission, the assembler, and the peepholes.

```bash
make test_irsim     # whole corpus through -runoos and -runirc
make test_irsim_p   # same, parallel
```

---

## Differential fuzzing (`tools/fuzz.py`)

`make fuzz` (or `python3 tools/fuzz.py -n N -seed S`) generates random C89
programs inside the supported subset and runs each through four oracles —
`-O2`→sim_c, `-O0`→sim_c, `-runoos`, `-runirc` — plus an `IR_VERIFY=1`
compile. Any r0 disagreement, crash, compile timeout, or verifier failure
saves the program to `fuzz_failures/` for reduction into a `tests/cases/`
reproducer. The generator stays inside target-defined behavior (unsigned
arithmetic, `|1`-guarded divisors, masked shift amounts) so the oracles must
agree. Its first session found a sim_c ISA-semantics bug (`rdivli`/`rmodli`
executed signed) and a compiler hang (unbounded copy-chain walk in legalize
Pass E).

---

## Compiler debug flags

| Mechanism | What it shows |
|---|---|
| `IR_VERIFY=1` | Runs the IR verifier (`verify.c`) after every pass group: pred/succ symmetry, terminator discipline, phi arity, dead-def uses, use_count exactness (post-OOS), phys_reg assignment (post-IRC). On violation it prints all findings plus an IR dump and exits — the failing stage name localizes the broken pass |
| `-ssa file` / `-oos file` / `-irc file` | Per-function IR dump after Braun / OOS / IRC |
| `DUMP_IR=1` | Post-OOS and post-IRC IR to stderr |
| `-ann` | Annotate emitted assembly with source lines |
| `OPT_STATS=1` | Per-pass fire counters |
| `CSE_DEBUG` / `LICM_DEBUG` / `LSR_DEBUG` / `DBG_IRC` | Per-pass tracing to stderr |

**Debugging rule:** debug failures at the earliest pipeline stage where they
appear (Braun SSA → OOS → legalize → IRC → emission). The irsim modes are the
fastest way to bisect: if `-runoos` is right and `-runirc` is wrong, the bug
is in legalize/IRC; if both are right and `sim_c` is wrong, it's in emission
or the assembler.

---

## pytest Test Cases (`tests/cases/`)

### Setup (one time)

```bash
pip install pytest pytest-xdist
```

### Running

```bash
pytest tests/cases/             # all tests (sim_c mode)
pytest tests/cases/ -n8         # parallel, 8 workers
pytest tests/cases/ --irsim     # irsim modes (-runoos / -runirc)
pytest tests/cases/ops/         # one subdirectory
pytest -k "multifile or error"  # filter by name
pytest -v tests/cases/          # verbose output
```

### Writing a test

A test is a `.c` file placed anywhere under `tests/cases/`. Metadata lives in
`//` comments at the very top of the file — before any non-comment line. The
presence of at least one `EXPECT_*` key makes pytest pick up the file.

#### Check a return value

```c
// EXPECT_R0: 42
int main() { return 42; }
```

`EXPECT_R0` is a signed decimal integer. The 32-bit `r0` value is
sign-extended before comparison, so negative expected values work:

```c
// EXPECT_R0: -1
int main() { int x = -1; return x; }
```

#### Check that compilation fails

```c
// EXPECT_COMPILE_FAIL
int main() { return 1 }   // missing semicolon
```

`smallcc` must exit non-zero. The test passes if it does. Useful for parser
error-recovery tests and deliberate unsupported-syntax checks.

#### Check putchar output

```c
// EXPECT_R0: 0
// EXPECT_STDOUT: Hello
int main() {
    putchar('H'); putchar('e'); putchar('l'); putchar('l'); putchar('o');
    return 0;
}
```

`EXPECT_STDOUT` is matched against the exact string written by `putchar`
calls. The string is compared literally — no trailing newline is added by
`sim_c`. `EXPECT_R0` and `EXPECT_STDOUT` can be combined freely.

#### Multi-TU tests

Use `FILES` to list two or more source files to compile together. Paths are
relative to the directory of the file containing the `FILES` key.

```
tests/cases/multifile/
    lib.c          ← no EXPECT_ key; just a helper
    main.c         ← has EXPECT_R0 + FILES
```

`main.c`:
```c
// EXPECT_R0: 7
// FILES: lib.c main.c
extern int add(int, int);
int main() { return add(3, 4); }
```

`lib.c`:
```c
int add(int a, int b) { return a + b; }
```

The `FILES` list is compiled as a single `smallcc` invocation. Only the file
containing `FILES` needs `EXPECT_*` keys.

### Metadata reference

| Key | Value | Meaning |
|---|---|---|
| `EXPECT_R0` | signed decimal integer | Expected value of `r0` after execution |
| `EXPECT_COMPILE_FAIL` | (no value) | `smallcc` must exit non-zero |
| `EXPECT_STDOUT` | string | Exact string expected from `putchar` calls |
| `FILES` | space-separated filenames | Multi-TU: compile all listed files (relative to this file's directory) |
| `TARGET` | `sim` (default) or `hw` | Selects the crt0 variant; `hw` tests are skipped in irsim mode |

Multiple keys can appear in any order. All that are present are checked.

### Directory layout

```
tests/
  cases/
    ops/           ← operator and expression tests
    struct/        ← struct/union tests
    multifile/     ← cross-TU linkage tests
    errors/        ← compile-failure tests
    io/            ← putchar / stdout tests
    hw/            ← MMIO device-model tests (sim_c only)
    coremark/      ← CoreMark-derived reproducers
    jpeg/          ← NanoJPEG decode (SDRAM pointers, static tables)
  conftest.py      ← pytest plugin
```

New tests can go in any subdirectory under `tests/cases/`. Subdirectory names
are just organisation — pytest discovers all `.c` files recursively. The
corpus is also the cross-repo bug-exchange format (see `coordination.md`):
the hardware repo consumes the same files.

### How discovery works

`tests/conftest.py` registers a `pytest_collect_file` hook. When pytest
visits a `.c` file, the hook reads the leading `//` comments. If any key
starts with `EXPECT`, the file becomes a test item — once per execution mode.
Files without `EXPECT_*` keys (like `lib.c` in a multi-TU test) are silently
ignored.
