## 2026-03-15

- `10:50 user` preamble should come from lib/crt0.s, rather than be hardcoded
- `10:53 user` currently preamble assumes that memory is always zero, it doesn't clear globals. Add a pseudo assembler instruction to do this, when target is physical, this will be replaced with an actual clear memory loop
- `11:04 user` question: how reliable is get_compiler_dir()?
- `11:07 user` I'm about to start working in a different dir to compile eembc, so I want it to work on the path. It should work with limux and Macos
- `11:29 user` Defacto, we are going to allocate the top 256 bytes of address space to memory-mapped peripherals. The first is a 32 bit cycle counter, tied to the simulator cycle counter. This is necessary to port the EEMBC coremark benchmarks
- `11:32 user` Implement the following plan:

  # Plan: Memory-Mapped Peripheral Region + Cycle Counter

  ## Context
  EEMBC CoreMark requires a hardware timer to measure performance (`get_time()`). The CPU3 simulator has a 16-bit address space with no peripheral abstraction. Top 256 bytes (0xFF00–0xFFFF) are currently unused and safe to reserve — the stack grows down from 0x1000 and global data is placed after code, both well below 0xFF00.

  The `cycles` counter already exists in `sim_c.c` (as a local in `run_cpu()`). It needs to be exposed as a 32-bit read-only MMIO register so C code can time operations with `*(volatile unsigned long *)0xFF00`.

  ---

  ## Memory Map (0xFF00–0xFFFF)

  | Address | Size | Register | Access |
  |---------|------|----------|--------|
  | 0xFF00  | 4 B  | Cycle counter | RO |
  | 0xFF04–0xFFFF | — | Reserved | — |

  ---

  ## Files to Modify

  ### 1. `sim_c.c`

  - Promote `cycles` from local in `run_cpu()` to **`static uint32_t g_cycles = 0;`** at file scope. (Truncated to 32 bits; the MMIO register is 32-bit. Reset to 0 at top of `run_cpu()`.)
  - Add constant: `#define MMIO_BASE 0xFF00u`
  - Add helper:
    ```c
    static uint8_t mmio_read8(uint16_t a) {
        uint32_t off = (uint16_t)(a - MMIO_BASE);
        if (off < 4) return (uint8_t)(g_cycles >> (off * 8));
        return 0;
    }
    ```
  - Wrap `read8`, `read16`, `read32` with an MMIO guard at the top:
    ```c
    static uint8_t  read8 (uint16_t a) { if (a >= MMIO_BASE) return mmio_read8(a); return mem[a]; }
    static uint16_t read16(uint16_t a) { if (a >= MMIO_BASE) return (uint16_t)mmio_read8(a) | ((uint16_t)mmio_read8(a+1)<<8); return ...; }
    static uint32_t read32(uint16_t a) { if (a >= MMIO_BASE) return (uint32_t)mmio_read8(a)|((uint32_t)mmio_read8(a+1)<<8)|((uint32_t)mmio_read8(a+2)<<16)|((uint32_t)mmio_read8(a+3)<<24); return ...; }
    ```
  - Wrap `write8`, `write16`, `write32` to silently ignore writes to `a >= MMIO_BASE`.
  - In the run loop, increment `g_cycles` instead of `cycles`. Keep the output line printing `g_cycles`.

  ### 2. `cpu3/cpu.py`

  - Add `self.cycles = 0` to `CPU.__init__` (not `State` — avoids changing the `State.__repr__` output and breaking test harness).
  - Increment `self.cycles` at the top of `CPU.step()`.
  - In `CPU.__init__`, bind the cycle counter to `Mem`:
    ```python
    m.get_cycles = lambda: self.cycles
    ```
  - Add `self.get_cycles = lambda: 0` default in `Mem.__init__`.
  - Add MMIO dispatch in `Mem.read8`, `read16`, `read32`:
    ```python
    MMIO_BASE = 0xFF00
    def _mmio_read8(self, addr):
        off = (addr - MMIO_BASE) & 0xffff
        if off < 4:
            return (self.get_cycles() >> (off * 8)) & 0xff
        return 0
    ```
    Each read method checks `if addr >= 0xFF00:` first and dispatches.
  - Add MMIO guard to `write8`, `write16`, `write32`: silently return if `addr >= 0xFF00`.

  ### 3. `docs/isa.md` (or new `docs/peripherals.md`)

  Add a **Memory Map** section documenting the peripheral region, the cycle counter register, and a C access example:
  ```c
  /* Read 32-bit cycle counter */
  unsigned long t = *(volatile unsigned long *)0xFF00;
  ```

  ---

  ## Notes

  - `clearmem` in `makeimage()` zeros from `_globals_start` to end-of-image; this is always well below 0xFF00, so MMIO region is unaffected.
  - Writes to the MMIO region are silently dropped (read-only register). No error on write.
  - `sim_c.c` is what the test harness uses; `cpu.py` is the reference Python implementation. Both must be updated.

  ---

  ## Verification

  ```bash
  make sim_c                     # rebuild C simulator
  make test_all                  # all existing tests must still pass
  # Manual smoke test:
  cat > /tmp/cycle_test.c << 'EOF'
  unsigned long get_cycles(void) {
      return *(volatile unsigned long *)0xFF00;
  }
  int main(void) {
      unsigned long t0 = get_cycles();
      int i; for (i = 0; i < 100; i++) {}
      unsigned long t1 = get_cycles();
      return (t1 > t0) ? 1 : 0;   /* should return 1 */
  }
  EOF
  ./smallcc /tmp/cycle_test.c > /tmp/cycle_test.s && ./sim_c /tmp/cycle_test.s
  # Expect: r0:00000001 ... H:1
  ```


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/9eef1e2e-9c68-4551-9012-b589e523babc.jsonl
- `11:32 user` [Request interrupted by user for tool use]
- `12:27 user` allow defines on command line, like "-DPERFORMANCE_RUN=1"
- `12:31 user` #include "file.h" should search the directory of the including file, but if the source is like "cpu3/core_portme.c", the "cpu3" dir is not searched
- `12:46 user` add "#if" to preprocessor, the minimum required to resolve an integer
- `12:48 user` Implement the following plan:

  # Plan: Add `#if` / `#elif` to Preprocessor

  ## Context

  CoreMark and other real-world C code use `#if EXPR` and `#elif EXPR` extensively.
  The preprocessor currently only supports `#ifdef`/`#ifndef`/`#else`/`#endif`.
  This plan adds the minimum to evaluate integer constant expressions in `#if`/`#elif`,
  including `defined(NAME)`, numeric literals, arithmetic, bitwise, and logical operators.

  ---

  ## File to Modify

  **`preprocess.c`** only. No other files need changes.

  ---

  ## 1. Add `pp_cond_done[]` to the conditional stack

  The existing stack has two arrays:
  ```c
  static int pp_cond_emit [PP_MAX_COND_DEPTH];
  static int pp_cond_outer[PP_MAX_COND_DEPTH];
  ```

  Add a third:
  ```c
  static int pp_cond_done [PP_MAX_COND_DEPTH]; /* 1 = some branch in this chain was taken */
  ```

  Reset it in `reset_preprocessor()` (already zeroed by being static, but explicit clarity).

  **Update `#ifdef`/`#ifndef`** to initialise `pp_cond_done`:
  ```c
  int taken = outer && cond;
  pp_cond_outer[pp_cond_depth] = outer;
  pp_cond_emit [pp_cond_depth] = taken;
  pp_cond_done [pp_cond_depth] = taken;   // ← add this line
  pp_cond_depth++;
  ```

  **Update `#else`** to consult `pp_cond_done` (currently uses a naive flip):
  ```c
  int outer = pp_cond_outer[pp_cond_depth - 1];
  int done  = pp_cond_done [pp_cond_depth - 1];
  pp_cond_emit[pp_cond_depth - 1] = outer && !done;
  ```

  ---

  ## 2. Add the integer constant expression evaluator

  Add a self-contained recursive-descent evaluator **above** the `preprocess()` function.
  It operates on a `const char *` cursor `eval_p` (file-scope static during evaluation).

  ```c
  static const char *eval_p;

  static long long pp_eval_expr(void);   // top-level: calls pp_eval_logor
  ```

  **Operator precedence (lowest→highest):**

  | Function         | Operators        |
  |------------------|------------------|
  | `pp_eval_logor`  | `\|\|`           |
  | `pp_eval_logand` | `&&`             |
  | `pp_eval_bitor`  | `\|`             |
  | `pp_eval_bitxor` | `^`              |
  | `pp_eval_bitand` | `&`              |
  | `pp_eval_equal`  | `==` `!=`        |
  | `pp_eval_relat`  | `<` `>` `<=` `>=`|
  | `pp_eval_shift`  | `<<` `>>`        |
  | `pp_eval_add`    | `+` `-`          |
  | `pp_eval_mul`    | `*` `/` `%`      |
  | `pp_eval_unary`  | `!` `~` `-` `+`  |
  | `pp_eval_primary`| literals, `defined`, `(expr)`, identifier→0 |

  **`pp_eval_primary`** handles:
  - Integer literals: `strtoll(eval_p, &end, 0)`, skip `u`/`l`/`U`/`L` suffixes
  - `defined(NAME)` or `defined NAME` → `pp_find(name) != NULL ? 1 : 0`
  - `(expr)` → recursive `pp_eval_expr()`, expect `)`
  - Any remaining identifier (undefined after expansion) → treat as `0`

  All functions skip leading whitespace via a shared `eval_skip_ws()` helper.
  Division/modulo by zero → returns 0 (silent, safe).

  **Entry point:**
  ```c
  /* Macro-expand then evaluate. Returns non-zero for true. */
  static long long pp_eval_if_expr(const char *text)
  {
      char *expanded = pp_expand(text);
      eval_p = expanded;
      long long result = pp_eval_logor();
      free(expanded);
      return result;
  }
  ```

  ---

  ## 3. Add `#if` directive handling

  In the `/* Conditional directives: always processed */` block, add alongside `#ifdef`/`#ifndef`:

  ```c
  else if (strcmp(dir, "if") == 0)
  {
      int outer = currently_emitting();
      int cond  = 0;
      if (outer)
          cond = (pp_eval_if_expr(lp) != 0);
      if (pp_cond_depth >= PP_MAX_COND_DEPTH)
          error("preprocess: #if nested too deeply");
      pp_cond_outer[pp_cond_depth] = outer;
      pp_cond_emit [pp_cond_depth] = outer && cond;
      pp_cond_done [pp_cond_depth] = outer && cond;
      pp_cond_depth++;
  }
  ```

  (Expression evaluation is skipped when outer is not emitting — avoids erroring on dead-code expressions.)

  ---

  ## 4. Add `#elif` directive handling

  In the same `/* Conditional directives */` block alongside `#else`:

  ```c
  else if (strcmp(dir, "elif") == 0)
  {
      if (pp_cond_depth == 0)
          error("preprocess: #elif without matching #if");
      int outer = pp_cond_outer[pp_cond_depth - 1];
      int done  = pp_cond_done [pp_cond_depth - 1];
      int cond  = 0;
      if (outer && !done)
          cond = (pp_eval_if_expr(lp) != 0);
      int taken = outer && !done && cond;
      pp_cond_emit[pp_cond_depth - 1] = taken;
      if (taken) pp_cond_done[pp_cond_depth - 1] = 1;
  }
  ```

  ---

  ## Verification

  ```bash
  make smallcc        # must compile cleanly

  make test_all       # all existing tests must still pass

  # Smoke test 1: basic #if integer
  echo '#if 1
  int main(void){return 42;}
  #else
  int main(void){return 0;}
  #endif' | ./smallcc /dev/stdin > /tmp/t.s && ./sim_c /tmp/t.s
  # expect r0:0000002a

  # Smoke test 2: #if with macro value
  echo '#define VER 2
  #if VER == 1
  int main(void){return 1;}
  #elif VER == 2
  int main(void){return 2;}
  #else
  int main(void){return 3;}
  #endif' | ./smallcc /dev/stdin > /tmp/t.s && ./sim_c /tmp/t.s
  # expect r0:00000002

  # Smoke test 3: defined()
  echo '#define FOO 1
  #if defined(FOO) && !defined(BAR)
  int main(void){return 7;}
  #else
  int main(void){return 0;}
  #endif' | ./smallcc /dev/stdin > /tmp/t.s && ./sim_c /tmp/t.s
  # expect r0:00000007

  # Smoke test 4: #if 0 (dead branch)
  echo '#if 0
  #error "should not be reached"
  #endif
  int main(void){return 5;}' | ./smallcc /dev/stdin > /tmp/t.s && ./sim_c /tmp/t.s
  # expect r0:00000005
  ```


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/498e3cc7-534e-4ed7-a403-8bf92dc6dfa6.jsonl
- `12:48 user` [Request interrupted by user for tool use]
- `12:52 user` please commit
- `12:58 user` I'm currently trying to port coremark, and am hitting an error: smallcc -O2 -Icpu3  -DITERATIONS=0 -DPERFORMANCE_RUN=1 -o ./coremark.bin  core_list_join.c core_main.c core_matrix.c core_state.c core_util.c cpu3/core_portme.c cpu3/ee_printf.c 
  core_list_join.c:109:24: error: Expected 'COMMA' got 'IDENT'
  this is in ../coremark
- `16:05 user` create a minimal stddef.h in ./include
- `16:09 user` create a stdint.h
- `16:11 user` create stdbool.h
- `16:20 user` how difficult to print the source line of an error?
- `16:21 user` please implement
- `16:22 user` Implement the following plan:

  # Plan: Print Source Line on Error

  ## Context
  `src_error` currently emits only `file:line:col: error: message`. Printing the offending
  source line with a caret makes errors much easier to diagnose (like GCC/clang output).

  ## File to Modify
  **`smallcc.c`** — `src_error()` at line 45.

  ## What's Already Available
  - `token_ctx.user_input` — holds the full preprocessed source string (set at line 352,
    before tokenisation, so line numbers match exactly).
  - Every token already carries accurate `line` and `col` fields set by the tokeniser.
  - No new infrastructure needed.

  ## Change
  After printing the error message, walk `user_input` to find the start of the given line,
  then print the line text and a caret row:

  ```c
  void src_error(int line, int col, const char *fmt, ...)
  {
      fprintf(stderr, "%s:%d:%d: error: ",
              token_ctx.filename ? token_ctx.filename : "?", line, col);
      va_list ap;
      va_start(ap, fmt);
      vfprintf(stderr, fmt, ap);
      va_end(ap);
      fprintf(stderr, "\n");

      /* Print the offending source line and a caret */
      if (token_ctx.user_input) {
          const char *p = token_ctx.user_input;
          for (int l = 1; l < line && *p; p++)
              if (*p == '\n') l++;
          const char *end = p;
          while (*end && *end != '\n') end++;
          fprintf(stderr, "    %.*s\n    %*s^\n", (int)(end - p), p, col - 1, "");
      }

      exit(1);
  }
  ```

  ## Verification
  ```bash
  make smallcc
  echo 'int main(){return 5+;}' > /tmp/t.c && ./smallcc /tmp/t.c
  # expect: error line printed with caret under the offending token
  make test_all   # all existing tests must still pass
  ```


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/553ff2ac-79f4-4d10-a00b-2068a2d2e1df.jsonl
- `16:22 user` [Request interrupted by user for tool use]
- `16:28 user` add an option to output the postprocessed input
- `16:34 user` how difficult to use the input source line number, not the postprocessed, in error reporting?
- `16:37 user` Implement the following plan:

  # Plan: Original Source Line Numbers in Error Reporting

  ## Context

  Currently, errors report line/col numbers relative to the **preprocessed** source
  (`token_ctx.user_input`). After `#include` expansion, the line numbers in the
  preprocessed text don't match the original file — e.g. if `stdio.h` contributes 3
  lines, everything after the `#include` shifts by 3. Removed `#ifdef` blocks cause
  the same problem. The fix is to track original-file line/col through preprocessing
  so `src_error` can report `myfile.c:5:10` not `myfile.c:8:10`.

  ## Difficulty: Moderate (~2–3 hours)

  The industry-standard approach is to emit `#line` linemarkers from the preprocessor
  and parse them in the tokeniser. No new data structures needed beyond a couple of
  extra fields; the tricky part is the caret-display in `src_error` (see below).

  ---

  ## Files to Modify

  | File | What changes |
  |---|---|
  | `preprocess.c` | Emit `# N "file"` linemarkers at include-entry, include-exit, and after each consumed conditional directive |
  | `tokeniser.c` | Recognise and consume linemarker lines; update logical line/file state instead of emitting a token |
  | `smallcc.h` | Add `const char *filename` to `Token`; add `logical_*` fields to `TokenContext` |
  | `smallcc.c` | `src_error` uses logical location for header; caret display re-reads the original file |

  ---

  ## Step 1 — `preprocess.c`: emit linemarkers

  Emit `# <line> "<filename>\n"` into the output buffer:

  1. **At the top of `preprocess()`** (before processing any lines): emit `# 1 "<filename>"`.
  2. **When entering an `#include`** (just before recursing): emit nothing — the recursive
     call to `preprocess()` already emits its own `# 1 "<included_file>"` at step 1.
  3. **After returning from `#include`** (after appending the included text): emit
     `# <next_line_in_parent> "<parent_filename>"` to resync the parent file's line number.
  4. **After consuming each `#ifdef`/`#elif`/`#else`/`#endif` directive**: emit
     `# <next_line> "<filename>"` so skipped-block gaps don't shift line numbers.

  The preprocessor already tracks `pp_include_depth` and the current filename is passed
  as a parameter — both are readily available.

  The `# N "file"` lines must be hidden from `-E` output (or passed through, like
  GCC does — either is fine, but stripping them is cleaner for users).

  ---

  ## Step 2 — `tokeniser.c`: parse linemarkers

  In the main tokeniser loop, after skipping whitespace, check for a `#` at
  column 1 followed by a decimal integer. If found:

  ```c
  if (c == '#' && cur_col == 1 && isdigit(peek)) {
      // parse:  # <number> "<filename>"
      // update: cur_line = number;  cur_file = filename (arena_strdup'd)
      // consume the rest of the line; emit NO token
      continue;
  }
  ```

  `cur_file` is a new `const char *` local (or in the tokeniser's per-call state)
  that gets stamped onto each token alongside `cur_line`/`cur_col`.

  ---

  ## Step 3 — `smallcc.h`

  ```c
  struct Token {
      ...
      const char *filename;   // ADD: original source file for this token
  };

  typedef struct TokenContext {
      ...
      // ADD: logical (original) location — updated by tokeniser from linemarkers
      const char *logical_filename;
      int          logical_line;
      int          logical_col;
  } TokenContext;
  ```

  The tokeniser stamps `t->filename = cur_file` on every token it creates.
  `token_ctx.logical_*` mirrors the current token's logical location and is kept
  up-to-date by `advance()` in the parser (or by `src_error` reading
  `token_ctx.current->filename` / `->line`).

  ---

  ## Step 4 — `smallcc.c`: update `src_error`

  ```c
  void src_error(int line, int col, const char *fmt, ...)
  {
      // Use the current token's original filename, not token_ctx.filename
      const char *fname = token_ctx.current && token_ctx.current->filename
                          ? token_ctx.current->filename
                          : (token_ctx.filename ? token_ctx.filename : "?");

      fprintf(stderr, "%s:%d:%d: error: ", fname, line, col);
      va_list ap; va_start(ap, fmt);
      vfprintf(stderr, fmt, ap);
      va_end(ap);
      fprintf(stderr, "\n");

      // Re-read the original source file to show the actual line + caret.
      // (token_ctx.user_input holds preprocessed text; don't use it here.)
      FILE *f = fopen(fname, "r");
      if (f) {
          char buf[1024];
          for (int l = 1; l <= line; l++) {
              if (!fgets(buf, sizeof(buf), f)) { buf[0]='\0'; break; }
          }
          fclose(f);
          // strip trailing newline for display
          buf[strcspn(buf, "\n")] = '\0';
          fprintf(stderr, "    %s\n    %*s^\n", buf, col - 1, "");
      }

      exit(1);
  }
  ```

  The `line`/`col` arguments to `src_error` come from `token_ctx.current->line`
  and `->col` at call sites — those are already set to logical values by the
  tokeniser after Step 2.

  ---

  ## Edge Cases

  - **Errors before any token is parsed** (e.g. file not found): `token_ctx.current`
    may be NULL; fall back to `token_ctx.filename`. Already handled by the guard above.
  - **`#line` markers in `-E` output**: decide whether to pass them through or strip
    them. Stripping is cleaner; just skip emitting them to `out` when `preprocess_only`.
  - **Errors inside macros**: a token expanded from a macro still carries the line of
    the macro invocation in the original file (because the preprocessor expands inline;
    the linemarker before the expansion line is the accurate one).

  ---

  ## Verification

  ```bash
  make smallcc

  # Single-file: line number should match original source
  cat > /tmp/a.c <<'EOF'
  #include <stdio.h>
  int main() {
      int x = ;    /* syntax error — line 3 in original */
      return 0;
  }
  EOF
  ./smallcc /tmp/a.c
  # expect: /tmp/a.c:3:13: error: ...  (not line 6 or whatever after include expansion)

  # #ifdef skipped block: line after #endif should be correct
  cat > /tmp/b.c <<'EOF'
  #ifdef NOPE
  int missing_stuff;
  int more_missing;
  #endif
  int main() { return 0 }   /* error — line 5 in original */
  EOF
  ./smallcc /tmp/b.c
  # expect: /tmp/b.c:5:...

  make test_all   # all existing tests must still pass
  ```


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/cd14eea3-2e31-48d9-b2f0-83a1bd52d155.jsonl
- `16:37 user` [Request interrupted by user for tool use]
- `16:52 user` This code fails: [Pasted text #1 +5 lines]
- `17:03 user` please commit
- `17:08 user` in coremark: [Pasted text #1 +4 lines], memblock in the struct is definitely an array
- `17:31 user` in coremark: [Pasted text #1 +4 lines]
- `17:38 user` perfect!
- `17:42 user` ok, this one tougher to track: [Pasted text #1 +3 lines]
- `17:53 user` This fails: [Pasted text #1 +4 lines] but this works: (dsa) Simons-MacBook-Pro:coremark simonj$ smallcc -Icpu3 -DFLAGS_STR='""' -o out.s core_matrix.c , is the guarded include reset each TU?
- `18:05 user` getting error on "volatile", should just be ignored? [Pasted text #1 +4 lines]
- `18:21 user` create stdarg.h (presumably just a placeholder)
- `18:27 user` Runs out of arena memory, the code is small, so stuck in loop? (dsa) Simons-MacBook-Pro:coremark simonj$ smallcc -O2 -Icpu3 -I. -DFLAGS_STR=\""-O2 -DPERFORMANCE_RUN=1  "\" -DITERATIONS=0 -DPERFORMANCE_RUN=1 -o ./coremark.bin  core_list_join.c core_main.c core_matrix.c core_state.c core_util.c cpu3/core_portme.c cpu3/ee_printf.c
  arena exhausted (used 16777168, requested 136)
- `18:43 user` [Pasted text #1 +4 lines]

## 2026-03-16

- `08:53 user` smallcc -O2 -Icpu3 -I. -DFLAGS_STR=\""-O2 -DPERFORMANCE_RUN=1  "\" -DITERATIONS=0 -DPERFORMANCE_RUN=1 -o ./coremark.bin  core_list_join.c core_main.c core_matrix.c core_state.c core_util.c cpu3/core_portme.c cpu3/ee_printf.c 
  fp op_kind 15 not handled in codegen
- `08:59 user` [Pasted text #1 +4 lines]
- `09:07 user` actually please revert this, coremark provides this function in an additional file
- `09:10 user` coremark cvt.c includes math.h in order to use modf. What is in a minimal math.h?
- `09:14 user` [Pasted text #1 +3 lines]
- `09:17 user` (dsa) Simons-MacBook-Pro:coremark simonj$ smallcc -O2 -Icpu3 -I. -ann -DFLAGS_STR=\""-O2 -DPERFORMANCE_RUN=1  "\" -DITERATIONS=0 -DPERFORMANCE_RUN=1 -o ./coremark.bin  core_list_join.c core_main.c core_matrix.c core_state.c core_util.c cpu3/core_portme.c cpu3/ee_printf.c cpu3/cvt.c 
  (dsa) Simons-MacBook-Pro:coremark simonj$ sim_c coremark.bin 
  undefined symbol: _l-1
- `09:55 user` please implement #error in preprocessor
- `10:03 user` using #include <stdio.h> then putchar(c); in ee_printf.c, but its not using the built-in: (dsa) Simons-MacBook-Pro:coremark simonj$ smallcc -O2 -Icpu3 -I. -ann -DFLAGS_STR=\""-O2 -DPERFORMANCE_RUN=1  "\" -DITERATIONS=1 -DPERFORMANCE_RUN=1 -o ./coremark.bin  core_list_join.c core_main.c core_matrix.c core_state.c core_util.c cpu3/core_portme.c cpu3/ee_printf.c cpu3/cvt.c 
  (dsa) Simons-MacBook-Pro:coremark simonj$ sim_c  coremark.bin 
  undefined symbol: putchar
- `10:08 user` Hmm, I think it should be renamed to not collide with the standard library, and have a minimal implementation that calls the renamed built-in. Ultimately, it will be replaced with real hardware (and simulation thereof)
- `10:19 user` [Pasted text #1 +4 lines] caused by adding ee_printf("Started, %d\n", 1); return 0; as first line of main() in core_main.c
- `10:52 user` Question: was the variadic implementation broken in general before? Why did tests not find a problem?
- `10:55 user` so properly portable code would look at sizeof(int) vs sizeof(long)?
- `10:58 user` please document this as a hack, to be rolled back in future. Properly porting coremark should involve modifying ee_printf.c, not changing our ABI
- `11:01 user` Hmm, still not working and failing in similar way: ee_printf("Started, %d\n", 1); return 0;
- `11:01 user` [Request interrupted by user]
- `11:01 user` [Pasted text #1 +3 lines]
- `11:05 user` [Request interrupted by user]
- `11:06 user` stop. the source for ee_printf is in ../coremark/cpu3/ee_printf.c, its not a macro
- `11:18 user` so the structure of coremark HAS_PRINTF is fundamentally flawed? Or we shouldn't have ee_printf.c as part of our sources?
- `11:22 user` Ok, so now we know the bug is triggered by ee_printf(): [Pasted text #1 +3 lines]
- `11:42 user` Implement the following plan:

  # Plan: Add `adjw imm16` instruction to CPU3 ISA

  ## Context

  `adj imm8` (opcode 0x41) has a signed 8-bit operand, limiting stack adjustments to
  -128..127 bytes. Functions with large local arrays (e.g. `char buf[1026]` in
  `ee_printf`) need allocations beyond this range. The backend currently emits multiple
  `adj -128` chunks as a workaround, which is wasteful (8+ instructions for a 1KB
  buffer) and was the root cause of a stack-corruption bug in coremark.

  The fix is a new `adjw imm16` instruction at opcode 0x89 that performs `sp += imm16`
  with a full signed 16-bit operand, covering all possible local frame sizes in the
  16-bit address space.

  ---

  ## Files to Modify

  | File | Change |
  |---|---|
  | `cpu3/cpu.py` | Add `'adjw': (0x89, 2)` to `G.ptable`; add `elif i == 'adjw': s.sp += imm` handler |
  | `sim_c.c` | Add `{"adjw",0x89,2}` to `itab[]`; add `case 0x89: sp=(uint16_t)(sp+(int16_t)imm16);` |
  | `backend.c` | Emit `adjw N` for `\|N\| > 127`; keep single `adj N` for small values |

  `cpu3/assembler.py` requires **no changes** — format 2 encoding is handled generically
  via `G.ptable` already. Adding `adjw` to the ptable in `cpu.py` is sufficient since
  `assembler.py` imports `G` from `cpu`.

  ---

  ## Step 1 — `cpu3/cpu.py`

  ### ptable entry (after `'ssp': (0x88, 2)`)
  ```python
  'adjw'  :   (0x89, 2),
  ```

  ### Execution handler (after `elif i == 'adj': s.sp += imm`)
  ```python
  elif    i == 'adjw':    s.sp += imm
  ```

  The format 2 fetch path already sign-extends the 16-bit operand via `sexw(imm)` at
  line 304, so negative values work correctly out of the box.

  ---

  ## Step 2 — `sim_c.c`

  ### `itab[]` (after `{"ssp",0x88,2}`)
  ```c
  {"adjw",0x89,2},
  ```

  ### `run_cpu()` switch (after `case 0x88: /* ssp */`)
  ```c
  case 0x89: /* adjw */ sp = (uint16_t)(sp + (int16_t)imm16); break;
  ```

  `imm16` is declared `uint16_t`; the cast to `int16_t` provides the required sign-extension for negative adjustments (allocating locals).

  After adding the case, rebuild sim_c: `cc -O2 -o sim_c sim_c.c -lm`

  ---

  ## Step 3 — `backend.c`

  Replace the multi-adj loop in the `IR_ADJ` case:

  ```c
  case IR_ADJ: {
      int v = p->operand;
      if (v < -128 || v > 127)
          fprintf(asm_out, "    adjw    %d\n", v);
      else if (v != 0)
          fprintf(asm_out, "    adj     %d\n", v);
      break;
  }
  ```

  No new IR opcode needed — `IR_ADJ` already carries the full integer operand; the
  backend chooses between `adj` and `adjw` based on the value.

  ---

  ## Verification

  ```bash
  make smallcc

  # 1. Basic large-local test
  echo 'int main(){char buf[1026]; buf[0]=4; return buf[0];}' > /tmp/t.c
  ./smallcc /tmp/t.c > /tmp/t.s
  grep adjw /tmp/t.s          # should show "adjw -1026"
  ./cpu3/sim.py /tmp/t.s      # should exit with r0=4

  # 2. Ensure small adj still uses adj (not adjw)
  echo 'int main(){int x=5; return x;}' > /tmp/t2.c
  ./smallcc /tmp/t2.c > /tmp/t2.s
  grep -v adjw /tmp/t2.s | grep adj   # should show "adj -2"

  # 3. Full test suite
  make test_all
  ```


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/b60fd2c6-1285-4b73-9fbd-64e8cab45545.jsonl
- `11:42 user` [Request interrupted by user for tool use]
- `11:45 user` CLAUDE.md should be updated to current state of documentation, and note that sim_c.c has superceeded cpu3/sim.py as main simulator, the python kept as reference and to support legacy test infrastructure
- `11:47 user` please commit
- `11:50 user` Ok, its still not working, but rather than debug ee_printf, I'd like you to implement floats in our printf, and revert the va_args hack
- `11:54 user` Implement the following plan:

  # Plan: Add %f to printf and revert variadic 4-byte slot hack

  ## Context

  The variadic 4-byte slot promotion hack was introduced so that `ee_printf` (written for
  a 32-bit host where `int==long==4`) could read args without garbage. Since we're
  abandoning `ee_printf` and implementing float formatting in our own `printf` instead,
  the hack needs to go: natural ABI is restored (int args are 2-byte slots, float/double
  are 4-byte slots since `sizeof(double)==4` on this target), and `printf("%f", x)` works
  because `double` is already 4 bytes and `va_arg(ap, double)` advances by 4 correctly.

  ---

  ## Files to Modify

  | File | Change |
  |---|---|
  | `lib/stdio.c` | Add `_print_long_dec`, `_print_float`, and `%f` case in `printf` |
  | `codegen.c` | Revert variadic slot promotion in `push_args_list`; revert `push_args_list` signature; revert `slot` in `ND_VA_ARG` |
  | `smallcc.h` | Revert `get_function_type` prototype (remove `num_params`); remove `num_params` from `Type.fn` |
  | `types.c` | Revert `get_function_type` definition and its two call sites |

  ---

  ## Step 1 — `lib/stdio.c`: add float printing

  Add two helpers before `printf`, then add a `%f` case:

  ```c
  static void _print_long_dec(long n)
  {
      /* n must be >= 0 */
      if (n >= 10) _print_long_dec(n / 10);
      putchar('0' + (int)(n % 10));
  }

  static void _print_float(double f)
  {
      if (f < 0.0) { putchar('-'); f = -f; }
      long ipart = (long)f;
      double fpart = f - (double)ipart;
      _print_long_dec(ipart);
      putchar('.');
      int i;
      for (i = 0; i < 6; i++) {
          fpart *= 10.0;
          int d = (int)fpart;
          putchar('0' + d);
          fpart -= (double)d;
      }
  }
  ```

  In `printf`'s `switch (*fmt)`, add (after the existing cases, before the closing `}`):

  ```c
  case 102:   // 'f'
  {
      double v = va_arg(ap, double);
      _print_float(v);
      break;
  }
  ```

  `double` is 4 bytes on this target, so `va_arg(ap, double)` advances `ap` by 4 — correct
  natural slot size with no special treatment needed.

  ---

  ## Step 2 — `codegen.c`: revert the hack

  ### 2a. Revert `push_args_list` signature
  ```c
  // Before (hacked):
  static int push_args_list(Node *first_arg, Type *fn_type)

  // After (reverted):
  static int push_args_list(Node *first_arg)
  ```

  ### 2b. Remove hack variables and promote logic

  Remove lines 345–365 (the HACK block comment + `is_variadic`/`num_named` vars).

  In Phase 3, revert to the simple push decision:
  ```c
  // Before (hacked):
  bool variadic_slot = is_variadic && (i >= num_named) && (s < 4);
  if (s == 4 || variadic_slot)
  {
      if (variadic_slot) { /* sign extend ... */ }
      ir_append(IR_PUSH, 0, NULL);
      param_size += 4;
  }
  else
  {
      ir_append(IR_PUSHW, 0, NULL);
      param_size += WORD_SIZE;
  }

  // After (reverted):
  if (s == 4)      { ir_append(IR_PUSH,  0, NULL); param_size += 4; }
  else             { ir_append(IR_PUSHW, 0, NULL); param_size += WORD_SIZE; }
  ```

  ### 2c. Update the 4 call sites (remove `fn_type` argument)

  | Location | Before | After |
  |---|---|---|
  | `gen_callfunction_via_ptr` | `push_args_list(node->ch[0], fn_type)` | `push_args_list(node->ch[0])` |
  | `gen_callfunction_via_deref` | `push_args_list(node->ch[1], fn_type)` | `push_args_list(node->ch[1])` |
  | `gen_callfunction` | `push_args_list(node->ch[0], node->symbol->type)` | `push_args_list(node->ch[0])` |
  | `gen_expr` (member fn call) | `push_args_list(node->ch[1], NULL)` | `push_args_list(node->ch[1])` |

  ### 2d. Revert `ND_VA_ARG` slot size (line ~1132)

  ```c
  // Before (hacked):
  int slot = s < 4 ? 4 : s;  // HACK: 4-byte minimum

  // After (reverted):
  int slot = s;
  ```

  ---

  ## Step 3 — `smallcc.h` + `types.c`: remove `num_params`

  ### `smallcc.h` — Type.fn union
  Remove the `num_params` field:
  ```c
  // Before:
  struct { Type *ret; Param *params; bool is_variadic; int num_params; } fn;
  // After:
  struct { Type *ret; Param *params; bool is_variadic; } fn;
  ```

  Revert the prototype:
  ```c
  // Before:
  Type *get_function_type(Type *ret, Param *params, bool is_variadic, int num_params);
  // After:
  Type *get_function_type(Type *ret, Param *params, bool is_variadic);
  ```

  ### `types.c` — definition and call sites

  Revert the function signature and body (remove `num_params` from matching and assignment).

  Call site 1 — `type_from_direct_decl` (remove `nparams` counting):
  ```c
  // Before:
  int nparams = 0;
  if (s->ch[0] && s->ch[0]->ch[0])
      for (Node *pp = s->ch[0]->ch[0]; pp; pp = pp->next) nparams++;
  t = get_function_type(t, NULL, variadic, nparams);
  // After:
  t = get_function_type(t, NULL, variadic);
  ```

  Call site 2 — `make_basic_types` (builtin `__putchar`):
  ```c
  // Before:
  insert_builtin("__putchar", get_function_type(t_int, p, false, 1));
  // After:
  insert_builtin("__putchar", get_function_type(t_int, p, false));
  ```

  ---

  ## Verification

  ```bash
  make smallcc

  # 1. %f basic
  echo 'int main(){ printf("%f\n", 3.14); return 0;}' > /tmp/tf.c
  ./smallcc /tmp/tf.c > /tmp/tf.s && ./sim_c /tmp/tf.s
  # Expected stdout: 3.140000

  # 2. Negative float
  echo 'int main(){ printf("%f\n", -2.5); return 0;}' > /tmp/tf2.c
  ./smallcc /tmp/tf2.c > /tmp/tf2.s && ./sim_c /tmp/tf2.s
  # Expected stdout: -2.500000

  # 3. Mixed format string
  echo 'int main(){ printf("%d %f\n", 42, 1.5); return 0;}' > /tmp/tf3.c
  ./smallcc /tmp/tf3.c > /tmp/tf3.s && ./sim_c /tmp/tf3.s
  # Expected stdout: 42 1.500000

  # 4. Verify int args still use 2-byte slots (spot-check assembly)
  echo 'int f(int n,...){return 0;} int main(){f(2,10,20);return 0;}' > /tmp/tv.c
  ./smallcc /tmp/tv.c > /tmp/tv.s
  grep pushw /tmp/tv.s   # should show pushw for the int args

  # 5. Full test suite
  make test_all
  ```


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/58c7e7a7-2ffa-4fb2-bdd5-db9e7418aed9.jsonl
- `11:54 user` [Request interrupted by user for tool use]
- `12:03 user` seems to be ignoring the coremark.h line 44: (dsa) Simons-MacBook-Pro:coremark simonj$ smallcc -O2 -Icpu3 -I. -ann -DFLAGS_STR=\""-O2 -DPERFORMANCE_RUN=1  "\" -DITERATIONS=1 -DPERFORMANCE_RUN=1 -o ./coremark.bin  core_list_join.c core_main.c core_matrix.c core_state.c core_util.c cpu3/core_portme.c
  (dsa) Simons-MacBook-Pro:coremark simonj$ sim_c -v coremark.bin 
  undefined symbol: ee_printf
- `12:23 user` printf does not support %lu
- `12:56 user` please update docs an commit
- `13:10 user` something wrong: [Pasted text #1 +3 lines] outputs 65537000
- `13:19 user` bug in coremark, core_main.c, I've added line 222, TOTAL_DATA_SIZE is earlier assigned to results[0].size, but prints: 2000 666
- `14:10 user` ah! sorry, thanks
- `14:19 user` results[0].size gets corrupted in core_list_init(): [Pasted text #1 +9 lines]
- `20:14 user` Make sure that each bug you fixed has a text case, that any changes are documented, that the many useful debug facilities you added to the simulator are documented
- `20:27 user` so the coremark/MHz number is about 0.044. For a simple processor, we should expect at least 1, so about 25x improvement is on the table. Assess the critical loops, examine the generated code, and see if there are obvious optimisation or ISA changes that would help. The output of this plan should be a doc, describing all the above, and the various tradeoffs and difficulties. E.g one possibility is a 2 register form, giving TOS(2), research with forth and jvm shows this gives optimal improvements. Anyway, think hard and write an interesting document
- `21:01 user` Implement the following plan:

  # Plan: CPU3 Performance Analysis Document

  ## Context

  CoreMark/MHz for CPU3 + smallcc is approximately **0.044**, against an industry baseline
  of ~1.0 for simple in-order RISC processors. That 25× gap is the subject of this document.
  The output is `docs/performance.md` — a thorough written analysis covering: what is slow
  and why, quantified ISA opportunities, compiler optimization opportunities, the two-register
  (TOS2) model from Forth/JVM research, and prioritized recommendations with tradeoffs.
  **No code changes — document only.**

  ---

  ## File to Create

  `docs/performance.md`

  ---

  ## Document Structure

  ### 1. Baseline and Motivation

  - What CoreMark measures (list processing, matrix multiply, state machine / CRC, string search)
  - Scoring formula: `iterations / elapsed_seconds`; CLOCKS_PER_SEC = 1,000,000; sim_c counts
    one cycle per instruction
  - Current result: 0.044 CoreMark/MHz; target: ≥1.0 (25× improvement)
  - How to reproduce: `./sim_c coremark/coremark.s`
  - Comparison table: simple RISC (ARM Cortex-M0 ~1.0, RISC-V RV32E ~0.9), JVM HotSpot
    (after JIT ~5–15), Forth (interpreted Forth ~0.1–0.3, native Forth >1.0)

  ---

  ### 2. CPU3 Architecture Recap (brief)

  - 1-register (r0) stack machine; all ALU pops left operand from stack, uses r0 as right
  - Three encoding formats: Format-0 (1 byte), Format-1 (2 bytes, signed 8-bit imm),
    Format-2 (3 bytes, 16-bit imm)
  - `int`/pointer = 2 bytes; `long`/`float`/`double` = 4 bytes
  - No cache, no pipeline, 1 instruction = 1 cycle
  - Measured average encoding density: **1.77 bytes/instruction** (8,319 instructions in
    14,746 bytes of text for coremark.s)

  ---

  ### 3. Instruction Mix Analysis

  Static instruction counts from `coremark.s` (non-data section):

  | Instruction | Count | % | Category |
  |---|---|---|---|
  | push  | 1615 | 19.4% | Stack |
  | lea   | 1448 | 17.4% | Address |
  | immw  | 1046 | 12.6% | Immediate |
  | lw    |  809 |  9.7% | Load |
  | add   |  510 |  6.1% | ALU |
  | ll    |  366 |  4.4% | Load |
  | adj   |  249 |  3.0% | Stack |
  | pushw |  241 |  2.9% | Stack |
  | jl    |  230 |  2.8% | Call |
  | jnz   |  181 |  2.2% | Branch |
  | sw    |  178 |  2.1% | Store |
  | …     |  …   |  …   |  …  |

  Category summary: stack moves 23.6%, address calculation 17.4%, loads 14.9%,
  immediates 12.7%, ALU 13.6%, branches 4.5%, calls/returns 5.4%, stores ~3.5%.

  Key insight: **loading a local variable** (the most common operation in any program) always
  takes 3 instructions: `lea N` + (optional) `push` + `lw`. The `lea+lw` pattern appears
  **641 times** statically; `push+lea+lw` appears **416 times**. Together these account for
  over 3,000 instructions — 36% of the entire program.

  ---

  ### 4. Critical Loop Analysis

  #### 4a. `matrix_mul_matrix` — inner loop

  The matrix operations dominate CoreMark's scoring. The inner loop body is approximately
  71 instructions, plus 12 for the loop counter increment and back-branch, totalling ~83
  instructions per iteration. Key overhead:

  - Each `a[i][j]` access: `lea base`, `push`, stride multiply, `add`, `ll` = ~5–7 instr
  - Loop variable `k++`: `lea -N`, `lw`, `push`, `immw 1`, `add`, `lea -N`, `push`, `sw`
    = 8 instructions for a trivial increment (could be 1 with `inc_local`)
  - The multiply-accumulate `sum += a[i][k] * b[k][j]` is actually the fast part (~4 instr)

  Annotated assembly fragment for `k++`:
  ```asm
  lea     -14      ; address of k
  lw               ; load k
  push             ; save k
  immw    0x0001   ; constant 1
  add              ; k + 1
  lea     -14      ; address again (could reuse from peephole)
  push
  sw               ; store k+1
  ```
  = 8 instructions to increment a local integer.

  #### 4b. `core_list_find` — linked-list traversal

  `while (list && list->info->idx != info->idx)` — memory-indirect load chain:
  ```asm
  ; list != NULL check
  lea     -2       ; &list
  lw               ; list ptr
  jz      exit
  ; list->info->idx
  lea     -2
  lw               ; list
  push
  immw    0x0002   ; offset of .info (pointer field)
  add              ; &list->info
  lw               ; list->info (pointer)
  push
  immw    0x0000   ; offset of .idx
  add
  lw               ; list->info->idx value
  ```
  = 13 instructions just for one side of the comparison. A 32-bit RISC would do this in
  3–4 instructions using register indirect addressing.

  #### 4c. `core_state_transition` — CRC / state machine

  Repeated `switch` dispatch on a small integer. Each case arm re-evaluates the selector:
  ```asm
  ; selector == CORE_STATE_START ?
  lea     -4
  lw               ; selector
  push
  immw    0x0000   ; case value
  eq               ; result in r0
  jnz     Lcase0
  ; selector == CORE_STATE_INVALID ?
  lea     -4
  lw               ; reload selector (!)
  push
  immw    0x0001
  eq
  jnz     Lcase1
  ; ...
  ```
  The selector is reloaded for every case check — 4 instructions (lea+lw+push+immw) per case.
  A `switch` with 7 cases: 28 redundant instructions per dispatch.

  ---

  ### 5. ISA Optimization Opportunities

  Each idea includes: description, encoding, before/after, estimated static savings, and
  estimated runtime savings (static count × average dynamic multiplier ~270×).

  #### 5a. `lw_local N` — combined lea + lw (Format-1, 2 bytes)

  Load a local variable into r0 in one instruction.
  Encoding: opcode + signed 8-bit offset from bp (covers ±127 bytes of locals — enough for
  most functions).

  ```
  Before (5 bytes, 2 instr):   lea -N  [3 bytes]  +  lw  [1 byte]
  After  (2 bytes, 1 instr):   lw_local -N
  ```

  Similarly `ll_local N` for 4-byte locals (long/float).

  **Estimated savings**: 641 (`lea+lw`) + 270 (`lea+ll`) = **911 static instances** → 911
  fewer instructions. Runtime: 911 × 270 ≈ 246,000 instructions per CoreMark iteration.
  Instruction count reduction: ~11%.

  #### 5b. `push_local N` — combined push + lea + lw (Format-1, 2 bytes)

  Push a local variable onto the stack in one instruction.
  This is the operand-fetch pattern for every binary operation.

  ```
  Before (7 bytes, 3 instr):   lea -N + lw + push   [or lea -N + push + lw]
  After  (2 bytes, 1 instr):   push_local -N
  ```

  **Estimated savings**: 416 static instances → 832 fewer instructions (two instructions per
  occurrence). Runtime: ~224,000 fewer instructions per iteration. Combined with 5a, these
  two instructions alone save ~22% of total instruction count.

  #### 5c. `inc_local N` / `dec_local N` — in-place increment of local variable

  The single most impactful instruction for loop performance.

  ```
  Before (8+ instr):  lea -N; lw; push; immw 1; add; lea -N; push; sw
  After  (2 bytes):   inc_local -N
  ```

  Static occurrences of the `immw 1; add; [lea; push;] sw` tail pattern: **47**. Dynamic
  multiplier for loop variables is much higher (the inner loop variable increments on every
  iteration). Estimated savings: 7 × 47 = 329 static, but perhaps 100,000+ runtime
  instructions per iteration.

  #### 5d. `sw_local N` — combined lea + push + sw (store to local)

  The store-to-local pattern after a computation:
  ```
  Before (4 bytes, 3 instr):   lea -N + push + sw
  After  (2 bytes, 1 instr):   sw_local -N
  ```

  Pairs with `push_local` to make assignment loops more compact.

  #### 5e. Combine-and-branch: `beq_local N, imm, L` / `bne_local N, imm, L`

  The dominant loop pattern is: load local, push, load immediate, compare, jump.

  ```
  Before (8–10 instr):  lea -N; lw; push; immw K; eq/ne; jz/jnz L
  After  (1 instr):     beq_local -N, K, L      (3-operand, needs new encoding)
  ```

  This requires a new instruction format (>3 bytes). Alternatively, a 2-byte `bz_local N, L`
  where the branch condition is `local[N] == 0`.

  Encoding challenge: needs 3 operands. Options:
  1. Multi-byte format (5-6 bytes) — still smaller than current 15+ bytes
  2. Split into `cmp_local N, K` (produces flags) + conditional branch — but CPU3 has no flags

  Implementation difficulty: **high** (new format, assembler changes, simulator changes).

  ---

  ### 6. Two-Register (TOS2) Model

  #### 6a. The Problem

  In a pure stack machine, every binary operation costs:
  ```
  push    ; save left operand to memory (1 instr)
  [compute right operand]
  op      ; pop left, compute result (1 instr)
  ```
  Even on a simple `a + b`, two instructions are dedicated purely to stack management.
  On a register machine, `add r0, r1, r2` is one instruction.

  #### 6b. TOS Register Caching

  The classic optimization: keep the top of the expression stack in a register (here, r1)
  rather than in memory. The "stack" pointer only moves when we need more than one saved value.

  In Forth implementations, "TOS in a register" (sometimes called "TOS caching") is universal.
  Chuck Moore's cmForth was among the first to demonstrate that a single cached register
  eliminates ~30–40% of stack traffic in typical Forth code. The optimization is described
  in "Moving Forth" (Brad Rodriguez) and "A Minimal FORTH Compiler and Tutorial" (Jones).

  For JVM: the Hotspot interpreter uses a TOS state machine; the interpreter tracks whether
  a value is "cached in r0" or "on the native stack." A 1997 paper by Ertl and Gregg
  ("The Structure and Performance of Efficient Interpreters") quantifies this at 15–30%
  speedup for typical bytecode.

  #### 6c. TOS2: Two-Register Model

  TOS2 keeps r0 and r1 as the top two stack slots. This matches the common arithmetic
  pattern `a OP b`:

  ```
  Before (TOS0 — pure stack):
    push_local -N    ; push a (1 instr, goes to memory)
    lw_local   -M    ; r0 = b
    add              ; r0 = mem[sp] + r0; sp += 4

  After (TOS2):
    r0 = a           ; r0 is TOS, no memory write
    r1 = b           ; TOS2: second register
    add_2reg         ; r0 = r0 + r1
  ```

  JVM evidence: the "two-slot buffer" model reduces stack traffic by ~50% vs no buffering
  for purely stack-based arithmetic (referenced in "Optimizing Java Bytecode Using the
  Metaheap" and the SOOT framework analyses).

  #### 6d. TOS2 Implementation Difficulty

  1. **ABI change**: calling convention must specify whether r1 is caller-saved or
     callee-saved. Currently there is no concept of r1 in the ABI.

  2. **State tracking in codegen**: the code generator must know at each point whether a
     value is "live in r1" or "on the memory stack." This is a significant compiler change —
     essentially a simple register allocator for r1.

  3. **Spill code**: when r1 is live and a call is made, r1 must be pushed; after the call
     it must be restored. This partially defeats the purpose for call-heavy code.

  4. **Push/pop instruction redesign**: the current `push`/`pop` explicitly move r0 to/from
     memory. A TOS2 architecture needs "shuffle" instructions: `dup` (copy r0→r1), `swap`,
     `drop` (r1→r0), etc. This is the Forth dual-stack model.

  5. **ISA impact**: adds 4–8 new instructions, grows the simulator and assembler.

  **Estimated gain**: 20–30% instruction reduction for arithmetic-heavy kernels like matrix
  multiply. Less benefit for pointer-chasing code (list_find), which is memory-latency bound.

  **Recommendation**: TOS2 is the single highest-leverage architectural change but requires
  re-designing the ABI, compiler backend, and ISA simultaneously. Treat as a v2.0 project.

  ---

  ### 7. Compiler Optimizations (No ISA Changes)

  #### 7a. Better Peephole: Store-Reload Through More Patterns

  Current Rule 9 handles `LEA N; PUSH; …expr…; SW/SB/SL` followed by `LEA N; LW/LB/LL`.
  Extensions:
  - Extend the depth-tracking scan to handle `PUSH; IMMW; ADD` (pointer arithmetic)
  - `IMMW sym; PUSH; …; SW; IMMW sym; LW` — global variable reload elimination
  - `LEA N; PUSH; SW; LEA N; LW` — the specific `a = expr; use(a)` pattern

  Estimated savings: modest (~5%), but these rules are safe and easy to implement.

  #### 7b. Switch Selector Caching

  Instead of reloading the selector expression for each `case`, compute it once and cache
  it in a local variable before dispatch. This is a code-generation fix in `gen_switchstmt`:

  ```c
  // Current: re-evaluates ch[0] for each case comparison
  // Fix: evaluate ch[0] once, store in a temp local, compare against temp

  int tmp_offset = allocate_temp_local(WORD_SIZE);
  gen_expr(selector);
  gen_store(tmp_offset, WORD_SIZE);
  // then each case uses: lea tmp_offset; lw; push; immw caseN; eq; jnz
  ```

  Savings for `core_state_transition`: 7 cases × 4 instructions saved = 28 instructions per
  dispatch. Given the frequency of state transitions in CoreMark, potentially 10–15% total
  savings.

  #### 7c. Peephole: `LEA N; LW; PUSH; LEA N; LW` → `LEA N; LW; PUSH; lw` (reload)

  When the same local is loaded twice in sequence (e.g. `a op a` or `a = a + expr`), the
  second load can use `push` to save r0 and then reload without re-computing the address.
  Actually the more useful rule: after any sequence that ends with `LEA N; LW`, if the next
  instruction is another `LEA N; LW`, replace it with a `push` of the already-held value.

  #### 7d. Constant Propagation Through Locals

  Currently constants assigned to locals are re-loaded as unknowns. A simple single-basic-
  block pass could track `local[N] = const` and substitute `immw K` instead of `lea N; lw`.
  Particularly useful for loop bounds that are compile-time constants.

  ---

  ### 8. Encoding Density Comparison

  | Architecture | Bytes/Instr | Instr/Loop-Iter (typical) | Notes |
  |---|---|---|---|
  | CPU3 (current) | 1.77 | ~83 | Stack machine, no TOS caching |
  | CPU3 with new instr | ~1.5 est | ~55 est | lw_local, push_local, inc_local |
  | ARM Thumb-2 | ~1.6 | ~15–20 | 16/32-bit mixed; 3-addr ops |
  | RISC-V RV32I | 4.0 | ~8–12 | 32-bit fixed; 3-addr; dense in practice |
  | x86-64 | ~3.5 | ~6–10 | CISC; mem operands; complex addressing |
  | JVM bytecode | ~1.2 | ~30–40 | Pure stack, dense encoding |

  CPU3's encoding is actually competitive with JVM bytecode. The problem is instructions
  per operation, not bytes per instruction.

  ---

  ### 9. What a Real 1.0 CoreMark/MHz Processor Does

  A Cortex-M0 (ARMv6-M, in-order, 1-cycle-per-instruction for most ops) achieves ~1.0
  CoreMark/MHz. It has:
  - 3-address load/store: `LDR r0, [r1, #offset]` = 1 instruction for what CPU3 does in 2–3
  - `LDR r0, [r1, r2]` = pointer + index in one instruction
  - `ADDS r0, r0, #1` = increment with result in register, no store
  - Loop branches: `SUBS r1, #1; BNE loop` = 2 instructions for decrement+branch

  The gap in instruction count per operation is the dominant factor, not instruction
  throughput or encoding density.

  ---

  ### 10. Prioritized Recommendations

  | Priority | Change | Effort | Estimated Gain |
  |---|---|---|---|
  | 1 | `lw_local N` + `ll_local N` instructions | Low (new opcode, simulator, codegen) | ~11% instructions |
  | 2 | `push_local N` instruction | Low | ~10% instructions |
  | 3 | Switch selector caching in codegen | Low (compiler only) | ~5–10% for state machine workloads |
  | 4 | `inc_local N` / `dec_local N` | Low | ~5% static, large runtime gain in loops |
  | 5 | Extended store/reload peephole rules | Medium | ~5% |
  | 6 | `sw_local N` (store to local) | Low | ~3–5% |
  | 7 | Combined compare-branch `bne_local` | High (new encoding format) | ~10–15% |
  | 8 | TOS2 two-register model | Very High (ABI + ISA + compiler redesign) | ~20–30% |

  Items 1–4 together: ~25–30% instruction count reduction, bringing score to ~0.055–0.058.
  Items 1–7: plausibly ~40% reduction → ~0.073.
  Items 1–8 (TOS2): plausibly ~60% reduction → ~0.11.

  Reaching 1.0 CoreMark/MHz requires not just fewer instructions, but also matching the
  instruction-per-operation density of 3-address RISC. The fundamental constraint is that
  a pure 1-register stack machine needs at minimum 2 instructions per binary operation,
  while a 2-register or 3-register machine needs 1. This cannot be fixed by peephole rules
  alone — it requires the TOS2 model or equivalent register introduction.

  ---

  ## Verification

  No code is changed. The document can be verified by:
  1. Cross-checking instruction counts against `coremark.s` using `grep -c`
  2. Running annotated compilation of a matrix loop: `./smallcc -ann -O1 ...`
  3. Manually tracing loop bodies in `coremark.s` to confirm stated instruction counts


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/cd312c57-550d-40d2-b0ff-5929c86f48f2.jsonl
- `21:01 user` [Request interrupted by user for tool use]
- `21:07 user` good work. So what about codegen that uses the current IR but targets a 2 or 3 operand risc-like? Can a suitable codegen and peephole recover density of operations per cycle? Again, respond with documentation
- `21:16 user` Implement the following plan:

  # Plan: RISC Backend Codegen Analysis Document

  ## Context

  Following the CPU3 performance analysis (`docs/performance.md`), the user asks whether
  retargeting the existing smallcc IR to a 2- or 3-address RISC ISA — while keeping the
  current IR structure — could recover operation-per-cycle density. This is the natural
  next design question: instead of patching CPU3, could the compiler emit efficient RISC
  code from its existing intermediate form?

  **Output: `docs/risc-backend.md` — documentation only, no code changes.**

  ---

  ## Document to Create

  `docs/risc-backend.md`

  ---

  ## Key Findings from Codebase Exploration

  ### IR Character

  The IR (IRInst, `smallcc.h:528`) is a flat, singly-linked list with:
  - `op` (IROp enum, 51 opcodes), `operand` (int), `sym` (const char*), `line` (int), `next`
  - **Stack-shaped**: `IR_PUSH` saves r0 to memory; binary ops (`IR_ADD` etc.) pop left
    operand from memory stack, use r0 as right operand
  - **No dataflow information**: no liveness, no SSA, no reaching-definition annotations
  - The only dataflow inference is Rule 9's stack-depth counter (not value tracking)

  ### IR-CPU3 Coupling

  The IR is tightly shaped around the CPU3 stack machine:
  - `IR_PUSH` → `push` (sp -= 4; mem[sp] = r0)
  - `IR_ADD` → `add` (r0 = mem[sp] + r0; sp += 4)
  - `IR_LEA -N` → `lea -N` (r0 = bp - N)
  - `IR_LW` → `lw` (r0 = mem16[r0])
  - Stack IS the temporary-value store — there is no register allocation

  This means the IR cannot be naively re-targeted. `IR_ADD` doesn't mean "add two values";
  it means "pop stack and add to r0". A RISC backend must interpret this stack discipline
  and convert it to register operations.

  ### Expression Stack Depth

  The expression stack is shallow in practice:
  - `a + b` (two locals): depth 1 (one push before the add)
  - `a[i]` (array index): depth 2-3 (base, stride, index)
  - `p->field`: depth 1
  - `(a + b) * (c + d)`: depth 2
  - Sethi-Ullman reordering already minimizes depth for commutative ops

  ---

  ## Document Structure

  ### 1. The Question

  Can the existing stack-shaped IR be retargeted to a 2 or 3-address RISC ISA without
  redesigning the IR? If so, does the resulting code approach RISC instruction densities?

  ### 2. The Coupling Problem

  The IR encodes CPU3 stack-machine semantics, not abstract values. Explain:
  - `IR_PUSH` + `IR_ADD` is not "load a, load b, add a b" — it's a protocol
  - A naïve 1:1 IR→RISC translation is impossible (RISC has no stack ops)
  - The backend must "decode" the stack protocol back into register operations
  - This is equivalent to performing register allocation in the backend

  ### 3. Two Approaches to a RISC Backend

  **Approach A — Backend register allocation (keeping current IR as-is)**:
  The backend maintains a "virtual register file" that mirrors the IR expression stack.
  When it sees `IR_PUSH`, instead of emitting `push`, it assigns r0's value to the next
  scratch register. When it sees `IR_ADD`, it knows scratch_reg[top] has the left operand
  and r0 has the right operand → emit `add r0, scratch, r0`.

  **Approach B — Lift to a register-based IR first**:
  Add a new IR layer (e.g., SSA or 3-address IR) between codegen and the backend. The
  existing `gen_ir()` emits the register IR directly. Backend becomes trivially simple.
  This is how LLVM, GCC, and cranelift work.

  ### 4. Approach A in Detail: Backend Stack → Register Decoding

  The backend walks the IR list and maintains a small register file for the expression
  stack. Key transformations:

  | IR sequence | RISC emission | Registers used |
  |---|---|---|
  | `IR_LEA -N; IR_LW` | `LDR r0, [bp, -N]` | r0 |
  | `IR_PUSH` | — (assign r0 → scratch[d]) | r0 = right, scratch[d] = left |
  | `IR_ADD` | `ADD r0, scratch[d], r0` | r0 = result |
  | `IR_LEA -N; IR_LW; IR_PUSH; IR_LEA -M; IR_LW; IR_ADD` | `LDR r1, [bp,-N]; LDR r0, [bp,-M]; ADD r0, r1, r0` | 2 regs |

  For typical expression depths (≤3), 3-4 scratch registers suffice.

  When the virtual stack exceeds available registers, the backend spills to the real memory
  stack — same as today, but only for complex expressions.

  Show the key idioms:
  - `a + b` (locals): 6 CPU3 → 3 RISC
  - `k++`: 8 CPU3 → 2 RISC
  - `p->field`: 5 CPU3 → 2 RISC (needs load-with-offset: `LDR r0, [r1, #offset]`)
  - `a[i]`: 9 CPU3 → 4 RISC

  ### 5. What Peephole Can Recover on the IR Side

  Before the backend's register decoding runs, the existing peephole (and extensions) can
  collapse stack patterns:
  - The existing Rule 9 already eliminates store/reload — a primitive form of this
  - New rules can collapse `PUSH; binary_op` pairs into tagged 2-operand sequences
  - But IR-level peephole cannot assign register names — it can only simplify depth

  The conclusion: IR-level peephole reduces stack depth but cannot complete the
  register-allocation step. That requires the backend.

  ### 6. What RISC ISA Is Needed

  Minimum for efficient retargeting:
  - Load-with-offset: `LDR rd, [rs, #N]` (eliminates `lea + lw` pair)
  - Store-with-offset: `STR rs, [rd, #N]` (eliminates `lea + push + sw`)
  - 3-address ALU: `ADD rd, rs1, rs2`
  - Register-immediate ALU: `ADD rd, rs, #N`
  - Register-relative branch: `BNE rs, rt, label`

  Show a minimal RISC register file for this use case:
  - r0: return value / accumulator
  - r1–r3: expression scratch (caller-saved)
  - r4–r7: loop induction variables etc. (callee-saved)
  - bp: frame pointer (preserved)
  - sp: stack pointer (preserved)

  ### 7. Load-with-Offset is the Critical Instruction

  The single most impactful RISC feature is `LDR rd, [rs, #offset]`. This collapses:
  - Local variable load: `lea; lw` → `LDR r0, [bp, #-N]` (2→1)
  - Parameter load: same
  - Struct field access: `ptr+push+imm+add+lw` → `LDR r0, [r1, #offset]` (5→1)

  Quantify: 641 `lea+lw` + 416 `push+lea+lw` instances = 1,057 patterns, saving
  ~1,500 instructions at current frequencies.

  ### 8. Approach B: Register IR Layer

  Briefly describe what adding a proper 3-address IR would look like:
  - New IR node kinds: `IR_REG_LOAD [bp, -N] → rd`, `IR_REG_ADD rd, rs1, rs2`, etc.
  - `gen_ir()` emits these instead of stack ops
  - Backend is a trivial 1:1 translation
  - Register allocator (e.g., linear scan) maps virtual regs → physical regs
  - This is the path to 1.0+ CoreMark/MHz

  Tradeoff vs Approach A: Approach A reuses existing IR and codegen; Approach B requires
  rewriting gen_ir() (the most complex part of the compiler) but achieves better code.

  ### 9. Expected Instruction Counts with RISC Backend (Approach A)

  | Kernel | CPU3 now | RISC (Approach A) | RISC (Approach B) |
  |---|---|---|---|
  | `a + b` (locals) | 6 | 3 | 2 |
  | `k++` (loop counter) | 8 | 2 | 2 |
  | `p->field` (struct access) | 5 | 2 | 1 |
  | `a[i]` (array) | 9 | 4 | 2 |
  | `switch` (per case, 7 cases) | 28 | 10 | 7 |
  | Matrix inner loop body | ~83 | ~25 | ~15 |

  ### 10. CoreMark/MHz Projection

  - Current: 0.044
  - With RISC Approach A + load-with-offset: ~3-4× improvement → 0.13–0.18 CoreMark/MHz
  - With RISC Approach B (register IR): ~5-7× improvement → 0.22–0.31 CoreMark/MHz
  - Residual gap to 1.0: pointer-chasing (list_find) is memory-access bound, not
    instruction-count bound — the 10-instruction loop still makes 10 memory accesses
  - True 1.0 CoreMark/MHz requires both the RISC backend AND ILP (pipeline, cache)

  ### 11. What Doesn't Change

  Regardless of backend:
  - The front-end (tokeniser, parser, type system) is backend-neutral
  - The IR structure stays the same — only the backend and possibly new IR opcodes change
  - The peephole optimiser continues to run; its rules still apply
  - The switch-selector-caching fix in codegen.c still saves instructions before the backend

  ### 12. Recommendation

  Approach A is the pragmatic path: implement a backend stack-decoder that maps the IR
  expression stack to 3-4 physical registers, add a `LDR rd, [rs, #N]` load-with-offset
  class to the target ISA, and run the existing peephole before the backend pass. Estimated
  3-4× gain with moderate implementation effort.

  Approach B (register IR) is the correct long-term path to match true RISC performance but
  requires redesigning the code generator — the most complex component of the compiler.

  ---

  ## Verification

  No code changes. The document can be verified conceptually by:
  1. Tracing the IR sequence for `a + b` through the proposed backend register decoder
  2. Counting instructions in the RISC column for the matrix inner loop and comparing
     to CPU3 actual count in coremark.s
  3. Cross-referencing the load-with-offset savings against the `lea+lw` count (~641)


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/a9a14e64-fc98-455b-9742-f16e572de906.jsonl
- `21:16 user` [Request interrupted by user for tool use]

## 2026-03-17

- `09:58 user` a different approach, keep the stack IR, add: a) stack to 3addr SSA, b) peephole on SSA, c) backend to risc target. To start with a) can just be within basic block. Later we can move to full SSA with \phi nodes. Write this as a plan doc, it seems like a cleaner separation of concerns, with incremental steps and no disruption of the codegen for stack IR, which has a lot of subtlety. The stack IR allows complete reconstruction of full 3addr SSA
- `10:26 user` Implement the following plan:

  # Plan: docs/cpu4-migration.md

  ## Context

  CPU4 is a new RISC-like ISA that replaces the CPU3 stack-machine target for higher
  instruction density. cpu4/ currently exists as a blank copy of cpu3/. The migration
  document defines what CPU4 will become, specifies the software infrastructure changes
  needed in cpu3/-parallel cpu4/ files and in the shared tools (sim_c, smallcc), and
  describes the new compilation pipeline.

  **Output: `docs/cpu4-migration.md` — documentation only, no code changes.**

  ---

  ## Document Structure

  ### 1. Design Goals for CPU4

  CPU3 is a stack machine: all expression temporaries pass through memory (sp-relative
  push/pop). Measured CoreMark performance shows 85-95% of cycles are memory accesses.

  CPU4 goals:
  - Named general-purpose registers: expression temporaries stay in registers
  - Load-with-offset (`ldr rd, [rs, #N]`): collapse `lea+lw` pairs (the most frequent
    CPU3 idiom) into a single instruction
  - 3-address ALU: `add rd, rs1, rs2` (no push before every binary operation)
  - Compatible calling convention: same frame layout as CPU3 (enter/ret/adj) so that
    cross-arch calls remain possible
  - Same 16-bit address space and memory model as CPU3

  ---

  ### 2. CPU4 Register File (proposed)

  | Register | Width | Role |
  |---|---|---|
  | r0 | 32-bit | Accumulator and return value |
  | r1–r5 | 32-bit | Scratch (caller-saved) |
  | r6–r7 | 32-bit | Preserved (callee-saved, loop variables etc.) |
  | bp | 16-bit | Frame pointer |
  | sp | 16-bit | Stack pointer (grows downward) |
  | lr | 16-bit | Link register |
  | pc | 16-bit | Program counter |
  | H | 1-bit | Halt flag |

  r0 doubles as return value; r1–r5 are expression temporaries assigned by the register
  allocator. SSA expression depth is ≤ 3 for all typical C idioms, so r1–r3 suffice for
  scratch with r4–r5 available for loop induction variables etc.

  ---

  ### 3. Proposed CPU4 Instruction Set

  Encoding has three formats (keeping CPU3-compatible format detection by top 2 bits):

  | Format | Width | Encoding | Used for |
  |---|---|---|---|
  | F0 | 1 byte | `00xxxxxx` | No-operand (ret, halt, putchar, etc.) |
  | FR | 2 bytes | `01xxxxxx rrrrrrrr` | Reg-reg: dst[3:0] + src1[3:0] |
  | FI | 3 bytes | `10xxxxxx rrrrrrrr iiiiiiii iiiiiiii` | Reg + imm16 |
  | FRI | 3 bytes | `11xxxxxx rddssiii iiiiiiii` | 3-register or reg-reg-imm (load/store offset) |

  *(Exact bit assignments TBD — the ISA design is the user's responsibility; this
  document specifies what instructions are needed and why.)*

  **Format F0 — No Operand (same as CPU3 where unchanged):**

  | Opcode | Mnemonic | Semantics |
  |---|---|---|
  | 0x00 | `halt` | H = 1 |
  | 0x01 | `ret` | restore bp/lr from frame; return |
  | 0x1e | `putchar` | sys.stdout.write(chr(r0 & 0xff)) |
  | 0x1f | `blr r0` | lr = pc; pc = r0 (indirect call via r0) |
  | 0x28 | `itof` | r0 = float_bits(float(signed32(r0))) |
  | 0x29 | `ftoi` | r0 = int(float(r0)) |
  | *(float arith from CPU3 unchanged)* | | |

  **Format FR — Register–Register:**

  | Mnemonic | Semantics |
  |---|---|
  | `mov rd, rs` | rd = rs |
  | `add rd, rs1, rs2` | rd = rs1 + rs2 |
  | `sub rd, rs1, rs2` | rd = rs1 − rs2 |
  | `mul rd, rs1, rs2` | rd = rs1 × rs2 |
  | `div rd, rs1, rs2` | rd = rs1 / rs2 (unsigned) |
  | `divs rd, rs1, rs2` | rd = signed(rs1) / signed(rs2) |
  | `mod rd, rs1, rs2` | rd = rs1 % rs2 (unsigned) |
  | `mods rd, rs1, rs2` | signed variant |
  | `shl rd, rs1, rs2` | rd = rs1 << rs2 |
  | `shr rd, rs1, rs2` | rd = rs1 >> rs2 (logical) |
  | `shrs rd, rs1, rs2` | arithmetic shift right |
  | `and rd, rs1, rs2` | bitwise AND |
  | `or rd, rs1, rs2`  | bitwise OR |
  | `xor rd, rs1, rs2` | bitwise XOR |
  | `eq rd, rs1, rs2`  | rd = (rs1 == rs2) ? 1 : 0 |
  | `ne rd, rs1, rs2`  | etc. |
  | `lt rd, rs1, rs2`  | unsigned < |
  | `lts rd, rs1, rs2` | signed < |
  | *(le, gt, ge, les, ges, gts — same pattern)* | |
  | `sxb rd, rs` | sign-extend byte |
  | `sxw rd, rs` | sign-extend word |
  | `fadd rd, rs1, rs2` | FP add |
  | *(fsub, fmul, fdiv, flt, fle, fgt, fge — same)* | |

  **Format FI — Register + Immediate:**

  | Mnemonic | Semantics |
  |---|---|
  | `movi rd, #N` | rd = zero-extend(N) |
  | `moviw rd, #N` | rd = (rd & 0xffff) \| (N << 16) (upper half) |
  | `addi rd, rs, #N` | rd = rs + sext(N) |
  | `ldr rd, [rs, #N]` | rd = mem16[rs + sext(N)] (16-bit load with offset) |
  | `ldrb rd, [rs, #N]` | rd = mem8[rs + sext(N)] |
  | `ldrl rd, [rs, #N]` | rd = mem32[rs + sext(N)] |
  | `str rs, [rd, #N]` | mem16[rd + sext(N)] = rs |
  | `strb rs, [rd, #N]` | mem8 variant |
  | `strl rs, [rd, #N]` | mem32 variant |
  | `cbz rs, label` | if rs == 0: pc = label |
  | `cbnz rs, label` | if rs != 0: pc = label |
  | `b label` | pc = label (unconditional branch) |
  | `bl label` | lr = pc; pc = label (call) |
  | `lea rd, [bp, #N]` | rd = bp + sext(N) (address-of local) |
  | `enter N` | save lr/bp; bp = sp−8; sp −= N+8 (same as CPU3) |
  | `adj N` | sp += sext(N) (local alloc/free; same as CPU3) |
  | `ssp N` | sp = N (startup) |

  **Load/store-with-offset is the critical addition.** It replaces the most frequent
  CPU3 idiom: `lea -N; lw` → `ldr r0, [bp, -N]` (2 instructions → 1).

  ---

  ### 4. cpu4/ Software Structure

  Mirrors cpu3/ exactly:

  | File | Role |
  |---|---|
  | `cpu4/cpu.py` | `ptable` for CPU4 instructions (new opcode assignments) |
  | `cpu4/assembler.py` | Two-pass assembler — identical to cpu3/assembler.py since it reads `ptable` dynamically; can be symlinked or copied unchanged |
  | `cpu4/sim.py` | Python simulator — imports cpu4/cpu.py; thin wrapper identical to cpu3/sim.py |

  Only `cpu4/cpu.py` requires substantive work: define the `ptable` dict with CPU4
  opcodes, formats, and execution handlers.

  ---

  ### 5. sim_c Changes: `-arch cpu3|cpu4`

  `sim_c` gains an `-arch` flag (default `cpu3`). The decode loop is currently a flat
  `switch(op)` — this becomes two switch tables, selected at startup:

  ```
  sim_c [-v] [-arch cpu3|cpu4] file.s
  ```

  Implementation approach:
  - Keep the existing decode loop as `exec_cpu3()`
  - Add `exec_cpu4()` with the new instruction set
  - `main()` dispatches based on `-arch` flag
  - Assembler front-end: `sim_c` currently has a built-in assembler (the 2-pass assembler
    in sim_c.c). The assembler's opcode table (`ptable_c[]`) needs a CPU4 variant.
    Add `ptable_c4[]` and select at runtime.

  The existing `-v` verbose flag and write-watchpoint/crash-dump infrastructure apply
  to both architectures unchanged.

  ---

  ### 6. smallcc Changes: `-arch cpu3|cpu4`

  `smallcc` gains `-arch cpu3|cpu4` (default `cpu3`). The flag controls two things:

  **Preamble**: CPU3 emits `ssp 0x1000; jl main; halt`. CPU4 emits the same (enter/ret
  frame convention is identical).

  **Backend dispatch** after `peephole()`:

  ```
  if arch == cpu3:
      backend_emit_asm(ir_head)       // existing CPU3 backend, unchanged

  if arch == cpu4:
      ssa = lift_to_ssa(ir_head)      // stack IR → 3-addr SSA
      ssa_peephole(ssa)               // SSA-level optimization
      risc_backend_emit(ssa)          // SSA → CPU4 assembly
      free_ssa(ssa)
  ```

  **IR peephole gate**: Rule 9 (store/reload elimination) is gated on `arch == cpu3`.
  Rules 1–8 run for both architectures.

  The assembly syntax for CPU4 uses the same `.text=0 / .data=N / label: / byte/word/long`
  directives as CPU3 — only instruction mnemonics differ. The same assembler framework
  in sim_c handles both.

  ---

  ### 7. Calling Convention for CPU4

  Identical to CPU3:

  ```
  bp+8 + 2*(n-1)   param n
      ...
  bp+8             param 0
  bp+4             return address (lr)
  bp+0             saved bp        ← enter sets bp here
  bp-2             local 0
      ...
  ```

  `enter N` / `ret` / `adj` semantics are identical. This means:
  - The frame layout is unchanged
  - `codegen.c` generates correct frame offsets for CPU4 with no modification
  - The SSA lifter reads `IR_ENTER`, `IR_ADJ`, `IR_LEA` and forwards them directly

  Callee-saved registers (r6, r7): any CPU4 function that uses r6/r7 must save them
  in its prologue and restore in its epilogue. The register allocator in `risc_backend.c`
  is responsible for emitting these save/restores when it assigns r6/r7.

  ---

  ### 8. Migration Steps (in order)

  1. **Define `cpu4/cpu.py`** — write the `ptable` with CPU4 opcodes, formats, handlers
  2. **Copy/symlink `cpu4/assembler.py`** — reuse from cpu3 (no changes needed)
  3. **Copy `cpu4/sim.py`** — update import to `from cpu4 import cpu`
  4. **Add CPU4 decode table to `sim_c.c`** — `ptable_c4[]`, `exec_cpu4()`
  5. **Add `-arch` flag to `sim_c.c`** — dispatch to `exec_cpu3` or `exec_cpu4`
  6. **Implement SSA lift + peephole** (`ssa.c`, `ssa_opt.c`) — see companion plan
  7. **Implement `risc_backend.c`** — SSA → CPU4 assembly
  8. **Add `-arch` flag to `smallcc.c`** — route to new pipeline when `cpu4`
  9. **Test**: `smallcc -arch cpu4 t.c | sim_c -arch cpu4` returns same r0 as CPU3

  ---

  ### 9. What Stays the Same

  | Component | CPU3 | CPU4 |
  |---|---|---|
  | Tokeniser, parser, type system | unchanged | unchanged |
  | codegen.c (stack IR generation) | unchanged | unchanged |
  | IR structure (IRInst, IROp) | unchanged | unchanged |
  | IR peephole Rules 1–8 | runs | runs |
  | IR peephole Rule 9 | runs | skipped |
  | mark_basic_blocks() | runs | runs |
  | Frame layout (enter/ret/adj) | CPU3 | CPU4 identical |
  | Assembly directives (.text, byte, word) | same | same |

  ---

  ### 10. Relationship to docs/risc-backend.md

  `docs/risc-backend.md` analyzed the design space. This document is the implementation
  spec. The three-step pipeline (stack IR → SSA → RISC backend) described there as
  "Approach B" is what is being implemented here for CPU4. CPU3 remains the default
  and is completely unchanged.

  ---

  ## Files to Create

  | File | Action |
  |---|---|
  | `docs/cpu4-migration.md` | This document |
  | `docs/cpu4-isa.md` | Full ISA reference (analogous to docs/isa.md) — to be written when cpu4/cpu.py is defined |


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/1b2937c5-4a5c-4e71-96f3-843a2cafea18.jsonl
- `10:26 user` [Request interrupted by user for tool use]
- `12:53 user` question, does the abi guanantee that items on the stack frame are aligned?
- `14:30 user` Whats missing here? I'm trading irregular encoding for density, a work-in-progress
- `14:30 user` [Request interrupted by user]
- `14:30 user` Whats missing here? I'm trading irregular encoding for density, a work-in-progress [Pasted text #1 +115 lines]
- `14:33 user` this is three operand, you just swap the source order and use the other sense
- `14:37 user` standard in risc to use "or rd, rs, rs" for mov. eq, ne bitwise works for float, or are you thinking about strict iee754 compliance? Good point about addi and lea. Exchange was
- `14:37 user` an idea
- `14:39 user` ok about xpc, agree. So there is a big space of 8 bit instrs that can be filled with hotspot specifics as need arises
- `14:46 user` no space in f2a for lea, unless I drop one of the sign-extend ldx,lwc
- `14:47 user` [Request interrupted by user]
- `14:47 user` I mean lbx,lwx
- `14:47 user` [Request interrupted by user]
- `14:47 user` no space in f2a for lea, unless I drop one of the sign-extend lbx,lwx
- `14:51 user` ok, its gone in 3c as "lea     rx = bp + imm16", overkill, but this is going to be an iterative process
- `14:54 user` Ok, 63 instructions is quite small, I'll paste the revised, can you update the cpu5-migration doc with this please? Don't really care about opcode assignments: [Pasted text #2 +99 lines]
- `15:01 user` please add a note that the bp-relative load/store implicitly assume that stack frame items are aligned
- `15:08 user` Hmm, scaled access is also going to have to zero bottom one or two bits of bp,  not completely straightforward access generation
- `15:09 user` thats a clean solution, yes, please add
- `17:01 user` how much are the sign extend instructions likely to be used if there are sign extend load variants
- `17:04 user` there is space in f3b to add sign extend versions, which would then be symmetric with f2a
- `17:11 user` I think I'm going to add an escape to f1 that is single reg op: [Pasted text #3 +105 lines]

## 2026-03-18

- `13:55 user` I've updated cpu4/cpu.py to implement the new isa. Please see if there are any obvious bugs, check with me, they may be intentional
- `14:24 user` please check again
- `14:34 user` bug 2, its an or, doesn't need masking
- `14:35 user` ok, please use this as a reference, and update assembler.py and sim.py to work with a sample test.s, also updated for the new isa
- `14:41 user` [Request interrupted by user]
- `14:41 user` stop
- `14:50 user` Implement the following plan:

  # Plan: cpu4/assembler.py + cpu4/sim.py + cpu4/test.s

  ## Context

  cpu4/cpu.py now has the correct ISA (F1a 2-apart spacing fixed by user). The assembler
  is a stale copy of cpu3/assembler.py with the wrong ptable interface and no multi-operand
  parsing. sim.py is a verbatim copy of cpu3/sim.py with one latent bug (`State.__repr__`
  references `self.r0`..`self.r7` which don't exist — only `self.r[0..7]`).

  Task: rewrite assembler.py for the CPU4 7-format ISA, fix the `__repr__` crash in cpu.py,
  and create a test.s that exercises all major formats and returns a known value in r0.

  ---

  ## Files to modify

  | File | Change |
  |---|---|
  | `cpu4/cpu.py` | Fix `State.__repr__`: `self.r0` → `self.r[0]` etc. (line 335-336) |
  | `cpu4/assembler.py` | Full rewrite for CPU4 multi-operand parsing + 7-format encoding |
  | `cpu4/sim.py` | Verify imports only (no changes needed) |
  | `cpu4/test.s` | New file: sample program exercising F0/F1a/F2/F3a/F3b/F3c |

  ---

  ## assembler.py design

  ### Line syntax

  ```
  [label:]  mnemonic  [op1[, op2[, op3]]]
  ```

  Commas separate operands. Labels are `word:` at start of token. Section directives are
  `.text=N`, `.data=N`. Comments start with `;`.

  ### Operand types

  - Register: `r0`–`r7` → integer 0–7
  - Immediate: decimal, hex `0x...`, or label name (resolved in assemble pass)

  ### Encoding per format (derived from cpu4/cpu.py decode logic)

  | Format | Bytes | Encoding | Assembler ops |
  |---|---|---|---|
  | F0 | 1 | `first_byte` | — |
  | F1a | 2 | `byte0 = first_byte\|(rd>>2); byte1 = ((rd&3)<<6)\|(rx<<3)\|ry` | rd, rx, ry |
  | F1b | 2 | `byte0 = 0x7e\|(rd>>2); byte1 = ((rd&3)<<6)\|subop` | rd |
  | F2 | 2 | `byte0 = first_byte\|(rx>>1); byte1 = ((rx&1)<<7)\|(imm7&0x7f)` | rx, imm7 |
  | F3a | 3 | `[first_byte, imm16>>8, imm16&0xff]` | imm16 or label |
  | F3b | 3 | `byte1 = (rx<<5)\|(ry<<2)\|((imm10>>8)&3); byte2 = imm10&0xff` | rx, ry, imm10 or label |
  | F3c | 3 | `byte0 = first_byte\|(rd&7); byte1,byte2 = imm16 big-endian` | rd, imm16 |

  **F3b branches** (beq/bne/blt/bgt/blts/bgts): imm10 is PC-relative; assembler computes
  `(target_addr - (instr_addr + 3)) & 0x3ff`.

  ### Pseudo-ops

  | Pseudo | Expands to |
  |---|---|
  | `mov rd, rx` | `or rd, rx, rx` |
  | `ble rx, ry, L` | `bgt ry, rx, L` |
  | `bge rx, ry, L` | `blt ry, rx, L` |
  | `bles rx, ry, L` | `bgts ry, rx, L` |
  | `bges rx, ry, L` | `blts ry, rx, L` |

  ### Format detection

  Read `(first_byte, extra_bytes, subfmt, subop)` from `G.ptable[ins]`.
  Derive format from `(extra, subfmt, first_byte & 0xc0)`:
  - extra=0 → F0
  - extra=1, subfmt=0, top2=01 → F1a
  - extra=1, subfmt=1 → F1b
  - extra=1, subfmt=0, top2=10 → F2
  - extra=2, subfmt=0 → F3a
  - extra=2, subfmt=1 → F3b
  - extra=2, subfmt=2 → F3c

  ---

  ## test.s

  Exercises F0 (halt/ret), F1a (add), F2 (sw/lw), F3a (ssp/jl/enter), F3b (beq/llw),
  F3c (immw/lea). Expected result: **r0 = 60**.

  ```asm
  .text=0
      ssp     0x1000              ; F3a: set stack pointer
      jl      main               ; F3a: call main
      halt                       ; F0:  stop

  main:
      enter   8                  ; F3a: save frame, allocate 8 bytes for locals
      immw    r1, 10             ; F3c: r1 = 10
      immw    r2, 20             ; F3c: r2 = 20
      add     r0, r1, r2        ; F1a: r0 = 30
      sw      r0, -1             ; F2:  mem[bp-2] = r0 (imm=-1 scaled*2 = -2)
      lw      r3, -1             ; F2:  r3 = mem[bp-2] = 30
      beq     r0, r3, same       ; F3b: branch if r0==r3 (taken)
      immw    r0, 99             ; skipped
  same:
      lea     r4, -2             ; F3c: r4 = bp - 2
      llw     r5, r4, 0          ; F3b: r5 = mem16[r4+0] = 30
      add     r0, r0, r5         ; F1a: r0 = 30 + 30 = 60
      ret                        ; F0:  return
  ```

  ---

  ## Verification

  ```bash
  cd cpu4
  python3 sim.py test.s -v
  ```

  Expected final state: `r0:0000003c` (60 = 0x3c).

  ---

  ## State.__repr__ fix (cpu.py line 335-336)

  Change `self.r0, self.r1, ...self.r7` → `self.r[0], self.r[1], ...self.r[7]`.


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/be085dd2-b024-46cf-bc97-13584115deee.jsonl
- `14:50 user` [Request interrupted by user for tool use]
- `16:37 user` please commit, then update sim_c
- `16:57 user` now move on to implementing the migration plan for smallcc
- `17:48 user` please commit
- `17:50 user` what is the status of hte bash tests? I thought we were moving entirely to the pytest setup?
- `17:52 user` remove bash suite, make pytest canonical, fix the putchar expectation
- `18:05 user` does the test harness run both arches?
- `18:06 user` yes
- `18:36 user` crt0.s works for either arch because the instrs happen to have same names, in future, the memory loop will likely not. Make crt0_cpu3.s and crt0_cpu4.s, selected by the -arch flag
- `18:39 user` simulator outputs loads of WRITE status messages
- `19:47 user` describe the debugging diagnostics built into the sim
- `19:50 user` I'm attempting to run coremark with cpu4. compiles fine, loops outputting WRITE diagnostics: [Pasted text #1 +18 lines]
- `20:03 user` [Request interrupted by user]
- `20:36 user` tell me how you compiled and ran coremark
- `20:40 user` when I compile with smallcc -Icpu3 -I. -arch cpu4 -ann -DFLAGS_STR=\""-O2 -DPERFORMANCE_RUN=1  "\" -DITERATIONS=5 -DPERFORMANCE_RUN=1 -o ./coremark.s  core_list_join.c core_main.c core_matrix.c core_state.c core_util.c cpu3/core_portme.c, and run with sim_c -arch cpu4 coremark.bin, I get: [Pasted text #1 +28 lines]
- `21:38 user` working! please commit
- `21:40 user` so running with -O2, we get 0.05 coremark/MHz
- `21:44 user` Oh! CLOCKS_PER_SEC is 1000000! That means the its 0.5 cm/MHz
- `21:48 user` remember, the simulator is essentially a perfect cache, perfect single issue processor. But actually this is very good performance with no tuning. Ah, now I've run again the cpu3 arch, and its 0.45 cm/MHz, so not nearly as bad as I thought. I was sure I'd set clock to 10MHz. Well, that means there is plenty of scope to get really quite good numbers
- `21:50 user` think about profiling tools for the simulator, so I can really see the hot loops and whats happening. Given we have assembler already annotated with source, additional output from sim of counts and percentages per line would be great
- `22:02 user` Implement the following plan:

  # Plan: Profiling support in sim_c

  ## Context

  The coremark run on CPU4 shows 0.5 cm/MHz — good for untuned codegen, but there is clear
  headroom. To guide optimisation work (register allocation, instruction selection, etc.)
  we need to know which instructions are hot. The sim_c already counts cycles; the assembly
  output already carries source annotations (`-ann` flag). Adding a `-profile` mode that
  counts executions per instruction address and correlates them back to source lines will
  show hot loops and enable data-driven improvement.

  ---

  ## Files to modify

  | File | Change |
  |---|---|
  | `sim_c.c` | Add `-profile` flag, per-address execution counters, source-line mapping, and `print_profile()` output |

  No other files need changing.

  ---

  ## Design

  ### New global state (near top of sim_c.c, after `g_cycles`)

  ```c
  static bool      g_profile   = false;
  static uint32_t  prof_count[65536];   /* execution count per PC address */
  static int16_t   prof_lineno[65536];  /* addr → source line index (-1 = none) */
  static char    **prof_lines  = NULL;  /* original source line strings (malloc'd) */
  static int       prof_nlines = 0;
  ```

  `prof_lineno` uses `int16_t` (65 536 × 2 = 128 KB). `prof_count` is 256 KB.
  Both are only used when `-profile` is active; they are zero-initialised by default.
  `prof_lines` is a `malloc`'d array of `strdup`'d source lines, built once before assembly.

  ### Step 1 — source-line splitting (new helper, called from `main` before assembly)

  ```c
  static void prof_split_lines(const char *src);
  ```

  Iterates through `src`, splits on `\n`, stores one `strdup`'d copy per line in
  `prof_lines[0..prof_nlines-1]`.  Lines are stored **before** comment-stripping so the
  full original text (including `;` annotations from `-ann`) is preserved for display.

  ### Step 2 — address→line mapping during assembly (pass 2 only)

  Both `assemble()` and `assemble_cpu4()` already loop over lines with a running `cur`
  address. Add a `lineno` counter (initialised to 0 at the start of pass 2) that increments
  for **every** source line, including blanks and comments, so it stays in sync with
  `prof_lines[]`. Skip the `prof_lineno` update for blank/comment lines (where `cur` does
  not advance) and for labels (which emit no bytes).

  At the point where `cur` is about to advance (i.e. just before `write8/write16/write32`
  calls for the instruction bytes in pass 2), record:

  ```c
  if (g_profile && pass == 2)
      prof_lineno[cur] = (int16_t)lineno;
  ```

  Because `cur` can advance by 1, 2, or 3 bytes per instruction, only the **first** byte
  address of each instruction gets the lineno (which is what we want for per-instruction
  profiling).

  ### Step 3 — count increments in execution loops

  In both `run_cpu()` and `run_cpu4()`, immediately after `uint16_t oldpc = pc` (and
  before `pc++`), add:

  ```c
  if (g_profile) prof_count[oldpc]++;
  ```

  This keeps the hot path branchless in normal mode (compile-time `g_profile` is not
  constant, but the branch predictor should handle it cleanly for long runs).

  ### Step 4 — `print_profile()` output

  Called at the end of `main()` when `g_profile` is set. Output goes to **stderr** so it
  doesn't mix with the simulated program's stdout.

  Two sections:

  **Section A — top 40 hottest instructions** (sorted by count descending):

  ```
  Profile: 11,095,638 instructions executed

  Top 40 hot instructions:
    count     %total   addr  source
    827561     7.46%   00e2  llw r0, r0, 0
    814212     7.34%   01a4  lw r0, -3
    ...
  ```

  Collect all `(count, addr)` pairs where `prof_count[addr] > 0`, qsort by count
  descending, print up to 40.

  **Section B — annotated assembly listing**:

  Iterate `lineno` from 0 to `prof_nlines-1`. For each line:
  - Find the first address `a` where `prof_lineno[a] == lineno` (pre-build a reverse
    `int lineno2addr[prof_nlines]` array, initialised to -1, filled during `print_profile`
    from a single sweep of `prof_lineno`).
  - If that address has `prof_count[a] > 0`, print `count  percent  addr  | line_text`
  - Otherwise print `              |  line_text` (blank count column, preserving context)
  - Lines with no matching address and zero surrounding counts can be suppressed to keep
    the output manageable; a simple heuristic: suppress blank lines and comment-only lines
    that are not adjacent to an executed instruction. OR just print everything — for coremark
    the assembly is ~12 K lines which is fine for file output.

  Example:
  ```
               |_l21:
    500000  4.51%  0124  |     lw r0, 4
    500000  4.51%  0126  |     llb r0, r0, 0
    500000  4.51%  0127  |     jz _l22
    500000  4.51%  0128  |     adjw -4
               |; --- bb3 ---
               |; return n * fact(n - 1);
    450000  4.06%  012c  |     lea r0, 2
  ```

  This mirrors `perf annotate` and makes hot loops immediately visible, especially when
  the assembly was compiled with `-ann` (the C source comments appear inline).

  ### Step 5 — `-profile` flag in `main()`

  ```c
  else if (strcmp(argv[i], "-profile") == 0) g_profile = true;
  ```

  Update usage string. Call `prof_split_lines(src)` before `assemble()`/`assemble_cpu4()`,
  and call `print_profile()` after `run_cpu()`/`run_cpu4()`.

  ---

  ## Memory overhead summary

  | Array | Size | Active |
  |---|---|---|
  | `prof_count[65536]` | 256 KB | only when `-profile` |
  | `prof_lineno[65536]` | 128 KB | only when `-profile` |
  | `prof_lines[]` | ≤ source file size | only when `-profile` |

  Total < 600 KB extra; negligible.

  ---

  ## Verification

  ```bash
  # Build
  make sim_c

  # Quick test (use factorial or any annotated output)
  echo 'int fact(int n){if(n<=1)return 1;return n*fact(n-1);}
  int main(){return fact(10);}' > /tmp/t.c
  ./smallcc -ann -arch cpu4 -o /tmp/t.s /tmp/t.c
  ./sim_c -arch cpu4 -profile /tmp/t.s 2>/tmp/t.prof
  cat /tmp/t.prof   # should show recursive fact hot-loop with C annotations

  # Coremark profile
  ./sim_c -arch cpu4 -profile /tmp/cm4_fixed.s 2>/tmp/cm4.prof
  head -60 /tmp/cm4.prof   # top 40 hottest + start of annotated listing

  # Regression: all existing tests still pass
  make test
  ```


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/51500f0b-b6f3-4175-9d75-dd3dea6e5b12.jsonl
- `22:02 user` [Request interrupted by user for tool use]

## 2026-03-19

- `08:24 user` think carefully about separation of concerns for ssa.c ssa_opt.c rsic_backend.c. It seems that allocating registers in ssa.c is pre-empting better register allocation, and this should be a pure SSA ouput, with a completely separate physical regster allocation/optimisation pass
- `08:41 user` Implement the following plan:

  # Plan: Separate SSA construction from physical register allocation

  ## Context

  `lift_to_ssa()` in `ssa.c` simultaneously converts stack-based IR to 3-address form AND
  assigns CPU4 physical registers (r0-r7). The physical register is chosen as `depth + 1`,
  baked directly into `SSAInst.rd/.rs1/.rs2` during construction. By the time `ssa_peephole`
  and `risc_backend_emit` see the SSA list, physical registers are already locked in and
  there is no opportunity for a smarter allocator.

  The fix is a clean pipeline with three separated concerns:

  1. **`ssa.c`** — pure virtual-register SSA construction (uses virtual regs ≥ `VREG_START`)
  2. **`ssa_opt.c`** _(new)_ — SSA-level optimisations on virtual registers (copy prop, future DCE)
  3. **`regalloc.c`** _(new)_ — maps virtual → physical registers; swappable for future improvement
  4. **`risc_backend.c`** — unchanged; receives physical registers as today

  ---

  ## Virtual Register Convention

  ```
  VREG_START = 8

  virtual reg for depth d = VREG_START + d
    VREG_START+0 = v8   →  accumulator (physical r0)
    VREG_START+1 = v9   →  depth-1 scratch (physical r1)
    VREG_START+2 = v10  →  depth-2 scratch (physical r2)
    ...

  Special encodings (unchanged):
    -2 = bp-relative addressing  (not a register)
    -1 = no destination
    0..7 = physical registers    (only present after regalloc)
  ```

  Choosing fixed-per-depth virtual regs (Option A, not fresh-per-definition) because:
  - No liveness analysis required for the initial trivial allocator
  - `flush_for_call_n` spill/restore round-trips transparently (same virtual reg ID in
    STORE before call and LOAD after)
  - Sethi-Ullman bound ≤ 3 means ≤ 4 virtual regs are ever simultaneously live
  - A smarter allocator (linear scan, graph coloring) can replace `regalloc.c` later
    without touching `ssa.c` at all

  ---

  ## New Pipeline

  ```
  gen_ir()               →  IRInst list (stack-based IR)
  mark_basic_blocks()
  peephole(opt_level)

  [CPU4 only:]
  lift_to_ssa(ir_head)   →  SSAInst (rd/rs1/rs2 ∈ {-2,-1} ∪ [VREG_START..])
  ssa_peephole(ssa)      →  optimize on virtual regs       [ssa_opt.c]
  regalloc(ssa)          →  SSAInst (rd/rs1/rs2 ∈ {-2,-1} ∪ [0..7])  [regalloc.c]
  risc_backend_emit(ssa) →  CPU4 assembly                  [unchanged]
  free_ssa(ssa)
  ```

  ---

  ## Files

  | File | Action | Change summary |
  |---|---|---|
  | `ssa.h` | Modify | Add `VREG_START 8`; update `rd/rs1/rs2` comments; declare `regalloc()` |
  | `ssa.c` | Modify | Emit virtual regs in `lift_to_ssa`; remove `ssa_peephole` body |
  | `ssa_opt.c` | **Create** | `ssa_peephole()` moved here; future SSA optimisations |
  | `regalloc.c` | **Create** | `regalloc()` — trivial `physical = vreg - VREG_START` |
  | `smallcc.c` | Modify | Insert `regalloc(ssa)` call between peephole and risc_backend_emit |
  | `Makefile` | Modify | Add `ssa_opt.c regalloc.c` to the `smallcc` source list |
  | `risc_backend.c` | **No change** | Already works with physical regs; unchanged |

  ---

  ## `ssa.h` changes

  ```c
  /* Add near top: */
  #define VREG_START 8   /* virtual regs >= VREG_START; physical regs 0-7 */

  /* Update SSAInst field comments:
     int rd;   // VREG_START+ = virtual (pre-regalloc), 0-7 = physical (post), -1 = none, -2 = bp-rel
     int rs1;  // same encoding
     int rs2;  // same encoding
  */

  /* Add declaration: */
  void regalloc(SSAInst *head);
  ```

  ---

  ## `ssa.c` changes — `lift_to_ssa()`

  Every site that writes a physical register number into `rd`, `rs1`, or `rs2` must change.
  All changes are mechanical: replace the physical constant with a virtual-reg expression.

  | Location | Field | Before | After |
  |---|---|---|---|
  | IR_IMM, IR_LEA, loads, ALU results | `s->rd` | `0` | `VREG_START + 0` |
  | IR_PUSH/PUSHW: accumulator source | `s->rs1` | `0` | `VREG_START + 0` |
  | IR_PUSH/PUSHW: scratch dest | `s->rd` | `vs_depth + 1` | `VREG_START + vs_depth + 1` |
  | IR_PUSH/PUSHW: vs_reg[] entry | `vs_reg[vs_depth]` | `vs_depth + 1` | `VREG_START + vs_depth + 1` |
  | Binary ALU ops: destination | `s->rd` | `0` | `VREG_START + 0` |
  | Binary ALU ops: rs2 (accumulator) | `s->rs2` | `0` | `VREG_START + 0` |
  | Unary ALU ops: rd and rs1 | both | `0` | `VREG_START + 0` |
  | Stores: value source | `s->rs1` | `0` | `VREG_START + 0` |
  | IR_SW/SB/SL: address base | `s->rd` | `vs_reg[depth]` | unchanged (already in vs_reg[]) |
  | IR_ALU rs1 (popped value) | `s->rs1` | `vs_reg[depth]` | unchanged (already in vs_reg[]) |

  **`flush_for_call_n`**: No logic changes. `vs_reg[k]` now holds virtual reg IDs instead
  of physical; the STORE (`rs1 = vs_reg[k]`) and LOAD (`rd = spill_reg[k]`) still work
  because `spill_reg[k]` is set from `vs_reg[k]` and the same virtual reg ID round-trips
  through the spill and restore.

  **`ssa_peephole`**: Remove body from `ssa.c` (it moves to `ssa_opt.c`). The function
  declaration remains in `ssa.h`.

  ---

  ## `ssa_opt.c` (new file)

  ```c
  /* ssa_opt.c — SSA-level optimizations (operate on virtual registers,
   * before physical register allocation). */
  #include "ssa.h"

  void ssa_peephole(SSAInst *head) {
      for (SSAInst *p = head; p; p = p->next) {
          /* Kill identity moves: MOV vN, vN */
          if (p->op == SSA_MOV && p->rd == p->rs1 && p->rd >= 0)
              p->op = (SSAOp)-1;
      }
  }
  ```

  Future passes to add here: copy propagation, constant propagation, DCE.

  ---

  ## `regalloc.c` (new file)

  ```c
  /* regalloc.c — Physical register allocation for the CPU4 RISC backend.
   *
   * Input:  SSAInst list with virtual regs (rd/rs1/rs2 >= VREG_START=8).
   * Output: Same list, virtual regs replaced by physical regs (0-7) in-place.
   *         Special values -1 and -2 are passed through unchanged.
   *
   * Initial strategy: trivial depth-based mapping.
   *   physical(vreg) = vreg - VREG_START
   * Produces identical assembly to the old implicit allocation.
   * Sethi-Ullman depth ≤ 3 guarantees no interference conflicts; r0-r3 suffice.
   *
   * To improve: replace alloc_reg() with a linear-scan or graph-coloring
   * implementation without touching ssa.c or risc_backend.c. */
  #include "ssa.h"

  static int alloc_reg(int vreg) {
      if (vreg < VREG_START) return vreg;   /* -2, -1, or already physical */
      return vreg - VREG_START;
  }

  void regalloc(SSAInst *head) {
      for (SSAInst *p = head; p; p = p->next) {
          if (p->rd  >= VREG_START) p->rd  = alloc_reg(p->rd);
          if (p->rs1 >= VREG_START) p->rs1 = alloc_reg(p->rs1);
          if (p->rs2 >= VREG_START) p->rs2 = alloc_reg(p->rs2);
      }
  }
  ```

  ---

  ## `smallcc.c` — CPU4 dispatch change

  ```c
  /* Before: */
  SSAInst *ssa = lift_to_ssa(codegen_ctx.ir_head);
  ssa_peephole(ssa);
  risc_backend_emit(ssa);
  free_ssa(ssa);

  /* After: */
  SSAInst *ssa = lift_to_ssa(codegen_ctx.ir_head);
  ssa_peephole(ssa);    /* ssa_opt.c: optimize on virtual regs */
  regalloc(ssa);        /* regalloc.c: virtual → physical regs */
  risc_backend_emit(ssa);
  free_ssa(ssa);
  ```

  ---

  ## Implementation Order

  Each step leaves the codebase building and (where noted) all tests passing:

  1. **Add `VREG_START` to `ssa.h`; add `regalloc()` declaration** — inert define, safe
  2. **Create `ssa_opt.c`** with `ssa_peephole` moved from `ssa.c`; update Makefile — tests pass
  3. **Create `regalloc.c`** with trivial allocator; add to Makefile — not yet called, tests pass
  4. **Modify `ssa.c`** to emit virtual regs — tests break (risc_backend receives virtuals)
  5. **Wire `regalloc()` into `smallcc.c`** — tests pass again, assembly identical to before

  Steps 4 and 5 go together in one commit.

  ---

  ## Verification

  ```bash
  # Capture assembly BEFORE refactoring
  echo 'int main(){int a=3,b=4; return a*b+a;}' > /tmp/t.c
  ./smallcc -arch cpu4 -o /tmp/before.s /tmp/t.c

  # Apply changes, rebuild
  make smallcc

  # Capture assembly AFTER refactoring
  ./smallcc -arch cpu4 -o /tmp/after.s /tmp/t.c
  diff /tmp/before.s /tmp/after.s   # must be empty

  # Run full test suite
  make test   # all 469 tests must pass

  # Also test the CPU3 path is unaffected
  ./smallcc -o /tmp/before3.s /tmp/t.c
  # (should still work identically)
  ```


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/b83bc44a-3ec4-4294-98f5-381e2751f544.jsonl
- `08:41 user` [Request interrupted by user for tool use]
- `08:48 user` question: the ssa is still based on basic blocks only, there is no phi function
- `08:53 user` ok, write a document analysing possible optimisations for cpu4 code generation, given the current IR
- `09:13 user` Implement the following plan:

  # Plan: CPU4 Optimisation Opportunities Document

  ## Context

  The CPU4 backend pipeline is:

  ```
  codegen.c → IR list → [peephole (optimise.c)] → lift_to_ssa() → ssa_peephole() → regalloc() → risc_backend_emit()
  ```

  `optimise.c` runs 9 rules on the stack IR (O1/O2), but several of those rules are gated on
  `g_target_arch == 3` and do not run for CPU4. The SSA-level pass (`ssa_opt.c`) currently only
  eliminates identity moves. The backend (`risc_backend.c`) emits correct but suboptimal code for
  several common patterns. The task is to write `docs/optimisations.md` — a reference document
  cataloguing all identified opportunities, grouped by where in the pipeline the fix lives.

  ## Output

  Create `docs/optimisations.md`.

  No code changes — documentation only.

  ## Document Structure

  ### 1. Current Pipeline (brief recap)

  Pipeline diagram. List what optimisations already exist and where they live:
  - optimise.c rules 1-9 (stack IR level)
  - ssa_opt.c: identity-move elimination only
  - risc_backend.c inline: skip adjw 0; skip identity MOV

  ### 2. Missed Opportunities — Grouped by Fix Location

  #### A. Stack-IR peephole (`optimise.c`) — easy wins

  **A1. Store/reload elimination (Rule 9) is gated on CPU3 only**
  Rule 9 pattern: `LEA N; PUSH; <expr>; SW/SB/SL … LEA N; LW/LB/LL` → kill the reload.
  This rule exists but `g_target_arch == 3` guards it. Remove/generalise the guard so it also
  fires for CPU4. Payoff: one load eliminated per compound-assign/store-then-read pattern.

  #### B. SSA-level peephole (`ssa_opt.c`) — medium wins

  **B1. SSA_ADJ merging**
  `peephole(optimise.c)` merges adjacent `IR_ADJ; IR_ADJ` (rule 4). After `lift_to_ssa`,
  `flush_for_call_n` and scope management can emit adjacent `SSA_ADJ` nodes that aren't
  re-merged. Add a pass (loop over the SSA list, merge consecutive `SSA_ADJ`) to cover this.

  **B2. Compare-branch fusion**
  The pattern `SSA_ALU(LTS/LT/EQ/NE/…) + SSA_JZ` and `SSA_ALU(…) + SSA_JNZ` appears at
  every `if`/`while`/`for` condition. The CPU4 ISA has F3b branch instructions (`blt`, `bgt`,
  `blts`, `bgts`, `beq`, `bne` — 3 bytes each). Fusing the pair into a single `SSA_BRANCH`
  SSAOp saves one instruction (2 bytes: F1a compare) per conditional branch.

  **B3. LEA→STORE forwarding (F3b → F2 downgrade)**
  Pattern: `SSA_LEA rd=rN, imm=N` immediately followed by `SSA_STORE rd=rN, rs1=rM, imm=0`.
  The address in rN is `bp + N`, which is bp-relative. If `N` is in F2 range for the store
  size, the pair can be replaced by a single `SSA_STORE rd=-2, rs1=rM, imm=N` (F2 sw/sb/sl,
  2 bytes) and the `SSA_LEA` eliminated. Payoff: every assignment to a local variable shrinks
  from `lea + slw` (5 bytes) to `sw` (2 bytes).

  **B4. le/ge expansion reduction (via branch fusion)**
  `le` expands to `gt + immw 0 + eq` (3 instructions, 7 bytes). Combined with B2, a
  `(a <= b) → JZ` sequence would become a single `bgt rs1, rs2, target` (3 bytes) — a 7×
  reduction in instruction count for that pattern.

  #### C. Backend (`risc_backend.c`) — targeted fixes

  **C1. F2 stores currently unreachable**
  `SSA_STORE` with `rd >= 0` always emits F3b `slw/slb/sll`. The F2 store path (`rd == -2`)
  is only reachable when `flush_for_call_n` emits spill/argument stores. For normal
  assignments, `ssa.c` always sets `rd = vs_reg[depth]` (a register, never -2).
  Fix: implement B3 above in `ssa_opt.c`, which will turn register-relative stores into
  bp-relative (`rd=-2`) stores so the existing F2 emit path fires.

  **C2. LOAD into non-r0 (fuse SSA_LOAD + SSA_MOV)**
  Pattern: `SSA_LOAD rd=v8, rs1=-2, imm=N` followed by `SSA_MOV rd=v9, rs1=v8`.
  After regalloc this is `lw r0, N/2; or r1, r0, r0`.
  Could be emitted as `lw r1, N/2` (F2 with rd=r1) — one instruction fewer.
  The F2 load instruction in CPU4 has an explicit rd field so loading directly into r1 is valid.
  This requires either a combined-emit rule in `risc_backend.c` (peek at next instruction) or
  a new SSA_LOAD_FUSED opcode in `ssa_opt.c`.

  ### 3. Bigger / Structural Opportunities

  **D1. Long/float local alignment for F2 access**
  Currently `long`/`float` locals are not aligned to 4-byte boundaries. F2 `ll`/`sl` requires
  the byte offset to be a multiple of 4. Unaligned locals fall back to the 2-instruction
  `lea + lll/sll` sequence. Fix: `finalize_local_offsets()` should round `long`/`float` slots
  to 4-byte alignment. Payoff: F2 access for all float/long locals (halves code size for
  float-heavy loops).

  **D2. Callee-saved registers r6/r7 (call-site spill reduction)**
  `flush_for_call_n` spills all outer expression temporaries to memory before every call.
  r0–r3 are treated as caller-saved (spilled unconditionally). If a function could designate
  r6/r7 as callee-saved and the called function preserved them, outer temps could stay in
  r6/r7 across the call without spilling. This requires regalloc to know which regs are
  callee-saved and function prologues to emit `push r6; push r7` / `pop r7; pop r6`.

  **D3. Direct F3b branch instructions for all comparisons**
  The ISA's F3b comparison-branch instructions (`blt`, `bgt`, `blts`, `bgts`, `beq`, `bne`)
  take `rs1`, `rs2`, and a target label. They subsume the compare+branch pattern entirely.
  Implementing B2 (compare-branch fusion) unlocks these. The `le`/`ge` cases map to `bgt`/
  `blt` with swapped operands (the ISA assembler pseudo-ops already do this).

  **D4. Add-immediate instructions are unused**
  The ISA has two forms of add-immediate, neither emitted by the backend:
  - **F2 `addi rx, imm7`** (2 bytes): `rx = rx + sxt(imm7)` — in-place, same source and dest.
    F2 has 16 slots with 9 used (lb/lw/ll/sb/sw/sl/lbx/lwx/addi), leaving 7 spare for future ops.
  - **F3b `addli rx, ry, imm10`** (3 bytes): `rx = ry + sxt(imm10)` — separate source and dest.
    F3b has 16 slots with 15 used, leaving 1 spare.

  The backend always emits `immw r0, K; add rd, rs, r0` (5 bytes, 2 instructions) for any
  constant addition. Recognising `SSA_MOVI K + SSA_ALU ADD` pairs in `ssa_opt.c` and replacing
  them with a new `SSA_ADDI` SSAOp would allow `risc_backend.c` to emit:
  - `addi rd, K` (F2, 2 bytes) when `rd == rs` and `K` fits in imm7 — saves 3 bytes
  - `addli rd, rs, K` (F3b, 3 bytes) when `rd != rs` and `K` fits in imm10 — saves 2 bytes

  Particularly valuable for `i++` / `i--` loop counters (K=±1 always fits both) and
  pointer-bump operations (small struct field offsets). F1b `inc`/`dec` (2 bytes) cover
  the K=±1 in-place case even more compactly.

  ### 4. Priority / Effort Matrix

  | ID | Payoff | Effort | Where |
  |----|--------|--------|-------|
  | A1 | medium | trivial | `optimise.c`: remove arch guard on rule 9 |
  | B1 | low | trivial | `ssa_opt.c`: ADJ merge loop |
  | B2/B3 | high | medium | `ssa_opt.c`: LEA→STORE fwd + compare-branch fusion |
  | C2 | medium | small | `ssa_opt.c` or `risc_backend.c`: load-mov fusion |
  | D1 | high | small | `types.c` `finalize_local_offsets`: align long/float slots |
  | D4 | medium | medium | new `SSA_ALU_IMM` + backend support |
  | D2/D3 | high | large | regalloc rewrite + calling convention |

  ## Verification

  Document only — no build/test step required.
  Confirm the document is at `docs/optimisations.md` and renders cleanly.


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/a2f64bd8-8527-4520-8432-dc4f680cbd05.jsonl
- `09:13 user` [Request interrupted by user for tool use]
- `09:23 user` the docs isa.md and cpu4-migration.md should be renamed as cpu3-isa.md and cpu4-isa.md. cpu4-isa.md should be checked for alignment with the reference cpu4/cpu.py
- `09:33 user` inc/dec is definitely an omission, let me fix, cpu.py updated, please update sim_c and cpu4-isa.md, and note availability for optimisations
- `09:46 user` The document architecture.md needs updating to briefly cover the backend files but not in detail. the document codegen.md should describe in detail the flow from AST to emitted assembly for both cpu3 and cpu4. It should describe the stack IR and the SSA IR
- `10:09 user` commit, then implement the optimisations in optimisations.md
- `10:49 user` what does the -O flag now control for cpu4
- `10:51 user` please move -O2 peephole into -O1, and make ssa_peephole active at -O2
- `10:58 user` neither cpu3 or cpu4 now work with coremark. Because this type of regression results in heavy set of work, I'd like you to make three test cases that cover the essence of the three coremark loops: matrix operation, state machine, and linked list operations. Start with very simple versions (eg not the full mergesort), when they work expand the test cases to greater complexity
- `11:03 user` Implement the following plan:

  # Plan: Coremark-pattern regression tests

  ## Context

  Coremark stopped working on both CPU3 and CPU4 after recent changes. Rather than debug
  coremark directly, the goal is to add targeted regression tests that exercise the same
  three computation patterns coremark benchmarks:

  1. **Matrix** — triple-nested loop, 2D array indexing, integer accumulation
  2. **State machine** — switch dispatch on state, processes an array of inputs
  3. **Linked list** — struct pointer chasing, traversal, searching

  Two tiers per workload: a **simple** file that should be added first and verified, then
  an **expanded** file that adds more complexity (larger data, function pointers, more
  language features). Both tiers must pass on both `cpu3` and `cpu4`.

  ---

  ## New Files

  All files go in `tests/cases/coremark/` (new directory).

  ---

  ### Tier 1 — Simple

  #### `matrix_simple.c` — 2×2 matrix multiply

  ```c
  // EXPECT_R0: 69
  // 2x2 matrix multiply: exercises 2D array indexing and triple-nested loop
  int main() {
      int a[2][2];
      int b[2][2];
      int c[2][2];
      int i, j, k;
      a[0][0]=1; a[0][1]=2; a[1][0]=3; a[1][1]=4;
      b[0][0]=5; b[0][1]=6; b[1][0]=7; b[1][1]=8;
      for (i=0; i<2; i=i+1)
          for (j=0; j<2; j=j+1) {
              c[i][j]=0;
              for (k=0; k<2; k=k+1)
                  c[i][j]=c[i][j]+a[i][k]*b[k][j];
          }
      /* c[0][0]=19, c[1][1]=50 → 69 */
      return c[0][0]+c[1][1];
  }
  ```

  Verification: `c[0][0] = 1×5 + 2×7 = 19`, `c[1][1] = 3×6 + 4×8 = 50`. Sum = 69.

  ---

  #### `state_simple.c` — switch-based state machine

  ```c
  // EXPECT_R0: 3
  // State machine (switch dispatch): counts 1→0 transitions
  int main() {
      int input[8];
      int state, count, i;
      input[0]=1; input[1]=0; input[2]=1; input[3]=1;
      input[4]=0; input[5]=1; input[6]=0; input[7]=1;
      state=0; count=0;
      for (i=0; i<8; i=i+1) {
          switch (state) {
          case 0: if (input[i]==1) state=1; break;
          case 1: if (input[i]==0) { state=2; count=count+1; } break;
          case 2: state = (input[i]==1) ? 1 : 0; break;
          }
      }
      return count;
  }
  ```

  Verification (trace):
  - i=0: s=0,in=1 → s=1
  - i=1: s=1,in=0 → s=2, count=1
  - i=2: s=2,in=1 → s=1
  - i=3: s=1,in=1 → stay s=1
  - i=4: s=1,in=0 → s=2, count=2
  - i=5: s=2,in=1 → s=1
  - i=6: s=1,in=0 → s=2, count=3
  - i=7: s=2,in=1 → s=1

  Return 3. ✓

  ---

  #### `list_simple.c` — linked list traversal and sum

  ```c
  // EXPECT_R0: 15
  // Linked list: build 5-node list on stack, traverse with pointer, sum vals
  struct Node { int val; struct Node *next; };
  int main() {
      struct Node a, b, c, d, e;
      struct Node *p;
      int sum;
      a.val=1; a.next=&b;
      b.val=2; b.next=&c;
      c.val=3; c.next=&d;
      d.val=4; d.next=&e;
      e.val=5; e.next=0;
      p=&a; sum=0;
      while (p!=0) { sum=sum+p->val; p=p->next; }
      return sum;
  }
  ```

  Verification: 1+2+3+4+5 = 15. ✓

  ---

  ### Tier 2 — Expanded

  #### `matrix.c` — 4×4 matrix multiply with trace

  ```c
  // EXPECT_R0: 80
  // 4x4 matrix multiply: a[i][j]=i+j+1, b=diag(2) + ones; return trace of c
  #define N 4
  int a[N][N], b[N][N], c[N][N];
  void mat_mul(void) {
      int i,j,k;
      for (i=0;i<N;i=i+1) for (j=0;j<N;j=j+1) {
          c[i][j]=0;
          for (k=0;k<N;k=k+1) c[i][j]=c[i][j]+a[i][k]*b[k][j];
      }
  }
  int main() {
      int i,j,trace;
      for (i=0;i<N;i=i+1) for (j=0;j<N;j=j+1) {
          a[i][j]=i+j+1;
          b[i][j]=(i==j)?2:1;
      }
      mat_mul();
      trace=0;
      for (i=0;i<N;i=i+1) trace=trace+c[i][i];
      return trace;
  }
  ```

  Verification:
  - c[0][0] = 1×2+2×1+3×1+4×1 = 11
  - c[1][1] = 2×1+3×2+4×1+5×1 = 17
  - c[2][2] = 3×1+4×1+5×2+6×1 = 23
  - c[3][3] = 4×1+5×1+6×1+7×2 = 29
  - trace = 80 ✓

  Features added vs simple: global arrays, separate function, loop-initialised matrices, trace loop.

  ---

  #### `state_machine.c` — function-pointer dispatch

  ```c
  // EXPECT_R0: 5
  // State machine with function-pointer dispatch (coremark core_state.c style)
  typedef int (*state_fn)(int);
  int count_a, count_b;
  int state_A(int in) {
      if (in==0) return 1;
      count_a=count_a+1; return 0;
  }
  int state_B(int in) {
      if (in==1) return 0;
      count_b=count_b+1; return 1;
  }
  int main() {
      state_fn states[2];
      int input[12];
      int state,i;
      states[0]=state_A; states[1]=state_B;
      input[0]=1; input[1]=1; input[2]=0; input[3]=0;
      input[4]=1; input[5]=0; input[6]=1; input[7]=1;
      input[8]=0; input[9]=0; input[10]=1; input[11]=0;
      count_a=0; count_b=0; state=0;
      for (i=0;i<12;i=i+1) state=states[state](input[i]);
      return count_a+count_b;
  }
  ```

  Verification (trace):
  - i=0: A(1)→count_a=1, s=0
  - i=1: A(1)→count_a=2, s=0
  - i=2: A(0)→s=1
  - i=3: B(0)→count_b=1, s=1
  - i=4: B(1)→s=0
  - i=5: A(0)→s=1
  - i=6: B(1)→s=0
  - i=7: A(1)→count_a=3, s=0
  - i=8: A(0)→s=1
  - i=9: B(0)→count_b=2, s=1
  - i=10: B(1)→s=0
  - i=11: A(0)→s=1

  count_a=3, count_b=2, return 5 ✓

  Features added vs simple: global state, function pointers, typedef, two separate functions.

  ---

  #### `list_search.c` — array-allocated list with search function

  ```c
  // EXPECT_R0: 2
  // Linked list: array-allocated nodes, search by value, return matching id
  struct Node { int val; int id; struct Node *next; };
  struct Node *find(struct Node *head, int target) {
      struct Node *p=head;
      while (p!=0) { if (p->val==target) return p; p=p->next; }
      return 0;
  }
  int main() {
      struct Node nodes[5];
      struct Node *found;
      int i;
      for (i=0;i<5;i=i+1) {
          nodes[i].val=(i+2)*(i+1);   /* 2,6,12,20,30 */
          nodes[i].id=i;
          nodes[i].next=(i<4)?&nodes[i+1]:0;
      }
      found=find(&nodes[0],12);
      if (found==0) return -1;
      return found->id;
  }
  ```

  Verification:
  - nodes[0].val=2, nodes[1].val=6, nodes[2].val=12, nodes[3].val=20, nodes[4].val=30
  - find(head,12) returns &nodes[2], id=2 ✓

  Features added vs simple: struct with multiple fields, separate search function, array-allocated nodes, loop initialisation, pointer returned from function.

  ---

  ## Implementation Order

  1. Create directory `tests/cases/coremark/`
  2. Write the three simple tests; run `make test` — all 6 new items (3 files × 2 archs) must pass
  3. Write the three expanded tests; run `make test` — all 12 new items must pass

  ## Verification

  ```bash
  make test                          # must show 469+N passed, 0 failed
  python3 -m pytest tests/cases/coremark/ -v   # inspect just the new tests
  ```


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/f39c7b24-f1f8-4b48-8273-9a7fd8eb11a8.jsonl
- `11:03 user` [Request interrupted by user for tool use]
- `11:56 user` why is "Calling through a function-pointer stored in an array element is
    not implemented."
- `11:59 user` please fix and update the c89-status.md
- `12:48 user` now fix the cpu4 SSA lifter bug
- `12:54 user` please commit
- `13:04 user` coremark still fails. Looking at the generated code and the profile output in ../profile.s, there seems to be a problem with a "jz _l412" but the label does not exist
- `13:51 user` add some test cases that cover potential unaligned accesses
- `13:56 user` coremark cpu3 -O0, -O1, -O2 work, cpu4 -O2 fails

## 2026-03-20

- `10:32 user` question, have you created tests that exercised each of these bugs?
- `10:36 user` ok, commit the alignment tests. Explain to me (mostly a hardware engineer with deep VLSI knowledge) what are the current software engineering approaches to code coverage? With verilog we would never fab code that had no coverage form the test suite
- `10:54 user` I've just executed coremark at different opt levels, cpu3_O2=0.445269, cpu4_O0=0.457731, cpu4_O1=0.503381 cpu4_O2=0.622755. Update the docs/performance.md document, its now very out-of-date, and based on my mistaken 10MHz clock freq. The actual timer measures instruction cycles and is set at 1MHz, so the coremark score is the coremark/MHz score. Perform the same critical loop analysis on cpu4 code
- `11:02 user` Implement the following plan:

  # Plan: Update docs/performance.md with CPU4 analysis and corrected scores

  ## Context

  The existing `docs/performance.md` has two problems:

  1. **Wrong CoreMark/MHz scores** — all scores are 10× too low. The author divided by an
     assumed 10 MHz clock. The actual timer is 1 MHz (1 instruction = 1 cycle = 1 µs).
     CoreMark/MHz = iterations/sec directly. Old score: **0.044**; correct: **0.445**.

  2. **CPU4 not covered** — CPU4 has been implemented, benchmarked, and ships SSA-level
     compiler optimisations (B2/B3/C1/C2/D4). The document only analyses CPU3 code.

  New measured scores (user-provided, PERFORMANCE_RUN seeds, 1 iteration):

  | Build | CoreMark/MHz |
  |---|---|
  | CPU3 -O2 | 0.445 |
  | CPU4 -O0 | 0.458 |
  | CPU4 -O1 | 0.503 |
  | CPU4 -O2 | 0.623 |

  ---

  ## Critical file

  `docs/performance.md` — 703-line document, only this file changes.
  `coremark/coremark.s` is a read-only input for instruction counting.

  ---

  ## Section-by-Section Changes

  ### §1 — Baseline and Motivation  *(fix scores and gap statement)*

  The scoring-formula paragraph already says "1 cycle = 1 'MHz'" which is correct; only
  the final number is wrong. Fix:
  - Replace "Current result: 0.044" with a four-row score table (see above)
  - Update industry comparison table: CPU3 row 0.044 → 0.445; add CPU4 row at 0.623
  - Revise the "gap to Cortex-M0 is ~25×" sentence: gap is now ~1.6× for CPU4 -O2

  ### §2 — CPU3 Architecture Recap  *(unchanged)*

  Add one-sentence forward reference: "CPU4, a 3-address RISC successor, is described in §5."

  ### §3 — CPU3 Instruction Mix Analysis  *(unchanged)*

  Retitle to "§3. CPU3 Instruction Mix" for symmetry with the new §6.

  ### §4 — CPU3 Critical Loop Analysis  *(unchanged)*

  Keep §4a/4b/4c exactly. Add forward reference: "Equivalent CPU4 loops are in §7."

  ---

  ### NEW §5 — CPU4 Architecture

  Brief recap tying format innovations directly to performance gains:

  - **F2 (2 bytes)**: `lw rx, [bp+N]` — collapses CPU3's `lea; lw` (4 bytes, 2 instr)
    to 1 instruction. This is the single largest win; directly implements the old §5a
    `lw_local` proposal.
  - **F1a (2 bytes)**: 3-address ALU `add rd, rx, ry` — eliminates `push; op` pattern
  - **F3b (3 bytes)**: register-indirect with imm10 `llw rx, [ry+N]` — pointer chasing
  - **F1b (2 bytes)**: `inc rd` / `dec rd` — in-place increment (implements old §5c)
  - **Compiler opts at -O2**: B2 fused compare-branch, B3/C1 LEA→STORE forwarding,
    C2 LOAD+MOV fusion, D4 add-immediate

  Score progression table:
  ```
  CPU4 -O0 → 0.458  (raw RISC ISA benefit vs CPU3 stack machine)
  CPU4 -O1 → 0.503  (+10%, stack-IR peephole)
  CPU4 -O2 → 0.623  (+24% on top of -O1, SSA-level optimisations)
  ```

  ---

  ### NEW §6 — CPU4 Instruction Mix

  Count instructions from `coremark/coremark.s` (CPU4 -O2 build) using:

  ```bash
  cd coremark
  grep -E '^\s+[a-z]' coremark.s | wc -l          # total
  grep -cE '^\s+lw\s'   coremark.s                 # F2 bp-relative word load
  grep -cE '^\s+sw\s'   coremark.s                 # F2 bp-relative word store
  grep -cE '^\s+ll\s'   coremark.s                 # F2 bp-relative long load
  grep -cE '^\s+sl\s'   coremark.s                 # F2 bp-relative long store
  grep -cE '^\s+llw\s'  coremark.s                 # F3b reg-indirect word load
  grep -cE '^\s+lll\s'  coremark.s                 # F3b reg-indirect long load
  grep -cE '^\s+slw\s|^\s+sll\s' coremark.s        # F3b reg-indirect stores
  grep -cE '^\s+or\s'   coremark.s                 # F1a pseudo-mov
  grep -cE '^\s+add\s'  coremark.s                 # F1a add
  grep -cE '^\s+mul\s'  coremark.s                 # F1a mul
  grep -cE '^\s+immw\s' coremark.s                 # F3c immediate load
  grep -cE '^\s+addli\s' coremark.s                # F3b add-with-imm10
  grep -cE '^\s+lea\s'  coremark.s                 # F3c address (out-of-F2-range)
  grep -cE '^\s+jl\s'   coremark.s                 # calls
  grep -cE '^\s+jz\s|^\s+jnz\s' coremark.s         # conditional branches
  ```

  Present a category table paralleling the CPU3 §3 table. Key differences to highlight:
  - F2 loads/stores (`lw`, `sw`, `ll`, `sl`) should dominate — the main CPU4 win
  - `or` pseudo-moves should be visible (SSA copy residue)
  - `push`/`pop` should be **zero** (stack eliminated)
  - `lea` should be rare (only for out-of-F2-range frames)

  Also compute bytes/instruction for the text section and compare with CPU3's 1.77.

  ---

  ### NEW §7 — CPU4 Critical Loop Analysis

  Three loops paralleling §4a/4b/4c. Assembly from `coremark/coremark.s`.

  #### §7a. `matrix_mul_matrix` — Inner k-loop

  **Loop increment** (`k = k + 1`, where k is a `long` local):

  ```asm
  ll    r1, [bp-3]      ; old k (F2, 2 bytes)
  ll    r3, [bp-3]      ; k again — REDUNDANT (F2, 2 bytes)
  addli r0, r3, 1       ; k + 1 (F3b, 3 bytes)
  sl    r0, [bp-3]      ; k = k+1 (F2, 2 bytes)
  or    r0, r1, r1      ; restore old k — DEAD (F1a, 2 bytes)
  ```

  **5 instructions / 11 bytes** (CPU3: 8 instructions / ~14 bytes). Remaining waste:
  - Second `ll` is redundant — the old value is already in `r1`
  - `or r0, r1, r1` is dead — the pre-increment value of `k` is unused

  Root cause: stack IR emits `LOAD k; PUSH (old val); LOAD k; MOVI 1; ADD; STORE k`.
  The push captures old k; the second load reloads k for the +1. A same-address
  consecutive-load elimination in `ssa_opt.c` would collapse these.

  **Array access** `a[i][k]` costs ~14 instructions (mul + add + shl + F3b load) —
  identical to CPU3 because the computation overhead dominates; only the final load
  shrinks from 2 to 1 instruction.

  #### §7b. `core_list_find` — Pointer Chain

  One level of `list->info->idx`:

  ```asm
  addli r0, r1, 2     ; &list->info (F3b, 3 bytes)
  llw   r1, r0, 0     ; r1 = list->info (F3b, 3 bytes)
  addli r0, r1, 2     ; &list->info->idx (F3b, 3 bytes)
  llw   r0, r0, 0     ; r0 = idx value (F3b, 3 bytes)
  sxw   r0            ; sign-extend (F1b, 2 bytes)
  ```

  **5 instructions / 14 bytes** (CPU3: 10 instructions). The `addli + llw` pair (6 bytes)
  replaces CPU3's `immw; add; lw` (5 bytes, 3 instr) — one instruction saved per level
  but the chain penalty remains. ARM: `LDR r1,[r0,#2]; LDR r0,[r1,#2]` = 2 instructions.

  #### §7c. `core_bench_state` — Switch Dispatch

  B2 fuses each case comparison to `ne rx, ry, rz; jz _lN` (2 instr, 5 bytes) vs CPU3's
  `push; immw K; eq; jnz L` (4 instr, 6 bytes). The selector stays in a register across
  cases (no per-case reload), addressing the CPU3 §4c redundant-reload problem.

  ---

  ### NEW §8 — CPU4 Remaining Optimisation Opportunities

  What still underperforms and why:

  | Inefficiency | Root cause | Fix location | Est. gain |
  |---|---|---|---|
  | Redundant load in post-increment | Stack IR `PUSH; LOAD` idiom | `ssa_opt.c` — same-address load elim | 5–10% |
  | Dead `or` pseudo-moves | SSA MOV not fully eliminated by C2 | `ssa_opt.c` — DCE pass | 2–3% |
  | Array indexing: `mul` by power-of-2 | Stride strength-reduce fires in `optimise.c` but SSA lift doesn't see it | Gate Rule 7 before SSA lift, or add SSA strength-reduce | 3–5% |
  | Switch: O(n) linear scan | No jump-table codegen | `codegen.c` + `risc_backend.c` | 5–10% on FSM code |
  | Loop induction vars spill each iter | r6/r7 not used as dedicated loop regs | Register allocator | 10–15% on loops |

  ---

  ### §9 — ISA Optimization Opportunities  *(retitle "CPU3 ISA...")*

  Keep §5a–5e content unchanged but add a header note:

  > These CPU3 ISA proposals are **architecturally implemented in CPU4** via F2 (`lw_local`
  > = F2 `lw`, `sw_local` = F2 `sw`), F1b (`inc`/`dec`), and the B2/D4 compiler passes.
  > They remain relevant for the CPU3 target.

  ### §10–§11 — TOS2 and Compiler Optimizations  *(unchanged)*

  ### §12 — Encoding Density  *(add CPU4 column)*

  | Architecture | Bytes/Instr | Instr/binary-op | Instr/local-load |
  |---|---|---|---|
  | CPU3 | 1.77 | 4 | 2 |
  | **CPU4 -O2** | **~2.1** | **2** | **1** |
  | ARM Thumb-2 | ~1.6 | 1 | 1 |
  | RISC-V RV32I | 4.0 | 1 | 1 |

  Note: CPU4 bytes/instruction is *higher* than CPU3 (F2=2B, F3b/F3c=3B vs many 1B CPU3
  opcodes) but **instructions per operation is halved**. Density is the wrong metric.

  ### §13 — What 1.0 CoreMark/MHz Does  *(update gap ratios)*

  | Operation | Cortex-M0 | CPU3 | CPU4 -O2 |
  |---|---|---|---|
  | Load local | 1 instr | 2 instr | **1 instr** ← closed |
  | Increment local | 1 instr | 8 instr | 3 useful (2 wasted) |
  | Array access `a[i]` | 1 instr | 9 instr | ~13 instr |
  | Binary op (locals) | 2 instr | 6 instr | 4 instr |

  ### §14 — Prioritized Recommendations  *(update for CPU4 baseline)*

  Replace the old roadmap (CPU3-centric, items 1–5 already done by CPU4) with:

  | Priority | Change | Effort | Est. CoreMark/MHz |
  |---|---|---|---|
  | Baseline | CPU4 -O2 (current) | — | 0.623 |
  | 1 | Dead `or` DCE in `ssa_opt.c` | Low | ~0.64 |
  | 2 | Redundant post-increment load elim | Medium | ~0.67 |
  | 3 | Array stride strength-reduce (SSA) | Medium | ~0.69 |
  | 4 | Jump table for dense switch | Medium | ~0.72 |
  | 5 | Loop induction via r6/r7 | High | ~0.80 |

  ---

  ## Document Structure After Edit

  ```
  # CPU3 + CPU4 + smallcc Performance Analysis

  §1   Baseline and Motivation         [FIX scores, table, gap statement]
  §2   CPU3 Architecture Recap         [unchanged + fwd ref to §5]
  §3   CPU3 Instruction Mix            [unchanged, retitled]
  §4   CPU3 Critical Loop Analysis     [unchanged + fwd ref to §7]
  §5   CPU4 Architecture               [NEW]
  §6   CPU4 Instruction Mix            [NEW — count at implementation time]
  §7   CPU4 Critical Loop Analysis     [NEW]
  §8   CPU4 Remaining Opportunities    [NEW]
  §9   CPU3 ISA Optimization Opps      [retitled; add "done in CPU4" note]
  §10  Two-Register Model              [unchanged]
  §11  Compiler Optimizations (CPU3)   [unchanged]
  §12  Encoding Density Comparison     [add CPU4 column]
  §13  What 1.0 CoreMark/MHz Does      [update ratios for CPU4]
  §14  Prioritized Recommendations     [replace CPU3 roadmap with CPU4 roadmap]
  Appendix                             [add CPU4 grep commands]
  ```

  ---

  ## Verification

  ```bash
  # Confirm the four scores are reproducible
  cd coremark
  python3 ../smallcc/cpu4/sim.py --maxsteps 50000000 coremark.s 2>&1 | grep "Iter"

  # Check document after edit (no broken tables, correct header)
  head -60 docs/performance.md
  ```


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/3ae13ecc-18ac-4da3-bc27-2d177e8fdc29.jsonl
- `11:02 user` [Request interrupted by user for tool use]
- `11:28 user` please correct the figure for Cortex M0, on the ARM website it is listed as 2.33 coremark/mhz
- `11:49 user` the section on array element access has three cases of op->mov, eg mul r0,r3,r0;or r3,r0,r0, and non-use of llwx, vs llw;sxw. The core_list_find pointer chain has addli r0,r1,2;llw r1,r0,0 twice when this could be folded into an llw offset. core_state_transition does lea r1,-5;llw r1,r1,0 when this could be lw r1,-5
- `12:02 user` remove section 9 and references to it, we no longer care about cpu3 optimisation. Remove section 10 and 11. Current sect 14 prioritised recommendations should be merged/combined with section 8. I suspect the estimated gains in section 8 are low, but we shall see
- `12:09 user` Implement priorities 1-5
- `12:21 user` Implement the following plan:

  # Plan: Implement CPU4 SSA optimisation passes E1–E5

  ## Context

  `docs/performance.md` §8 lists 9 priorities. Priorities 1–5 are all "Low" effort and address
  missed patterns visible in `coremark/coremark4.s`:

  | # | Pattern seen in coremark4.s | Fix |
  |---|---|---|
  | 1 | `lea r1,-N; llw r1,r1,0` → should be `lw r1,-N` | E1: SSA LEA+LOAD → bp-relative LOAD |
  | 2 | `addli r0,r1,2; llw r1,r0,0` → should be `llw r1,r1,1` | E2: SSA ADDLI+LOAD → offset LOAD |
  | 3 | `mul r0,r3,r0; or r3,r0,r0` → should be `mul r3,r3,r0` | E3: SSA ALU+MOV → ALU with target reg |
  | 4 | `llw r0,rx,0; sxw r0` → should be `llwx r0,rx,0` | E4: LOAD+SXW/SXB → sign-extending LOAD |
  | 5 | Dead `or` pseudo-moves left after E3 | E5: DCE for unreferenced SSA_MOV dests |

  E1, E2, E3, E5 are new passes in `ssa_opt.c`. E4 is a lookahead in `risc_backend.c`.

  ---

  ## Critical Files

  - **`ssa_opt.c`** — add passes E1, E2, E3, E5 (numbered Pass 6–9 after existing 0–5)
  - **`risc_backend.c`** — add sign-extend lookahead in `SSA_LOAD` emit (E4)
  - **`ssa.h`** — no changes needed (SSA_ALU_IMM already exists)

  ---

  ## Key Facts from Code Exploration

  ### ssa_opt.c existing infrastructure

  ```c
  static int use_count[MAX_VREGS];   // indexed by vreg - VREG_START
  static void count_uses(SSAInst *head);
  static int single_use(int vreg);   // use_count[vreg-VREG_START] == 1
  static void mark_dead(SSAInst *p) { p->op = (SSAOp)-1; }
  static SSAInst *next_live(SSAInst *p);   // skips dead nodes, returns next live
  static int used_before_redef(SSAInst *start, SSAInst *end, int vreg);
  ```

  Existing passes (in order):
  - Pass 0: identity SSA_MOV elimination
  - Pass 1: B1 ADJ merge
  - Pass 2: B3/C1 LEA→STORE forwarding
  - Pass 3: C2 LOAD+MOV fusion (template for E3)
  - Pass 4: B2 compare-branch fusion
  - Pass 5: D4 MOVI+ALU → ALU_IMM (template for E1/E2)

  ### risc_backend.c existing infrastructure

  ```c
  static void emit_bp_load(int rd, int byte_off, int size);
      // Emits F2 lw/lb/ll when in range, else lea + llw/llb/lll
  static int f2_word_ok(int byte_off);   // even, imm7∈[-64,63]
  static int f2_long_ok(int byte_off);   // 4-aligned, imm7∈[-64,63]
  static int f2_byte_ok(int byte_off);   // imm7∈[-64,63]
  ```

  `SSA_LOAD` case (rs1 >= 0) already divides `p->imm / size` as F3b imm10 — correct for E2.

  Dead nodes have `p->op = (SSAOp)-1`; risc_backend already skips `op < 0`.

  ---

  ## Implementation Plan

  ### Pass E1 — LEA+LOAD → bp-relative LOAD (in `ssa_opt.c`)

  **Pattern:** `SSA_LEA rd=vA, imm=N` followed (as next live node) by `SSA_LOAD rd=vB, rs1=vA, imm=0, size=S`

  **Condition:** `single_use(vA)` and `!used_before_redef(lea_node->next, load_node, vA)`

  **Transform:**
  ```c
  load->rs1 = -2;   // mark as bp-relative
  load->imm = N;    // carry the bp offset
  mark_dead(lea);
  ```

  `emit_bp_load()` already handles the `rs1==-2` path and will select F2 automatically.

  Call `count_uses()` before this pass. Insert after Pass 5 (D4).

  ---

  ### Pass E2 — ADDLI+LOAD → offset LOAD (in `ssa_opt.c`)

  **Pattern:** `SSA_ALU_IMM rd=vA, rs1=vB, imm=N` (alu_op=IR_ADD) followed by `SSA_LOAD rd=vC, rs1=vA, imm=0, size=S`

  **Condition:** `single_use(vA)` and `!used_before_redef(addli->next, load_node, vA)` and `vB >= VREG_START` (i.e. vB is a virtual reg, not bp-relative sentinel)

  **Transform:**
  ```c
  load->rs1 = vB;   // use the base register directly
  load->imm = N;    // the scaled byte offset (risc_backend divides by size for F3b)
  mark_dead(addli);
  ```

  Note: `risc_backend.c` `SSA_LOAD rs1>=0` path emits `llw rx, [ry + imm/2]` — the division by size is already done, so `load->imm = N` (in bytes) is correct.

  Insert after E1.

  ---

  ### Pass E3 — ALU+MOV → ALU with target reg (in `ssa_opt.c`)

  **Pattern:** `SSA_ALU rd=vA, rs1=vB, rs2=vC` followed by `SSA_MOV rd=vD, rs1=vA`

  **Condition:** `single_use(vA)` and `!used_before_redef(alu->next, mov_node, vA)` and `vD != vB && vD != vC` (avoid output=input clobber unless target ISA allows; CPU4 F1a has no restriction)

  **Transform:**
  ```c
  alu->rd = vD;
  mark_dead(mov);
  ```

  This mirrors the C2 pass structure exactly. Insert after E2.

  ---

  ### Pass E4 — LOAD+SXW/SXB → sign-extending LOAD (in `risc_backend.c`)

  **Location:** Inside the `SSA_LOAD` emit case, after deciding which load instruction to emit but before emitting it.

  **Method:** Lookahead to `next_live(p)`. If the next live node is `SSA_ALU1 alu_op=IR_SXW, rd==p->rd, rs1==p->rd` (or IR_SXB), use `lwx`/`lbx` (F2) or `llwx`/`llbx` (F3b) instead of `lw`/`lb`/`llw`/`llb`, then `mark_dead(nxt)`.

  **Changes to `risc_backend.c`:**

  1. Add local helper `next_live_rb(SSAInst *p)` (same logic as `next_live` in ssa_opt.c, or factor a shared one).
  2. In `SSA_LOAD` case, after computing rs1/imm/size:
  ```c
  SSAInst *nxt = next_live_rb(p);
  int sign_ext = (nxt && nxt->op == SSA_ALU1 && nxt->rd == p->rd
                       && nxt->rs1 == p->rd
                       && (nxt->alu_op == IR_SXW || nxt->alu_op == IR_SXB));
  if (sign_ext) { nxt->op = (SSAOp)-1; }
  // then select lbx/lwx or llbx/llwx based on sign_ext flag
  ```
  3. `emit_bp_load()` needs a `sign_extend` parameter (or a separate `emit_bp_load_sx()`) to emit `lbx`/`lwx` instead of `lb`/`lw`.

  Only applies to size 1 (`lbx`) and size 2 (`lwx`/`llwx`/`llbx`). Size 4 (long) has no sign-extending variant needed.

  ---

  ### Pass E5 — Dead SSA_MOV DCE (in `ssa_opt.c`)

  **Purpose:** After E3 redirects ALU destinations, some original `SSA_MOV` nodes may have been killed (E3 does this). But there may also be `SSA_MOV rd=vA` nodes where vA is never used (e.g. from push/pop sequences where the intermediate copy is dead).

  **Method:**

  1. Call `count_uses(head)` to build use counts.
  2. For each `SSA_MOV rd=vA, rs1=vB` where `vA >= VREG_START` and `use_count[vA - VREG_START] == 0`: `mark_dead(p)`.

  This is simpler than full DCE — just check the use count of the destination virtual register.

  Insert as the last pass (after E3, before or after E4 since E4 is in risc_backend).

  ---

  ## Pass Ordering in `ssa_peephole()`

  ```
  Pass 0: identity MOV elim
  Pass 1: B1 ADJ merge
  Pass 2: B3/C1 LEA→STORE forwarding
  Pass 3: C2 LOAD+MOV fusion
  Pass 4: B2 compare-branch fusion
  Pass 5: D4 MOVI+ALU → ALU_IMM
  Pass 6: E1 LEA+LOAD → bp-relative LOAD   [NEW]
  Pass 7: E2 ADDLI+LOAD → offset LOAD      [NEW]
  Pass 8: E3 ALU+MOV → retarget ALU        [NEW]
  Pass 9: E5 Dead MOV DCE                  [NEW]
  ```

  E4 is in `risc_backend_emit()`, not `ssa_peephole()`.

  Each new pass calls `count_uses(head)` at its start (the existing passes that need it already do this; the new passes each need their own call since earlier passes may have invalidated counts).

  ---

  ## Verification

  ```bash
  make smallcc
  make test_all                    # must pass all existing tests

  # Check instruction counts improved
  cd coremark
  python3 ../smallcc/cpu4/sim.py --maxsteps 50000000 coremark4.s 2>&1 | grep -E "Iter|cycle"

  # Spot-check assembly output for eliminated patterns
  ./smallcc -O2 -o /tmp/t.s tests/test_ops.c
  grep -E 'lea.*; llw|or r' /tmp/t.s | head -20
  ```

  Expected: `or` pseudo-moves from E3 targets reduced; `lw`/`lwx` appear where `lea+llw` and `llw+sxw` were; `llw` with non-zero offsets where `addli+llw` was.


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/cda3d014-9dcb-4c50-929c-04ba850bb084.jsonl
- `12:21 user` [Request interrupted by user for tool use]
- `12:38 user` new score 0.695688
- `13:42 user` with -ann, I don't seem to see all source lines as comments in the output assembly
- `14:14 user` patterns I'm seeing that could be optimised: "immw r0,0;or r1,r0,r0", "add r0,r1,r0;or r1,r0,r0", "llwx r0,r0,0;sxw r0". It would be really useful for debugging when the -ann flag present to have a comment after each bp relative load or store with the local symbol name
- `14:36 user` cm/MHz now 0.712464
- `14:41 user` for loop end has code that loads loop var twice,, possibly one is intended for top of loop, but that is redundant because the loop var is loaded at the lop of loop anyway: [Pasted text #1 +5 lines]
- `14:58 user` cm/MHz=0.743917. I'm still seeing "add r0,r1,e0;or r1,r0,r0", and "llwx r0,r0,0; or r3,r0,r0". One of the big costs in the matrix loops is repeated loads of loop vars, when they could fit in the unused regs
- `15:27 user` no longer works, output is odd: rr  WRITE16 to 0000 = 0000  at pc=0ce0 sp=e7a4 bp=e7c8 r0=00000000
    WRITE16 to 0002 = fff8  at pc=0cec sp=e7a4 bp=e7c8 r0=0000fff8
  r0:0000ffff r1:00000001 r2:00001921 r3:0000e7bc r4:0000e7d4 r5:0000e7ac r6:00000001 r7:01477ffe sp:e7a0 bp:e7c8 lr:0dc9 pc:0d75 H:0 cycles:100000000
- `15:43 user` not working, stuck in core_list_init looping until sim ticks run out. see coremark/profile.s line 1708
- `15:44 user` [Request interrupted by user]
- `15:45 user` caller save doesn't seem to reload after call to core_list_insert_new()
- `16:12 user` it doesn't work, -O0,-O1 complete but with errors, -O2 loops forever

## 2026-03-21

- `12:48 user` [Request interrupted by user for tool use]
- `12:48 user` [Request interrupted by user]
- `12:55 user` [Request interrupted by user for tool use]
- `12:57 user` Note in CLAUDE.md how to build coremark in ../coremark. You keep rediscovering this, taking lots of tokens: smallcc -Icpu3 -I. -arch cpu4 -ann -DFLAGS_STR=\""-O2 -DPERFORMANCE_RUN=1  "\" -DITERATIONS=8 -DPERFORMANCE_RUN=1 -O2 -o ./coremark.s  core_list_join.c core_main.c core_matrix.c core_state.c core_util.c cpu3/core_portme.c
- `12:58 user` ok, tidy up, document the current status, and commit
- `13:10 user` It seems to me that the current not-fully SSA arch has lead to a complex and delicate peephole optimiser that is hard to reason about and hard to debug
- `13:17 user` Abandon this current direction. Make lift_to_ssa generate true SSA, remove all optimisation from ssa_opt.c, make a minimum complexity register allocator. Start by building a CFG from the basic blocks
- `14:32 user` Implement the following plan:

  # Plan: True SSA Form, CFG, and Linear-Scan Register Allocator

  ## Context

  The current `lift_to_ssa()` uses **depth-indexed virtual registers**: v8 is always the
  accumulator, v9 always depth-1 scratch, etc. The same register is defined hundreds of
  times per function, so SSA's "single definition" invariant does not hold. This forces every
  optimisation pass to use fragile structural heuristics (`used_before_redef`, structural
  adjacency as a proxy for single-use) instead of proper def-use analysis. The E3 pass had
  an undiagnosed correctness bug for `vA == VREG_START` as a direct consequence.

  The fix: assign a **fresh unique virtual register at every value-producing instruction**.
  PUSH/POP become pure name-stack manipulations emitting no SSAInst. No phi nodes are
  needed because the expression stack is always empty at basic-block boundaries (the stack
  machine IR guarantees this). This gives true SSA form for expression temporaries, enabling
  correct and straightforward optimisation passes later.

  ---

  ## Key Insight: No Phi Nodes Needed

  Every value that crosses a basic-block boundary (locals, params) is accessed via
  `SSA_LOAD`/`SSA_STORE` on each side. Expression temporaries (held in virtual registers)
  never cross BB boundaries — the expression stack is empty at every label target and after
  every branch. Therefore: unique-vreg-per-definition is true SSA for this IR without
  phi nodes.

  ---

  ## Critical Files

  | File | Change |
  |---|---|
  | `ssa.c` | Rewrite `lift_to_ssa()`: fresh_vreg scheme; PUSH/POP → no SSAInst; post-call restore uses new vregs |
  | `ssa_opt.c` | Gut to a no-op stub; delete all 12 passes and all helpers |
  | `regalloc.c` | Replace 4-line depth map with linear-scan allocator |
  | `ssa.h` | Update virtual register namespace comment; add CFG declarations |
  | `cfg.c` | **New file**: CFG construction from SSA list |
  | `cfg.h` | **New file**: BasicBlock, CFG structs |
  | `Makefile` | Add `cfg.c` to `smallcc` target |
  | `risc_backend.c` | **No changes** |
  | `smallcc.c` | **No changes** |

  ---

  ## Phase 1: `cfg.h` and `cfg.c` — CFG from SSA List

  ### `cfg.h`

  ```c
  #ifndef CFG_H
  #define CFG_H
  #include "ssa.h"

  typedef struct BasicBlock {
      int       id;
      SSAInst  *first;          /* first live node in this block */
      SSAInst  *last;           /* terminator node */
      int       succs[2];       /* successor block IDs; -1 = none */
      int       nsuccs;
      int      *preds;          /* heap-allocated predecessor IDs */
      int       npreds;
      int       serial_start;   /* instruction serial# of first node */
      int       serial_end;     /* instruction serial# of last node */
  } BasicBlock;

  typedef struct CFG {
      BasicBlock *blocks;
      int         nblocks;
      int         nserials;     /* total live instruction count */
  } CFG;

  CFG  *cfg_build(SSAInst *fn_first, SSAInst *fn_last);
  void  cfg_free(CFG *cfg);
  #endif
  ```

  ### `cfg.c` algorithm

  **Pass 1 — identify leaders and count blocks.**
  A node is a BB leader if it is:
  1. The first live node of the function.
  2. A `SSA_LABEL` node (potential jump target).
  3. The first live node after a terminator (`SSA_J`, `SSA_JZ`, `SSA_JNZ`, `SSA_BRANCH`, `SSA_RET`).

  Data nodes (`SSA_WORD`, `SSA_BYTE`, `SSA_ALIGN`) are skipped entirely.
  Build a `label_id → block_id` lookup table (array indexed by `label_id - base`).

  **Pass 2 — assign first/last, resolve edges.**
  For each terminator in each block:
  - `SSA_J  imm=L`     → `succs[0] = block_for_label(L)`
  - `SSA_JZ/JNZ imm=L` → `succs[0] = next_block`, `succs[1] = block_for_label(L)`
  - `SSA_BRANCH imm=L` → same as JZ/JNZ
  - `SSA_RET`          → no successors
  - `SSA_CALL/CALLR`   → NOT terminators; fall-through only

  Register each block as a predecessor of each successor.

  **Scope:** `cfg_build` takes the SSAInst range for a single function (from SSA_SYMLABEL to just before the next SSA_SYMLABEL+SSA_ENTER, or end-of-list). The regalloc calls it once per function.

  ---

  ## Phase 2: Rewrite `ssa.c` — Fresh Virtual Registers

  ### New state: TU-global vreg counter

  ```c
  static int next_vreg;   /* starts at VREG_START; never reset between functions */

  static int fresh_vreg(void) { return next_vreg++; }
  ```

  `next_vreg` is initialised to `VREG_START` at the top of `lift_to_ssa()`. It is **not** reset at function boundaries (SSA_SYMLABEL). All vregs in a TU are globally unique; this is correct because `regalloc` processes each function independently.

  `reset_fn_state()` (called at each SSA_SYMLABEL + SSA_ENTER boundary) resets:
  `vs_depth`, `vsp`, `spill_count`, `spill_total_bytes`, `await_post_call_adj`, `lvsp` table.
  It does **not** touch `next_vreg`.

  ### PUSH/POP: no instruction emitted

  ```c
  case IR_PUSH: case IR_PUSHW:
      vs_reg[vs_depth] = vs_reg[0];
      vs_size[vs_depth] = (p->op == IR_PUSH) ? 4 : 2;
      vs_depth++;
      break;   /* no SSAInst */

  case IR_POP: case IR_POPW:
      vs_depth--;
      vs_reg[0] = vs_reg[vs_depth];
      break;   /* no SSAInst */
  ```

  `vs_reg[0]` is no longer fixed to `VREG_START`; it holds the fresh vreg of the most
  recently computed value.

  ### Every value-producing instruction assigns a fresh vreg

  Template (shown for `IR_IMM`; all others follow the same pattern):
  ```c
  case IR_IMM: {
      int v = fresh_vreg();
      SSAInst *s = emit(p->sym ? SSA_MOVSYM : SSA_MOVI);
      s->rd = v; s->imm = p->operand; s->sym = p->sym; s->line = p->line;
      vs_reg[0] = v;
      break;
  }
  ```

  For `IR_ADD` (and all binary ALU ops):
  ```c
  vs_depth--;
  int v = fresh_vreg();
  SSAInst *s = emit(SSA_ALU);
  s->rd = v; s->rs1 = vs_reg[vs_depth]; s->rs2 = vs_reg[0];
  s->alu_op = p->op; s->line = p->line;
  vs_reg[0] = v;
  ```

  For `IR_SXW/SXB/ITOF/FTOI` (ALU1):
  ```c
  int v = fresh_vreg();
  SSAInst *s = emit(SSA_ALU1);
  s->rd = v; s->rs1 = vs_reg[0]; s->alu_op = p->op; s->line = p->line;
  vs_reg[0] = v;
  ```

  For `IR_LW/LB/LL` (standalone register-relative load):
  ```c
  int v = fresh_vreg();
  SSAInst *s = emit(SSA_LOAD);
  s->rd = v; s->rs1 = vs_reg[0]; s->imm = 0; s->size = sz; s->line = p->line;
  vs_reg[0] = v;
  ```

  For `IR_LEA` with peek-ahead collapse (already present; add `vs_reg[0] = v`):
  - Collapsed `SSA_LOAD rs1=-2`: fresh vreg to `rd`; update `vs_reg[0]`.
  - Standalone `SSA_LEA`: fresh vreg to `rd`; update `vs_reg[0]`.

  For **stores** (`IR_SW/SB/SL`): no fresh vreg (stores produce no value). `vs_reg[0]` unchanged.

  ### Post-call restore: new vregs for restored values

  In `flush_for_call_n` and the post-call restore path, the restore loads get fresh vregs:
  ```c
  int v = fresh_vreg();
  SSAInst *ld = emit(SSA_LOAD);
  ld->rd = v; ld->rs1 = -2; ld->imm = spill_off[k]; ld->size = spill_sz[k];
  vs_reg[k] = v;   /* update stack name to the freshly restored value */
  ```

  `spill_reg[k]` is still used for the spill-store emit (before the call); it is no longer
  used as the restore destination.

  ### save_fp path (JLI with outer temps)

  The function pointer vreg (`vs_reg[0]`, a fresh vreg) is moved to physical r4 before
  flush, then moved back to a fresh vreg after flush. Physical registers (< VREG_START) pass
  through `map_reg()` unchanged in regalloc.

  ---

  ## Phase 3: `ssa_opt.c` — No-op Stub

  Delete all content. Replace with:

  ```c
  /* ssa_opt.c — SSA-level peephole optimisation.
   *
   * Gutted: all depth-indexed heuristic passes removed.
   * With unique vregs from lift_to_ssa(), the old structural-adjacency
   * approximations (E3, B3, C2, F1...) do not apply.
   * Correct passes will be added here once the allocator is stable.
   */
  #include "ssa.h"

  void ssa_peephole(SSAInst *head) { (void)head; }
  ```

  All helpers (`count_uses`, `used_before_redef`, `find_store_for`, `single_use`,
  `next_live`, `use_count[]`) are deleted.

  ---

  ## Phase 4: `regalloc.c` — Linear-Scan Allocator

  ### Data structures

  ```c
  typedef struct {
      int vreg;     /* virtual register id */
      int start;    /* serial# of defining instruction */
      int end;      /* serial# of last use */
      int preg;     /* assigned physical register; -1 = unassigned */
  } Interval;
  ```

  Intervals are heap-allocated per function: `calloc(next_vreg, sizeof(Interval))`.
  This is always large enough since vregs are TU-global and monotonically increasing.

  ### Live interval computation

  Single forward scan of the function's SSAInst list with a serial counter:
  - Node with `rd >= VREG_START`: `intervals[rd].start = serial`
  - Node with `rs1 >= VREG_START`: `intervals[rs1].end = serial`
  - Node with `rs2 >= VREG_START`: `intervals[rs2].end = serial`
  - `SSA_STORE` with `rd >= VREG_START`: `intervals[rd].end = serial` (rd is used as address)

  Sentinels (`-1`, `-2`) and physical regs (`0..7`) are ignored (`< VREG_START`).

  ### Linear scan

  ```
  sort intervals by start (nearly sorted already since fresh_vreg() is called
  in execution order, making start values monotonically non-decreasing)

  free_regs = {0, 1, 2, 3, 5}   /* r4 reserved for save_fp scratch */
  active = [] (sorted by end)

  for each interval I (sorted by start):
      expire: remove J from active, return J.preg to free_regs, for all J.end < I.start
      if free_regs empty:
          spill_candidate = active entry with largest end
          if spill_candidate.end > I.end:
              I.preg = spill_candidate.preg
              spill_candidate.preg = -1   /* mark as spilled */
              remove spill_candidate from active
          else:
              I.preg = -1   /* spill I */
          /* see spill handling below */
      else:
          I.preg = lowest free reg; remove from free_regs
          insert I into active (sorted by end)
  ```

  Physical registers available: **r0, r1, r2, r3, r5** (5 registers).
  r4 is reserved for the `save_fp` indirect-call path.
  r6, r7 are reserved for future callee-saved use.

  Spills should never occur for normal C code (expression depth ≤ 3 per Sethi-Ullman,
  meaning at most 3 expression vregs live simultaneously). If a spill is needed:
  emit `fprintf(stderr, "regalloc: spill in %s\n", fn_name)` and fall back to
  assigning r0 (incorrect but will be caught by tests — serves as a debugging aid
  until spill insertion is implemented).

  ### le/ge expansion constraint

  `risc_backend.c` expands `SSA_ALU` with ops `IR_LE`/`IR_GE`/`IR_LES`/`IR_GES`/
  `IR_FLE`/`IR_FGE` into three instructions using rs1 as an intermediate scratch
  register. If `rd == rs1` physically, the expansion produces wrong results.

  **Post-allocation fixup:** after rewriting all vregs, scan for `SSA_ALU` nodes with
  expansion ops. If `rd == rs1` (physically), insert `SSA_MOV rd=free_reg, rs1=rs1_preg`
  before the node and update `rs1` to `free_reg`. A suitable free register is any physical
  register not in `{rd, rs1, rs2}` — typically the one just freed when rs1's interval ended.
  In practice this is rare but must be handled.

  ### Physical register rewrite pass

  After linear scan, one forward pass over the function's SSAInst list:
  ```c
  static int map_reg(int r, Interval *iv, int n) {
      if (r < VREG_START) return r;     /* sentinel or physical: pass through */
      int i = r - VREG_START;
      return (i < n && iv[i].preg >= 0) ? iv[i].preg : 0;
  }
  // Apply to rd, rs1, rs2 of each node.
  ```

  ### Function boundary detection

  ```c
  for (SSAInst *p = head; p; p = p->next) {
      if (p->op == SSA_SYMLABEL) {
          SSAInst *nx = /* next live */ ...;
          if (nx && nx->op == SSA_ENTER) {
              if (fn_start) regalloc_fn(fn_start, p);
              fn_start = p;
          }
      }
  }
  if (fn_start) regalloc_fn(fn_start, NULL);
  ```

  Each `regalloc_fn()` call:
  1. Scans the function's SSAInst list to compute live intervals.
  2. Runs linear scan.
  3. Rewrites vregs to physical registers.
  4. Frees the interval table.

  ---

  ## `ssa.h` Updates

  - Update the virtual register namespace comment to describe unique-vreg semantics.
  - Remove the depth-indexed mapping documentation (v8→r0, v9→r1...).
  - Add `#include "cfg.h"` or declare `CFG` forward reference for any new function signatures.
  - Consider increasing or removing the `VREG_START = 8` comment that currently describes depth-indexed slots.
  - `MAX_VREGS` (currently in `ssa_opt.c` as a static array size) moves to `cfg.h` or is eliminated (replaced by heap allocation in regalloc).

  ---

  ## Makefile Update

  ```makefile
  smallcc: smallcc.c tokeniser.c parser.c types.c codegen.c backend.c \
           preprocess.c optimise.c ssa.c ssa_opt.c regalloc.c \
           risc_backend.c cfg.c
  	$(CC) $(CFLAGS) $^ -o $@
  ```

  ---

  ## Verification

  ```bash
  # Phase 1: after cfg.c + cfg.h (no other changes)
  make smallcc && make test        # must still pass: 495 tests

  # Phase 2: after ssa.c fresh_vreg + linear-scan regalloc
  make smallcc && make test        # 495 tests must pass
  # Coremark CRC check:
  cd ../coremark
  ../smallcc/smallcc -Icpu3 -I. -arch cpu4 -DFLAGS_STR=\""-O2 -DPERFORMANCE_RUN=1  "\" \
    -DITERATIONS=8 -DPERFORMANCE_RUN=1 -O2 -o coremark4.s \
    core_list_join.c core_main.c core_matrix.c core_state.c core_util.c cpu3/core_portme.c
  python3 ../smallcc/cpu4/sim.py --maxsteps 200000000 coremark4.s \
    | grep -E "validated|crclist|crcstate|Iterations"
  # Expect: "Correct operation validated", crclist=0xe714, crcstate=0x8e3a

  # Phase 3: after ssa_opt gutted to no-op
  # Same checks; score will be lower but CRCs must still be correct.

  # Inspect output for absence of or/push (should see none):
  ./smallcc -arch cpu4 -O2 -ann -o /tmp/t.s tests/cases/ops/test_add.c
  grep -cE '^\s+or\s' /tmp/t.s   # should be 0 (no push/pop MOVs)
  ```

  Expected outcome after all phases:
  - 495 tests pass
  - Coremark CRCs correct
  - No `or` pseudo-mov instructions from push/pop (they simply won't be emitted)
  - Score comparable to CPU4 -O0 (~0.46) or better since LEA+LOAD collapse in lifter is preserved


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/6a3d6032-8117-4fd6-8dc9-b427fe2aad0e.jsonl
- `14:32 user` [Request interrupted by user for tool use]
- `20:08 user` [Request interrupted by user]
- `20:08 user` [Request interrupted by user for tool use]
- `20:09 user` you are using the slow python simulator instead of the fast sim_c simulator, which supports cpu4 with "sim_c -arch cpu4 code.s", note this in CLAUDE.md and memory
- `22:10 user` interesting. I seem to be getting correct coremark crc values, but the iterations/sec is implausibly high at 130, are the instructions being correctly counted
- `22:24 user` update the performance doc with the current performance. Create a new doc cpu4_compiler_arch.md that describes the entire flow from stack IR to instruction emission. Cover the overall architectures, choices and design tradeoffs, then talk about the implementation of each stage

## 2026-03-22

- `09:46 user` revert to the last fully working post full-SSA state, and commit that
- `09:48 user` delete the plan
- `09:50 user` so -O2 fails, -O1 works. Tell me what currently happens in O2. I don't want you to debug
- `10:02 user` so fundamentally, I still think the SSA-IR is too tightly tied to the ISA, particularly register limitations. And those same limitations in registers lead to problems later, with eg ad-hoc register pinning for loop variables. So: I want the plan to focus on a) a generic 3op SSA (op, load, store, compare, branch), which will be lowered in stages to the real ISA, Braun et al. "Simple and Efficient Construction of SSA Form", mem2reg, and phis. Keep linear register allocator and simple asm emitter. No optimisations at this stage. The aim is to get a comprehensive IR representation, when that is working, we will make future plans for optimisation
- `10:37 user` Implement the following plan:

  # Plan: Generic SSA IR with Braun Construction and Linear-Scan Regalloc

  ## Context

  The current CPU4 SSA (`ssa.c`) is fundamentally tied to the ISA's register count. Virtual
  registers are depth-indexed (`VREG_START+depth → rDepth`), so the entire representation
  depends on the Sethi-Ullman guarantee that expression depth ≤ 3. This causes:

  1. **Structural-adjacency heuristics** in `ssa_opt.c` (no real def-use graph) → bugs at -O2.
  2. **No join-point values** (no phi nodes) → future loop optimisations require ad-hoc register
     pinning (G1, rb_prescan).
  3. **Fragility** when any pass changes the instruction ordering — the depth invariant breaks.

  The fix: replace `lift_to_ssa` + `regalloc` with a proper SSA pipeline:

  1. Generic 3-address SSA IR (`IR3Inst`) — unlimited vregs, phi nodes, no ISA-specific ops.
  2. CFG construction from the stack IR.
  3. Braun et al. SSA construction with mem2reg (promotes scalar locals to SSA values).
  4. Phi elimination (parallel copy insertion).
  5. Linear-scan register allocator.
  6. Lowering to existing `SSAInst` → feed unchanged `risc_backend_emit`.

  No SSA optimisation passes at this stage. Goal: correct IR. Perf improvements are a future plan.

  ---

  ## New Pipeline (CPU4 only)

  ```
  Stack IR (codegen.c, unchanged)
      ↓  peephole() — unchanged
      ↓  build_cfg()         [ir3.c]   → BB[] with pred/succ links
      ↓  braun_ssa()         [braun.c] → IR3Inst list + phi nodes per BB
      ↓  elim_phis()         [braun.c] → phi nodes → parallel copies (SSA_MOV)
      ↓  linscan_regalloc()  [linscan.c] → vregs → physical r0–r7
      ↓  ir3_lower()         [ir3_lower.c] → IR3Inst → SSAInst
      ↓  risc_backend_emit() [risc_backend.c] — UNCHANGED
  ```

  **Removed from pipeline:** `lift_to_ssa`, `ssa_peephole`, `rb_prescan`, old `regalloc`.

  **smallcc.c change (lines 448–455):**
  ```c
  if (g_target_arch == 4) {
      int n_blocks;
      BB *blocks = build_cfg(codegen_ctx.ir_head, &n_blocks);
      IR3Inst *ir3 = braun_ssa(blocks, n_blocks);  /* includes phi elim */
      linscan_regalloc(ir3);
      SSAInst *ssa = ir3_lower(ir3);
      risc_backend_emit(ssa);
      free_ssa(ssa);
      free_ir3(ir3);
      free_cfg(blocks, n_blocks);
  }
  ```

  ---

  ## Data Structures (`ir3.h`)

  ### IR3Inst — generic 3-address SSA instruction

  ```c
  typedef enum {
      IR3_CONST,    /* rd = imm | sym                              */
      IR3_LOAD,     /* rd = mem[rs1 + imm], size                  */
      IR3_STORE,    /* mem[rs1 + imm] = rs2, size  (rd unused)    */
      IR3_LEA,      /* rd = bp + imm (local address, non-promoted) */
      IR3_ALU,      /* rd = rs1 alu_op rs2                        */
      IR3_ALU1,     /* rd = alu_op(rs1)  (sxb/sxw/itof/ftoi)     */
      IR3_CALL,     /* call sym                                   */
      IR3_CALLR,    /* indirect call through rs1                  */
      IR3_RET,      /* return                                     */
      IR3_J,        /* jump to block imm                          */
      IR3_BRANCH,   /* if rs1 alu_op rs2 jump to block imm        */
      IR3_PHI,      /* rd = phi(...)  — eliminated before lower   */
      IR3_ENTER,    /* enter N                                    */
      IR3_ADJ,      /* sp += imm                                  */
      /* Data-section ops (pass-through, no vregs): */
      IR3_SYMLABEL, IR3_LABEL,
      IR3_WORD, IR3_BYTE, IR3_ALIGN,
      IR3_PUTCHAR, IR3_COMMENT,
  } IR3Op;

  typedef struct IR3Phi {
      int block_id;   /* predecessor block */
      int vreg;       /* value from that predecessor */
  } IR3Phi;

  typedef struct IR3Inst {
      IR3Op        op;
      IROp         alu_op;     /* for IR3_ALU / IR3_ALU1 / IR3_BRANCH */
      int          rd;         /* dest vreg (0 = none) */
      int          rs1, rs2;   /* source vregs (0 = none) */
      int          imm;
      int          size;       /* 1/2/4 for LOAD/STORE */
      const char  *sym;
      int          line;
      int          block_id;   /* which BB this inst belongs to */
      /* Phi operands (IR3_PHI only): */
      int          n_phi;
      IR3Phi      *phi_in;     /* heap-alloc'd array, length n_phi */
      struct IR3Inst *next;
  } IR3Inst;
  ```

  Virtual register IDs: global counter starting at 1. 0 = "no register". No fixed mapping to
  physical registers. Physical registers are assigned by `linscan_regalloc` (written back into
  `rd`/`rs1`/`rs2`).

  ### BB — basic block for CFG and Braun state

  ```c
  #define BB_MAX_PREDS 8
  #define BB_MAX_SUCCS 2

  typedef struct VarDef {    /* per-block current definition of a local variable */
      int bp_offset;         /* local's bp-relative byte offset (key) */
      int vreg;              /* current SSA value (0 = undefined) */
      struct VarDef *next;
  } VarDef;

  typedef struct IncompletePhi {   /* phi not yet filled (unsealed block) */
      int bp_offset;
      IR3Inst *phi_inst;
      struct IncompletePhi *next;
  } IncompletePhi;

  typedef struct BB {
      int   id;
      int   preds[BB_MAX_PREDS];
      int   n_preds;
      int   succs[BB_MAX_SUCCS];
      int   n_succs;
      bool  sealed;               /* all predecessors known */
      VarDef          *cur_def;   /* Braun: current def per bp_offset in this block */
      IncompletePhi   *inc_phis;  /* incomplete phi nodes (if unsealed) */
      IRInst          *ir_first;  /* first stack-IR instruction in this block */
      IRInst          *ir_last;   /* last stack-IR instruction (terminator) */
      IR3Inst         *first;     /* first IR3 instruction produced for this block */
      IR3Inst         *last;      /* last IR3 instruction produced for this block */
  } BB;
  ```

  ---

  ## Algorithm Details

  ### Phase 1: `build_cfg` (`ir3.c`)

  Single pass through the stack IR:

  1. **Find leaders**: first instruction; any `IR_LABEL`; first instruction after any terminator
     (`IR_J`, `IR_JZ`, `IR_JNZ`, `IR_RET`). Data ops (`IR_WORD`, `IR_BYTE`, `IR_ALIGN`) and
     function labels (`IR_SYMLABEL`) are treated as function boundaries — reset everything.
  2. **Allocate `BB[]`**, record `ir_first`/`ir_last` for each.
  3. **Link succ/pred**: for each terminator, resolve label → BB id and link edges.
  4. **Seal all blocks** immediately (we have the complete CFG before running Braun).

  ### Phase 2: `braun_ssa` (`braun.c`)

  Process each BB in order. Per BB, maintain:
  - **Abstract expression stack** `estk[]` of vreg IDs (always empty at BB entry/exit — stack IR guarantee).
  - **Braun variable table** (shared via the `BB.cur_def` fields).

  **Promotability — marked during AST walk, not inferred from IR:**

  1. Add `bool addr_taken` to `struct Symbol` (`smallcc.h`).
  2. In `codegen.c`, when the `TK_AMPERSAND` unary case calls `gen_addr` on a local
     (SYM_LOCAL or SYM_PARAM), look up the symbol and set `sym->addr_taken = true`.
     Arrays, structs, and non-scalar types are always considered non-promotable (their
     address inherently escapes during initialisation and field access).
  3. Before calling `braun_ssa`, codegen builds a small `PromoSet` (array of `{bp_offset,
     promotable}` pairs) from the current function's symbol table and passes it to
     `braun_ssa`. A local is promotable iff it is scalar (not array/struct), not addr_taken,
     and SYM_LOCAL or SYM_PARAM.
  4. In `braun.c`, `is_promotable(int bp_offset, PromoSet *ps)` does an O(n) lookup.

  Non-promotable offsets keep `IR3_LEA` + `IR3_LOAD`/`IR3_STORE` in the output.

  **Braun primitives:**
  ```c
  /* Record that in block b, local at bp_offset now has value vreg. */
  static void write_var(BB *blocks, int b, int bp_offset, int vreg);

  /* Look up the reaching definition of bp_offset in block b.
   * If unknown, recursively query predecessors; insert phi at join points. */
  static int read_var(BB *blocks, int n_blocks, int b, int bp_offset);
  ```

  `read_var` follows the Braun et al. algorithm exactly (see §3 of the paper):
  - If `cur_def[b]` has an entry for `bp_offset`, return it.
  - If block has one predecessor: recurse to predecessor.
  - If block has multiple predecessors: create phi node, `write_var(b, bp_offset, phi.rd)` to
    break cycles, fill phi operands by recursing each predecessor.
  - Trivial phi elimination: if all phi operands are the same value, replace phi with that value.

  **Stack IR → IR3 mapping per instruction:**

  | Stack IR | IR3 output |
  |---|---|
  | `IR_IMM K` (int) | `IR3_CONST rd=fresh, imm=K`; push rd |
  | `IR_IMM S` (sym) | `IR3_CONST rd=fresh, sym=S`; push rd |
  | `IR_PUSH/PUSHW` | push top of estk (no instruction) |
  | `IR_POP/POPW` | pop from estk (no instruction) |
  | `IR_ADD` (etc.) | pop rs2, rs1; `IR3_ALU rd=fresh, rs1, rs2`; push rd |
  | `IR_SXB/SXW/ITOF/FTOI` | `IR3_ALU1 rd=fresh, rs1=top`; replace top |
  | `IR_LEA off; IR_LW/LB/LL` (promotable) | `rd = read_var(b, off)`; push rd |
  | `IR_LEA off; IR_SW/SB/SL` (promotable) | `write_var(b, off, top)`; pop rs2 (value) |
  | `IR_LEA off` (non-promotable) | `IR3_LEA rd=fresh, imm=off`; push rd |
  | `IR_LW/LB/LL` (standalone, ptr deref) | pop rs1; `IR3_LOAD rd=fresh, rs1, imm=0`; push rd |
  | `IR_SW/SB/SL` (standalone, ptr store) | pop rs1 (addr), rs2 (val); `IR3_STORE rs2, rs1, 0` |
  | `IR_LEA off; IR_SW` (non-promotable) | pop rs2 (val); `IR3_STORE rs2, IR3_LOAD(bp,off), 0` or direct IR3_STORE with rs1=BP_VREG |
  | `IR_JL sym` | emit `IR3_CALL sym`; define fresh rd for return value |
  | `IR_JLI` | pop rs1; emit `IR3_CALLR rs1` |
  | `IR_JZ L` | pop rs1; `IR3_BRANCH IR_EQ, rs1, 0, block_L` (or fuse with preceding compare) |
  | `IR_JNZ L` | pop rs1; `IR3_BRANCH IR_NE, rs1, 0, block_L` |
  | `IR_J L` | `IR3_J block_L` |
  | `IR_ENTER N` | `IR3_ENTER imm=N` |
  | `IR_ADJ N` | `IR3_ADJ imm=N` |
  | `IR_RET` | `IR3_RET` |
  | `IR_SYMLABEL S` | `IR3_SYMLABEL sym=S`; reset BB-local state |
  | `IR_LABEL L` | `IR3_LABEL imm=L`; begin new BB |
  | data ops | pass through unchanged |

  For `IR_JZ`/`IR_JNZ`, treat the popped value as a boolean: emit `IR3_BRANCH IR_NE, val, ZERO_VREG, block_L` (where `ZERO_VREG` is `IR3_CONST 0` hoisted to block entry if needed). Branch opcodes map to CPU4 `beq`/`bne`/etc. in the lowering step.

  **Calling convention**: expression stack is always empty at call sites (guaranteed by the stack
  IR: `flush_for_call_n` does this in the current lifter). With proper SSA and linear-scan
  regalloc, the allocator preserves live values across calls automatically — no manual spill
  needed in the SSA builder.

  ### Phase 3: Phi elimination (end of `braun.c`)

  Standard SSA destruction via parallel copy insertion:
  - For each `IR3_PHI rd = phi(v1 from b1, v2 from b2, ...)`:
    - On each predecessor edge `bi → b`, insert `IR3_ALU(IR_OR) rd_copy, vi, vi` (i.e., a MOV)
      at the end of block `bi` before its terminator.
    - Replace the phi with an assignment `rd = rd_copy` (or just unify the names).
  - Handle the "lost copy" problem: use temporary vregs where phi operands are also phi
    destinations in the same block (swap via fresh temp).
  - After elimination, mark all `IR3_PHI` nodes dead.

  ### Phase 4: `linscan_regalloc` (`linscan.c`)

  Per-function linear scan (standard Poletto/Sarkar algorithm):

  1. **Assign serials**: number each live `IR3Inst` 0, 1, 2, ... per function.
  2. **Compute live intervals**: for each vreg, `start = serial of defining inst`, `end = serial of
     last use`. `IR3_STORE` and `IR3_BRANCH` use rs1/rs2 as uses; `IR3_LOAD` defines rd.
  3. **Sort intervals by start.**
  4. **Allocate**: available pool `{r0, r1, r2, r3, r5}` (r4 reserved for legacy indirect-call
     scratch; r6/r7 callee-saved/reserved).
  5. **Expire**: remove intervals whose end < current start, return their regs.
  6. **Spill**: if pool empty, spill longest-live interval; insert `IR3_STORE`/`IR3_LOAD` around
     uses. Adjust all bp-relative offsets.

  No r0 restrictions (the old `rb_prescan` restriction on r0 for JZ/JNZ no longer applies —
  the new `IR3_BRANCH` encodes the condition directly without an implicit r0 test).

  ### Phase 5: `ir3_lower` (`ir3_lower.c`)

  Translates `IR3Inst` (with physical registers 0–7 in `rd`/`rs1`/`rs2`) to `SSAInst` for
  `risc_backend_emit`. One-to-one mapping:

  | IR3 | SSAInst |
  |---|---|
  | `IR3_CONST K` | `SSA_MOVI rd, K` |
  | `IR3_CONST sym` | `SSA_MOVSYM rd, sym` |
  | `IR3_LOAD rs1=BP_VREG` | `SSA_LOAD rs1=-2, imm=off` (bp-relative) |
  | `IR3_LOAD rs1=reg` | `SSA_LOAD rs1=reg, imm=0` |
  | `IR3_STORE rs1=BP_VREG` | `SSA_STORE rd=-2, imm=off` (bp-relative) |
  | `IR3_STORE rs1=reg` | `SSA_STORE rd=reg, imm=0` |
  | `IR3_LEA` | `SSA_LEA rd, imm` |
  | `IR3_ALU` | `SSA_ALU rd, rs1, rs2, alu_op` |
  | `IR3_ALU1` | `SSA_ALU1 rd, rs1, alu_op` |
  | `IR3_BRANCH` | `SSA_BRANCH alu_op, rs1, rs2, imm` |
  | `IR3_J` | `SSA_J imm` |
  | `IR3_CALL` | `SSA_CALL sym` |
  | `IR3_CALLR` | `SSA_CALLR` |
  | `IR3_RET` | `SSA_RET` |
  | `IR3_ENTER` | `SSA_ENTER imm` |
  | `IR3_ADJ` | `SSA_ADJ imm` |
  | data/label ops | pass-through unchanged |

  `BP_VREG` is a reserved virtual register ID (e.g., `IR3_VREG_BP = -2`, matching the existing
  `SSAInst` sentinel) so `risc_backend.c`'s existing bp-relative selection logic is triggered
  unchanged.

  ---

  ## Critical Files

  | File | Action |
  |---|---|
  | `ir3.h` | **New** — IR3Inst, BB, IR3Phi structs; `IR3_VREG_BP` constant; function declarations |
  | `ir3.c` | **New** — `build_cfg()`, `free_cfg()`, `free_ir3()`, fresh-vreg counter |
  | `braun.c` | **New** — `braun_ssa()`: promotability scan, Braun construction, phi elimination |
  | `linscan.c` | **New** — `linscan_regalloc()`: interval computation, linear scan, spill insertion |
  | `ir3_lower.c` | **New** — `ir3_lower()`: IR3 → SSAInst translation |
  | `smallcc.c` | **Modify** — replace CPU4 pipeline block (lines 448–455) |
  | `ssa_opt.c` | **Gut** — `ssa_peephole()` becomes a no-op stub; all passes removed |
  | `regalloc.c` | **Keep** (unused by new pipeline; remove reference from Makefile or keep for CPU3 fallback) |
  | `ssa.c` | **Keep** (unused; can be deleted later after new pipeline is stable) |
  | `Makefile` | **Modify** — add `ir3.c braun.c linscan.c ir3_lower.c` to `smallcc` target |
  | `risc_backend.c` | **Unchanged** |
  | `smallcc.h` | **Minor** — add `bool addr_taken` to `struct Symbol` |
  | `ssa.h` | **Minor** — add `SSA_PHI` to `SSAOp` if phi nodes need to appear in SSAInst temporarily (may not be needed if phi is eliminated in IR3) |

  ---

  ## Verification

  ```bash
  # After each sub-phase (build_cfg; braun_ssa; linscan; lower), run:
  make smallcc && python3 -m pytest tests/cases/ -q

  # Full test pass target: 495 tests (same as current -O1 baseline)

  # CoreMark CRC check at -O2 (should match -O1 since no ssa_opt):
  cd ../coremark
  ../smallcc/smallcc -Icpu3 -I. -arch cpu4 -DFLAGS_STR=\""..."\" \
    -DITERATIONS=8 -DPERFORMANCE_RUN=1 -O2 \
    -o coremark4.s \
    core_list_join.c core_main.c core_matrix.c core_state.c core_util.c cpu3/core_portme.c
  ../smallcc/sim_c -arch cpu4 coremark4.s
  # Expect: crclist=0xe714, crcstate=0x8e3a, crcmatrix=0x1fd7, "Correct operation validated"
  # Score will be lower than 0.683 (no ssa_opt), but CRCs must be correct at all opt levels.
  ```

  Expected outcomes after this plan is complete:
  - All 495 tests pass at -O0, -O1, and -O2.
  - CoreMark CRCs correct at all opt levels (no more -O2 breakage).
  - Score at -O2 ≈ -O1 baseline (no SSA optimisations yet).
  - Clean foundation for future optimisation passes on IR3.


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/cb99e7f4-1634-40aa-9fa5-a9071a1a249f.jsonl
- `10:37 user` [Request interrupted by user for tool use]
- `11:26 user` write a detailed document about the new flow from stack IR to instruction emit
- `11:31 user` Implement the following plan:

  # Plan: Document the New CPU4 Pipeline in docs/codegen.md

  ## Context

  The CPU4 backend was completely replaced. The old pipeline (`lift_to_ssa` → `ssa_peephole`
  → `regalloc` → `risc_backend_emit`) is gone; the new pipeline is:

  ```
  Stack IR  →  braun_ssa()  →  linscan_regalloc()  →  ir3_lower()  →  risc_backend_emit()
   (IR)        [braun.c]       [linscan.c]             [ir3_lower.c]   [risc_backend.c]
  ```

  `docs/codegen.md` still documents the old pipeline (ssa.c, ssa_opt.c, regalloc.c, depth-indexed
  vregs, rb_prescan). The `## CPU4 Backend` section needs to be replaced wholesale.

  ---

  ## Action

  **One file modified**: `docs/codegen.md`

  Replace everything from `## CPU4 Backend` to end-of-file with the new content below.
  The `risc_backend_emit()` instruction-selection table and F2/le/ge sections are still accurate
  and should be kept; only the earlier passes (IR, lift, opt, regalloc) change.

  ---

  ## New `## CPU4 Backend` Section

  ```markdown
  ## CPU4 Backend

  After `peephole`, the CPU4 path runs three additional passes (braun_ssa, linscan_regalloc,
  ir3_lower) before reaching the unchanged `risc_backend_emit`.

  ```
  Stack IR (codegen.c, unchanged)
      ↓  peephole(opt_level)      [optimise.c]    — unchanged
      ↓  braun_ssa()              [braun.c]        stack IR → IR3Inst (fresh vregs)
      ↓  linscan_regalloc()       [linscan.c]      vregs → physical r0–r7 (in-place)
      ↓  ir3_lower()              [ir3_lower.c]    IR3Inst → SSAInst
      ↓  risc_backend_emit()      [risc_backend.c] SSAInst → CPU4 assembly (unchanged)
  ```

  **Removed from old pipeline:** `lift_to_ssa` (`ssa.c`), `ssa_peephole` (`ssa_opt.c`, now
  a no-op stub), `rb_prescan`, depth-indexed `regalloc` (`regalloc.c`).

  ---

  ### IR3: the intermediate representation

  #### `IR3Inst` struct (`ir3.h`)

  ```c
  typedef struct IR3Inst {
      IR3Op        op;      /* operation kind                                    */
      IROp         alu_op;  /* IR3_ALU / IR3_ALU1: original stack-IR opcode      */
      int          rd;      /* dest vreg  (IR3_VREG_NONE = no destination)       */
      int          rs1;     /* source vreg 1                                     */
      int          rs2;     /* source vreg 2                                     */
      int          imm;     /* integer immediate, bp-relative byte offset, or
                               numeric label id                                  */
      int          size;    /* 1 / 2 / 4 bytes (IR3_LOAD / IR3_STORE)            */
      const char  *sym;     /* call target name, symbolic label, or NULL         */
      int          line;    /* source line for -ann annotation                   */
      struct IR3Inst *next;
  } IR3Inst;
  ```

  #### `IR3Op` enum

  | Op | Description |
  |---|---|
  | `IR3_CONST` | `rd = imm` (integer); or `rd = &sym` (symbolic) when `sym != NULL` |
  | `IR3_LOAD` | `rd = mem[rs1 + imm]`, `size` bytes; `rs1 = IR3_VREG_BP` → bp-relative |
  | `IR3_STORE` | `mem[rd + imm] = rs1`, `size` bytes; `rd = IR3_VREG_BP` → bp-relative |
  | `IR3_LEA` | `rd = bp + imm` — non-promoted local address, no load |
  | `IR3_ALU` | `rd = rs1 alu_op rs2` |
  | `IR3_ALU1` | `rd = alu_op(rs1)` — in-place sign-extend or float convert |
  | `IR3_MOV` | `rd = rs1` — register copy |
  | `IR3_CALL` | `jl sym` — direct call; result in ACCUM (r0) |
  | `IR3_CALLR` | `jlr` — indirect call via r0 |
  | `IR3_RET` | `ret` |
  | `IR3_J` | unconditional jump to numeric label `imm` |
  | `IR3_JZ` | jump if r0 == 0 |
  | `IR3_JNZ` | jump if r0 != 0 |
  | `IR3_ENTER` | `enter N` |
  | `IR3_ADJ` | `sp += imm` |
  | `IR3_SYMLABEL` | named label (function entry or data label) |
  | `IR3_LABEL` | numeric label |
  | `IR3_WORD / IR3_BYTE / IR3_ALIGN` | data directives (pass-through) |
  | `IR3_PUTCHAR / IR3_COMMENT` | special pass-throughs |

  #### Virtual register encoding

  | Value | Meaning |
  |---|---|
  | `-2` (`IR3_VREG_BP`) | bp-relative addressing sentinel (not a real register) |
  | `-1` (`IR3_VREG_NONE`) | no destination / unused operand |
  | `0–7` | physical registers (only after `linscan_regalloc`) |
  | `100` (`IR3_VREG_ACCUM`) | accumulator — always maps to physical r0; never allocated |
  | `101, 102, …` | fresh scratch vregs, allocated by `ir3_new_vreg()` |

  The fresh-vreg scheme replaces the old depth-indexed scheme (`VREG_START + depth`). Every
  expression result gets a unique ID, so the linear-scan allocator can assign physical registers
  based on actual live ranges rather than expression depth.

  ---

  ### Pass 1: `braun_ssa()` (`braun.c`)

  Converts the flat stack-IR list to IR3Inst with fresh virtual registers. No phi nodes and no
  local promotion — all locals go through `IR3_LEA + IR3_LOAD / IR3_STORE`. The name "braun"
  reflects the planned future extension to full Braun SSA construction with local promotion.

  #### Virtual stack model

  Identical to the old `lift_to_ssa`, except virtual register IDs are fresh instead of
  depth-indexed:

  ```c
  static int vs_reg[MAX_VDEPTH];   // fresh vreg at each depth slot
  static int vs_size[MAX_VDEPTH];  // push size (2 or 4 bytes) per slot
  static int vs_depth;             // current virtual stack depth
  static int vsp;                  // running (sp - bp) byte offset for scope-adj tracking
  ```

  `vs_reg[0]` is always `IR3_VREG_ACCUM` (100, physical r0). `IR_PUSH` saves the accumulator
  into a fresh vreg; `IR_POP` restores a previously saved vreg to the accumulator.

  #### Stack IR → IR3 mapping

  | Stack IR | IR3 output | Notes |
  |---|---|---|
  | `IR_IMM K` (integer) | `IR3_CONST rd=ACCUM, imm=K` | |
  | `IR_IMM S` (symbolic) | `IR3_CONST rd=ACCUM, sym=S` | |
  | `IR_LEA N; IR_LW/LB/LL` | `IR3_LOAD rd=ACCUM, rs1=BP, imm=N, size` | peek-ahead collapses pair |
  | `IR_LEA N` (standalone) | `IR3_LEA rd=ACCUM, imm=N` | non-promotable local address |
  | `IR_LW/LB/LL` | `IR3_LOAD rd=ACCUM, rs1=ACCUM, imm=0, size` | reg-relative deref |
  | `IR_SW/SB/SL` | `IR3_STORE rd=vs_reg[top], rs1=ACCUM, imm=0, size` | addr from vstack |
  | `IR_PUSH/PUSHW` | `IR3_MOV rd=fresh, rs1=ACCUM`; push fresh onto vstack | |
  | `IR_POP/POPW` | `IR3_MOV rd=ACCUM, rs1=vs_reg[top]`; pop vstack | |
  | `IR_ADD` … `IR_FGE` | `IR3_ALU rd=ACCUM, rs1=vs_reg[top], rs2=ACCUM`; pop vstack | result directly to r0 |
  | `IR_SXB/SXW/ITOF/FTOI` | `IR3_ALU1 rd=ACCUM, rs1=ACCUM, alu_op` | in-place on r0 |
  | `IR_JL sym` | flush args + `IR3_CALL sym` | see call flushing below |
  | `IR_JLI` | save fp → fresh, flush args, restore → ACCUM, `IR3_CALLR` | |
  | `IR_JZ L` | `IR3_JZ imm=L` | condition implicit in r0 |
  | `IR_JNZ L` | `IR3_JNZ imm=L` | |
  | `IR_J L` | `IR3_J imm=L` | |
  | `IR_ENTER N` | `IR3_ENTER imm=N`; reset vsp | |
  | `IR_ADJ N` | `IR3_ADJ imm=N`; update vsp; trigger post-call restore if pending | |
  | `IR_RET` | `IR3_RET` | |
  | `IR_SYMLABEL S` | `IR3_SYMLABEL sym=S`; reset all vstack / vsp / spill state | function boundary |
  | `IR_LABEL L` | `IR3_LABEL imm=L`; apply any saved vsp from forward-branch table | |
  | data ops | pass-through | |

  **ALU result directly to ACCUM**: Binary ALU ops write their result directly to
  `IR3_VREG_ACCUM` (`rd = ACCUM, rs1 = vs_reg[top], rs2 = ACCUM`). CPU4's 3-address
  F1a instructions allow `rd == rs2` (both sources are read before the destination is written),
  so `add r0, r1, r0` is architecturally valid. This eliminates the extra `IR3_MOV` that a
  fresh-vreg-then-copy approach would require.

  **ALU1 in-place on r0**: `sxb`, `sxw`, `itof`, `ftoi` are encoded as F1b / F0 instructions
  that operate in-place on a single register. `braun_ssa` models this as
  `IR3_ALU1 rd=ACCUM, rs1=ACCUM`; `risc_backend_emit` emits the in-place instruction using
  only `rd`.

  #### Call flushing (`flush_for_call_n`)

  Before every `IR_JL` / `IR_JLI`, `braun_ssa` must partition the virtual stack into argument
  slots (bottom of the stack, pushed by the caller) and outer expression temporaries (above
  the args, live across the call). The key difference from the old `ssa.c` implementation: after
  the call, outer temps are restored into **fresh vregs** (not the same vreg IDs). This ensures
  no vreg's live range spans a call instruction, so `linscan_regalloc` can assign physical
  registers without any call-clobber awareness.

  1. **Detect the arg boundary**: scan ahead for the post-call `IR_ADJ +N` to determine
     `arg_bytes`; bottom `n_args` vstack slots are args.
  2. **Spill outer temps** to memory (`IR3_STORE rd=BP, rs1=vs_reg[k], imm=vsp+k*sz`),
     preceded by `IR3_ADJ -outer_bytes` to allocate the spill area.
  3. **Flush args** to the machine stack (`IR3_STORE` per arg) with `IR3_ADJ -arg_bytes`.
  4. **Emit the call** (`IR3_CALL` or `IR3_CALLR`).
  5. **Post-call restore** (triggered by the arg-cleanup `IR3_ADJ +arg_bytes`): reload each
     spilled temp into a **fresh vreg** via `IR3_LOAD`, update the vstack, then emit
     `IR3_ADJ +outer_bytes` to free the spill area.

  For indirect calls, the function pointer lives in ACCUM and must survive `flush_for_call_n`.
  It is saved to a fresh vreg before the flush and moved back to ACCUM immediately before
  `IR3_CALLR`.

  #### Forward-branch vsp fixup

  Same mechanism as the old `lift_to_ssa`: when a branch (`IR_J/JZ/JNZ`) is emitted, the
  current `vsp` is recorded in a table keyed by label id. When the target `IR_LABEL` is
  processed, `vsp` is restored from the table. This prevents scope-exit `adj` instructions
  from one branch arm from corrupting the sp model seen by the merge point.

  ---

  ### Pass 2: `linscan_regalloc()` (`linscan.c`)

  Standard Poletto/Sarkar (1999) linear-scan allocator. Runs per function (between
  `IR3_SYMLABEL` nodes). Rewrites `rd`/`rs1`/`rs2` in-place.

  #### Physical register pool

  `r1`–`r7` are available for allocation. `r0` is **not** in the pool — it is permanently
  reserved for `IR3_VREG_ACCUM` and is handled by a final rewrite pass (see below).

  #### Live intervals

  1. **Number instructions** sequentially (0, 1, 2, …) within the function.
  2. **Scan each `IR3Inst`**: `rd` records a definition; `rs1`/`rs2` record uses.
     Guard: `vreg <= IR3_VREG_ACCUM` (i.e., ≤ 100) skips ACCUM, BP, NONE, and physical regs.
  3. For each vreg, `interval.start = serial of first definition`, `interval.end = serial of
     last use`.

  #### Scan

  1. Sort intervals by `start`.
  2. For each interval, expire all active intervals whose `end < current.start` and return
     their registers to the pool.
  3. `pool_alloc()` returns the next available physical register (r1 first, then r2, …, r7).
  4. If the pool is empty, spill the longest-lived active interval: steal its register for the
     current interval; record a bp-relative spill slot for the displaced interval.

  #### ACCUM rewrite

  After the per-function scan, a final pass over the entire IR3 list rewrites every occurrence
  of `IR3_VREG_ACCUM` (100) to physical register 0. This handles ACCUM in SYMLABEL/LABEL/data
  nodes as well as inside function bodies.

  ---

  ### Pass 3: `ir3_lower()` (`ir3_lower.c`)

  Near-1:1 translation from `IR3Inst` (physical registers in `rd`/`rs1`/`rs2`) to `SSAInst`
  for `risc_backend_emit`. One `IR3Inst` → one `SSAInst` in all cases. Zero-imm `IR3_ADJ`
  nodes are dropped.

  | IR3Op | SSAOp | Notes |
  |---|---|---|
  | `IR3_CONST` (sym==NULL) | `SSA_MOVI` | |
  | `IR3_CONST` (sym!=NULL) | `SSA_MOVSYM` | |
  | `IR3_LOAD` | `SSA_LOAD` | rs1==-2 triggers bp-relative path in backend |
  | `IR3_STORE` | `SSA_STORE` | rd==-2 triggers bp-relative path |
  | `IR3_LEA` | `SSA_LEA` | |
  | `IR3_ALU` | `SSA_ALU` | alu_op forwarded |
  | `IR3_ALU1` | `SSA_ALU1` | alu_op forwarded |
  | `IR3_MOV` | `SSA_MOV` | |
  | `IR3_CALL` | `SSA_CALL` | |
  | `IR3_CALLR` | `SSA_CALLR` | |
  | `IR3_RET` | `SSA_RET` | |
  | `IR3_J` | `SSA_J` | |
  | `IR3_JZ` | `SSA_JZ` | |
  | `IR3_JNZ` | `SSA_JNZ` | |
  | `IR3_ENTER` | `SSA_ENTER` | |
  | `IR3_ADJ` (imm != 0) | `SSA_ADJ` | zero-imm nodes dropped |
  | `IR3_SYMLABEL` | `SSA_SYMLABEL` | |
  | `IR3_LABEL` | `SSA_LABEL` | |
  | `IR3_WORD/BYTE/ALIGN` | `SSA_WORD/BYTE/ALIGN` | |
  | `IR3_PUTCHAR` | `SSA_PUTCHAR` | |
  | `IR3_COMMENT` | `SSA_COMMENT` | |

  ---

  ### Pass 4: `risc_backend_emit()` (`risc_backend.c`) — unchanged

  Walks the post-lower `SSAInst` list and emits CPU4 assembly text.

  #### SSAInst struct

  ```c
  typedef struct SSAInst {
      SSAOp       op;
      IROp        alu_op;   // SSA_ALU/SSA_ALU1: original IR opcode
      int         rd;       // -2 = bp-relative, -1 = none, 0–7 = physical
      int         rs1;
      int         rs2;
      int         imm;      // immediate / bp offset / label id
      int         size;     // 1/2/4 for LOAD/STORE
      const char *sym;
      int         line;
      struct SSAInst *next;
  } SSAInst;
  ```

  #### Instruction selection

  | SSAOp | Condition | CPU4 emission |
  |---|---|---|
  | `SSA_MOVI` | imm fits in 16 bits | `immw rd, imm` |
  | `SSA_MOVI` | imm > 0xffff | `immw rd, lo16; immwh rd, hi16` |
  | `SSA_MOVSYM` | — | `immw rd, sym` |
  | `SSA_LEA` | — | `lea rd, imm` (F3c) |
  | `SSA_MOV` | rd != rs1 | `or rd, rs1, rs1` (F1a pseudo-mov) |
  | `SSA_LOAD` | rs1 == -2, in F2 range | `lw/lb/ll rd, [bp+imm7]` (F2, 2 bytes) |
  | `SSA_LOAD` | rs1 == -2, out of F2 range | `lea scratch, imm; llw/llb/lll rd, [scratch+0]` |
  | `SSA_LOAD` | rs1 >= 0 | `llw/llb/lll rd, [rs1+imm]` (F3b, 3 bytes) |
  | `SSA_STORE` | rd == -2, in F2 range | `sw/sb/sl rs1, [bp+imm7]` (F2, 2 bytes) |
  | `SSA_STORE` | rd == -2, out of F2 range | `lea scratch, imm; slw/slb/sll rs1, [scratch+0]` |
  | `SSA_STORE` | rd >= 0 | `slw/slb/sll rs1, [rd+imm]` (F3b, 3 bytes) |
  | `SSA_ALU` | most ops | `add/sub/mul/… rd, rs1, rs2` (F1a, 2 bytes) |
  | `SSA_ALU` | `le`/`ge` | `gt/lt rd, rs2, rs1; immw tmp, 0; eq rd, rd, tmp` (3 insns) |
  | `SSA_ALU` | `les`/`ges` | `gts/lts rd, rs2, rs1; immw tmp, 0; eq rd, rd, tmp` |
  | `SSA_ALU` | `fle`/`fge` | `fgt/flt rd, rs2, rs1; immw tmp, 0; eq rd, rd, tmp` |
  | `SSA_ALU1` | `sxb`/`sxw` | `sxb/sxw rd` (F1b, 2 bytes; in-place) |
  | `SSA_ALU1` | `itof`/`ftoi` | `itof`/`ftoi` (F0; r0 implicit) |
  | `SSA_ENTER` | — | `enter N` (F3a) |
  | `SSA_ADJ` | imm != 0 | `adjw imm` (F3a) |
  | `SSA_RET` | — | `ret` (F0) |
  | `SSA_CALL` | — | `jl sym` (F3a) |
  | `SSA_CALLR` | — | `jlr` (F0; r0 holds target) |
  | `SSA_J/JZ/JNZ` | — | `j/jz/jnz _lN` (F3a) |
  | `SSA_LABEL` | — | `_lN:` |
  | `SSA_SYMLABEL` | — | `sym:` |
  | `SSA_WORD/BYTE/ALIGN` | — | `word`/`byte`/`align` directives |
  | `SSA_PUTCHAR` | — | `putchar` (F0) |

  #### F2 range checks

  F2 bp-relative instructions use a scaled 7-bit signed immediate:

  | Access size | Scale | F2 byte-offset range |
  |---|---|---|
  | byte (`lb`/`sb`) | ×1 | −64 … +63 bytes from bp |
  | word (`lw`/`sw`) | ×2 | −128 … +126 bytes from bp (even offsets) |
  | long (`ll`/`sl`) | ×4 | −256 … +252 bytes from bp (multiples of 4) |

  Outside F2 range: `lea rd, imm` (F3c, 3 bytes) + `llw/slw rd, [rd+0]` (F3b, 3 bytes) = 6 bytes.

  #### `le`/`ge` expansion

  `le`/`ge` and their signed and float variants have no direct F1a encoding. When a 0/1
  comparison result is needed in a register, the backend expands `le rd, rs1, rs2` as:

  ```asm
  gt   rd, rs1, rs2    ; rd = (rs1 > rs2) ? 1 : 0
  immw tmp, 0
  eq   rd, rd, tmp     ; rd = (rd == 0) ? 1 : 0  ≡  !(rs1 > rs2)
  ```

  #### Annotation (`-ann`)

  `rb_emit_src_comment(line)` emits a `; source text` comment before the first instruction
  from each new source line, using the `ann_lines[]` index built by `set_ann_source()` in
  `smallcc.c`. Comments are suppressed at label nodes.
  ```

  ---

  ## Critical file

  | File | Change |
  |---|---|
  | `docs/codegen.md` | Replace `## CPU4 Backend` section (line ~319) to end-of-file |

  The replacement text is the new `## CPU4 Backend` section above (starting with the pipeline
  diagram and ending with the annotation subsection).

  ---

  ## Verification

  ```bash
  # Doc-only change — no build or test required.
  # Sanity-check: open docs/codegen.md and confirm no references to the old pipeline remain:
  grep -n "lift_to_ssa\|ssa_peephole\|rb_prescan\|VREG_START\|depth-indexed" docs/codegen.md
  # Expected: zero matches (all references replaced).
  ```


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/4efffa76-3015-49bf-b4c8-a8f8806c08bc.jsonl
- `11:31 user` [Request interrupted by user for tool use]
- `11:39 user` plan the implementation of local promotion and phis, currently unimplemented
- `11:58 user` Implement the following plan:

  # Plan: Local Promotion and Phis in braun_ssa

  ## Context

  `braun_ssa()` currently converts stack IR to IR3 with no local promotion: every C local
  variable access is a `IR3_LOAD`/`IR3_STORE` pair through a bp-relative slot.  Implementing
  local promotion eliminates these redundant memory operations for non-address-taken scalars and
  replaces control-flow merges with parallel-copy moves (serving as phis), so no IR3_PHI opcode
  is required and `linscan_regalloc` and `ir3_lower` need no changes.

  ---

  ## What counts as "promotable"

  A local scalar variable is **promotable** iff its address never escapes during the function:
  it is never the operand of `ND_UNARYOP TK_AMPERSAND` in the AST, and its type is a scalar
  (not an array, struct, or union). This is determined once during the AST walk in `codegen.c`,
  before IR generation, and encoded on every `IR_LEA` instruction for that variable.

  ---

  ## Step 1 — Mark address-taken symbols during the AST walk (`codegen.c`)

  ### 1a. Add `bool address_taken` to `Symbol` (`smallcc.h`)

  ```c
  struct Symbol {
      …
      bool  address_taken;   /* set during mark_address_taken(); never cleared */
  };
  ```

  ### 1b. Add `mark_address_taken(Node *n)` to `codegen.c`

  Recursive pre-scan called once per function, on the function-definition root node, before
  `gen_ir` runs:

  ```c
  static void mark_address_taken(Node *n) {
      if (!n) return;
      if (n->kind == ND_UNARYOP && n->op_kind == TK_AMPERSAND) {
          Node *op = n->ch[0];
          /* Strip any implicit casts inserted by insert_coercions */
          while (op && op->kind == ND_CAST) op = op->ch[1];
          if (op && op->kind == ND_IDENT && op->symbol) {
              Symbol *s = op->symbol;
              if (s->kind == SYM_LOCAL || s->kind == SYM_PARAM)
                  s->address_taken = true;
          }
      }
      for (int i = 0; i < 4; i++) mark_address_taken(n->ch[i]);
      mark_address_taken(n->next);
  }
  ```

  Call site (in `gen_function`, just before the existing per-function codegen):

  ```c
  mark_address_taken(func_node);
  ```

  ### 1c. Encode promotability on `IR_LEA` — use a sentinel `sym` value

  `IR_LEA` nodes for local variables currently carry `sym = variable_name` (a non-NULL pointer
  to the variable's name string). Structural LEAs (array-fill, struct copy buffers) carry
  `sym = NULL`. We add a third case:

  ```c
  /* codegen.c — module-level */
  const char ir_promote_sentinel[] = "";   /* unique address; empty string */
  ```

  Declare externally in `ir3.h`:

  ```c
  extern const char ir_promote_sentinel[];
  ```

  In `gen_varaddr_from_ident`, change the local-variable emission:

  ```c
  /* Was: ir_append(IR_LEA, -la.offset, name); */

  bool scalar = !istype_array(sym->type) &&
                sym->type->base != TB_STRUCT &&
                sym->type->base != TB_FUNCTION;
  bool promote = scalar && !sym->address_taken;
  ir_append(IR_LEA, -la.offset, promote ? ir_promote_sentinel : name);
  ```

  Result encoding on `IR_LEA.sym`:
  | `sym` value | Meaning |
  |---|---|
  | `NULL` | Structural LEA (array fill, copy buffer, etc.) — never promote |
  | `ir_promote_sentinel` | Promotable variable access |
  | any other non-NULL string | Non-promotable variable (address taken or aggregate) |

  Params follow the same rule (positive offset; currently `ir_append(IR_LEA, la.offset, name)`).

  ---

  ## Step 2 — Implement `build_cfg()` (`ir3.c`)

  ```
  build_cfg(ir_head, &n_blocks) → BB*
  ```

  Uses the `IR_BB_START` sentinels already inserted by `mark_basic_blocks()`:

  1. **Count BBs**: scan for `IR_BB_START` markers within function bodies; each run of
     non-`IR_BB_START` instructions forms one BB.
  2. **Fill `ir_first/ir_last`** and assign `bb->id` (sequential index).
  3. **Compute `bb->label_id`**: if the first non-sentinel instruction in the BB is
     `IR_LABEL`, record `label_id = p->operand`; else `-1`.
  4. **Compute `bb->succs`**: inspect the terminator of each BB:
     - `IR_J id` → one successor: the BB whose `label_id == id`
     - `IR_JZ id` / `IR_JNZ id` → two successors: the fall-through BB (next in array) and the BB whose `label_id == id`
     - `IR_RET` → no successors
     - fall-through (no branch) → one successor: next BB
  5. **Compute `bb->preds`**: invert the `succs` lists.

  Extend the `BB` struct in `ir3.h`:

  ```c
  typedef struct {
      int       id;
      int       label_id;          /* IR_LABEL operand starting this BB, or -1 */
      int       preds[16], n_preds;
      int       succs[2],  n_succs;
      IRInst   *ir_first, *ir_last;
  } BB;
  ```

  ---

  ## Step 3 — Local promotion in `braun_ssa()` (`braun.c`)

  ### 3a. New state variables (reset at each `IR_SYMLABEL` / function boundary)

  ```c
  #define PROMOTE_BASE  256          /* maps offset -256..+255 → index 0..511 */
  #define PROMOTE_SLOTS 512
  #define NO_OFFSET     INT_MIN

  static int  curr_def[PROMOTE_SLOTS];   /* vreg holding current value; IR3_VREG_NONE = unknown */
  static int  accum_offset;             /* bp-offset currently live in ACCUM, or NO_OFFSET */
  static int  vreg_lea_offset[4096];    /* vreg → bp-offset if vreg holds a var address */

  /* Forward-branch snapshot (indexed by label id) */
  #define MAX_SNAP 512
  typedef struct { int defs[PROMOTE_SLOTS]; IR3Inst *branch_tail; bool set; } LSnap;
  static LSnap fwd_snap[MAX_SNAP];

  /* Loop-header phi vregs (indexed by label id) */
  static int  loop_phi[MAX_SNAP][PROMOTE_SLOTS];   /* IR3_VREG_NONE = no phi */
  static bool is_back_edge[MAX_SNAP];              /* populated by pre-pass B */
  ```

  ### 3b. Pre-pass B — identify back-edge targets (O(n) over stack IR)

  Inside `braun_ssa`, before the main loop, per-function:

  ```c
  bool seen[MAX_LABEL_IDS] = {};
  for (IRInst *p = func_first; p; p = p->next) {
      if (p->op == IR_LABEL)            seen[p->operand] = true;
      if (p->op is branch && seen[p->operand]) is_back_edge[p->operand] = true;
  }
  ```

  (A "branch" here is `IR_J`, `IR_JZ`, `IR_JNZ` — calls are not branches.)

  ### 3c. Inline helper: `promote_slot(int offset)`

  ```c
  static bool promote_slot(int offset) {
      int idx = offset + PROMOTE_BASE;
      return (unsigned)idx < PROMOTE_SLOTS;
  }
  ```

  ### 3d. Changes to the main `braun_ssa` switch

  **`IR_LEA` (peek-ahead for load, extended)**

  ```c
  case IR_LEA:
      if (p->next is load AND p->sym == ir_promote_sentinel) {
          int N = p->operand, sz = load_size;
          if (promote_slot(N) && curr_def[N+PROMOTE_BASE] != IR3_VREG_NONE) {
              /* Hit: use register instead of loading from memory */
              int v = curr_def[N+PROMOTE_BASE];
              emit IR3_MOV rd=ACCUM, rs1=v;
              accum_offset = N;
              p = p->next;  /* consume the load */
              break;
          }
          /* Miss: load from memory and record the result vreg */
          int fresh = ir3_new_vreg();
          emit IR3_LOAD rd=fresh, rs1=BP, imm=N, size=sz;
          if (promote_slot(N)) { curr_def[N+PROMOTE_BASE] = fresh; }
          emit IR3_MOV rd=ACCUM, rs1=fresh;
          accum_offset = N;
          p = p->next;
          break;
      }
      /* Non-promotable or structural: fall through to existing IR3_LEA emission */
      /* Record LEA offset on ACCUM for store detection */
      if (p->sym == ir_promote_sentinel && promote_slot(p->operand))
          accum_offset = p->operand;  /* store pattern: LEA then PUSH then SW */
      else
          accum_offset = NO_OFFSET;
      /* emit existing IR3_LEA as before */
      …
  ```

  **`IR_PUSH` (extended)**

  After the existing `IR3_MOV rd=fresh, rs1=ACCUM` and `vs_reg[depth++] = fresh`:

  ```c
  /* Tag: if ACCUM just computed a variable address (standalone LEA), record it */
  if (accum_offset != NO_OFFSET && promote_slot(accum_offset))
      vreg_lea_offset[fresh] = accum_offset;
  else
      vreg_lea_offset[fresh] = NO_OFFSET;
  accum_offset = NO_OFFSET;
  ```

  **`IR_SW / IR_SB / IR_SL` (extended)**

  After `vs_depth--`:

  ```c
  int addr_vreg = vs_reg[vs_depth];
  int N = vreg_lea_offset[addr_vreg];

  if (N != NO_OFFSET && promote_slot(N)) {
      /* Promoted store: save ACCUM in a fresh vreg, skip memory store */
      int fresh = ir3_new_vreg();
      emit IR3_MOV rd=fresh, rs1=ACCUM;
      curr_def[N+PROMOTE_BASE] = fresh;
      /* Also fix up the IR3_LEA that was already emitted for addr_vreg:
         replace it with IR3_NOP / drop it */
      drop_last_ir3_lea_for(addr_vreg);   /* see implementation note below */
      break;
  }
  /* Non-promoted: emit IR3_STORE as before */
  ```

  **Implementation note — dropping the LEA**: When braun_ssa emits `IR3_MOV rd=fresh, rs1=ACCUM`
  for IR_PUSH (which saved the LEA result), and later discovers the store is promoted, the
  preceding `IR3_LEA` node is already in the output list. Drop it by negating its `op`
  (`ir3_node->op = -1`) — the same "dead" marker used by ssa_peephole — so `ir3_lower` and the
  backend skip it. Since we track `ir3_tail` as the most recently emitted node, and the
  IR3_LEA → IR3_MOV pair is the last two nodes, we can walk back one or two steps.

  **`IR_LABEL L` (forward merge)**

  ```c
  if (!is_back_edge[L] && fwd_snap[L].set) {
      for each promotable slot idx:
          int snap_v = fwd_snap[L].defs[idx];
          int fall_v = curr_def[idx];
          if (snap_v == fall_v || snap_v == IR3_VREG_NONE) continue;
          if (fall_v == IR3_VREG_NONE) { curr_def[idx] = snap_v; continue; }
          /* Genuine merge: emit parallel copies */
          int phi = ir3_new_vreg();
          /* Insert IR3_MOV phi, snap_v just before the branch instruction */
          insert_after(fwd_snap[L].branch_tail, IR3_MOV rd=phi, rs1=snap_v);
          /* Emit IR3_MOV phi, fall_v at current tail (before the LABEL) */
          emit IR3_MOV rd=phi, rs1=fall_v;
          curr_def[idx] = phi;
  }
  ```

  **`IR_LABEL L` (loop header — `is_back_edge[L]`)**

  ```c
  memset(loop_phi[L], IR3_VREG_NONE, sizeof(loop_phi[L]));
  for each promotable slot idx where curr_def[idx] != IR3_VREG_NONE:
      int phi = ir3_new_vreg();
      emit IR3_MOV rd=phi, rs1=curr_def[idx];  /* entry-path rename */
      loop_phi[L][idx] = phi;
      curr_def[idx] = phi;
  ```

  **`IR_J L` when `is_back_edge[L]` (back-edge branch)**

  ```c
  for each promotable slot idx where loop_phi[L][idx] != IR3_VREG_NONE:
      emit IR3_MOV rd=loop_phi[L][idx], rs1=curr_def[idx];  /* close back-edge phi */
  /* curr_def is dead after the unconditional back-edge branch */
  ```

  **`IR_J / IR_JZ / IR_JNZ L` (forward branch — snapshot)**

  ```c
  if (!is_back_edge[L]) {
      fwd_snap[L].defs = copy of curr_def
      fwd_snap[L].branch_tail = ir3_tail   /* node just before the branch we're about to emit */
      fwd_snap[L].set = true
  }
  ```

  **`IR_SYMLABEL` (function boundary reset)**

  ```c
  memset(curr_def,        IR3_VREG_NONE, sizeof(curr_def));
  memset(fwd_snap,        0,             sizeof(fwd_snap));
  memset(loop_phi,        IR3_VREG_NONE, sizeof(loop_phi));
  memset(is_back_edge,    0,             sizeof(is_back_edge));
  memset(vreg_lea_offset, NO_OFFSET,     sizeof(vreg_lea_offset));
  accum_offset = NO_OFFSET;
  /* existing state resets (vs_depth=0, vsp=0, etc.) unchanged */
  ```

  ---

  ## Files to modify

  | File | Change |
  |---|---|
  | `smallcc.h` | Add `bool address_taken` to `Symbol` struct |
  | `codegen.c` | Add `mark_address_taken()`; call it before `gen_ir` per function; change LEA emission in `gen_varaddr_from_ident` to use `ir_promote_sentinel` |
  | `ir3.h` | Add `label_id` to `BB` struct; declare `ir_promote_sentinel` |
  | `ir3.c` | Implement `build_cfg()` (~120 lines) |
  | `braun.c` | Add pre-pass B; add promotion state; extend IR_LEA / IR_PUSH / IR_SW/SB/SL / IR_LABEL / IR_J / IR_SYMLABEL cases (~250 lines) |
  | `smallcc.c` | No change — pipeline already wired |
  | `linscan.c` | No change |
  | `ir3_lower.c` | No change |

  ---

  ## Verification

  ```bash
  make smallcc
  make test_all      # all 19 suites must stay green

  # Loop variable should produce no bp-relative loads/stores after promotion
  cat > /tmp/promo.c << 'EOF'
  int main() {
      int i = 0;
      while (i < 10) i++;
      return i;   /* expected: 10 */
  }
  EOF
  ./smallcc -arch cpu4 -O1 -o /tmp/promo.s /tmp/promo.c
  ./sim_c -arch cpu4 /tmp/promo.s   # r0 should be 10
  # Check no wasted loads:
  grep -c '\blw\b\|\bsw\b' /tmp/promo.s   # ideally 0 for fully-promoted i

  # Address-taken must NOT be promoted (correctness check)
  cat > /tmp/addr.c << 'EOF'
  int g;
  void set(int *p) { *p = 42; }
  int main() { int x = 0; set(&x); return x; }   /* expected: 42 */
  EOF
  ./smallcc -arch cpu4 -O1 -o /tmp/addr.s /tmp/addr.c && ./sim_c -arch cpu4 /tmp/addr.s

  # Coremark: no performance regression
  cd ../coremark && python3 ../smallcc/cpu4/sim.py coremark4.s
  ```


  If you need specific details from before exiting plan mode (like exact code snippets, error messages, or content you generated), read the full transcript at: /Users/simonj/.claude/projects/-Users-simonj-projects-processor-smallcc/f3124dab-862f-4059-91b7-461027c5dfbf.jsonl
- `11:58 user` [Request interrupted by user for tool use]
- `14:19 user` so is this now a correct SSA with local promotion and phi operators?
- `14:23 user` so you seem to be somewhat resistant to implementing true SSA with dominance base phis. Hacking things at this level to avoid register pressure at a later stage is missing the point. Only once we have a true SSA implementation will we be able to to proper register allocation
- `14:26 user` as far as i understand, Brauns implicitly gives dominance frontier
- `14:27 user` plan first
- `14:59 user` yes, please implement
- `16:38 user` please tell me about the current plan, do not implement anything
- `16:42 user` ok, so the state of the current codebase is a working full SSA with phis and local promotion?
- `16:44 user` ok, thats good, the previous session seems to have lost track of what was happening, but still produce working code
- `16:44 user` please clean up all dead/redundant code, test again, then commit
- `16:49 user` ok, something you did broke coremark
- `17:21 user` ok, so we now have a full local promotion Braun SSA construction with phi, properly separated from register allocation and instruction emission?
- `17:23 user` ok, please write a document called braun_ssa_backend.md that describes the new architecture from stack IR to instruction emission
- `17:31 user` question: there are still a few hints of the original isa in the IR3 shape; mainly the privileging of r0 for cond jump and callr. Does this unduly limit later changes to changes in ISA?
- `17:42 user` Two things: callee-save for r6/r7 is the ABI, but I have no attachment to that, caller-save everything as the effective ABI is fine, maybe that should be formalised. And second: so right now, promotion is turned off because using it would trigger spills, which are not implemented yet, but some of the biggest coremark gains will come from loop variables ending in registers. What is your assessment of the work needed to make spilling work correctly
- `17:48 user` yes. first commit, then implement this plan: formalise the ABI, spill with r7 as scratch but with intention of moving to steal-back. Promotion heuristics should also be a consideration, but not implemented here
- `18:27 user` ok, carefully describe to me the issue with enabling promotion. I understand it may cause many spills, but if the logic for that is correct, it should still work, if slower
- `20:24 user` please implement this fix
- `22:46 user` [Request interrupted by user for tool use]
- `22:46 user` stop
- `22:47 user` think carefully. the braun algo does not need ad-hoc innovations if implemented properl

## 2026-03-23

- `11:02 user` how much work is the global struct init to fix?
- `11:03 user` yes please
- `11:09 user` commit and update all documentation please
- `11:29 user` Now we have a stable base from where to introduce SSA-based optimisation. I want you to investigate and plan three: 1) Dead code elimination, 2) Copy propagation, 3) Constant propagation and folding
- `13:02 user` please commit
- `13:07 user` This seems to be a blind spot, but le/ge do not need "expansion"! they are simply gt/lt with operand order swapped. I propose that you add le/ge and variants to the IR3 instruction set. And ensure that risc_backend emits exactly one instruction for these variants
- `14:20 user` question. now that the pass 1b SSA optimisations are working, it should be possible to expand the local promotion beyond leaf nodes without massive spill explosion
- `14:22 user` yes please
- `15:07 user` give me an overview of the linscan spill limitations and possible future mitigations. ultimately the register allocation and spill mechanism should not have limitations like this
- `15:15 user` what is the status of dead code elimination, copy propagation, and constant propagation and folding? And why isn't DCE and copy propagation helping with spills?
- `15:35 user` [Pasted text #1 +14 lines]
- `19:00 user` question: in theory, if there were working spill functionality, all locals could be promoted?
- `19:02 user` please update docs/codegen.md with details about all the changes in this session and commit
- `19:23 user` Notes on a new spill strategy: [Pasted text #1 +61 lines]
- `22:40 user` why are you not using sim_c? its much faster
- `22:44 user` [Request interrupted by user for tool use]
- `22:44 user` stop, this is still stuck in a loop

## 2026-03-24

- `08:23 user` describe the current state
- `08:27 user` please commit
- `08:30 user` examine the state_simple test failure
- `08:42 user` question: at what point are the phis lowered and resolved
- `08:44 user` what about the multiple predecsessor multiple successor problem?
- `08:47 user` construct a test case that causes multiple spills
- `12:51 user` add a flag to output the 3op SSA "-ssa <filename>" post global opts, pre phi lowering and regalloc
- `12:58 user` include source code annotations as comments
- `13:12 user` Please do not start debugging after this question. Coremark currently does not execute successfully, getting no further tham "Initialised" before starting to write into program space. What is your assessment of likely causes, given the large changes in moving to full SSA with split regalloc?
- `13:38 user` what is your assessment of the split interval allocator complexity?
- `13:48 user` please perform the same analysis on braun.c, I worry here about deficiencies in phi handling
- `14:02 user` Firstly LABEL_VID_MAX, MAX_LABELS and BB_MAX_PREDS violations should cause instant compile failure, we can make more robust later, but silent creation of bad code it much worse than termination. Fix the record_def for IR3_STORE. Run and debug using the standard tests, but not coremark
- `14:09 user` If any of these checks fail, we get an error message?
- `14:14 user` coremark compiles with no errors and still fails in the same way. Plan updates of linscan_regalloc and braun that refactor fragile and complex code, and fix the phi parallel copy bug
- `14:58 user` please commit
- `14:59 user` are there any redundant source files?
- `15:00 user` yes please
- `15:00 user` add the new test cases to the repo
- `15:36 user` There is no critical edge splitting for phis: Right before `braun_ssa` finishes (or right before `deconstructPhis`), write a pass that looks at every edge in your CFG. If `preds > 1` and `succs > 1`, insert an empty basic block in the middle. This will immediately stop your $\phi$-nodes from corrupting adjacent control flow.
- `15:53 user` The current linscan is fragile and flawed. The current ir3 opts are limited to basic blocks, failing to handle phi generated moves. Call spill does not handle live vregs across functions that are not promoted. Replace linscan and call_spill with proper Iterated Register Coalescing graph colouring
- `17:53 user` try again, I've increased the token limit to 64000
- `18:08 user` please tidy up, remove unused source and orphaned functions, then commit
- `18:25 user` Analysis: [Pasted text #1 +84 lines]
- `18:35 user` [Pasted text #1 +15 lines]
- `19:24 user` [Pasted text #1 +80 lines]

## 2026-03-25

- `08:47 user` please put in CLAUDE.md or memory that you do not attempt to debug a failing coremark test unless I ask you to
- `08:57 user` please tell me the current status
- `09:19 user` check carefully that all hardcoded limits (IRC_MAX_BB, VLO_SIZE etc) will cause instant compiler exit with error message, rather than silently failing and producing corrupt output
- `09:24 user` [Pasted text #1 +13 lines]
- `09:26 user` [Pasted text #1 +8 lines]
- `09:28 user` [Pasted text #1 +7 lines]
- `09:39 user` are there any tests fro struct copy?
- `09:40 user` this may be the source of coremark failures: [Pasted text #1 +12 lines]
- `18:56 user` look carefully at this possible source of coremark errors above and see what you think. Don't start editing, just evaluate
- `18:59 user` firstly, create a test case that copies struct and reproduces failure
- `19:04 user` please note in memory or CLAUDE.md that you can use sim_c for CPU4 code with "sim_c -arch cpu4". Its much faster than the python
- `19:05 user` Plan a principled fix, if there are alternatives, say what the different trade-offs are

## 2026-03-26

- `09:57 user` add the struct copy tests to the main regression set
- `10:02 user` Please examine this bug report: [Pasted text #1 +28 lines]
- `14:52 user` make a test where an array of struct{int a; unisgned long b;} get element b assigned a value then checked
- `14:59 user` after the struct S arr[3]; add "char block[1000];"
- `15:18 user` This code fragment [Pasted text #1 +8 lines] produces the wrong output but the correct return value: Current size 61296
  r0:000007d0 r1:0000000e r2:0000165e r3:0000165d r4:0000006c r5:00000000 r6:00000000 r7:00000000 sp:f000 bp:0000 lr:03ca pc:0007 H:1 cycles:849
- `15:22 user` [Request interrupted by user for tool use]
- `15:23 user` This example also fails, the problem is not the arg size: [Pasted text #1 +8 lines]
- `15:32 user` please create a test case
- `15:35 user` you can add a "// EXPECT_STDOUT: Current size 2000\n"
- `17:32 user` analyse the bug deeply, and propose possible principled fixes, with pros and cons for each
- `17:40 user` The ISA is not intended to support unaligned accesses, and the compiler should be aware of this, please check if that is the case for normal non-variadic parameters
- `17:49 user` so the fix of promoting all variadic types to 32 bits should work and is localised
- `18:37 user` revert the attempt to fix the printf issue, there are now unrelated tests failing
- `18:40 user` this is not so, the 8 failing tests were working before
- `18:52 user` please commit

## 2026-03-28

- `09:26 user` cpu4 misaligned accesses are forbidden and should be detected and errored by the simulators. This should feed through to test failures in the python test harness

## 2026-03-29

- `11:44 user` please fix the alignment errors
- `12:29 user` please commit this
- `12:44 user` question: I'm considering making some ISA changes, one may be to force bp and sp alignment to 32 bits. You'd still be able to write words or bytes to non-32 bit boundaries with sb,sw, but the frame would always be aligned. What are the implications?
- `12:48 user` would small locals always cost 4 bytes each? If you "enter 4", there would be space for four chars, still accessible with lb/sb
- `13:41 user` part of the reason for this is that the register spill mechanism fails when outside range of bp relative addressing, and there is no way to extend this without sacrificing a gen purpose reg. But if I add push/pop r0, this could circumvent that, but requires aligned sp to work
- `13:44 user` you could use r0 for lea calculation
- `13:47 user` I was thinking that r0 was already in use, so "save r0, use r0 for large offset calc with lea, spill register, restore r0"
- `14:10 user` I've added push/pop r0 to cpu4/cpu.py, and an exception if SP,BP ever has bits 0 or 1 set. I've not changed enter, ssp, adj, its the responsibility of the compiler to emit imm16 fields without those bits set. Please propagate this to sim_c and make necessaary changes to fix test regressions. Once that is done, plan correct spilling
- `14:22 user` r0 may be in use, then the strategy is "push r0; lea r0, imm16; sll rx, r0, 0; pop r0"
- `14:23 user` implement in IRC
- `14:58 user` analyse the printf_struct_array_bug failure
- `15:03 user` please fix this
- `18:34 user` there is no reason why you cannot use the stack, calculate, store, then restore stack, the sp is always pointing to available memory, the locals are between bp and sp. There are no scratch registers
- `18:51 user` I see the fundamental problem with only having push/pop for r0, if r0 is val_reg. So I've added pushr/popr to cpu4/cpu.py that are F1b instructions that take a src/dest register. So a) propagate the changes to sim_c b) use these new instructions. when r0 does NOT contain val_reg, you can use the short push/pop, when r0 IS val_reg, use the long form pushr/popr to save a different register

## 2026-03-30

- `11:17 user` all tests are now passing, what issue remains? Please explain clearly in detail
- `11:21 user` you have not explained why variadic params work when there is a small local frame but not when there is a large one
- `11:26 user` this does not explain, small negative offsets do not align with small positive offsets, they are *different*. The fix for large offset outside seven bit signed bp relative is to emit correct longer instruction sequences, not change the ABI
- `11:29 user` [Request interrupted by user]
- `11:35 user` Carefully examine each part of the cpu4 path from ast to emission looking for bugs, weaknesses, and code smells
- `14:29 user` [Request interrupted by user]
- `14:30 user` remove the task about variadic argument handling
- `14:41 user` please clean the ssa_push/pop codepaths out
- `14:43 user` please commit
- `15:03 user` assess the documentation. It has become cluttered with repeated information, and inconsistancies. Devise a plan to rationalise
- `15:28 user` Update docs/isa/cpu4.md to include push/pop, pushr/popr. SP and BP are *required* to be 32 bit aligned, any misalignment will cause an exception and the simulator will terminate. Also note that sim_c is a fast simulator for both cpu3 and cpu4 isa
- `15:36 user` document the handling of variadic parameters
- `15:45 user` please commit
- `15:47 user` I've added a new test case tests/cases/alignment/long_unsigned_printf.c which causes an alignment error in cpu4, please add to repo and investigate
- `16:24 user` variadic parameters should always be aligned to 32 bit boundaries for simplicity
- `16:35 user` variadic parameters should always be 32 bit aligned for simplicity
- `17:13 user` In ../coremark_test2 there is a failing alignment. The script ../coremark_test2/run_cpu4.sh shows how to compile and run to cause the error
- `17:38 user` [Request interrupted by user for tool use]
- `18:30 user` question: does the preprocesser respect all comment types?
- `18:34 user` what about "//#define"?
- `18:35 user` something is not working, I'm trying to create a test case, when I comment with "//" a define, the code compiles, when I delete the define, the code does not compile
- `18:41 user` case is in ../coremark_test2. Compile and run command is in ../coremark_test2/run_cpu4.sh. When compiled and run, the code shows an alignment error. When the commented out define line 2 core_util.c is deleted, the compile fails with a segfault
- `20:14 user` create regression tests to cover these fixes and add to the test set
- `20:55 user` carefully check all points where sp is set or altered (enter, ssp, adjw) to make sure that *only* 32 bit aligned constants are used
- `21:08 user` add testcases for these failures to the regression set
- `21:13 user` in ../coremark_min, I see an SP alignment error. The generated code coremark.s has "adjw 6" at line 735
- `21:16 user` this was with cpu4, using the script ../coremark_min/run_cpu4.sh to compile and run
- `21:49 user` [Request interrupted by user]
- `21:49 user` stop
- `21:51 user` I will generate a narrowly focussed test case
- `22:28 user` in ../coremark_test3/test_state.c is a cut down reproducer, with run_test_state.sh to compile and run

## 2026-03-31

- `09:58 user` [Request interrupted by user]
- `14:39 user` what is the current status?
- `14:41 user` [Request interrupted by user]
- `14:43 user` I've added a test case that is focussed on the switch error, test/cases/coremark/state_complex.c
- `14:57 user` Compiler frontend uses arena_alloc, but the backend uses calloc/free. This is inconsistent, the whole compiler should use arena
- `15:11 user` Investigate the hard limits on vregs (IRC_MAX_NODES/IRC_MAX_VREGS), basic block predecessor (BB_MAX_PREDS) and instructions (BB_INST_MAX). These can be quite easily triggered and unnecessarily constrain the compiler. Plan mitigation with arena allocated space
- `15:41 user` please commit
- `15:43 user` Consider this analysis: [Pasted text #1 +139 lines]
- `16:59 user` please commit
- `17:02 user` **Redundant `ir3_lower` Pass:**
      `ir3_lower.c` iterates over the `IR3Inst` linked list, allocates identical `SSAInst` structs, copies `rs1`, `rs2`, `rd`, and changes `IR3_` enums to `SSA_` enums.
      By the time `irc_regalloc` finishes, `IR3Inst` already contains physical registers. Converting it to `SSAInst` allocates unnecessary memory. `risc_backend_emit` could simply take the post-regalloc `IR3Inst` list directly.
- `17:14 user` please sweep the documentation to make sure it is up-to-date and consistent
- `17:19 user` please commit
- `17:21 user` Change the ABI to make r4-r7 callee saved. Analyse the work required and propose possible approaches
- `17:42 user` please update the documentation and commit
- `17:51 user` Analysis: [Pasted text #1 +29 lines]
- `18:12 user` [Pasted text #1 +52 lines]
- `18:17 user` [Pasted text #1 +30 lines]
- `18:30 user` I'm seeing lots of examples of "immw r3, 2; add r3, r1, r3; llw r4, r3, 0", which could be folded to "llw r4, r1, 1"
- `21:49 user` coremarks/MHz is now 0.994860, very good! Can you see other easy optimisations?
- `21:50 user` coremarks/MHz is now 0.994860, very good! Can you see other easy optimisations? ../coremark_min/profile.s

## 2026-04-01

- `13:28 user` please commit
- `13:44 user` while is coded as "lloop: cond; jz lbreak; body; j lloop; lbreak:". This causes j;jz; always, when rescheduling could just have jnz at end of loop
- `14:06 user` This change has broken coremark, with writes to addr 0 and early termination, see ../coremark_test4/run_cpu4.sh to reproduce
- `15:38 user` [Request interrupted by user]
- `15:38 user` revert the changes to loop structure
- `15:41 user` there doesnt seem to be any changes to risc_backend.c
- `15:47 user` the preprocessor should use the arena allocator, not malloc/calloc
- `16:14 user` investigate the impact of changing the calling convention to passing some parameters in registers. Pros and cons, code implications
- `16:32 user` save this plan for later but do not implement
- `16:54 user` I've added four new instructions to the ISA, in cpu4/cpu.py. zxb and zxw zero the top 24 and 16 bits respectively. shli and andi are immediate forms with an imm7 and a single register. Propagate these changes to sim_c. Plan to use these new instructions to improve code quality; in hot loops I see many examples of small immediates eg "immw r3,1; and r2,r2,r3"
- `17:06 user` implement this plan

## 2026-04-02

- `14:20 user` Added test case tests/cases/coremark/matrix_sum. This fails with the changes
- `17:14 user` please commit
- `17:16 user` question: this seems like a significant fix, would it explain the failure of the loop rotation atempt?
- `17:18 user` please try the loop rotation again
- `17:20 user` This breaks coremark, let me extract a small testcase
- `18:20 user` new test in tests/cases/coremark/alignment_bug.c, this fails with an alignment error

## 2026-04-03

- `11:29 user` in ../coremark_test4, compilation with "smallcc -arch cpu4 -O2 -o test_single.s test_single.c" fails with segfault in split_critical_edges(). Please examine the logic carefully for bad pointers
- `11:51 user` another error on ../coremark_test4, use run_test.sh, causes misaligned address access
- `12:31 user` [Request interrupted by user]
- `12:32 user` [Pasted text #1 +96 lines]
- `14:03 user` clear all previous plans
- `14:32 user` Please check this analysis: [Pasted text #1 +83 lines]
- `14:36 user` carefully examine braun.c for subtle errors and code smells, there is something that is producing incorrect code
- `14:52 user` [Request interrupted by user]
- `14:52 user` consider this analysis: [Pasted text #2 +61 lines]
- `14:58 user` please commot
- `15:18 user` Please assess this analysis: [Pasted text #1 +80 lines]
- `16:31 user` another analysis of braun, mostly performance issues, which can be deferred, but also some correctness: [Pasted text #1 +96 lines]
- `16:40 user` write some test cases that stress braun; deep parameters, lots of predecessors, complex switch cases, loop continues and breaks. There are still failures in coremark which are heisenbugs; changing the location of a return from main changes the maifestation and type of bug
- `16:54 user` now perform the same exercise with irc, stressing liveness spanning call sites, lots of promotions, register spilling when out of bp range

## 2026-04-06

- `11:08 user` adding the instructions zxb,zxw,shli,andi broke coremark in a heisenbug way. Add a flag to disable just these additions, and analyse carefully what might have caused regressions, proposing targettted tests for each hypothesis
- `11:23 user` proceed with the plan
- `11:33 user` continue
- `12:19 user` in ../coremark_single_file, run_cpu4.sh fails to compile, with error "scratch arena exhausted (used 4166648, requested 32832)". The run_cpu3.sh script works fine. Locate just the cause of the failure to compile
- `12:45 user` We now have an example: ../coremark_min fails to run correctly on cpu4, coremark_single_file runs correctly on cpu4
- `14:09 user` ok, this would seem likely to be the heisenbug. Regarding the andi difference immw+and, would you recommend changing andi to sign extend imm7 only to 16 bits and keep top 16 bits as zero
- `14:17 user` an alternative would be for immw to sign extend to 32 bits, like addi
- `14:18 user` ok, please make that option 1 change to cpu4/cpu.py and sim_c.c
- `14:37 user` Looking at profile output for coremark, there seem to be many places where the single operand andi and shli cannot be used because the src needs to be preserved. I'm considering adding an escape to the final f3b slot to give 2op + imm5 or imm6 or imm7 (32,16,8 instrs). See ../coremark_single_file/profile.s for examples
- `15:16 user` Performance issues with coremark: checking if reg is zero/nonzero, needs 2op. Switch is not jump table. Common subexpression elimination (particularly in matrix addressing). Still cases of "llwx; sxw"
- `16:16 user` Analyse work needed for proper LICM pass
- `17:02 user` do not proceed with switch jump table, I will add new jump indirect instruction later
- `18:46 user` These changes have broken coremark, see ../coremark_single_file/profile.s, the initialisation is stuck in a loop
- `19:46 user` coremark runs but gives incorrect results, see ../coremark_single_file. Correct results for -O0, incorrect for -O1 and -O2
- `20:26 user` please commit
- `20:32 user` profile contains this sequence: "andi r5,r3,1;immw r4,0xff;and r4,r2,r4;andi r4,1;xor r5,r5,r4;zxb r5", this could be reduced to andi r5,r3,1;andi r4,r2,1;xor r5,r5,r4"
- `21:24 user` seeing quite a few "llwx r0,r3,1;or r3,r0,r0"
- `21:35 user` still seeing, see ../coremake_single_file/profile.s line 3009
- `22:07 user` ../coremark_single_file/profile.s shows the current status, with cm/MHz at 1.19. Examine the profile carefully looking for optimisation possibilities

## 2026-04-07

- `09:22 user` status?
- `09:23 user` yes
- `10:48 user` consider this analysis: [Pasted text #1 +7 lines]
- `11:01 user` ../coremark_single_file now fails CRC
- `11:02 user` [Request interrupted by user for tool use]
- `11:02 user` ../coremark_single_file now fails CRC
- `11:03 user` [Request interrupted by user for tool use]
- `11:04 user` ../coremark_single_file now fails CRC
- `11:05 user` -O1 and -O2 fail
- `12:22 user` please consider this: [Pasted text #1 +6 lines]
- `13:57 user` Please investigate the state of the code, something is broken in coremark, it loops with -O1 and -O2

## 2026-04-08

- `07:48 user` Unknown skill: buddy
- `08:42 user` revert back to last working with coremark
- `08:59 user` [Request interrupted by user]
- `09:46 user` fix the compile errors and ensure -O0 works for coremark
- `09:50 user` [Request interrupted by user]
- `09:52 user` make a new branch from here called nanopass, and strip out all cpu4 related pipeline from braun onwards
- `11:26 user` carefully read docs/compiler-pipeline.md. This is an architecture for a new, mostly ISA agnostic, principled pipeline. Critique any major problematic areas, and write a plan for step-by-step implementation, initially no optimisation passes.
- `19:10 user` continue
- `19:35 user` please commit this
- `19:40 user` update all docs with the changes, including compiler-pipeline.md. Restore the -ssa flag to output the braun output
- `20:04 user` flag -ssa should take argument for output file
- `20:10 user` compiler should use lib/crt0_cpu4.s for cpu4 init
- `20:20 user` ../coremark_single_file/run_cpu4.sh fails (run_cpu3.sh works). This is a critical test, debug carefully and methodically, don't make random changes unless you see good reason for them

## 2026-04-09

- `08:38 user` [Request interrupted by user]
- `08:39 user` stop working on coremark, just ensure all the regression tests pass
- `08:40 user` please commit
- `09:34 user` add flag "-oos <file>" and "-irc <file>" to output the same forms as DUMP_IR=1 but to file
- `09:37 user` explain the use of the dominator analysis
- `09:40 user` consider what would be necessary to write a simulator to execute oos and irc representations directly
- `10:14 user` change the default stack start to 0xf000 everywhere, as in the crt0 files
- `10:16 user` add the new options to the usage, and create a "-h" usage page
- `10:20 user` add a new makefile target to run the regression tests on oos and irc IR
- `10:36 user` ok, this should make debugging coremark easier, firstly, I've created a failing testcase with printf: tests/cases/stdlib/complex_printf.c
- `11:08 user` [Request interrupted by user]
- `11:08 user` concentrate only on debugging at the oos level
- `11:12 user` [Request interrupted by user]
- `11:14 user` I've fixed the unsigned long literal in tests/cases/stdlib/complex_printf.c, test still fails in -runoos. Debug this, not the final cpu4 output. This should always be the rule, please note in CLAUDE.md, debug and fix test failures at the earliest point in the pipeline
- `12:50 user` I am going to modify the ISA so that R0 is not implicit in anything. Please check that there is no such assumptions in the pipeline before emit
- `14:24 user` I've changed cpu4/cpu.py to an ISA that has no r0 implicit dependencies, with other tweaks like jump indirect, beqz, bnez. Please examine the code to check for errors, query me if you find anything. Then once it looks good, plan to propagate to sim_c, then to emit.c
- `15:25 user` please debug tests/cases/struct/copy_pass.c::runoos
- `15:26 user` [Request interrupted by user]
- `15:27 user` the issue is present with oos simulation, start debugging there, not with assembler
- `15:32 user` Now fix the complex_printf, it passes with irc, so the problem must be in emit
- `15:48 user` for the runoos and runirc simulation modes, add a memory mapped instruction counter at address 0xff00, like sim_c
- `15:51 user` please commit the current state
- `16:15 user` what is the point of .data directive in crt0_cpu4.s?
- `16:16 user` I'm examining the code emitted for coremark, and the code starts at 0x4000 (see ../coremark_single_file/profile.s)
- `16:18 user` yes, remove the directive in crt0 and in the default
- `16:21 user` so, the simulator noting writes in the code area is functionally useless unless there is a proper separation of code and data
- `16:23 user` option 2, otherwise we can't clear bss
- `16:28 user` please commit
- `16:30 user` coremark_single_file works with -runoos and -runirc, fails with full assembler with writes to the code area and bad crc values. So the fault must lie in emit.c
- `17:05 user` [Request interrupted by user]
- `17:05 user` you can output a profile, that has addresses of instructions, Ican see that 0xde0 is in the middle of code
- `17:05 user` [Request interrupted by user]
- `17:06 user` generate addressed assembly with "sim_c -arch cpu4 -profile coremark.s 2> profile.s"
- `17:19 user` ../coremark_single_file returns the correct crc values, but the time is very wrong: "Total time (secs): -286.179321"
- `18:19 user` please commit
- `18:21 user` The sexp step before braun was concpetually useful for understanding, but is now a bit cumbersome. [Pasted text #1 +6 lines]
- `19:40 user` [Pasted text #1 +4 lines]
- `20:06 user` runoos and runirc no longer work
- `20:48 user` good. -runoos and -runirc now produce correct CRCs, but final assembly does not, so theres a bug in emit.c
- `20:54 user` ../coremark_single_file still fails crc
- `23:16 user` please commit

## 2026-04-10

- `08:02 user` update the docs with the current state
- `08:12 user` [Pasted text #1 +8 lines]
- `08:27 user` we should not be using malloc etc in braun onwards, convert to arena allocator
- `08:46 user` Consider this critique: [Pasted text #1 +6 lines]
- `09:07 user` please commit
- `09:21 user` cpu3 in this branch is obsolete. Plan a refactor that removes all cpu3 code, tests, and docs, to make the compiler pure cpu4
- `09:59 user` The -ann flag for annotating assembler should be restored, its important for debugging, the -On flag also will be relevant and should be restored
- `10:09 user` there dont seem to be any statements in the library code eg printf
- `10:14 user` carefully study the codebase, looking for code smells, obvious optimisations or redundancies, or actual errors. Fixed sized arrays which overflow should always cause compiler termination with error, for example
- `10:26 user` please commit
- `10:43 user` Please address this observation: [Pasted text #1 +15 lines]
- `10:50 user` Consider this critique: [Pasted text #2 +14 lines]
- `11:11 user` Consider this critque, the aim being to minimise ISA assumptions contaminating earlier pipeline stages as much as possible: [Pasted text #3 +87 lines]
- `12:28 user` ../coremark_single_file "smallcc -runirc coremark_single.c" fails, "smallcc -runos coremark_single.c" succeeds, something has gone wrong in IRC
- `13:29 user` Update all documentation and commit all changes
- `13:34 user` Some parts of compiler-pipeline.md are out of date, please check the whole document to make sure it all applies to the current situation
- `16:39 user` compiler-pipeline.md early sections need updating to reflect the removal of the sexp intermediate step; Regime 1 is now the AST tree
- `16:58 user` when IK_COPY or IK_ADDR are inserted, they get line=0, rather than inheriting from the spawning instruction
- `17:12 user` Plan a set of compiler optimisations, in regime 1, regime2, and a final peephole optimiser. Discuss the difficulty, risk, and  payoff of each item, and write a plan for incremental implementation
- `17:56 user` ../coremark_single_file fails
- `18:47 user` commit and move to the next phase
- `20:26 user` Next phase?
- `20:26 user` individually

## 2026-04-11

- `09:58 user` go
- `13:35 user` question, how are the various options enabled at the different -O levels?
- `14:28 user` Coremark/MHz has not really changed, and there are many poor instruction sequences in ../coremark_single_file/profile.s, like [Pasted text #1 +8 lines]
- `16:15 user` document *every* optimisation pass, then commit
- `19:21 user` examine the file ../coremark_single_file/profile.s. There are many examples of very poor code, eg [Pasted text #2 +5 lines] that are not caught by the optimisation passes. Please analyse and propose solutions for the issues you find
- `20:25 user` cpu4/cpu.py is the reference for the ISA, please update the document docs/cpu4.md
- `21:22 user` unneccesary sequence to cast (presumably), a constant [Pasted text #1 +4 lines]
- `21:33 user` note that coremark should be tested using ../coremark_single_file, please note in CLAUDE.md
- `21:35 user` 4736   0.39%  064d  |    immw r0, 255
        4736   0.39%  0650  |    and r5, r5, r0 should be zxb
- `21:41 user` unnecessary sign extend:       3920   0.32%  06ec  |    immw r0, 57
        3920   0.32%  06ef  |    sxb r0
- `21:54 user` please check the register colouring and allocation carefully. core_state_transition saves r4 and restores r4, but makes no use of the other registers r3,r5-r7 thrashing everything around in r0,r1,r2 with pushes and pops
- `22:10 user` crcu8 is not using all callee save regs, but in the hot loop does [Pasted text #2 +4 lines]
- `22:14 user` please commit
- `22:27 user` in core_state_transition, redundant jumps to next instr: [Pasted text #3 +5 lines]
- `22:33 user` in matrix_mul_matrix, loads through r0, then move to r6 then unnecessary sign extend. And multiplies by small powers of two.       2916   0.30%  0d8a  |    llwx r0, r0, 0
        2916   0.30%  0d8d  |    or r6, r0, r0
        2916   0.30%  0d8f  |    sxw r6
- `22:53 user` no loop rotation: [Pasted text #4 +10 lines]
- `22:58 user` suspect code in core_list_find: [Pasted text #5 +3 lines]
- `23:19 user` in matrix_mul_matrix_bitextract, very inefficient code: [Pasted text #6 +24 lines]
- `23:30 user` core_list_find, doing addi, to address, then load with zero offset: [Pasted text #7 +5 lines]
- `23:34 user` this could be llb r1:       2820   0.35%  137d  |    llwx r0, r0, 0
        2820   0.35%  1380  |    or r1, r0, r0
        2820   0.35%  1382  |    zxb r1
- `23:40 user` core_init_state: [Pasted text #8 +10 lines]
- `23:51 user` modified "andi rd, imm7" to not sign extend, more useful. Added "shrsi rd, imm", saves quite a few "immw;shrs". Changes are in cpu4/cpu.py,, please propagate to sim_c and doc, and make available to emit

## 2026-04-12

- `00:11 user` in crcu8, seems like could use shrsi instead of immw, shr. And jz r5 _crcu8_B9 instead of the two jump: [Pasted text #9 +3 lines]
- `13:11 user` commit and plan for switch using jump table, and inlining small functions
- `14:59 user` In this sequence in crcu8, the zxb is unnecessary, since there both xor operands have been anded with 1, and the "immw;shr" can be shrsi
- `14:59 user` In this sequence in crcu8, the zxb is unnecessary, since there both xor operands have been anded with 1, and the "immw;shr" can be shrsi [Pasted text #1 +9 lines]
- `15:33 user` please describe the inlining conditions
- `15:38 user` ok, so only crc16 and ee_isdigit would appear to qualify in coremark. What are the issues with allowing more than one statement?
- `15:59 user` not yet, I want to understand the tradeoffs. This code in core_state_transition could be compare/branch fused; the label is never a target: [Pasted text #2 +3 lines]
- `16:02 user` how much work?
- `16:05 user` please implement, and also there look to be cases where shrsi use has left dangling immw:                     |;; data >>= 1;
        4736   0.64%  055e  |    immw r0, 1
        4736   0.64%  0561  |    shrsi r2, 1
- `16:19 user` This sequence tries to save r0 to do the zxb, but r0 is effectively dead already: [Pasted text #3 +3 lines]
- `16:28 user` This section of code in code list find doesnt fuse compare branch, presumably because there is another use of r1 in the dead code just after: [Pasted text #4 +6 lines]
- `16:58 user` core_list_reverse is a critical loop, but wastes cycles rotating the registers. Investigate ways of unrolling this. [Pasted text #5 +9 lines]  One possible way: [Pasted text #6 +16 lines]
- `18:07 user` crcu8 seems to constants inside loop that I would expect LICM to hoist:       4736   0.67%  055a  |    inc r3
        4736   0.67%  055c  |    immw r0, 8
        4736   0.67%  055f  |    blts r3, r0, _crcu8_B2, also still spilling register inside loop when not all registers are live: [Pasted text #1 +3 lines]
- `20:22 user` excellent work! Analyse deeply the best approaches to clean up the whooe codebase

## 2026-04-13

- `09:10 user` implement these changes
- `10:07 user` consider all the optimisation passes that exist and the dependencies between them. document these in a new document, consider a bitmask approach to enabling them so we can explore effectiveness over multiple paths
- `10:29 user` coremark_single_file fails with -O0 (alignment error) and -O1 (writes to code area and infinite loop), but works with -O2. Use the bitmap capability to disable single passes to isolate responsibility. Obviously, there may be deeper reasons why this wont work, but start with that
- `10:47 user` tell me more about the safety net path
- `10:49 user` so the safety net is not guaranteed to produce correct code?
- `10:56 user` monotonic spilling guarantees convergence, but is this at a less optimal reg alloc in the general case? Shouldn't a monotonic spill pass *be* the safety net if a more aggressive IRC attempt fails to converge?
- `10:59 user` This seems sound. Earlier you talked about possible heuristics that might aid convergence, please do a bit of research into this; ideally we want to start with the most aggressive assumptions, and dial back gradually until we use a guaranteed but conservative approach if all else fails
- `11:14 user` Good. The number of iterations on tier 1 is only three, does the full aggressive IRC always tend to converge that quickly, or not at all?
- `11:17 user` great, please implement this tiered approach
- `11:25 user` please commit, then debug the coremark failures using the mask to disable single passes
- `11:52 user` great. please commit
- `12:08 user` write a script to test every possible optimisation mask with coremark_single_file, and show pass/fail, and the coremark/MHz number for each run on single line per run
- `12:28 user` the figure for coremark/MHz is the one output as "Iterations/Sec   : 1.5", not the 1e6/cycles
- `12:31 user` I'm not seeing the pass names with parallel
- `12:35 user` Interesting, see cm_results_sorted.txt. Even with no optimisation, we get a reasonable cm/MHz of 1.32, compared to the much lower results (~0.7) we were getting initially. I guess the effect of varous reg alloc and braun bugs?
- `12:38 user` please enhance sim_c with flag to output assembled code, nicely formatted with addresses, instruction encoding, symbols, and symbol table
- `15:25 user` This section of matrix_mul_matrix profile has clear optimisation potential, the inner loop is recalculating i*N every time, there is load to r0, move to r6 instead of load to r6
- `15:26 user` This section of matrix_mul_matrix profile has clear optimisation potential, the inner loop is recalculating i*N every time, there is load to r0, move to r6 instead of load to r6: [Pasted text #1 +39 lines]
- `15:36 user` yes, start with that, then general LICM
- `17:09 user` This hasn't actually fixed the inner loop of matrix_mul, although its flushed out other bugs. I agree that register pressure is tight, but I just handcoded the matrix_mul loop with the inner expr using 14 instead of 21 instructions. Please check for errors, I haven't included save callee: [Pasted text #2 +47 lines]
- `17:21 user` Great corrections, and nice inner body design. Please research approaches to fix this, with generality being important. GVN seems like long-term most principled, but see if anything else arises. This is an intersting target, the number of registers is such that optimisation is very sensitive
- `19:05 user` please implement phase 1
- `19:54 user` there is still a spurious load/move pair in B10:       2916   0.43%  0d66  |    llwx r0, r0, 0
        2916   0.43%  0d69  |    or r6, r0, r0
        2916   0.43%  0d6b  |    mul r0, r4, r1
- `20:01 user` ok, please give me the details of the proposed phase 2
- `20:06 user` This seems quite low risk, please implement
- `20:16 user` now look at implementing gvn
- `20:36 user` ok, so earlier you rewrote the matrix_mul B10 moving i*N out of the inner loop, yet the optimisations still cannot manage this. Can you explain why? And what approaches might succeed?
- `20:42 user` plan for LSR followed by pre-OOS GVN
- `21:37 user` how do you assess the current inner loop of matrix_mul? Is this the limit of the available optimisations? [Pasted text #1 +15 lines]
- `21:44 user` implement scalar promotion
- `22:11 user` i*N+k could be held as i*N*2, and incremented by 2 each k iteration
- `22:38 user` nice work, please commit

## 2026-04-14

- `10:08 user` matrix_sum is not using pointer arithmetic: [Pasted text #2 +6 lines]
- `10:18 user` printf is generating this code, but r1 does not appear to be live: [Pasted text #3 +8 lines]
- `10:37 user` In matrix_mul_matrix_bitextract_B10, there is a redundant move: [Pasted text #4 +8 lines]
- `10:58 user` In core_list_find, info->idx and info->data16 are invariant but are loaded inside the hot loops, even though there are free registers
- `11:09 user` step limit timeouts are infinite loop failures, 4e6 cycles is way more than unoptimised coremark take
- `11:18 user` please commit
- `11:23 user` On the principle that the earlier in the pipeline an optimisation happens, the better, perform a deep analysis of all optimisation passes to see if they could be done earlier, in a more principled way
- `11:30 user` yes
- `11:40 user` question: since you now have the live registers available at emit, is it easy to annotate each assembly instruction with a comment showing the mask? eg "shrsi r2, 1 ; live:00110100"
- `11:47 user` prefer to have the live on same line as instruction
- `11:53 user` comment should always start at a fixed column, eg col 40
- `11:57 user` This code in crcu8: [Pasted text #5 +4 lines] can be replaced with "xor r1,r2,r5;andi r1,1"
- `12:08 user` please explain the fix, and also tell me about generic arithmetic expression reductions; because of the "& 1" in the inner expr, the cast to unsigned is irrelevant
- `12:11 user` it does, and it is true that we're using coremark to drive the optimisations, but multiple pattern matches for specific cases is less elegant than a general known bits approach that may be more boradly applicable
- `12:33 user` code in crcu8 has 1) loop termination value always loaded with immw, when dead registers available, 2) spurious "immw r0,1": [Pasted text #6 +24 lines]
- `13:22 user` question: is licm on ssa? or on oos?
- `13:24 user` question: the LICM budget heuristic, what is it based on?
- `13:28 user` so we only know once we get to irc_allocate() whether we were pessimistic. If we make this optimistic, will be get correct but slow spilling?
- `13:30 user` So are there other heuristics that likewise we can only tell after irc_allocate() whether they were conservative or too aggressive?
- `13:35 user` right now, compiling coremark is basically instant. In the longer term, characterising the true effect of the heuristic parameters for better static values would be one improvement approach. Another would be exactly the speculative approach, at the cost of compilation time
- `13:39 user` I'd actually be interested that approach; centralising the parameters into an aggressive and conservative set, try both, pick the best
- `13:44 user` yes please
- `14:17 user` tell me the different param values for conservative vs aggressive
- `14:25 user` An interesting investigation, we should probably do a more principled exploration of each parameter in the future. Please do a sweep to make sure the code is clean, then update the optimisation doc
- `14:28 user` [Request interrupted by user]
- `14:29 user` please keep most details of the optimisation passes in the optimization-passes.md doc, and add references from the compiler-pipeline.md and from CLAUDE.md to that doc
- `15:01 user` [Pasted text #7 +3 lines]
- `15:21 user` [Pasted text #8 +8 lines]
- `15:39 user` in matrix_mul_matrix_bitextract, the second move from r6 is unnecessary, that is the last use of r6:
- `15:39 user` in matrix_mul_matrix_bitextract, the second move from r6 is unnecessary, that is the last use of r6: [Pasted text #9 +11 lines]
- `15:51 user` but that second mov is unnecessary too! It can just be "andi r6, 127; mul r0, r7, r6"
- `16:02 user` proposal: bit extract instruction f3c bitex rd, rx, imm10, where rd = (rx >> (imm10 & 1f)) & ~(-1 << (imm10>>5))
- `16:08 user` looks like 4 instructions with bitex, not 5, but yes, I'll think about shuffling the opcode space
- `17:52 user` I'll steal a bit from addli, and make a 9 bit bitex, since there are <512 usable shift mask combinations
- `18:13 user` I've added bitex to cpu4/cpy.py. Please can you check it, propagate to sim_c etc, and update the documentation
- `18:26 user` ok, now detect viable uses of bitex in the pipeline, prime candidate is matrix_mul_matrix_bitextract hot loop
- `18:40 user` please commit
- `20:33 user` are any parameters passed on r1-r3?
- `20:37 user` it was a general question, because I don't understand the moves of params in crcu8, seem like they could be kept in their orig registers:
- `20:37 user` it was a general question, because I don't understand the moves of params in crcu8, seem like they could be kept in their orig registers: [Pasted text #10 +33 lines]
- `20:43 user` theyre still used 600 times each, non-trivial. Shouldn't the colouring take account of the leaf-node nature of the function? there is no need to colour the caller-saved
- `21:05 user` no, this still needs better thinking. crcu8 is a leaf node, r0-r3 are available for use, why are params immediately moved? r2->r6, 0->r4, r1->r2, r4->r3, 8->r1, in the preamble? None of these are needed
- `21:38 user` [Request interrupted by user]
- `21:39 user` ok , step back entirely and revert to that last good state, this this thrashing around
- `21:44 user` ok, consider a narrowly scoped optimisation. crcu8 a) is a leaf node b) saves callee save regs r4,r5,r6 c) shuffles values around that came in r0-r3. There are two things: IRC has the wrong priors for this case, which may be a more compilcated thing to diagnose. There are biasses that can be imposed that minimise register moves that are unnecessay

## 2026-04-15

- `09:12 user` in crcu8, this code can be replaced with bitex: [Pasted text #12 +3 lines]
- `10:44 user` in ../coremark_single_file/profile.s, find me all the occurrances of immw followed by a branch using the register
- `10:48 user` please give me all of them, I'm looking at four new "compare immediate and branch" instructions. Interesting there are some that are not fused
- `10:52 user` please show me
- `11:14 user` theres quite a few r0=0 blt r0, rx, which is just bnez, and quite a few r0=0 beq rx,r0 and r0=0 bne rx,r0
- `11:15 user` no, these microoptimisations will disappear with the new instructions
- `13:18 user` I've updated cpu4/cpu.py. encoding has changed to give two new instructions: cbeq and cbne, compare with 8bit immediate and branch with 7 int signed displacement. Please check the file carefully to see everything is consistent and catch any mistakes I might have made
- `13:24 user` great. Please update the documentation and propagate the isa changes to sim_c. Update assembler/disassembler so that cb** rx, imm8, disp works, rather than a single aggregated immediate
- `13:39 user` integrate cbeq/cbne into the compiler
- `13:58 user` in ../coremark_single_file/profile.s, find me all the occurrances of immw followed by a branch using the register
- `14:50 user` changed encoding. I've moved cbeq/cbne to a new encoding that gives two more bits to displacement range. Pease carefully check cou6/cpu.py for mistakes, then propagate to docs and sim_c
- `15:04 user` please explain why the P17 range check does not use the full amount available
- `15:08 user` surely a second peephole pass could fixup
- `15:10 user` ok, fair enough, what about error recovery, right now silent failure and corruption isn't very friendly
- `15:12 user` these should actually terminate, the code is corrupt
- `15:35 user` I'm seeing some occurrances of multiply by 1:         54   0.01%  0874  |    immw r0, 1                           ; live:11110101
          54   0.01%  0877  |    mul r0, r0, r2                       ; live:11110001
- `15:39 user` This sequence is odd:        592   0.12%  04bb  |    immw r4, 0                           ; live:01010010
         592   0.12%  04be  |    or r2, r1, r1                        ; live:01010100
         592   0.12%  04c0  |    or r3, r4, r4                        ; live:01011100
- `15:44 user` please continue
- `15:49 user` please continue
- `15:54 user` please continue
- `15:58 user` please continue
- `16:06 user` please continue
- `16:07 user` strange sequence in crcu8: [Pasted text #1 +3 lines]
- `16:17 user` seems like a spurious immw r4, 0 in matrix_mul_matrix: [Pasted text #2 +21 lines]
- `16:22 user` yes please
- `16:53 user` inside core bench list, should use beqz:                     |;; if (this_find == ((void *)0)) {
         204   0.04%  1605  |    immw r0, 0                           ; live:11010011
         204   0.04%  1608  |    beq r4, r0, _core_bench_list_B5
- `19:08 user` strange construct: [Pasted text #1 +3 lines]
- `19:16 user` please commit
- `19:17 user` make a commit of everything, I'm about to change the ISA again
- `21:26 user` new cpu4/cpu.py. New instruction format created by dividing format 0 further, provides complete set of 2op + imm9 instructions, subsuming addli and bitex. Please check carefully for mistakes
- `21:43 user` Ok, please check again. I've removed the float-with-immediate, actually they don't make much sense
- `21:57 user` please check my fixes
- `22:05 user` added mod guards and aligned comments. Please propagate changes to docs and to sim_c, then make new instructions available to compiler
- `22:30 user` ok, there are many cases of "immw rx, i; les ry, rz, rx" and similar, which can be replaced with the new f0b 2op+imm instructions
- `22:43 user` please commit

## 2026-04-16

- `08:49 user` carefully examine the codebase, looking for code smells, areas for simplicfication and clarification without regression
- `08:56 user` do all items
- `09:11 user` in crcu8, r1 contains either 0 or 1, but then is loaded with 1 or 0 in crcu8_B5, crcu8_B6: [Pasted text #1 +24 lines]
- `09:37 user` This is not true, my last run from just prior to this work shows 497647, so there has been a regression. Also, the redundant immws in crcu8 are still there: [Pasted text #1 +18 lines]
- `10:26 user` yes, extend known-bits
- `10:30 user` please go
- `10:49 user` please explain in detail why the optimisation attempt did not work, and plan a strategy to allow it to work
- `10:59 user` please implement
- `11:11 user` The pattern is still there: [Pasted text #2 +24 lines]
- `12:00 user` excellent, please commit, then plan a deep dive examination of the optimisation passes, and consider the general principle of the earlier the better to see if passes can by reorganised in a principled way
- `13:29 user` both correctness preserving and potential speedup. But also reduced code size that is more principled and easier to reason about. Leave LICM move for separate evaluation, and compile time is not an issue
- `13:33 user` please proceed
- `14:41 user` Give me your impression of the cleanup
- `14:44 user` good. please commit. Do you think its worth keeping the speculative attempt infrastructure? Get feeling its a bit of unnecessary complexity
- `14:45 user` yes please
- `14:55 user` please commit
- `14:58 user` right, now phase 2, please recap me on the plan
- `15:01 user` phase 1 measurement
- `15:10 user` keep in place and proceed
- `15:19 user` next step
- `15:31 user` plan status?
- `15:33 user` attempt phase 2 step 3, but if it looks like you're spending a lot of time, revert
- `15:39 user` ok, go too step 4
- `17:34 user` q

## 2026-04-17

- `12:47 user` read the file ../coremark_single_file/profile.s and see if you can see opportunities for optimisations we are missing
- `12:55 user` yes
- `13:05 user` please implement the second step
- `13:14 user` Thats a big impact!
- `13:16 user` ok, now look at jump tables for switches. We considered this earlier, but that was before compare and branch, which makes the range check much cheaper
- `13:23 user` right now, we don't use the full jump range of branches, instead using a heuristic to account for not knowing true branch distance, this feels like performance being possibly thrown away
- `14:27 user` there was a regression, attached my terminal logs from pre- and post- branch estimator
- `14:27 user` there was a regression, attached my terminal logs from pre- and post- branch estimator: [Pasted text #1 +6 lines]
- `14:33 user` this is the conversation:  [Pasted text #2 +25 lines]
- `14:39 user` explain the trace seeding
- `14:41 user` this is the log: [Pasted text #3 +99 lines]
- `14:46 user` clearly it wasn't a transient. Do you not have access to your complete set of actions? Or do I?
- `14:49 user` great, now explain the heuristic and the failure mode
- `14:54 user` but emit does know the length of already emitted instructions, so could fixup backwards, or a complete pass with non-fused, or padding around fused could absolutely guarantee
- `14:56 user` I'd go for 2, since the current estimator is pessimistic (3 byte assumption) and the gains from changing the heuristic were actually quite large
- `15:20 user` please commit
- `15:28 user` ok, we're at 2.28 Coremark/MHz, this is quite competitive. Do another careful look at the current profile result, in ../coremark_single_file/profile.s, this time considering whether there are any zero or single op new instructions that might help?
- `15:44 user` do it. I initially thought 8 bits would be useful, but 7 covers normal ascii characters
- `15:51 user` please commit
- `17:44 user` Assess if cross block CSE causing the IRC allocator to blow up is still true, now that various bugs and optimisations have been found
- `17:47 user` Does it open the doo to other potential global optimisations?
- `17:49 user` Yes, try the sweeps
- `17:54 user` commit this tweak, and try options 2-4 as well
- `18:02 user` ok, commit this
- `18:05 user` Investigate the arena allocator use. It may be better to have a separate global and per-TU area. Make sure that per TU is cleared each TU. Also consider converting the arena allocator to a chunked arena so not limited to fixed size
- `18:33 user` I agree. But does the per-TU usage reset? Or just carry on growing?
- `18:34 user` ok, park for now
- `18:36 user` Certain aspects are algorithmically very inefficient, O(n^2), do a deep sweep and look for cases like this where it would be relatively simple to improve
- `18:45 user` we still have realloc?
- `18:47 user` arena realloc is fine. The tokeniser seems to have an actual realloc
- `18:47 user` Thats fine. Yes, do the tier 1

## 2026-04-18

- `11:59 user` please commit
- `12:08 user` move main branch to historic, make nanopass into main
- `12:09 user` please do
- `12:18 user` What do I have to do to be compliant with the coremark license if I want to include the coremark_single_file in the repo publicly?
- `12:22 user` what are the current heuristics for inlining and unrolling?
- `12:26 user` explain the inlining restrictions, seems like there is potential for a bit more performance if the body restriction is relaxed, but not a too great a complexity cost
- `12:30 user` please do the straight line prefix
- `12:41 user` please analyse ../coremark_single_file/dump.s looking at code size. We are at about the limit of performance, what opportunities are there for code densification? I can already see that there are many immw rx, 0
- `12:51 user` please commit
- `12:54 user` could have zero0 - zero7 one byte instructions, imms short immediate 2 byte, also neg and com 2 byte
- `12:57 user` yes, use that order
- `13:13 user` the bytecount looks wrong
- `13:17 user` please commit
- `13:18 user` I don't gave a feel for how good this density is, any figures on cortex m0+?
- `13:21 user` have a bit more of a dig into redundant moves, the "pre-existing bug" is a bit suspect and may no longer apply. If it does, it should be properly fixed
- `13:26 user` now look at the redundant moves, if they exist
- `14:36 user` we could add an f1a ins that either ignored one op, or did something potentially useful, before zero-extend-16, another for sxw, perhaps
- `14:40 user` any gain in an op like zxw(and/or) which degenerates to pure cross reg zxw if both operands the same?
- `14:42 user` yes, zxwor/sxwor + compiler fix
- `14:50 user` bytes is wrong
- `14:52 user` assess the move-before-store issue
- `15:00 user` agree. the callee save and ABI forced: if functions are not called from multiple places, the ABI is basically immaterial, and registers in theory can be allocated freely
- `15:02 user` please proceed with a
- `15:15 user` ok, we're now at 2.38 coremark/MHz and ~6kbytes with an ISA and compiler only a few weeks old. Very coompetitive
- `15:16 user` is M3 actually doing some dual issue? Or has it got a MAC?
- `15:17 user` please commit

## 2026-04-20

- `10:01 user` add sprintf and snprintf to stdio
- `10:17 user` I now have real hardware, with a memory mapped ascii framebuffer. What would you recommend as the best way of structuring libraries for both targetting the hardware, and maintaining compatibility with the current test structure?
- `10:37 user` please implement
- `18:49 user` zxwor and sxwor are not in cpu.py, check that sim_c, the python simulator and the python assembler all align
- `19:54 user` currently we have "clearmem _globals_start" in crt0.s. But this doesn't say how much memory should be cleared
- `19:58 user` yes please

## 2026-04-22

- `09:31 user` I'm working on a hardware implementation, and associated debugger. For this, I need compilation to emit debug information: [Pasted text #1 +21 lines]
- `09:32 user` please go with 1
- `09:44 user` Please change sim_c to not actually execute when running -dump or -linemap. Also, please add a help flag
- `11:30 user` add another option -hex <file> that outputs whitespace delimited hex bytes of the assembled code and data
- `11:38 user` in line with the other flags, please change -dump to take a file argument, rather than output to stderr
- `18:36 user` -linemap is slightly confusing, the DAP tool searches backwards from the pc to find the appropriate source line. When entering a function, the "enter" instruction does not appear to be counted as relating to the first line of the function, meaning that a source line from the previous function is highlighted.  Perhaps the func declaration line should be added to the linemap?
- `18:49 user` please update the help for smallcc

## 2026-04-23

- `10:44 user` please modify sim_c so that the trace option is "-trace <file>", it has no stdout or stderr, the fields are uniform, so "pc=xxxx ins=xxxxxx r0=xxxxxxxx ... sp=xxxx bp=xxxx lr=xxxx". All registers shown, insis 2,4,6 hex digits, left justified, with spaces to pad to constant width.

## 2026-05-02

- `14:59 user` plan: there is nothing inherent in the ISA that prevents a 32 bit data space for load/store, with stack and program space remaining at 16 bit space. Assess the implications for the compiler
- `15:05 user` sp and bp remain 16 bits, but pointers are always 32 bits. Data address generation is already 32bit register + offset or 16 bit bp + offset
- `15:07 user` sp and bp remain 16 bits, but pointers are always 32 bits. Data address generation is already 32bit register + offset or 16 bit bp + offset. We don't need to change IK_GADDR at all, everything compiler related lives in 16 bit space, but this change allows access to large address space on the hardware with very little change
- `15:12 user` I'd like you to document this but not implement at this stage. This is scoping out potential for the hardware system being implemented at ../cpu4_hardware, please take a look if you like.
- `15:22 user` good. Please research this: I'd like to implement CSP style channels (but with a single buffer) between contexts of the hardware cpu, and between multple instantiations of the cpu. This will require a minimal set of new ISA instructions to send, receive, test, and some notion of intrinsics in the compiler. Potential interesting sources of data: XMOS xc
- `15:32 user` Does this seem like a fun project? I'm trying to explore the parallelism path not taken, now that building a compiler, ISA, and hardware has been make much easier
- `15:35 user` Probably agent-based simulation, doesn't map very well onto GPU, but does to sea-of-processors
- `15:38 user` I have a zu3eg board for larger things, once I've reverse engineered the bitstream
- `15:42 user` claude code has made it possible to consider RE'ing the bitstream as just crunching through a set of work methodically. Conceptually easy, but too much work for humans to devote to without being paid, and corporate don't want to

## 2026-05-03

- `10:29 user` please tell me about c89 short int long and ptr sizes, and what the standard constrains about their relationship
- `10:31 user` I'm musing moving  pointers and ints to 32 bits, keeping long at 32 bits, and all compiler infrastructure remaining in 16 bits, as discussed earlier. With ptr at 32, it seems more natural for int to be 32 as well
- `10:37 user` ok, make sure the current state is committed, then move to 32 bit ptrs and int. Oh, one thing I noticed elsewhere was printf %lx was non-functional
- `10:40 user` [Request interrupted by user]
- `10:41 user` ok, dont persue the %lx bug if its not apparent, it could have been an artifact of other hardware failures, park that for now
- `11:13 user` ok, please have a look at the failure in ../coremark_single_file. I'd observe that pointer chasing is a large part of the list section, and the cycle drop is very suspicious
- `11:15 user` [Request interrupted by user]
- `11:15 user` please resume
- `14:08 user` the single file coremark is stripped back of as many defines etc as possible, and probably lost various sizes on the way. Please use the ../coremark, which is pretty much the original code, and create a new single_file version
- `14:55 user` Ok, so we're at 2.14 coremark/MHz, which is reasonable, but lower than the 2.37 we saw with LP32. This was correct, though, the crcs were cannonical
- `14:57 user` good work, we can chase a bit more performance later. But this sets the compiler up nicely for future work
- `15:18 user` plan: the ISA, the compiler and the hardware implementation of the ISA as a parallel barrel processor are closely linked but separate projects, each has its own claude code context. What is the best way for the two claude codes to coordinate and share knowledge? Is it ok to take looks at the others work? Or some other way?
- `16:23 user` please create abi.md and also write the coordination.md doc
- `16:30 user` please place a reference in your CLAUDE.md to coordination.md, I'll communicate this to hardware Claude
- `17:33 user` please not in abi.md that the hardware currently does not support exceptions, eg on misaligned addresses. This is future work of low priority
- `23:06 user` ok, hardware claude is like you! they've made little changes...

## 2026-05-04

- `10:56 user` please examine ../cpu4_hardware/docs/handoffs/smallcc-sxw-on-pointers.md
- `11:19 user` The test is identical to tests/cases/coremark/matrix_simple.c, probably, like reference docs, these tests should be symlinked or submoduled i the future

## 2026-05-07

- `21:57 user` please look at ../cpu4_hardware/docs/handoffs/smallcc-llbx-on-unsigned-char.md

## 2026-05-08

- `10:01 user` plan: I've placed a new test case in tests/cases/jpeg, a nanojpeg based decoder and test image, that should decode the image and display on a bitmapped framebuffer. I would like this to work in sim_c, which needs to be extended to support 32MB RAM, and the MMIO framebuffer control detailed in ../cpu4_hardware/docs/architecture.md. Two strands, first upgrade sim_c with simple tests, then get the jpeg decoder working, which appears to be a reasonably good stress-test of smallcc
- `10:56 user` Please commit here, then continue debugging
- `11:33 user` please continue
- `11:42 user` [Image #1]
- `12:16 user` Perfect! Confirmed working on hardware too
- `12:17 user` yes please
- `12:20 user` please update sim_c to error on out-of-ramge immediates, there is no usecase for silently masking
- `12:28 user` Great work. It seems that porting real-world apps is a great stress tester
- `12:33 user` can you upgrade the tests/cases/jpeg test to a proper full test please? Not sure exactly what best approach would be for testing against golden decoded image, whether it should be a extended test (not sure how long it takes to run in sim_c) rather than standard regression
- `12:52 user` good work
- `12:54 user` [Image #2] pic of the actual hardware

## 2026-07-02

- `12:17 user` This project has reached a relatively stable state. Please examine and see what could be tidied up, any code smells, organisation. But also, are there important architectural wins that we've missed, that don't compromise the ideals of a small and comprehensible compiler
- `12:34 user` Excellent. Please follow your proposed order
- `14:11 user` I think the "Errors detected" is just noise from not running 10s worth, immaterial for compiler benchmarking
- `14:57 user` So, a lot of my tuning the compiler to a good coremark/MHz was looking at profile outputs and spotting the holes in optimisation. You are a much more capable model than I was using at the time, could you profile coremark and see if you can spot some optimisation possibilities? This can include ISA changes too, if a point ISA change can hit a hotspot and is probably of general use
- `17:43 user` ok, sketched in a dbnz instruction, f3d, 1 op with imm10 displacement in cpu.py. Go with down count conversion and use the new instruction. Sim etc will need updating, please check my working
- `18:07 user` please look at core_state_transition
- `18:27 user` Ok, CM/MHz is now 2.68. This is world class for a single issue processor, very good
- `18:29 user` Yes. My processor arch is a barrel processor with eight contexts, so each context will see zero branch penalties. The aggregate CM/MHz should be identical to the instr counted number
- `18:41 user` is everything committed?
- `18:43 user` please clear them
- `20:29 user` please can you write a handoff for the new instruction and sign issue
- `20:31 user` great, thanks

## 2026-07-12

- `13:54 user` An experimental ray tracer surfaced some bug, please can you examine ../ray_tracer/docs/smallcc-bug-struct-args.md
- `14:19 user` please update abi.md to reflect compiler
