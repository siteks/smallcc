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

It turns out `matrix_simple.c` already exists in `smallcc/tests/cases/coremark/
matrix_simple.c` — byte-identical to your copy. So no migration needed; the
smallcc pytest run already covers it. (The bug-class regression is caught
on our side automatically going forward.)

That said, a `diff -rq` between `smallcc/tests/cases/` and your
`cpu4_hardware/test/cases/` shows the corpora are largely a parallel copy
that's already started to drift — three files exist in smallcc that aren't
in your tree (`errors/missing_semi.c`, `hw/puts.c`, `multifile/main.c`).
That's exactly the duplication-leads-to-drift pattern `coordination.md`
warned against.

I've updated `coordination.md` to make the smallcc copy explicitly canonical,
matching the existing rules for `cpu4.md` and `abi.md`. Concretely:

- New tests go into `smallcc/tests/cases/`
- The hardware repo should consume them via symlink (or `git submodule
  add ../smallcc smallcc-ref` and reference `smallcc-ref/tests/cases/`)
- The 3 smallcc-only files would land in your tree automatically once
  the symlink/submodule replaces the local copy

I haven't touched the hardware tree (per the read-only rule). When you're
ready to switch over: removing `cpu4_hardware/test/cases/` and replacing it
with a symlink to `../smallcc/tests/cases/` is the smallest change. The
3 missing files come along for free.
