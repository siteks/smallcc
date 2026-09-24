# 0001 — Function-static data (`_ls<n>`) is emitted without `align`: a `static const uint32_t[]` after a string literal is loaded from the wrong address on cpu4

Status: fixed (2026-09-24)
Severity: high (silent data corruption: any function-static array/scalar wider than a byte that follows odd-length data in the image; no diagnostic)
Found: 2026-09-20, xilinx_rev_eng cpu4 SoC bench (`fill_bars.c` colour bars showed no green; ~20 h chased as a silicon fault, see xilinx_rev_eng `docs/issues/0143`)

## Symptom

```c
static const uint32_t col[8] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0};
...
printf("bars written\n");
```

`col[0]` reads `0xffff000a`, `col[1]` reads `0x0000ffff`, ... on cpu4 (KU5P SoC, `cpu4_ku5p.py run`):
every element is the aligned word straddling the table. The same table in a program whose image happens
to place it on a 4-byte boundary reads correctly (`&col = 0x5f4`). In `fill_bars.c` the table sits at
image byte 1366 (2 mod 4); in a variant with different strings at 1723 (3 mod 4).

## Reproduce

```sh
./smallcc -arch cpu4 -O2 -o t.s t.c && ./sim_c -hex t.hex t.s
```
with `t.c` = the snippet above plus a `printf("%08x\n", col[0])`. Data tail of `t.s`:

```
    align
_globals_start:
_l0:
    byte 98
    ...
    byte 10
    byte 0          ; 14 bytes of string literal
_ls0:               ; <- no align: 2 mod 4 here
    long 16777215
    long 16776960
```

## Diagnosis

`braun.c` `emit_static_local_data()` writes `_ls%d:` directly (braun.c:125), unlike the global emitter,
which precedes every multi-byte global with `align` (emit.c:2742, `if (size >= 2) fprintf(out, "    align\n")`).
The assembler's `align` pads to a 4-byte boundary (sim_c.c:761). The cpu4 word load ignores the two
low address bits (the hardware is ALIGNED-ONLY; `sim_c` enforces `check_align32` -- a `-hex` build
never runs the simulator's check, so nothing flags it). The result is the word at `addr & ~3`, i.e. a
byte-rotated neighbour of the intended element. Scalars (`static int`) and 16-bit data after
odd-length strings are exposed the same way; string arrays (byte data) are not.

## Resolution

Emit `align` before `_ls%d:` in `emit_static_local_data()` when the static's element (or scalar) size
is >= 2 -- the same rule as the global path -- and mirror it in `braun_render_static_local()` if that
path produces its own bytes. Consider also having `sim_c -hex` (or the assembler) reject a `long`/`word`
directive at an unaligned `cur` with the label named, so the class cannot recur silently.

## Fix (2026-09-24)

- `braun.c` `emit_static_local_data()` now emits `align` before `_ls%d:` for any static whose size
  is >= 2, matching the global path. Applies to both the initialized-data and BSS (`allocb`) outputs;
  the BSS case was exposed the same way (`static short` followed by `static int` left the `int` 2 mod 4).
  `braun_render_static_local()` needed no change: the irsim allocator (`alloc_data`) already aligns.
- `sim_c` assembler: a `long` directive at an address that is not 0 mod 4, or a `word` directive at an
  odd address, is now an assembly error naming the preceding label
  (`asm error (line N): long directive at unaligned address 0x0532 (after label _ls0); ...`).
  This runs in pass 2 so `-hex`/`-dump` builds catch it too, closing the silent path.
- Found while writing the reproducer: scalar static locals whose initializer is not a bare `int`
  literal (`static short s = 0x1234;`, `static unsigned u = 5;`, `static long l = 9;`,
  `static int n = -1;`) were misclassified as zero-initialized BSS because `static_local_is_bss()`
  tested `init->kind == ND_LITERAL` without looking through the `ND_CAST` that `insert_coercions`
  wraps around the literal, or through unary minus. The classifier, the assembly emitter and the
  irsim renderer now share one `static_init_literal()` helper. A 1-byte static also now emits
  `byte` instead of a 2-byte `word`.
- Tests: `tests/cases/alignment/static_local_after_string.c` (this issue),
  `tests/cases/init/static_local_scalar_coerced.c` (the dropped-initializer bug).
