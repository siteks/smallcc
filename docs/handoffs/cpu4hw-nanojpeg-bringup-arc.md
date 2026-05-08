# NanoJPEG bring-up — bug arc summary, ABI/encoding edge cases

**To:** cpu4_hardware Claude
**Subject:** Test corpus mining: nine compiler bugs found bringing up the
NanoJPEG decoder, two of which are ABI/encoding-shaped and worth replicating
in the RTL test suite.
**Status:** All bugs fixed in smallcc; cat image decodes correctly at -O2 in
sim_c and on hardware (confirmed by Simon).

The full bring-up sits in commit range `e7976f8..866dea3` (latest at HEAD),
visible in `git log --oneline`. This note distills it into the part you'd
want to mine for RTL test coverage.

## Why this matters for hardware

Of the nine compiler bugs, **seven are pure compiler logic** — wrong IR
shape, wrong pass ordering, wrong liveness handling. Those fix in smallcc
alone and the RTL never sees a wrong instruction byte.

**Two are encoding/ABI-shaped** and *would* hit the RTL identically if
either the compiler or a hand-written asm program produced the same
instruction stream. Those are worth knowing about because they're the
class of bug where "smallcc tests pass + sim_c happy" might still mean
"compiled program is wrong on real silicon."

The list below is in chronological order of discovery; ABI/encoding
bugs are flagged ★.

## The nine bugs

| Tag | Commit | Class | Description |
|---|---|---|---|
| 1 | `a9a28f1` | opt | `opt_remove_dead_blocks` left phi operands behind when stripping pred edges |
| 2 | `a9a28f1` | codegen | function-static `static const T arr[] = {…}` initializers silently dropped — emitted as `allocb size` instead of per-element bytes |
| 3 | `ffa5363` | regalloc | IRC tier-3 monotonic spilling never converged on rematerializable values (constant flood from IDCT W-coefficients) |
| 4 | `ffa5363` | codegen | `cg_expr` on `ND_UNARYOP "*"` of pointer-to-array type emitted a spurious load instead of decaying to address |
| 5 | `834533d` | opt | `unwrap_for_mask` walked through `IK_COPY` chains rooted in `IK_PARAM` / `IK_CALL` landings, extending caller-saved live ranges across calls |
| 6 | `6c1a9e7` | codegen | Braun `for(;;)` infinite-loop path sealed `body_blk` at 0 preds → cached const 0 for every loop-carried variable |
| 7 ★ | `3afcd00` | **emit/encoding** | `emit_prologue` / `IK_RET` used F2 `sl`/`ll` (imm7) for callee-save store/restore unconditionally; on >256-byte frames the offset overflowed imm7 and the assembler silently masked it |
| 8 | `866dea3` | parser | `sizeof(s.x)` and `sizeof(p->f)` returned 0 (parser evaluated literal at parse time, before `derive_types`) |
| 9 ★ | `866dea3` | regalloc | IRC tier-3 force-spilled already-slotted values, leaving them uncolored; emit.c's `r0` fallback collided with the spill-store's `IK_ADDR` register |

## ★ Bug 7 (commit `3afcd00`): silent F2 imm7 overflow

**This is the one most worth filing as an assembler defect, separate from
the compiler fix.**

emit.c was emitting `sl r4, -79` for callee-save store when the saved
register's frame slot was 316 bytes below bp. F2 `sl rd, imm7` has a 7-bit
*signed* immediate (-64..63), scaled ×4 for the 32-bit form → byte offsets
-256..252. -316 byte offset means imm7=-79, which is outside that range.

The current sim_c/cpu4 assembler [sim_c.c:803] just does:

```c
uint8_t b1 = (uint8_t)(((rx2 & 1) << 7) | (imm7 & 0x7f));
```

`imm7 & 0x7f` masks -79 (= 0xFFFFFFB1 as int32) down to 0x31 = +49. The
encoded instruction stores at `bp + 49*4 = bp+196` instead of `bp-316`. No
diagnostic.

In the NanoJPEG bring-up this corrupted the saved callee-saved register
("saved" to bp+196, then "restored" from bp+196 reading whatever was there),
so the restored register held junk; on the next ret the unpacked
`(lr<<16)|bp` word was junk and PC went somewhere bizarre. Symptom in sim_c
was "main appears to be re-entering" because the corrupted PC happened to
land at 0; on hardware the same pattern would just go to whatever address.

Compiler fix is in emit.c: when the byte offset is outside `f2_range_long`,
fall back to F3b `lea` (imm14×4, ±32K range) plus F3c `sll`/`lll` (3-reg
indirect). That's safe.

**Hardware/assembler-side action items:**

1. **Assembler should refuse out-of-range imm7** (and imm9, imm10, imm14,
   imm16) instead of masking. A diagnostic at assemble time would have
   caught this immediately. Same logic applies to all the variable-width
   immediate fields. Test coverage: hand-craft `sl r4, -79` in an .s file
   and confirm the assembler errors rather than silently producing 0xa1
   0xb1 (or whatever the masked encoding ends up being).

2. **RTL test for full-range F2 offsets.** Generate stores/loads that
   exercise the legitimate ±63 imm7 range AND verify that imm7=−64 and
   imm7=+63 each compute the correct effective address (sign extension
   of the 7-bit field). This is the kind of bug that's only visible
   when the compiler asks for the boundary cases, which it didn't until
   a function with frame > 256 bytes appeared.

3. **`tests/cases/abi/large_frame_callee_save.c`** in this repo (added
   in commit `3afcd00`) is a self-checking 568-byte-frame test that
   exercises the F3b/F3c fallback path. Symlink it into the hardware
   test corpus per `coordination.md` so the RTL tests it too.

## ★ Bug 9 (commit `866dea3`): IRC over-spill / register clobber

This one is a register-allocation bug, not an ISA encoding bug — but the
instruction stream that came out of it would still corrupt data on
hardware. Mentioning it for completeness:

The compiler emitted `lea r0, -1248 ; sll r1, r0, 0` where `r1` was the
input pointer (still live) and `r0` was supposed to hold the spilled value
from the previous `shli r0, 11`. The lea clobbered r0 before the store
read it, so the store wrote `r1` (input pointer) to the spill slot
instead of the shifted value. The IDCT then ran on garbage coefficients
and produced saturated-color output.

This is a "well-formed but semantically wrong" instruction stream. RTL
sees the `lea` and `sll` instructions and dutifully executes them; both
encode correctly. There's nothing for the assembler or RTL to refuse.

Test coverage equivalent: any IDCT-heavy code (or really any code with
register pressure high enough to spill into out-of-F2-range slots) hits
this path. The cat JPEG decoder is now in `tests/cases/jpeg/jpeg_test.c`
(via the canonical test corpus) so will exercise this on the hardware
side too.

## The other seven bugs

For completeness and so you can search the smallcc commit log if anything
similar shows up in your testing:

- **Bug 1, 4, 5, 6, 8**: pure SSA / parser / opt issues. The wrong IR
  produced wrong asm, but the asm itself was always well-formed
  instructions. Nothing for the RTL to look at.
- **Bug 2**: `static const T arr[] = {…}` was emitting `allocb size`
  instead of the actual byte values. The data section came out
  zero-filled. Visible in any function-local table init; NanoJPEG's
  njZZ table happened to be at file scope so wasn't affected, but
  njDecodeDHT's local `static unsigned char counts[16]` was a related
  near-miss (turned out to be uninitialized which is correct for an
  uninitialized static).
- **Bug 3**: pure regalloc convergence; IRC just kept iterating without
  reaching a stable coloring. Manifests as the "EXHAUSTED N iterations"
  warning previously seen on njRowIDCT/njColIDCT.

## Coordination note

Per `coordination.md`, smallcc's `tests/cases/` is canonical for the
behavioural test corpus. The hardware repo should consume it via
symlink/submodule, including:

- `tests/cases/abi/large_frame_callee_save.c` — F2 imm7 overflow regression
- `tests/cases/loops/for_infinite_carries.c` — Braun `for(;;)` regression
- `tests/cases/init/static_local_array.c` — function-static array init
- `tests/cases/jpeg/jpeg_test.c` + `test_cat_jpeg.h` — full integration test;
  expects to write a 320×240 32bpp framebuffer to SDRAM at 0x10000 and set
  `DISP_MODE = 1` via MMIO.

If your test runner can compare framebuffer output against a golden PPM
(equivalent to sim_c's `-fb FILE` flag), the JPEG test becomes a useful
end-to-end smoke test.
