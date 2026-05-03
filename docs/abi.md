# CPU4 ABI

Calling convention, stack layout, type sizes, and object-file conventions for
code targeting CPU4. This is the contract between the compiler (smallcc), the
hardware (cpu4_hardware), and the reference simulator (sim_c). The ISA itself
is described in [`isa/cpu4.md`](isa/cpu4.md); this document describes how
software *uses* the ISA.

---

## 1. Type model (ILP32)

| Type | Size | Align |
|---|---|---|
| `char` / `unsigned char` | 1 | 1 |
| `short` / `unsigned short` | 2 | 2 |
| `int` / `unsigned int` | 4 | 4 |
| `long` / `unsigned long` | 4 | 4 |
| `float` | 4 | 4 |
| `double` | 4 | 4 |
| any pointer | 4 | 4 |
| function pointer | 4 | 4 |

`sizeof(int) == sizeof(void *) == 4`. This is the same model used by every
mainstream 32-bit C target. Stack and code addresses live in the low 64 KB
(sp/bp/pc are 16-bit registers), but pointers are stored as 4-byte values
with the upper half zero for any address the compiler emits. User code may
construct pointers with non-zero upper halves to address SDRAM beyond the
64 KB BRAM region.

**Signedness defaults:** `char` is signed; `int`, `short`, `long` are signed.
`unsigned` qualifier flips signedness; conversions follow C89 §3.2.1.

**Endianness:** little-endian. `mem32[addr]` stores byte 0 of the value at
`addr`, byte 1 at `addr+1`, etc.

**Integer promotions (C89 §3.2.1.1):** char/short/uchar/ushort all promote to
plain `int` in expressions (since `int` is wide enough to represent every
value of `unsigned short`). On a hypothetical narrower-int target, `unsigned
short` would promote to `unsigned int`; that is not the case here.

---

## 2. Registers

| Register | Width | Role | Save responsibility |
|---|---|---|---|
| `r0` | 32 | Scalar return value; first scratch | caller-save |
| `r1` | 32 | 1st argument; scratch | caller-save |
| `r2` | 32 | 2nd argument; scratch | caller-save |
| `r3` | 32 | 3rd argument; scratch | caller-save |
| `r4` | 32 | General-purpose | callee-save |
| `r5` | 32 | General-purpose | callee-save |
| `r6` | 32 | General-purpose | callee-save |
| `r7` | 32 | General-purpose | callee-save |
| `bp` | 16 | Frame pointer | callee-save (saved by `enter`) |
| `sp` | 16 | Stack pointer (grows down) | callee-save |
| `lr` | 16 | Link register (return address) | caller-save (saved by `enter` if needed) |
| `pc` | 16 | Program counter | — |
| `H` | 1 | Halt flag | — |

**Caller-save (r0–r3, lr):** the caller must spill these around any call site
if it needs the values to survive the call.

**Callee-save (r4–r7, sp, bp):** the callee must preserve these. If a function
uses any of r4–r7, it spills them to its frame in the prologue and restores in
the epilogue. The compiler does this automatically via `insert_callee_saves`
in `alloc.c`. `enter` saves `bp` (and packs `lr` alongside) atomically.

**Scalar return value:** in `r0`. Promoted to 32 bits per the type rules
above (a returned `char` is sign- or zero-extended into r0).

**Float return value:** in `r0`, holding the IEEE-754 bit pattern.

**Struct return value:** see §4.4.

---

## 3. Stack and frame layout

### Stack discipline

- Grows downward.
- `sp` is always 4-byte aligned at function-call boundaries.
- `bp` is 4-byte aligned for the entire active body of every function.
- Initial `sp` at program entry is `0xF000` (set by crt0 via `immw r0, 0xF000;
  ssp r0`). The 4 KB above is the framebuffer/MMIO region; never store there.

### `enter N` semantics

A standard prologue is `enter N` (3 bytes, F3a opcode 0xc2):

```
mem32[sp-4] := (lr << 16) | bp     ; pack saved lr+bp into one 32-bit word
bp          := sp - 4              ; new bp points at the saved word
sp          := sp - 4 - N          ; reserve N bytes for locals/spills
```

`N` is unsigned, encoded in 14 bits, so up to 16383 bytes per frame. If a
function needs more (rare), the compiler emits an additional `adjw` after the
`enter`.

### `ret` semantics

```
sp        := bp + 4                ; release locals + saved-word
tmp       := mem32[bp]
bp        := tmp & 0xFFFF
pc        := tmp >> 16             ; restored lr is the return address
```

This packed save/restore costs one 4-byte slot of frame overhead, not the
two-slot scheme used by older CPU3 toolchains.

### Frame layout (caller and callee combined)

```
              high addresses
              ┌─────────────────────────┐
              │  caller's frame          │
   sp_caller →├─────────────────────────┤
              │  outgoing arg N          │  bp+4+4*(N-1)   (last stack arg)
              │  …                       │
              │  outgoing arg 0          │  bp+4           (first stack arg)
   bp_callee →├─────────────────────────┤
              │  saved (lr<<16)|bp       │  bp+0           (4 bytes)
              ├─────────────────────────┤
              │  callee's locals/spills  │  bp-1 .. bp-frame
              ├─────────────────────────┤
              │  callee-saved r4..r7     │  bp-(frame+4) .. bp-(frame+16)
   sp_callee →├─────────────────────────┤
              │  ...                     │
              ▼  low addresses
```

- **Stack-passed args** start at `bp+4` (immediately after the saved word) and
  are 4-byte slots regardless of declared argument width. The caller
  zero/sign-extends narrower arguments to 4 bytes before pushing.
- **Locals** sit at negative offsets from bp. The compiler aligns each local
  to its natural alignment by rounding the running offset up.
- **Callee-saved registers** are stored *below* the local area in the order
  r4, r5, r6, r7 (only the ones actually used are saved).
- The compiler accumulates `frame_size` (locals + spills, rounded up to a
  multiple of 4) and `callee_frame` (4 × number of saved r4–r7 registers),
  then emits `enter (frame_size + callee_frame)`.

### Alignment requirement (hardware-enforced)

All multi-byte memory access must be naturally aligned:
- 16-bit load/store at addresses divisible by 2.
- 32-bit load/store at addresses divisible by 4.

Misaligned access raises an alignment exception. Both `sim_c` and the RTL
trap on this. The compiler is responsible for placing every local and spill
slot at a properly-aligned bp-relative offset.

---

## 4. Calling convention

### 4.1 Argument passing

Arguments are evaluated left-to-right, then assigned to slots:

| Slot | Location |
|---|---|
| 1st register-eligible arg | `r1` |
| 2nd register-eligible arg | `r2` |
| 3rd register-eligible arg | `r3` |
| 4th and later args | stack at `bp+4`, `bp+8`, … in declaration order |

A "register-eligible" argument is any scalar (int, long, float, double, any
pointer) of size ≤ 4 bytes. Structs are not register-eligible (see §4.3).

**Calls with stack args** push args right-to-left so the leftmost stack arg
ends up at the lowest address (bp+4 from the callee's view). After the call,
the caller adjusts `sp` back up (`adjw imm14`) to drop the stack args.

**Narrowing/widening:** If a 1- or 2-byte argument is passed (e.g. a `short`
to a `short` parameter), the compiler still allocates a full 4-byte slot. For
register-passed args the high bits are sign- or zero-extended per the source
type's signedness. For stack-passed args the same extension is applied before
the push.

### 4.2 Variadic functions

Variadic functions (`int printf(const char *fmt, ...)`) follow a hybrid
convention:

- **Named parameters** before `...` are passed in registers (r1, r2, r3) up
  to the usual 3-arg limit, with later named params on the stack.
- **Variadic arguments** (the `...` portion) are *always* passed on the stack
  in 4-byte slots, immediately after the named-parameter stack region. The
  caller does not put any variadic args in registers, even if r2 or r3 would
  be free.
- **Default argument promotions** apply to variadic args before the push:
  `char`/`short` → `int`, `float` → `double` (where `double` is 4 bytes on
  this target, same as float).

`<stdarg.h>` macros:
- `va_list` is a pointer-sized opaque type (4 bytes on this target).
- `va_start(ap, last_named)` initialises `ap` to point at `&last_named +
  sizeof(last_named)` — the first variadic slot.
- `va_arg(ap, T)` reads `*ap`, advances `ap` by `max(4, sizeof(T))`, and
  returns the value with appropriate sign/zero extension.
- `va_end(ap)` is a no-op.
- `va_arg(ap, struct S)` is **not supported** (struct ABI uses hidden
  pointers, incompatible with raw `sizeof(T)` slot reads). Pass a `struct S
  *` and dereference instead.

### 4.3 Struct argument passing

Structs of any size are passed by reference, with the caller making a copy.
The compiler:

1. Allocates a stack temporary the size of the struct.
2. Copies the struct argument into the temporary.
3. Passes the address of the temporary as a 4-byte pointer through the normal
   register/stack slot.

The callee receives a pointer and dereferences as needed. There is no
struct-by-value tail-call optimisation.

### 4.4 Struct return values

Functions returning a struct use a hidden first parameter:

1. The caller allocates a stack buffer the size of the return type.
2. The caller passes the buffer's address as a hidden 0th argument in `r1`,
   shifting the user's 1st argument to `r2`, etc.
3. The callee writes the return value through the hidden pointer.
4. The callee returns normally; `r0` is unused for the value.

The compiler signals this in the `CallDesc.hidden_sret` flag; both caller and
callee must agree based on the function signature.

In practice, with 3 user arguments + 1 hidden sret pointer, only the first 2
user arguments stay in registers (r2, r3); the 3rd spills to the stack.

### 4.5 Function pointers

A function pointer is stored as a 4-byte value but the actual call target is
the low 16 bits (since `pc` is 16-bit). The high 16 bits are "don't care" at
call time: `jlr rd` masks `rd & 0xFFFF` for the new `pc`. User code that
manipulates function-pointer values (e.g. dispatch tables) should keep the
high bits zero unless tagging.

---

## 5. Symbol naming and visibility

The assembler / linker is single-pass; all symbols share one global
namespace. To support C's notion of static-internal linkage and translation-
unit-private statics, the compiler mangles names:

| C entity | Emitted label |
|---|---|
| Non-static global `foo` | `foo` |
| File-static global `foo` (TU index N) | `_sN_foo` |
| Local static (Nth created) | `_lsN` |
| String literal (Nth) | `_lN` (note: shared counter, monotonic across all TUs) |
| Function block label (Nth in func `foo`) | `_foo_BN` |
| Compiler-generated block in static fn `foo` (TU N) | `__sN_foo_BM` (extra leading `_`) |

Cross-TU references resolve through the unmangled name; the compiler's
`harvest_globals` mechanism makes globals defined in TU N implicitly visible
as `extern` in TU N+1, …

---

## 6. Sections and program image

The assembler emits a flat memory image with two regions:

```
0x0000  ┌────────────────────────────────┐
        │  .text  (code + initialized    │
        │   data: globals, string lits)  │
        │                                 │
   _globals_start                          ← end of code, start of init data
        │  initialized globals            │
   _bss_start                              ← start of zero-init region
        │  zero-init globals              │
   _bss_end                                ← end of zero-init region
        ├────────────────────────────────┤
        │   (unallocated)                 │
        ├────────────────────────────────┤
0xE000* │   stack (grows down from sp_init=0xF000) │
        ├────────────────────────────────┤
0xF000  │   MMIO / framebuffer region    │
0xFFFF  └────────────────────────────────┘
```

\* Stack is unbounded below `0xF000`; the linker doesn't reserve a fixed size.

Symbols `_globals_start`, `_bss_start`, `_bss_end` are emitted by the
compiler and used by the C runtime (`crt0`) to clear the BSS region.

### MMIO

| Address | Size | Register | Access |
|---|---|---|---|
| `0xFF00` | 4 B | Microsecond counter (32-bit, wraps at 2³²−1) | RO |
| `0xFF40` | 4 B | Performance counter base (impl-defined) | RO |
| `0xF000`–`0xFEFF` | 4 KB | ASCII framebuffer (80×30 chars) | RW (hw target only) |
| `0xFFFF` | 1 B | Halt-condition writeback (sim_c only) | WO |

The compiler does not emit MMIO accesses except via the `__putchar` builtin
(which assembles to a single `putchar rd` instruction, opcode F1b 0x3f, not
an MMIO access).

---

## 7. C runtime startup

A minimal `crt0` performs three steps before calling `main`:

```asm
.text=0
    immw    r0, 0xf000
    ssp     r0                  ; sp = 0xF000
    ; Zero the BSS region [_bss_start .. _bss_end).
    immw    r1, _bss_start
    immw    r2, _bss_end
    zero3
_bss_init_loop:
    beq     r1, r2, _bss_init_done
    slb     r3, r1, 0
    inc     r1
    j       _bss_init_loop
_bss_init_done:
    jl      main                ; main() with no argc/argv
    halt
```

`main` is called as `main(void)` by the default crt0 — argc/argv are not
passed. A user can supply a custom crt0 if needed. `main`'s return value is
left in `r0` and ignored after `halt`.

The selectable `-target` flag (sim/hw) chooses between two crt0 variants:
the `hw` crt0 also clears the 80×30 framebuffer at startup; the `sim` crt0
omits that since `sim_c`'s memory is zeroed at load time.

---

## 8. Implementation notes (compiler-side)

These are not part of the ABI contract per se but document where the
compiler enforces each rule:

| ABI rule | Enforced in |
|---|---|
| Param 1–3 → r1/r2/r3 | `legalize.c` Pass A (pre-color `IK_PARAM`) |
| Args 1–3 → pre-colored copies | `legalize.c` Pass B |
| Stack arg layout (bp+4, +8, …) | `braun.c` (`cg_call_args` push side; param-load side) |
| Variadic stack-only convention | `braun.c` `is_variadic` branch in param setup |
| Hidden sret pointer | `braun.c` `make_calldesc` + parser's `shift_param_offsets_for_struct_ret` |
| r4–r7 callee-save | `alloc.c` `insert_callee_saves` (prologue stores, epilogue loads) |
| Struct argument copy-and-pass | `braun.c` argument lowering |
| Local alignment | `types.c` `do_align` in `insert_local_ident`; `braun.c` for spill homes |
| Spill-slot alignment | `alloc.c` `rewrite_spills` (round-up before reservation) |
| `enter`/`ret` packing | `emit.c` prologue/epilogue emission |

Hardware implementing the ISA does not need to know any of this — the ABI is
visible to the hardware only through the ISA-level instructions the compiler
emits.

---

## 9. ABI version

There is no formal version number yet. Significant changes to date:

- **(historical)** LP32 model: `int` and pointer were 2 bytes. Stack args at
  bp+8 (CPU3-era 8-byte enter overhead). Migrated to ILP32 in 2026.
- **Current:** ILP32, packed-`enter` 4-byte overhead, stack args at bp+4.
