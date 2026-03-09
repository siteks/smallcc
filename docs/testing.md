# Testing and Simulation

## Overview

Two test systems run in parallel:

- **Bash test suites** (`tests/test_*.sh`) — the original harness, driven by `make test_all`. Tests are C snippets embedded as strings in shell scripts.
- **pytest test cases** (`tests/cases/`) — newer file-based harness. Each test is a standalone `.c` file with metadata in magic comments. Supports multi-TU tests, compile-failure tests, and stdout capture.

Both systems share the same compiler (`mycc`) and simulator (`sim_c`).

---

## The C Simulator (`sim_c`)

`sim_c` is a native C binary that assembles and runs CPU3 assembly files. It replaces the Python simulator (`cpu3/sim.py`) for all test purposes.

### Build

```bash
make sim_c
```

This compiles `sim_c.c` (self-contained, ~310 lines) against the system libc with `-lm`.

### Usage

```bash
./sim_c file.s          # run; print final register state to stdout
./sim_c -v file.s       # verbose: trace each instruction to stderr
```

### Output format

After the program halts, `sim_c` prints exactly one line to **stdout**:

```
r0:0000002a sp:1000 bp:0000 lr:0006 pc:0007 H:1
```

All values are hex. `r0` is 32-bit; `sp`, `bp`, `lr`, `pc` are 16-bit. `H:1` means halted normally.

`putchar` output goes to **stderr**, keeping stdout clean for register parsing.

### Step limit

`sim_c` allows up to 10,000,000 instruction steps before giving up (vs. 1,000 for `cpu3/sim.py`). Programs with large loops no longer need to worry about the step cap.

### Architecture

`sim_c.c` is a single file divided into three sections:

1. **Assembler** — two-pass: pass 1 builds a symbol table and computes addresses; pass 2 emits bytes into a 64 KB memory array. Handles all assembly directives (`byte`, `word`, `long`, `align`, `allocb`, `allocw`, `.text=N`, `.data=N`) and label references in data directives.
2. **CPU** — `switch`-based dispatch over all 42 opcodes. Float operations use `memcpy` between `float` and `uint32_t` for correct IEEE 754 bit-pattern semantics.
3. **Main** — reads the file, runs the assembler, runs the CPU, prints the state line.

### Cross-checking with the Python simulator

`cpu3/sim.py` remains the reference ISA implementation. Use it to cross-check `sim_c` if results look wrong:

```bash
./mycc -o /tmp/t.s t.c
./sim_c /tmp/t.s
./cpu3/sim.py /tmp/t.s 2>/dev/null | tail -1   # strips assembler debug noise
```

---

## Bash Test Suites

### Running

```bash
make test_all           # run all 19 suites sequentially
make -j8 test_all       # run all suites in parallel (safe)
make test_ops           # run a single suite
```

Temp files use `$$`-suffixed names (`_tmp_$$.c`, `tmp_$$.s`) so concurrent suite runs do not collide.

On failure, `sim_c -v` output is written to `error.log` for inspection.

### Structure

Each suite is a shell script in `tests/`. The `assert` function in `test.sh` does:
1. Write the input string to a temp `.c` file.
2. Compile with `mycc -o`.
3. Simulate with `sim_c`; parse `r0` from the output.
4. Compare the signed integer value of `r0` against the expected value.

```bash
assert 42 "int main() { return 42; }"
```

---

## pytest Test Cases (`tests/cases/`)

### Setup (one time)

```bash
pip install pytest pytest-xdist
```

### Running

```bash
pytest tests/cases/             # all new-style tests
pytest tests/cases/ -n8         # parallel, 8 workers
pytest tests/cases/ops/         # one subdirectory
pytest -k "multifile or error"  # filter by name
pytest -v tests/cases/          # verbose output
```

### Writing a test

A test is a `.c` file placed anywhere under `tests/cases/`. Metadata lives in `//` comments at the very top of the file — before any non-comment line. The presence of at least one `EXPECT_*` key makes pytest pick up the file.

#### Check a return value

```c
// EXPECT_R0: 42
int main() { return 42; }
```

`EXPECT_R0` is a signed decimal integer. The 32-bit `r0` value is sign-extended before comparison, so negative expected values work:

```c
// EXPECT_R0: -1
int main() { int x = -1; return x; }
```

#### Check that compilation fails

```c
// EXPECT_COMPILE_FAIL
int main() { return 1 }   // missing semicolon
```

`mycc` must exit non-zero. The test passes if it does. Useful for parser error-recovery tests and deliberate unsupported-syntax checks.

#### Check putchar output

```c
// EXPECT_R0: 0
// EXPECT_STDOUT: Hello
int main() {
    putchar('H'); putchar('e'); putchar('l'); putchar('l'); putchar('o');
    return 0;
}
```

`EXPECT_STDOUT` is matched against the exact string written by `putchar` calls. The string is compared literally — no trailing newline is added by `sim_c`.

`EXPECT_R0` and `EXPECT_STDOUT` can be combined freely.

#### Multi-TU tests

Use `FILES` to list two or more source files to compile together. Paths are relative to the directory of the file containing the `FILES` key.

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

The `FILES` list is compiled as a single `mycc` invocation:
```bash
./mycc -o out.s tests/cases/multifile/lib.c tests/cases/multifile/main.c
```

Only the file containing `FILES` needs `EXPECT_*` keys.

### Metadata reference

| Key | Value | Meaning |
|---|---|---|
| `EXPECT_R0` | signed decimal integer | Expected value of `r0` after execution |
| `EXPECT_COMPILE_FAIL` | (no value) | `mycc` must exit non-zero |
| `EXPECT_STDOUT` | string | Exact string expected from `putchar` calls |
| `FILES` | space-separated filenames | Multi-TU: compile all listed files (relative to this file's directory) |

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
  conftest.py      ← pytest plugin (do not move; path is relative to tests/cases/)
  test_init.sh     ← original bash suites (unchanged)
  test_ops.sh
  ...
```

New tests can go in any subdirectory under `tests/cases/`. Subdirectory names are just organisation — pytest discovers all `.c` files recursively.

### How discovery works

`tests/conftest.py` registers a `pytest_collect_file` hook. When pytest visits a `.c` file, the hook reads the leading `//` comments. If any key starts with `EXPECT`, the file becomes a test item. Files without `EXPECT_*` keys (like `lib.c` in a multi-TU test) are silently ignored.

---

## Relationship between the two systems

The bash suites and the pytest cases are independent. `make test_all` runs only the bash suites. `pytest tests/cases/` runs only the file-based cases. There is no overlap — existing bash tests were not migrated.

New tests should go into `tests/cases/` as `.c` files. The bash suites remain for the existing test corpus and continue to work unchanged.
