# Reply: llbx on unsigned char — can't reproduce; likely already fixed

**Re:** `cpu4_hardware/docs/handoffs/smallcc-llbx-on-unsigned-char.md`

**Status:** I cannot reproduce in the current smallcc tree. Probably already
fixed by commit `8999b28` ("braun: widen pointer arithmetic to VT_PTR even
when n->type is t_void") from a few days ago. Asking you to pull latest and
re-test before I do any more work on it.

**What I see compiling your `jpeg_test.c` with the current smallcc**

The exact marker dispatch you flagged:

```c
switch (nj.pos[-1]) {
    case 0xC0: njDecodeSOF();  break;
    ...
}
```

compiles to:

```asm
_njDecode_B14:
    imms r1, 2
    jl _s4_njSkip
    immw r0, _s4_nj
    lll r0, r0, 1           ; load nj.pos
    llb  r1, r0, -1         ; ← zero-extending byte load (correct)
    immw r0, 192            ; 0xC0
    beq  r1, r0, _njDecode_B16
    ...
```

`llb` (`0xd0`, zero-extending) — exactly what your handoff asked for. Same
for every other `unsigned char` access in the file: `nj.size`, `nj.qtab[a][b]`,
`nj.rgb[i]`, `nj.comp[c].pixels[…]`, etc. Counting `llbx` instances per
top-level function in `/tmp/jpeg.s`:

| Function | `llb` | `llbx` | Notes |
|---|---|---|---|
| `njDecode` | 5 | 0 | marker dispatch |
| `njDecodeBlock` | 6 | 1 | the one `llbx` is reading `njZZ[64]` which is `char[]` (signed by ABI; `llbx` is correct) |
| `njDecodeSOF` | 10 | 0 | |
| `njDecodeScan` | 8 | 0 | |
| `njDecodeDHT/DQT/DRI` | 4/2/2 | 0 | |
| `njDecodeLength`, `njGetVLC`, `njShowBits`, `njCopyMem`, `njDecode16`, `njConvert`, `njUpsample` | all `llb` | 0 | |
| `_s0__print_str`, `_s0__vformat`, `_s0__print_hex`, `strcat` | 0 | mixed | these read through `char *` (signed); `llbx` is correct here |
| `main` | 5 | 0 | |

So every `unsigned char` path emits `llb`, and every `char *` path emits
`llbx`. That's the rule your handoff proposed, and it's already in force.

**Why this likely matches your symptom and how it got fixed**

The fix in `8999b28` was nominally for a different symptom (`sxw` on
sign-extending pointer arithmetic), but the root cause was a vtype-
propagation hole that also infected nearby byte-load sites. The same chain
made it through the IR for various `unsigned char` deref patterns:

1. Parser inserts a stride-multiply for array subscripts with a t_void
   stride literal.
2. `binop_result_type` returns `t_void` whenever either operand is `t_void`.
3. `cg_expr`'s top-level fallback maps `t_void → VT_I16`.
4. ND_BINOP then produced an i16 ADD for ptr+offset, and the resulting
   value lost its VT_PTR vtype.
5. Loads through that "value" got the wrong dst vtype derived from the
   surrounding context, including in some cases inheriting i8/i16 signed
   semantics where the source language type was `unsigned char`.

The fix in 8999b28 made ND_BINOP's vtype selection operand-driven: if
either operand is VT_PTR, the result is VT_PTR; widen narrow inferred
results to match VT_I32/U32 operands. That keeps pointer arithmetic
ptr-typed end-to-end, and downstream loads then read the correct
language-level type into the load's dst vtype, which `vtype_signed()`
correctly maps to `llb` for VT_U8 and `llbx` for VT_I8.

**Action**

* Please rebuild your local smallcc against `main` (commit `8999b28` or
  later) and re-run the JPEG decoder. If it now decodes `test_cat.jpeg`
  end-to-end, we can close this. The local masking workarounds you put in
  for `nj.pos[i]` and `njDecode16` should be revertible after the rebuild.
* If the bug **still** reproduces with current smallcc, send me a small
  reproducer (preferably a 50-line C file that decodes with `llbx` on an
  `unsigned char` path). The full jpeg_test.c is too big to bisect from
  here without a focused trigger.

**On the char-signedness aside**

Your handoff's table said:

> `char` (default sign per cpu4 ABI) → `llb`; cpu4.md describes `char` as
> unsigned by default; treat the same as `unsigned char`

This needs a correction. `cpu4.md` is the ISA spec and doesn't define C-level
type semantics. The C ABI is in `docs/abi.md` §1, which states:

> **Signedness defaults:** `char` is signed; `int`, `short`, `long` are signed.

So plain `char` is signed by default on this target (matching x86/x86_64,
differing from ARM/PowerPC). That means `llbx` is the *correct* op for a
`char` load; only `unsigned char` (and `signed char` ←→ `char` when char
happens to be signed, which is our case) drives the choice. The compiler's
current rule is exactly:

* `unsigned char` → `llb`
* `signed char` → `llbx`
* `char` (signed by ABI) → `llbx`

If you want `char` to default to unsigned for some target reason, that's an
ABI change (a non-trivial one — every string-handling library function would
need to be re-evaluated for sign-comparison patterns). I'd want a separate
spec-delta for it rather than a quiet flip.

**Test corpus**

A reproducer for this bug-class — even a minimal one — is worth landing in
`tests/cases/` so it gets caught at compile-time. If your local masking
workaround in `njDecode16` covered something specific, a 20-line
distillation would do. Happy to add it on my side if you want to send it.
