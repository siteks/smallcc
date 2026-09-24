# 32-bit Data Pointers — Scoping (superseded — landed as ILP32)

**Status:** **superseded by the ILP32 transition.** The proposal here was to
widen pointers to 4 bytes while keeping `int` at 2 bytes. In the end the
project went one step further and adopted the full **ILP32** model (`int` is
also 4 bytes), giving `sizeof(int) == sizeof(void*)` and matching every
mainstream 32-bit C target. See the commit `types: switch to ILP32 — int
and pointer are now 4 bytes` for the final landing.

The analysis below is preserved as-is for reference. Notes where the actual
landing diverged from the original plan:

* `int` was widened to 4 bytes (this doc proposed keeping it at 2). Removes
  the int↔ptr coercion that this doc had to add.
* `unsigned short` now promotes to `int` (not `unsigned int`).
* `(int)ptr` round-trip is a no-op again (was going to need TRUNC/ZEXT).
* Struct globals get a new `(gfields ...)` sexp form because field sizes
  are heterogeneous (int=4, char=1, etc.).
* A latent parser bug — `f`/`F` treated as a float suffix on hex int
  literals like `0xbeef` — was masked under LP32 and shaken out by the
  widening; fixed in the same commit.
* `lea` in sim_c.c is now explicitly masked to 16 bits so the upper half
  of stack-relative addresses is provably zero.

----

(Original scoping below.)

## Motivation

The CPU4 hardware target (`hw/` in the processor monorepo) has a 32 MB SDRAM available
behind the pbus, while CPU code and stack live in 64 KB of BRAM. The CPU's
register width is already 32 bits, and the load/store instructions whose base
operand is a register (`llb`/`llw`/`lll` and stores in F3c) already accept a
full 32-bit base address. The only thing forcing the C side to a 16-bit data
world is the compiler's `WORD_SIZE = 2` assumption that ties `sizeof(void*)`
to the 16-bit code/stack address space.

If pointers become 4 bytes (with the upper 16 bits zero for compiler-generated
addresses but free to be nonzero for user-computed ones — casts from `long`,
MMIO reads, DMA descriptors, etc.) the hardware can address SDRAM directly
through ordinary C pointers with **no ISA change** and very little compiler
change.

## What does NOT change

- **ISA.** Zero changes. F3c already takes a 32-bit register base; F2 stays
  16-bit `bp`-relative which is correct for stack/locals.
- **`IK_GADDR` lowering.** Globals live in the 64 KB code/stack space, so
  `immw rd, label` continues to produce a clean 32-bit address with the upper
  half zero. No `immwh` is needed for global addresses.
- **`bp`-relative load/store.** `lea rd, imm14` still computes `bp + offset`;
  with `bp` being a 16-bit value and 32-bit register-width arithmetic, the
  result naturally lands in `0x0000_xxxx`. (Worth verifying once in
  `sim_c.c` and the Verilog `lea` implementation that the upper half is
  cleanly zero, not sign-extended from `bp[15]`.)
- **Register allocator, SSA passes, optimization pipeline.** All values are
  already 32-bit at the IR level; nothing notices that a pointer-typed value
  now occupies 4 bytes of memory rather than 2.
- **`sp`/`bp`.** Remain 16-bit. Stack stays in low 64 KB. `enter`/`ret` packed
  `(lr<<16)|bp` word is unaffected.
- **Function pointers.** `pc` is 16-bit, so a function pointer is semantically
  16-bit. For C ABI uniformity it is *stored* as a 4-byte value with the
  upper half zero; `jlr rd` already masks to 16 bits so the high half is
  ignored at call time.

## What does change

The change is concentrated in the C front-end's notion of pointer size and
the small amount of stack-ABI math that depends on it.

### Type system (`smallcc.h`, `types.c`)

- `WORD_SIZE` — the core knob — moves from 2 to 4. Currently:

  ```c
  // smallcc.h
  #define WORD_SIZE      2   // size of int and pointer (16-bit target)
  #define FRAME_OVERHEAD 8   // enter saves lr+bp (4 bytes each); params start at bp+8
  ```

  The comment on `WORD_SIZE` conflates two things — `sizeof(int)` (which stays 2)
  and `sizeof(void*)` (which becomes 4). The constant should be renamed/split:
  one for pointer/slot size (4), one for `int` size (still 2).

- Pointer type singletons set `size = 4, align = 4`. Function types follow the
  same rule (size 4 for ABI even though semantically 16-bit).
- `t_int` stays at `size = 2`. `int` and pointer sizes diverge for the first
  time in the codebase.
- `intptr_t` / `ptrdiff_t` typedef to `long` (`include/stdint.h`,
  `include/stddef.h`). `size_t` — keep `unsigned int` (16 bits, sufficient
  for any single allocation in the 64 KB stack/global region) or widen to
  `unsigned long` if user code is expected to do `memcpy` across SDRAM.
  Recommendation: widen.

### Frame and ABI math (`types.c`, `legalize.c`, `braun.c`)

- **Stack-passed args.** Currently `push_args_list`-style code uses
  `WORD_SIZE` for everything narrower than 4 bytes. With pointers at 4 bytes
  the stack-arg slot rule becomes:
  - struct → WORD_SIZE (passed as hidden-copy pointer, now 4 bytes)
  - 4-byte type (long/float/double/pointer) → 4 bytes
  - char/short/int → still 2 bytes (or widen-to-`int` 2 bytes)

  The mixed-size slot logic in `types.c` `insert_ident` and the caller-side
  push code in `braun.c` already branch on `type->size`, so the change is
  mostly: `WORD_SIZE` references become `t_pointer->size` or just the
  literal 4 where they actually mean "pointer slot".

- **Register-passed args (params 1–3).** No change. `r1`/`r2`/`r3` are 32-bit
  regardless of declared C size; a pointer arg fits in one register exactly
  as `int` does today.

- **`FRAME_OVERHEAD`.** The 8-byte value captures the bp-relative offset of
  the first param after `enter`'s save area. On CPU4 `enter` packs
  `(lr<<16)|bp` into a single 4-byte word (see `docs/isa/cpu4.md`), and
  param 0 is at `bp+4`, not `bp+8` — the constant's name and comment are
  CPU3-era stale. Worth fixing in the same pass as the `WORD_SIZE` split.

- **Hidden sret pointer.** The `add_struct_return_param` pass shifts all
  `SYM_PARAM` offsets by `WORD_SIZE` to make room for the hidden return
  buffer pointer. With pointers at 4 bytes the shift is 4. Already driven
  from `WORD_SIZE`, so falls out automatically.

### `stdarg` (`include/stdarg.h`, `braun.c`, `lib/stdio.c`)

- `va_list` typedef changes from `int` (2 bytes) to `long` (4 bytes), since
  it holds a pointer.
- `va_start(ap, last)` arithmetic — already uses `last_param.size` and
  `bp + offset`, so falls out automatically.
- `va_arg(ap, T)` advances by `sizeof(T)`. For `T = ptr_type` the advance
  is now 4. Already correct.
- `printf("%p", p)` in `lib/stdio.c` — extend hex width from 4 to 8 digits.
- `printf("%s", s)` — pulls a 4-byte pointer from `va_list` instead of 2.
  Falls out from the `va_arg` size, but worth grepping for any hardcoded 2.

### Casts and coercions (`parser.c` `insert_coercions`)

- `(int)ptr` — needs an explicit `IK_TRUNC` (4 → 2). Previously a no-op
  because both were 2 bytes.
- `(ptr)int` — needs an explicit `IK_ZEXT` (2 → 4). Previously a no-op.
- The legalize Pass D infrastructure already lowers `IK_ZEXT`/`IK_TRUNC` to
  mask-and; nothing new is needed in the backend. The change is purely in
  `insert_coercions` to *emit* these where they were previously elided.

### Globals data section (`emit.c` `emit_globals`)

- Pointer-typed global initializers currently emit `word label` (2 bytes).
  They become `long label` (4 bytes). This affects:
  - `char *names[] = { "a", "b", ... }` — array of pointer-to-string-literal
  - `struct { char *p; } g = { "abc" }` — struct fields of pointer type
  - Any global function-pointer table.
- The `gvar` sexp emitter and the array-of-string-pointer special case both
  need to switch from 2-byte to 4-byte slots when the element type is a
  pointer.

### Simulator (`sim_c.c`)

If the goal is just to make `sizeof(void*) == 4` observable to user code
without yet using SDRAM, no simulator change is needed — the high half of
every compiler-emitted address is zero, so loads/stores still hit the 64 KB
`mem[]` array.

If the goal is to actually exercise SDRAM addresses from the simulator, then:

- Grow the simulated memory (or model SDRAM as a separate region above
  some boundary, e.g. `0x0001_0000` and up).
- Decode 32-bit effective addresses in F3c load/store paths instead of
  masking to 16 bits.
- Decide policy for unaligned/out-of-range accesses.

Recommendation: do this as a follow-up *after* the compiler change, so the
two can be tested independently.

## Risks and verification

- **`lea` upper-half cleanliness.** Confirm in `sim_c.c` and the Verilog
  ALU that `lea rd, imm` produces `0x0000_xxxx` for any in-range frame
  offset, regardless of whether `bp` is treated as a sign- or zero-extended
  16-bit value. This is the foundation of "stack pointers are 32-bit values
  with high half zero".
- **Struct layout churn.** Every struct containing a pointer changes size
  and field offsets. CoreMark's `list_data_t`, `mat_params_t`, etc. shift.
  Tests that hardcode struct sizes or field offsets need re-baselining.
- **Test fallout.** Any test that does `int x = (int)&y; ... return (int*)x`
  or assumes `sizeof(int) == sizeof(void*)` will need updating. Likely a
  handful in `tests/cases/`.
- **`int main(int argc, char **argv)`.** `argv` becomes a 4-byte pointer to
  a 4-byte-pointer array. The runtime startup (`lib/crt0_cpu4.s`) needs to
  match.
- **Data-section size.** Globals containing pointers double in size. CoreMark
  has many — expect a small (single-digit-percent) data-section growth.
- **`%p` printf width.** Cosmetic but noticeable in golden-output tests.
- **`NULL`.** Defined as `0` in `include/stdlib.h`. Zero-extends to a 4-byte
  zero pointer correctly; no change needed.

## Performance expectations

- **Hot paths (bp-relative locals).** Zero change. F2 load/store unaffected.
- **Global pointer access.** Zero change. `immw + load` is the same number
  of bytes.
- **Pointer-in-pointer access.** Zero change. F3c load with 32-bit base.
- **Pointer comparison.** Zero change. Already a 32-bit register compare.
- **Pointer arithmetic stride scaling.** Zero change. Driven by
  `elem_type->size`, not by pointer size.
- **Stack frame growth.** Pointer locals grow from 2 to 4 bytes; small effect
  on overall frame size (often dominated by buffers).
- **Stack-passed pointer args.** Caller pushes 2 extra bytes per stack-passed
  pointer arg. Most calls take ≤3 args (register-passed), so the impact is
  small.
- **Code size.** Effectively unchanged. The instruction stream is identical;
  only data layout changes.

CoreMark estimate: <1% impact on cycle count, small data-section growth.

## Suggested rollout

1. Audit all `WORD_SIZE` uses; classify each as "pointer/slot size" or
   "`int` size" or "should-be-renamed". Rename to disambiguate.
2. Change pointer/function `Type` factory size+align to 4 in `types.c`.
   Build; expect test failures concentrated in struct-layout and stack-arg
   tests.
3. Update `insert_coercions` to emit `IK_ZEXT`/`IK_TRUNC` for `int↔ptr`
   casts.
4. Update `emit_globals` to emit `long label` for pointer-typed initializers.
5. Update `va_list` typedef and `printf("%p")`.
6. Re-baseline tests; investigate genuine miscompiles vs. cosmetic golden
   shifts.
7. (Follow-up) Extend `sim_c` to model SDRAM at addresses ≥ `0x1_0000`.
8. (Follow-up) Provide a small library API for constructing high-half
   addresses (`make_far_ptr(uint32_t addr)`) so user code does not need
   per-call `(uint32_t)` casts.

## Why now / why later

**Now (this doc):** the analysis is fresh; capturing it avoids re-deriving
the touch-list when the hardware-side SDRAM path is ready.

**Later (implementation):** the compiler change is small but invasive
(every struct with a pointer field re-lays-out); ideally batched with the
hardware milestone where SDRAM access becomes a real user-visible feature.
Until then, all data fits comfortably in 64 KB and the ABI churn would buy
nothing.
