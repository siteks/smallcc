# Reply: sxw on pointer arithmetic — fixed in compiler

**Re:** `cpu4_hardware/docs/handoffs/smallcc-sxw-on-pointers.md`

**Status:** fixed in smallcc commit `8999b28` (`braun: widen pointer arithmetic
to VT_PTR even when n->type is t_void`).

**What changed in the compiler**

Root cause was in the IR construction stage (`braun.c`). The parser
intentionally types stride literals (the `4` in `a + i*4`) as `t_void` so
`insert_coercions` doesn't try to apply a second round of scaling. That
`t_void` propagates up through `binop_result_type` and reaches braun, where
the historical fallback was `t_void → VT_I16`. Under LP32 that matched the
int width and silently worked; under ILP32 it produced an i16 ADD between a
VT_PTR (32-bit) and an i16 (16-bit) operand, truncating the pointer's upper
half. A later SEXT16 then sign-extended the truncated address — filling the
upper half with `0xFFFF` whenever `bp[15]=1`.

The fix is in `cg_expr`'s ND_BINOP handler: regardless of the parent's
inferred vtype, *operand-driven widening* now applies. If either operand is
VT_PTR, the result is VT_PTR. If an inferred narrow result (i8/i16/u8/u16/
VT_VOID) sees a 32-bit operand, the result widens to match. The default for
genuinely-unknown types becomes VT_I32 (the ILP32 int width).

**Confirmation**

```
$ ./smallcc -arch cpu4 -o /tmp/m.s ../cpu4_hardware/test/cases/coremark/matrix_simple.c
$ grep -c '\bsxw\b\|\bsxwor\b' /tmp/m.s
0
$ ./sim_c -arch cpu4 /tmp/m.s | head -1
r0:00000045 ...   # 0x45 = 69 ✓
```

The exact `lea r5, -48 ; add r5, r5, r4 ; sxw r5 ; ... ; lll r5, ...` pattern
called out in the handoff is gone from `matrix_simple.c`. CoreMark CRCs are
unchanged (canonical: 0xe714 / 0x1fd7 / 0x8e3a / 0xe714) and the full
smallcc pytest suite (301 cases) still passes.

**ABI clarification**

The `abi.md` §1 contract — "pointers are stored as 4-byte values with the
upper half zero for any address the compiler emits" — stands as-is. No
relaxation needed. The compiler now genuinely upholds it.

**On the hardware-side mitigation**

The mitigation in `cpu4_hardware/rtl/top.v` that treats both `0x0000` and
`0xFFFF` upper halves as BRAM-routed is no longer needed for
compiler-emitted code. Whether to keep it is your call. Two angles:

* **Defence in depth:** any future regression that re-introduces the
  sign-extension bug is silently caught (the test still produces the right
  answer instead of garbage). For a smallcc bug class that's already shipped
  once, that's worth something.
* **Cost:** loses the upper 64 KB of the SDRAM byte-address space
  (`0xFFFF_0000`–`0xFFFF_FFFF`). Practically zero impact today.

If the mitigation stays, `abi.md` could note in §1 that the hardware
*tolerates* `0xFFFF` upper halves on stack-relative addresses even though
the compiler doesn't emit them — that's a cheap addition to the spec.
Happy to add that note if you want it.

**Test corpus**

`matrix_simple.c` should graduate from cpu4_hardware's local test directory
into smallcc's `tests/cases/` so any future regression of this exact bug is
caught at compile-time on this side. I'll wait for your sign-off before
moving it (the file currently lives in your tree, and I want to copy rather
than steal).
