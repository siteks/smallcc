# Target ISA: CPU4

A custom 32-bit RISC-like machine with a dense variable-width encoding. Simulated by
`sim_c` (fast C simulator supporting both CPU3 and CPU4 ISAs) and `cpu4/sim.py` (Python
reference implementation). The canonical reference is `cpu4/cpu.py`.

## Registers

| Register | Width | Role |
|---|---|---|
| `r0`–`r7` | 32-bit | General-purpose; r0 is the return value and implicit operand for some F0/F3a ops |
| `r0` | 32-bit | Accumulator and function return value |
| `r1`–`r3` | 32-bit | Scratch (caller-saved); available for register allocation |
| `r4`–`r7` | 32-bit | Callee-saved; available for register allocation; saved/restored by callees |
| `bp` | 16-bit | Frame pointer |
| `sp` | 16-bit | Stack pointer (grows downward) |
| `lr` | 16-bit | Link register (return address) |
| `pc` | 16-bit | Program counter |
| `H` | 1-bit | Halt flag |

**Calling convention:** r0–r3 are caller-saved (a call may clobber them). r4–r7 are
callee-saved — any function that uses them must save them on entry and restore them before
returning. r0 holds the return value. The compiler enforces this via `insert_callee_saves`
in `irc.c`, which inserts prologue stores and epilogue loads for r4–r7 when they are
assigned by the register allocator.

**Alignment Requirements (CPU4 only):** `sp` and `bp` must be 32-bit aligned at all times.
Any misaligned access (16-bit or 32-bit load/store to an odd address, or 32-bit access to an
address not divisible by 4) causes an alignment exception and the simulator will terminate
with an error. The `sim_c` simulator enforces this strictly in CPU4 mode.

Memory is 65536 bytes. Data is little-endian. The stack starts at `sp = 0x1000` (set by `ssp`).

## Instruction Encoding

Seven formats with variable widths (1, 2, or 3 bytes). The format is determined by the top bits
of the first byte:

| Format | Width | First-byte pattern | Used for |
|---|---|---|---|
| F0 | 1 byte | `00oooooo` | No-operand (64 slots) |
| F1a | 2 bytes | `01ooooo ddd xxx yyy` | Three-register ALU (31 usable slots; `11111` is F1b escape) |
| F1b | 2 bytes | `0111111 ddd oooooo` | One-register ops (64 slots; triggered when F1a opcode = `11111`) |
| F2 | 2 bytes | `100 ooo xxx iiiiiii` | One reg + imm7; bp-relative load/store/addi (16 slots) |
| F3a | 3 bytes | `1100 oooo iiiiiiiiiiiiiiii` | Zero reg + imm16 (16 slots) |
| F3b | 3 bytes | `1101 oooo xxx yyy iiiiiiiiii` | Two reg + imm10 (16 slots) |
| F3c | 3 bytes | `111 oo xxx iiiiiiiiiiiiiiii` | One reg + imm16 (4 slots) |

F1b is an escape from F1a: when the F1a opcode field is all-ones (`11111`), the remaining bits
select a 1-register operation from a 64-slot space.

---

## Format F0 — No Operand (1 byte)

Operates on `r0` or special registers implicitly.

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0x00 | `halt` | H = 1 |
| 0x01 | `ret` | sp = bp; bp = mem32[sp]; pc = mem32[sp+4]; sp += 8 |
| 0x02 | `itof` | r0 = float_bits(float(signed32(r0))) |
| 0x03 | `ftoi` | r0 = int(float(r0)) truncated toward zero |
| 0x04 | `jlr` | lr = pc; pc = r0 & 0xffff (indirect call via r0) |
| 0x05 | `push` | sp -= 4; mem32[sp] = r0 |
| 0x06 | `pop` | r0 = mem32[sp]; sp += 4 |
| 0x1e | `putchar` | write chr(r0 & 0xff) to stderr; r0 unchanged |

*(~56 slots reserved for future hotspot encodings.)*

---

## Format F1a — Three-Register ALU (2 bytes)

`01ooooo ddd xxx yyy` — 5-bit opcode, 3-bit dst (`rd`), 3-bit src1 (`rx`), 3-bit src2 (`ry`).
Result written to `rd`; `rx` and `ry` are read-only. Opcode `11111` (0x7e) is the F1b escape.

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0x40 | `add rd, rx, ry` | rd = rx + ry |
| 0x42 | `sub rd, rx, ry` | rd = rx − ry |
| 0x44 | `mul rd, rx, ry` | rd = rx × ry |
| 0x46 | `div rd, rx, ry` | rd = rx / ry (unsigned) |
| 0x48 | `mod rd, rx, ry` | rd = rx % ry (unsigned) |
| 0x4a | `shl rd, rx, ry` | rd = rx << ry |
| 0x4c | `shr rd, rx, ry` | rd = rx >> ry (logical) |
| 0x4e | `lt rd, rx, ry` | rd = (rx < ry) ? 1 : 0 (unsigned) |
| 0x50 | `le rd, rx, ry` | rd = (rx <= ry) ? 1 : 0 (unsigned) |
| 0x52 | `eq rd, rx, ry` | rd = (rx == ry) ? 1 : 0 |
| 0x54 | `ne rd, rx, ry` | rd = (rx != ry) ? 1 : 0 |
| 0x56 | `and rd, rx, ry` | rd = rx & ry |
| 0x58 | `or rd, rx, ry` | rd = rx \| ry |
| 0x5a | `xor rd, rx, ry` | rd = rx ^ ry |
| 0x5c | `lts rd, rx, ry` | rd = (signed(rx) < signed(ry)) ? 1 : 0 |
| 0x5e | `les rd, rx, ry` | rd = (signed(rx) <= signed(ry)) ? 1 : 0 |
| 0x60 | `divs rd, rx, ry` | rd = signed(rx) / signed(ry) (0 if ry == 0) |
| 0x62 | `mods rd, rx, ry` | rd = signed(rx) % signed(ry) (0 if ry == 0) |
| 0x64 | `shrs rd, rx, ry` | rd = signed(rx) >> (ry & 31) (arithmetic) |
| 0x66 | `fadd rd, rx, ry` | rd = float_bits(float(rx) + float(ry)) |
| 0x68 | `fsub rd, rx, ry` | rd = float_bits(float(rx) − float(ry)) |
| 0x6a | `fmul rd, rx, ry` | rd = float_bits(float(rx) × float(ry)) |
| 0x6c | `fdiv rd, rx, ry` | rd = float_bits(float(rx) / float(ry)) |
| 0x6e | `flt rd, rx, ry` | rd = (float(rx) < float(ry)) ? 1 : 0 |
| 0x70 | `fle rd, rx, ry` | rd = (float(rx) <= float(ry)) ? 1 : 0 |

*(6 slots available: 0x72, 0x74, 0x76, 0x78, 0x7a, 0x7c.)*

Float operands are 32-bit IEEE 754 single-precision values stored as raw bit patterns.
`float(x)` means interpret the bit pattern `x` as IEEE 754; `float_bits(f)` means the
IEEE 754 bit pattern of `f`. **Float `eq`/`ne`** use the integer `eq`/`ne` opcodes (bitwise
equality is correct for IEEE 754 except NaN and signed-zero edge cases, which are acceptable
deviations on this target).

**Pseudo-ops (assembler only, no extra encoding):**

| Pseudo-op | Expands to |
|---|---|
| `gt rd, rx, ry` | `lt rd, ry, rx` |
| `ge rd, rx, ry` | `le rd, ry, rx` |
| `gts rd, rx, ry` | `lts rd, ry, rx` |
| `ges rd, rx, ry` | `les rd, ry, rx` |
| `fgt rd, rx, ry` | `flt rd, ry, rx` |
| `fge rd, rx, ry` | `fle rd, ry, rx` |
| `mov rd, rx` | `or rd, rx, rx` |

The ISA has `lt` and `le` (plus signed/float variants) as native instructions. All four
comparison directions are single instructions via operand swapping: `gt rd, rx, ry` becomes
`lt rd, ry, rx`, and `ge rd, rx, ry` becomes `le rd, ry, rx`.
`mov rd, rx` is the pseudo-op `or rd, rx, rx`; no dedicated encoding is needed.

---

## Format F1b — Single-Register (2 bytes)

`0111111 ddd oooooo` — triggered when F1a opcode field = `11111` (byte 0x7e). The `yyy`
bits from the F1a encoding become the low 3 bits of a 6-bit opcode, giving 64 single-register
slots.

| Subopcode | Mnemonic | Semantics |
|---|---|---|
| 0x00 | `sxb rd` | rd = sign_extend_8(rd) (in-place) |
| 0x01 | `sxw rd` | rd = sign_extend_16(rd) (in-place) |
| 0x02 | `inc rd` | rd = rd + 1 |
| 0x03 | `dec rd` | rd = rd − 1 |
| 0x04 | `pushr rd` | sp -= 4; mem32[sp] = rd |
| 0x05 | `popr rd` | rd = mem32[sp]; sp += 4 |

*(58 slots reserved for future hotspot encodings.)*

`inc`/`dec` cover the K=±1 in-place case in 2 bytes, compared to 5 bytes for the general
`immw r_tmp, 1; add rd, rd, r_tmp` sequence. They are particularly effective for loop counters
and pointer bumps by 1. The in-place `sxb`/`sxw` cover the overwhelming majority of
sign-extension uses; there is no separate F1a form for cross-register sign-extension.

---

## Format F2 — BP-Relative Load/Store/Addi (2 bytes)

`100 ooo xxx iiiiiii` — 3-bit opcode, 3-bit register (`rx`), 7-bit signed immediate.
All memory accesses are relative to `bp`. The immediate is scaled by access width.

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0x80 | `lb rx, [bp+imm7]` | rx = mem8[bp + sext7(imm7)] (zero-extended) |
| 0x84 | `lw rx, [bp+imm7*2]` | rx = mem16[bp + sext7(imm7) × 2] (zero-extended) |
| 0x88 | `ll rx, [bp+imm7*4]` | rx = mem32[bp + sext7(imm7) × 4] |
| 0x8c | `sb rx, [bp+imm7]` | mem8[bp + sext7(imm7)] = rx |
| 0x90 | `sw rx, [bp+imm7*2]` | mem16[bp + sext7(imm7) × 2] = rx |
| 0x94 | `sl rx, [bp+imm7*4]` | mem32[bp + sext7(imm7) × 4] = rx |
| 0x98 | `lbx rx, [bp+imm7]` | rx = sign_extend_8(mem8[bp + sext7(imm7)]) |
| 0x9c | `lwx rx, [bp+imm7*2]` | rx = sign_extend_16(mem16[bp + sext7(imm7) × 2]) |
| 0xa0 | `addi rx, imm7` | rx = rx + sext7(imm7) (in-place add of signed constant) |

*(7 slots available.)*

`lbx`/`lwx` (sign-extending loads) replace the `lb`/`lbx; sxb` pair that would otherwise be
needed for every signed `char` or `short` local read.

The scaled immediate gives `lw` a ±127-word (±254-byte) reach and `ll` a ±127-long
(±508-byte) reach from `bp`. `addi` is in-place: source and destination are the same register.
It is particularly useful for loop counters and pointer bumps when the constant fits in 7 bits
(−64 to +63).

**Alignment requirement:** `lw`/`sw` require the effective address (`bp + imm7 × 2`) to be
2-byte aligned; `ll`/`sl` require 4-byte alignment. This means:
- `bp` must be 4-byte aligned (achieved by rounding the `enter N` frame size up to a multiple
  of 4 in the backend).
- Individual `long`/`float` locals must be placed at 4-byte-aligned offsets within the frame
  (requires `finalize_local_offsets()` to insert alignment padding before each 4-byte local).

Until both changes are in place, `ll`/`sl` for misaligned locals fall back to the 2-instruction
`lea + lll`/`sll` sequence.

---

## Format F3a — Zero-Register + imm16 (3 bytes)

`1100 oooo iiiiiiiiiiiiiiii` — 4-bit opcode, 16-bit immediate. Operates on `r0` or `pc`
implicitly.

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0xc0 | `j imm16` | pc = imm16 |
| 0xc1 | `jl imm16` | lr = pc; pc = imm16 (direct call) |
| 0xc2 | `jz imm16` | if r0 == 0: pc = imm16 |
| 0xc3 | `jnz imm16` | if r0 != 0: pc = imm16 |
| 0xc4 | `enter imm16` | mem32[sp-4] = lr; mem32[sp-8] = bp; bp = sp-8; sp -= imm16+8 |
| 0xc5 | `ssp imm16` | sp = imm16 (startup stack pointer) |
| 0xc6 | `adjw imm16` | sp += sext16(imm16) |

*(9 slots available.)*

`enter` / `ret` / `adjw` use the same frame layout as CPU3. Direct calls use `jl`; indirect
calls through a register use `jlr` (F0).

---

## Format F3b — Two-Register + imm10 (3 bytes)

`1101 oooo xxx yyy iiiiiiiiii` — 4-bit opcode, 3-bit rx, 3-bit ry, 10-bit signed immediate.
Memory accesses are register-relative (ry is the base; rx is source/destination).

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0xd0 | `llb rx, [ry+imm10]` | rx = mem8[ry + sext10(imm10)] (zero-extended) |
| 0xd1 | `llw rx, [ry+imm10*2]` | rx = mem16[ry + sext10(imm10) × 2] (zero-extended) |
| 0xd2 | `lll rx, [ry+imm10*4]` | rx = mem32[ry + sext10(imm10) × 4] |
| 0xd3 | `slb rx, [ry+imm10]` | mem8[ry + sext10(imm10)] = rx |
| 0xd4 | `slw rx, [ry+imm10*2]` | mem16[ry + sext10(imm10) × 2] = rx |
| 0xd5 | `sll rx, [ry+imm10*4]` | mem32[ry + sext10(imm10) × 4] = rx |
| 0xd6 | `llbx rx, [ry+imm10]` | rx = sign_extend_8(mem8[ry + sext10(imm10)]) |
| 0xd7 | `llwx rx, [ry+imm10*2]` | rx = sign_extend_16(mem16[ry + sext10(imm10) × 2]) |
| 0xd8 | `beq rx, ry, imm10` | if rx == ry: pc += sext10(imm10) |
| 0xd9 | `bne rx, ry, imm10` | if rx != ry: pc += sext10(imm10) |
| 0xda | `blt rx, ry, imm10` | if rx < ry (unsigned): pc += sext10(imm10) |
| 0xdb | `ble rx, ry, imm10` | if rx <= ry (unsigned): pc += sext10(imm10) |
| 0xdc | `blts rx, ry, imm10` | if signed(rx) < signed(ry): pc += sext10(imm10) |
| 0xdd | `bles rx, ry, imm10` | if signed(rx) <= signed(ry): pc += sext10(imm10) |
| 0xde | `addli rx, ry, imm10` | rx = ry + sext10(imm10) (separate source and destination) |

*(1 slot available: 0xdf.)*

Branch offsets are **PC-relative** — the offset is added to `pc` (which has already been
advanced past the current instruction). `llbx`/`llwx` make F3b symmetric with F2: both
formats provide sign-extending load variants, so a signed `char` or `short` read through any
pointer costs one instruction regardless of whether the base is `bp` or a general register.

`addli` complements F2 `addi`: `addi` is in-place with a 7-bit immediate; `addli` allows a
separate destination register and a wider 10-bit immediate.

**Pseudo-ops:**

| Pseudo-op | Expands to |
|---|---|
| `bgt rx, ry, imm10` | `blt ry, rx, imm10` |
| `bge rx, ry, imm10` | `ble ry, rx, imm10` |
| `bgts rx, ry, imm10` | `blts ry, rx, imm10` |
| `bges rx, ry, imm10` | `bles ry, rx, imm10` |

---

## Format F3c — One-Register + imm16 (3 bytes)

`111 oo xxx iiiiiiiiiiiiiiii` — 2-bit opcode, 3-bit register, 16-bit immediate.

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0xe8 | `immw rd, imm16` | rd = zero_extend(imm16) |
| 0xf0 | `immwh rd, imm16` | rd = (rd & 0xffff) \| (imm16 << 16) (load upper half) |
| 0xf8 | `lea rd, imm16` | rd = bp + sext16(imm16) (bp-relative address; no load) |

*(1 slot available.)*

**Loading a 32-bit constant:** use `immw rd, lo16` followed by `immwh rd, hi16`. The lower 16
bits are written first (zeroing the upper half), then `immwh` fills the upper half.

`lea` computes a bp-relative address without performing a load — needed for `&local_var`,
passing locals by pointer, and `va_start`. The 16-bit signed offset covers any realistic
stack frame.

---

## Stack Frame Layout

Identical to CPU3:

```
bp+8+2*(n-1)   param n   (last parameter)
    ...
bp+8           param 0   (first parameter)
bp+4           return address (lr)
bp+0           saved bp        ← enter sets bp here
bp-2           local 0
bp-4           local 1
    ...
```

`enter N` executes: `mem32[sp-4] = lr; mem32[sp-8] = bp; bp = sp-8; sp -= N + 8`.
`ret` executes: `sp = bp; bp = mem32[sp]; pc = mem32[sp+4]; sp += 8`.

Locals within a compound statement are allocated with `adjw -size` on scope entry and
reclaimed with `adjw +size` on exit.

---

## Memory Map

Same as CPU3. The top 256 bytes are MMIO.

| Address | Size | Register | Access |
|---|---|---|---|
| `0xFF00` | 4 B | Cycle counter (32-bit, wraps at 2³²−1) | RO |
| `0xFF04–0xFFFF` | — | Reserved | — |

The cycle counter increments once per instruction executed.

---

## Assembly Syntax

Same assembler directives as CPU3:

```
.text=0               ; set text section origin
.data=N               ; set data section origin

label:                ; define label at current address
    mnemonic          ; zero-operand instruction
    mnemonic rx, N    ; instruction with register and immediate
    mnemonic rd,rx,ry ; three-register instruction

; Directives:
    byte  v1 v2 ...   ; emit one byte per value
    word  v1 v2 ...   ; emit one 16-bit word per value (labels allowed)
    long  v1 v2 ...   ; emit one 32-bit long per value
    allocb N          ; reserve N zero bytes
    allocw N          ; reserve N zero words
    align             ; align to next word boundary
```

Comments start with `;`.

## Typical Assembly Output

```asm
.text=0
    ssp     0x1000      ; set stack pointer
    jl      main        ; call main (F3a)
    halt

main:
    enter   4           ; save lr, bp; allocate 4 bytes for locals
    sw      r0, [bp-1]  ; a = param r0 (F2, imm7=-1, scaled×2 = bp-2)
    lw      r0, [bp-1]  ; load a (F2)
    immw    r1, 0x000a  ; r1 = 10 (F3c)
    add     r0, r0, r1  ; a + 10 (F1a)
    ret
```

**Key idiom — local variable access:** The CPU3 `lea -4; lw` pair (4 bytes, 2 instructions)
collapses to `lw r0, [bp-2]` (2 bytes, 1 instruction) on CPU4, the single most impactful
encoding improvement over the stack machine.
