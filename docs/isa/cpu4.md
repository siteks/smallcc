# Target ISA: CPU4

A custom 32-bit RISC-like machine with a dense variable-width encoding. Simulated by
`sim_c` (fast C simulator) and `cpu4/sim.py` (Python reference implementation). The
canonical reference is `cpu4/cpu.py`.

## Registers

| Register | Width | Role |
|---|---|---|
| `r0`–`r7` | 32-bit | General-purpose; r0 is the return value |
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
in `alloc.c`, which inserts prologue stores and epilogue loads for r4–r7 when they are
assigned by the register allocator.

**Alignment Requirements:** `sp` and `bp` must be 32-bit aligned at all times. Any
misaligned access (16-bit or 32-bit load/store to an odd address, or 32-bit access to an
address not divisible by 4) causes an alignment exception and the simulator will terminate
with an error.

Memory is 65536 bytes. Data is little-endian. The stack starts at `sp = 0xF000` (set by
`immw r0, 0xF000; ssp r0`).

## Instruction Encoding

Eleven formats with variable widths (1, 2, or 3 bytes). The format is determined by the top
bits of the first byte:

| Format | Width | First-byte pattern | Used for |
|---|---|---|---|
| F0a | 1 byte | `0000 oooo` | No-operand (16 slots) |
| F0b | 3 bytes | `0001 oooo odddxxxiiiiiiiii` | Two reg + imm9 ALU (32 slots via 16 first-bytes × 2 subops) |
| F0c | 3 bytes | `001 o ddd iiiiiiiiiiiiiiiii` | Compare-and-branch: one reg + imm8 + disp9 (2 slots) |
| F1a | 2 bytes | `01ooooo ddd xxx yyy` | Three-register ALU (31 usable slots; `11111` is F1b escape) |
| F1b | 2 bytes | `0111111 ddd oooooo` | One-register ops (64 slots; triggered when F1a opcode = `11111`) |
| F2 | 2 bytes | `10 oooo xxx iiiiiii` | One reg + imm7; bp-relative load/store/addi (16 slots) |
| F3a | 3 bytes | `110000 oo iiiiiiiiiiiiiiii` | Zero reg + imm16 (4 slots) |
| F3b | 3 bytes | `110001 o ddd iiiiiiiiiiiiii` | One reg + imm14 (2 slots) |
| F3c | 3 bytes | `1101 oooo xxx yyy iiiiiiiiii` | Two reg + imm10 (15 usable slots; `1111` is F3d escape) |
| F3d | 3 bytes | `11011111 xxx ooo iiiiiiiiii` | One reg + imm10 (8 slots; triggered when F3c opcode = `1111`) |
| F3e | 3 bytes | `111 oo xxx iiiiiiiiiiiiiiii` | One reg + imm16 (4 slots) |

F0a occupies the first-byte range 0x00–0x0f (16 one-byte instructions). F0b occupies
0x10–0x1f: each first-byte encodes two instructions via a subopcode bit in the second byte,
giving 32 two-register + 9-bit-immediate instructions. F0c uses 0x20–0x3f for compare-and-branch.
F1b is an escape from F1a: when the F1a opcode field is all-ones (`11111`), the remaining bits
select a 1-register operation from a 64-slot space. F3d escapes from F3c when the F3c opcode
field is `1111`.

---

## Format F0a — No Operand (1 byte)

`0000 oooo` — 4-bit opcode (0x00–0x0f). Operates on special registers implicitly.

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0x00 | `halt` | H = 1 |
| 0x01 | `ret` | sp = bp; tmp = mem32[sp]; bp = tmp & 0xffff; pc = tmp >> 16; sp += 4 |

`ret` unpacks the saved `(lr << 16) | bp` word written by `enter`.

*(14 slots reserved for future hotspot encodings.)*

---

## Format F0b — Two-Register + Immediate9 ALU (3 bytes)

`0001 oooo odddxxxiiiiiiiii` — 4-bit opcode in first byte (0x10–0x1b), 1-bit subopcode,
3-bit destination (`rd`), 3-bit source (`rx`), 9-bit signed immediate.

Each first-byte value encodes two instructions via the subopcode bit (bit 15 of the 24-bit
instruction): subop=0 and subop=1. This gives 32 instructions across 12 first-byte values.

Field extraction from the 24-bit instruction word:
- `subop = (ins >> 15) & 1`
- `rd = (ins >> 12) & 7`
- `rx = (ins >> 9) & 7`
- `imm9 = ins & 0x1ff` (sign-extended to 32 bits for arithmetic operations)

| First byte | Sub=0 | Sub=1 | Semantics (sub=0 / sub=1) |
|---|---|---|---|
| 0x10 | `addli rd, rx, imm9` | `subli rd, rx, imm9` | rd = rx + sext9 / rd = rx − sext9 |
| 0x11 | `mulli rd, rx, imm9` | `divli rd, rx, imm9` | rd = rx × sext9 / rd = rx / sext9 (unsigned, 0 if imm=0) |
| 0x12 | `modli rd, rx, imm9` | `shlli rd, rx, imm9` | rd = rx % sext9 (unsigned, 0 if imm=0) / rd = rx << (imm & 31) |
| 0x13 | `shrli rd, rx, imm9` | `leli rd, rx, imm9` | rd = rx >> (imm & 31) (logical) / rd = (rx <= sext9) unsigned |
| 0x14 | `gtli rd, rx, imm9` | `eqli rd, rx, imm9` | rd = (rx > sext9) unsigned / rd = (rx == sext9) |
| 0x15 | `neli rd, rx, imm9` | `andli rd, rx, imm9` | rd = (rx != sext9) / rd = rx & sext9 |
| 0x16 | `orli rd, rx, imm9` | `xorli rd, rx, imm9` | rd = rx \| sext9 / rd = rx ^ sext9 |
| 0x17 | `lesli rd, rx, imm9` | `gtsli rd, rx, imm9` | rd = (signed(rx) <= sext9) / rd = (signed(rx) > sext9) |
| 0x18 | `divsli rd, rx, imm9` | `modsli rd, rx, imm9` | rd = signed(rx) / sext9 (0 if imm=0) / rd = signed(rx) % sext9 (0 if imm=0) |
| 0x19 | `shrsli rd, rx, imm9` | `bitex rd, rx, imm9` | rd = signed(rx) >> (imm & 31) (arithmetic) / bit extract (see below) |
| 0x1a | `rsubli rd, rx, imm9` | `rdivli rd, rx, imm9` | rd = sext9 − rx / rd = sext9 / rx (0 if rx=0) |
| 0x1b | `rmodli rd, rx, imm9` | `rdivsli rd, rx, imm9` | rd = sext9 % rx (0 if rx=0) / rd = sext9 / signed(rx) (0 if rx=0) |

**`bitex` (bit extract):** extracts a bitfield from `rx` into `rd`. The 9-bit immediate
encodes two fields: bits [4:0] = shift amount (0–31), bits [8:5] = width code (0–15). The
width code `w` produces a mask of `w+1` ones, extracting 1 to 16 bits. Equivalent to
`rd = (rx >> shift) & ((1 << (width+1)) - 1)`.

**`r`-prefix instructions** (`rsubli`, `rdivli`, `rmodli`, `rdivsli`): reverse the operand
order — the immediate is the dividend/minuend and the register is the divisor/subtrahend.
Useful for expressions like `N - x` or `N / x` where N is a small constant.

**Assembly encoding:** `b0 = first_byte`, `b1 = (subop << 7) | (rd << 4) | (rx << 1) | (imm9 >> 8)`,
`b2 = imm9 & 0xff`.

---

## Format F0c — Compare-and-Branch (3 bytes)

`001 o ddd i iiiiiiiiiiiiiiii` — 1-bit opcode, 3-bit register (`rx`), 17-bit immediate.
The 17-bit immediate encodes two fields: bits [16:9] = 8-bit unsigned comparison value
(0–255), bits [8:0] = 9-bit signed PC-relative branch displacement (−256 to +255).

First bytes: 0x20–0x2f (cbeq), 0x30–0x3f (cbne).

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0x20 | `cbeq rx, imm8, disp9` | if rx == imm8: pc += sext9(disp9) |
| 0x30 | `cbne rx, imm8, disp9` | if rx != imm8: pc += sext9(disp9) |

Combines an immediate comparison and conditional branch into a single 3-byte instruction,
replacing the common 5–6 byte `immw rT, const; beq/bne rx, rT, target` sequences. The 8-bit
comparison value covers the most common small constants in switch/case dispatch and loop bound
checks.

The 9-bit signed displacement gives a range of −256 to +255 bytes from the end of the
instruction, covering most short-range branches in switch/case code and loop conditions.
Branches to more distant targets must fall back to the `immw + beq/bne` sequence.

**Assembly syntax:** `cbeq rx, imm8, label` / `cbne rx, imm8, label` — the assembler
computes the PC-relative displacement from the label address.

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

`0111111 ddd oooooo` — triggered when F1a opcode field = `11111` (byte 0x7e). The `ddd`
bits encode the destination/source register. The `oooooo` bits are a 6-bit subopcode,
giving 64 single-register slots.

| Subopcode | Mnemonic | Semantics |
|---|---|---|
| 0x00 | `sxb rd` | rd = sign_extend_8(rd) (in-place) |
| 0x01 | `sxw rd` | rd = sign_extend_16(rd) (in-place) |
| 0x02 | `inc rd` | rd = rd + 1 |
| 0x03 | `dec rd` | rd = rd − 1 |
| 0x04 | `pushr rd` | sp -= 4; mem32[sp] = rd |
| 0x05 | `popr rd` | rd = mem32[sp]; sp += 4 |
| 0x06 | `zxb rd` | rd = rd & 0xff (zero-extend byte) |
| 0x07 | `zxw rd` | rd = rd & 0xffff (zero-extend word) |
| 0x08 | `itof rd` | rd = float_bits(float(signed32(rd))) (in-place) |
| 0x09 | `ftoi rd` | rd = int(float(rd)) truncated toward zero (in-place) |
| 0x0a | `jlr rd` | lr = pc; pc = rd & 0xffff (indirect call via rd) |
| 0x0b | `jr rd` | pc = rd & 0xffff (indirect jump via rd) |
| 0x0c | `ssp rd` | sp = rd & 0xffff (set stack pointer from register) |
| 0x3f | `putchar rd` | write chr(rd & 0xff) to stderr; rd unchanged |

*(50 slots reserved for future hotspot encodings.)*

`inc`/`dec` cover the K=±1 in-place case in 2 bytes, compared to 5 bytes for the general
`immw r_tmp, 1; add rd, rd, r_tmp` sequence. They are particularly effective for loop counters
and pointer bumps by 1.

`sxb`/`sxw` and `zxb`/`zxw` are the sign-extending and zero-extending counterparts.
The in-place form covers the overwhelming majority of extension uses.

`itof`/`ftoi`, `jlr`/`jr`, `ssp`, and `putchar` all operate on an arbitrary register.

---

## Format F2 — BP-Relative Load/Store/Addi (2 bytes)

`10 oooo xxx iiiiiii` — 4-bit opcode, 3-bit register (`rx`), 7-bit signed immediate.
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
| 0xa4 | `shli rx, imm7` | rx = rx << (imm7 & 0x1f) (in-place shift left) |
| 0xa8 | `andi rx, imm7` | rx = rx & imm7 (in-place bitwise AND; no sign-extension) |
| 0xac | `shrsi rx, imm7` | rx = signed(rx) >> (imm7 & 0x1f) (in-place arithmetic right shift) |

*(4 slots available.)*

`lbx`/`lwx` (sign-extending loads) replace the `lb`/`lbx; sxb` pair that would otherwise be
needed for every signed `char` or `short` local read.

The scaled immediate gives `lw` a ±127-word (±254-byte) reach and `ll` a ±127-long
(±508-byte) reach from `bp`. `addi` is in-place: source and destination are the same register.
It is particularly useful for loop counters and pointer bumps when the constant fits in 7 bits
(−64 to +63).

`shli` provides an in-place shift-left by an immediate, useful for address computation and
power-of-2 multiplications. `andi` provides an in-place AND with a raw (unsigned) 7-bit mask
(0–127), useful for byte/word truncation. `shrsi` provides an in-place arithmetic right shift
by an immediate, useful for sign-preserving divisions by powers of 2.

**Alignment requirement:** `lw`/`sw` require the effective address (`bp + imm7 × 2`) to be
2-byte aligned; `ll`/`sl` require 4-byte alignment. This means:
- `bp` must be 4-byte aligned (achieved by rounding the `enter N` frame size up to a multiple
  of 4 in the backend).
- Individual `long`/`float` locals must be placed at 4-byte-aligned offsets within the frame
  (requires `finalize_local_offsets()` to insert alignment padding before each 4-byte local).

---

## Format F3a — Zero-Register + imm16 (3 bytes)

`110000 oo iiiiiiiiiiiiiiii` — 2-bit opcode, 16-bit immediate. Operates on `pc`, `sp`, `bp`,
and `lr` implicitly.

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0xc0 | `j imm16` | pc = imm16 |
| 0xc1 | `jl imm16` | lr = pc; pc = imm16 (direct call) |
| 0xc2 | `enter imm14` | mem32[sp-4] = (lr << 16) \| bp; bp = sp-4; sp -= imm14+4 |

*(1 slot available.)*

`enter` packs `lr` and `bp` into a single 32-bit word `(lr << 16) | bp` and saves it at
`sp-4`. The frame pointer is set to point at this saved word, and the stack pointer is
lowered by `imm14 + 4` bytes (4 for the saved word plus `imm14` for the local frame).

**Encoding note:** `enter` uses a 14-bit immediate (not 16-bit) because 3 bits of the
second and third bytes are reserved (same field layout as F3b). The 14-bit range (0–16383)
is more than sufficient for any realistic stack frame.

Direct calls use `jl`; indirect calls through a register use `jlr` (F1b).

---

## Format F3b — One-Register + imm14 (3 bytes)

`110001 o ddd iiiiiiiiiiiiii` — 1-bit opcode, 3-bit register (`rd`), 14-bit immediate.
The raw immediate is shifted left by 2 (multiplied by 4) before use, giving 4-byte-aligned
offsets with ±32768-byte reach.

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0xc4 | `adjw rd, imm14` | sp += sext16(imm14 << 2) |
| 0xc6 | `lea rd, imm14` | rd = bp + sext16(imm14 << 2) (bp-relative address; no load) |

`adjw` adjusts the stack pointer by a 4-byte-aligned amount. The register field is present
in the encoding but unused by `adjw` — only `sp` is modified.

`lea` computes a bp-relative address without performing a load — needed for `&local_var`,
passing locals by pointer, and `va_start`. The 4-byte-aligned offset covers any realistic
stack frame.

---

## Format F3c — Two-Register + imm10 (3 bytes)

`1101 oooo xxx yyy iiiiiiiiii` — 4-bit opcode, 3-bit rx, 3-bit ry, 10-bit signed immediate.
Memory accesses are register-relative (ry is the base; rx is source/destination). Opcode
`1111` (0xdf) is the F3d escape.

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

*(1 slot available: 0xde. 0xdf is the F3d escape.)*

Branch offsets are **PC-relative** — the offset is added to `pc` (which has already been
advanced past the current instruction). `llbx`/`llwx` make F3c symmetric with F2: both
formats provide sign-extending load variants, so a signed `char` or `short` read through any
pointer costs one instruction regardless of whether the base is `bp` or a general register.

**Pseudo-ops:**

| Pseudo-op | Expands to |
|---|---|
| `bgt rx, ry, imm10` | `blt ry, rx, imm10` |
| `bge rx, ry, imm10` | `ble ry, rx, imm10` |
| `bgts rx, ry, imm10` | `blts ry, rx, imm10` |
| `bges rx, ry, imm10` | `bles ry, rx, imm10` |

---

## Format F3d — One-Register + imm10 (3 bytes)

`11011111 xxx ooo iiiiiiiiii` — triggered when F3c opcode field = `1111` (byte 0xdf). The
`xxx` bits encode the register, `ooo` is a 3-bit subopcode (8 slots), and the remaining
10 bits are a signed immediate.

| Subopcode | Mnemonic | Semantics |
|---|---|---|
| 0x00 | `beqz rx, imm10` | if rx == 0: pc += sext10(imm10) |
| 0x01 | `bnez rx, imm10` | if rx != 0: pc += sext10(imm10) |

*(6 slots available.)*

`beqz`/`bnez` are PC-relative branches that test a single register against zero. They are
the short-range (10-bit offset) counterpart to `jz`/`jnz` (F3e, 16-bit absolute address).
Both test any register, not just r0.

---

## Format F3e — One-Register + imm16 (3 bytes)

`111 oo xxx iiiiiiiiiiiiiiii` — 2-bit opcode, 3-bit register, 16-bit immediate.

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0xe0 | `immw rd, imm16` | rd = zero_extend(imm16) |
| 0xe8 | `immwh rd, imm16` | rd = (rd & 0xffff) \| (imm16 << 16) (load upper half) |
| 0xf0 | `jz rd, imm16` | if rd == 0: pc = imm16 |
| 0xf8 | `jnz rd, imm16` | if rd != 0: pc = imm16 |

**Loading a 32-bit constant:** use `immw rd, lo16` followed by `immwh rd, hi16`. The lower 16
bits are written first (zeroing the upper half), then `immwh` fills the upper half.

`jz`/`jnz` test any register (not just r0) and jump to an absolute 16-bit address. For
short-range branches, use `beqz`/`bnez` (F3d) which are PC-relative with a 10-bit offset.

---

## Stack Frame Layout

```
bp+4+2*(n-1)   param n   (last parameter)
    ...
bp+4           param 0   (first parameter)
bp+0           saved (lr<<16 | bp)   ← enter sets bp here
bp-2           local 0
bp-4           local 1
    ...
```

`enter N` executes: `mem32[sp-4] = (lr << 16) | bp; bp = sp-4; sp -= N + 4`.
`ret` executes: `sp = bp; tmp = mem32[sp]; bp = tmp & 0xffff; pc = tmp >> 16; sp += 4`.

The saved link register and frame pointer are packed into a single 32-bit word, using only
4 bytes of stack overhead per frame. `enter` and `ret` are inverses: `enter` packs and
saves; `ret` loads and unpacks.

---

## Memory Map

The top 256 bytes are MMIO.

| Address | Size | Register | Access |
|---|---|---|---|
| `0xFF00` | 4 B | Cycle counter (32-bit, wraps at 2³²−1) | RO |
| `0xFF04–0xFFFF` | — | Reserved | — |

The cycle counter increments once per instruction executed.

---

## Assembly Syntax

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
    immw    r0, 0xf000  ; load stack address (F3e)
    ssp     r0          ; set stack pointer (F1b)
    jl      main        ; call main (F3a)
    halt

main:
    enter   4           ; save (lr<<16|bp); allocate 4 bytes for locals
    sw      r0, -1      ; a = param r0 (F2, imm7=-1, scaled×2 = bp-2)
    lw      r0, -1      ; load a (F2)
    immw    r1, 0x000a  ; r1 = 10 (F3e)
    add     r0, r0, r1  ; a + 10 (F1a)
    ret
```

**Key idiom — local variable access:** The CPU3 `lea -4; lw` pair (4 bytes, 2 instructions)
collapses to `lw r0, -1` (2 bytes, 1 instruction) on CPU4, the single most impactful
encoding improvement over the stack machine.
