# Target ISA: CPU3

A custom 32-bit accumulator/stack hybrid machine simulated by `cpu3/sim.py` (`cpu3/assembler.py` + `cpu3/cpu.py`).

## Registers

| Register | Width | Role |
|---|---|---|
| `r0` | 32-bit | Accumulator and return value |
| `sp` | 16-bit | Stack pointer (grows downward) |
| `bp` | 16-bit | Base/frame pointer |
| `lr` | 16-bit | Link register (return address) |
| `pc` | 16-bit | Program counter |
| `H` | 1-bit | Halt flag |

Registers are masked to their width after each instruction. Memory is 65536 bytes. The stack starts at `sp = 0x1000` (set by `ssp`). Data is little-endian.

## Instruction Encoding

Three formats:
- **Format 0** (1 byte): opcode only
- **Format 1** (2 bytes): opcode + signed 8-bit immediate
- **Format 2** (3 bytes): opcode + unsigned 16-bit immediate (little-endian)

## Instruction Set

### Format 0 — No Operand

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0x00 | `halt` | H = 1 |
| 0x01 | `ret` | sp = bp; bp = mem32[sp]; pc = mem32[sp+4]; sp += 8 |
| 0x02 | `push` | sp -= 4; mem32[sp] = r0 |
| 0x03 | `pushw` | sp -= 2; mem16[sp] = r0 |
| 0x04 | `pop` | r0 = mem32[sp]; sp += 4 |
| 0x05 | `popw` | r0 = mem16[sp]; sp += 2 |
| 0x06 | `lb` | r0 = mem8[r0] |
| 0x07 | `lw` | r0 = mem16[r0] |
| 0x08 | `ll` | r0 = mem32[r0] |
| 0x09 | `sb` | addr = mem32[sp]; mem8[addr] = r0; sp += 4 |
| 0x0a | `sw` | addr = mem32[sp]; mem16[addr] = r0; sp += 4 |
| 0x0b | `sl` | addr = mem32[sp]; mem32[addr] = r0; sp += 4 |
| 0x0c | `add` | r0 = mem32[sp] + r0; sp += 4 |
| 0x0d | `sub` | r0 = mem32[sp] - r0; sp += 4 |
| 0x0e | `mul` | r0 = mem32[sp] * r0; sp += 4 |
| 0x0f | `div` | r0 = mem32[sp] / r0; sp += 4 |
| 0x10 | `mod` | r0 = mem32[sp] % r0; sp += 4 |
| 0x11 | `shl` | r0 = mem32[sp] << r0; sp += 4 |
| 0x12 | `shr` | r0 = mem32[sp] >> r0; sp += 4 |
| 0x13 | `lt` | r0 = (mem32[sp] < r0) ? 1 : 0; sp += 4 |
| 0x14 | `le` | r0 = (mem32[sp] <= r0) ? 1 : 0; sp += 4 |
| 0x15 | `gt` | r0 = (mem32[sp] > r0) ? 1 : 0; sp += 4 |
| 0x16 | `ge` | r0 = (mem32[sp] >= r0) ? 1 : 0; sp += 4 |
| 0x17 | `eq` | r0 = (mem32[sp] == r0) ? 1 : 0; sp += 4 |
| 0x18 | `ne` | r0 = (mem32[sp] != r0) ? 1 : 0; sp += 4 |
| 0x19 | `and` | r0 = mem32[sp] & r0; sp += 4 |
| 0x1a | `or` | r0 = mem32[sp] \| r0; sp += 4 |
| 0x1b | `xor` | r0 = mem32[sp] ^ r0; sp += 4 |
| 0x1c | `sxb` | r0 = sign_extend_8(r0) |
| 0x1d | `sxw` | r0 = sign_extend_16(r0) |
| 0x1e | `putchar` | sys.stdout.write(chr(r0 & 0xff)); r0 unchanged |
| 0x1f | `jli` | lr = pc; pc = r0 & 0xffff  *(indirect call via r0)* |
| 0x20 | `fadd` | r0 = float_bits(float(stack) + float(r0)); sp += 4 |
| 0x21 | `fsub` | r0 = float_bits(float(stack) - float(r0)); sp += 4 |
| 0x22 | `fmul` | r0 = float_bits(float(stack) * float(r0)); sp += 4 |
| 0x23 | `fdiv` | r0 = float_bits(float(stack) / float(r0)); sp += 4 |
| 0x24 | `flt` | r0 = (float(stack) < float(r0)) ? 1 : 0; sp += 4 |
| 0x25 | `fle` | r0 = (float(stack) <= float(r0)) ? 1 : 0; sp += 4 |
| 0x26 | `fgt` | r0 = (float(stack) > float(r0)) ? 1 : 0; sp += 4 |
| 0x27 | `fge` | r0 = (float(stack) >= float(r0)) ? 1 : 0; sp += 4 |
| 0x28 | `itof` | r0 = float_bits(float(signed32(r0))) |
| 0x29 | `ftoi` | r0 = int(float(r0)) truncated toward zero |
| 0x2a | `lts` | r0 = (signed32(stack) < signed32(r0)) ? 1 : 0; sp += 4 |
| 0x2b | `les` | r0 = (signed32(stack) <= signed32(r0)) ? 1 : 0; sp += 4 |
| 0x2c | `gts` | r0 = (signed32(stack) > signed32(r0)) ? 1 : 0; sp += 4 |
| 0x2d | `ges` | r0 = (signed32(stack) >= signed32(r0)) ? 1 : 0; sp += 4 |
| 0x2e | `divs` | r0 = signed32(stack) / signed32(r0); sp += 4 (0 if divisor is 0) |
| 0x2f | `mods` | r0 = signed32(stack) % signed32(r0); sp += 4 (0 if divisor is 0) |
| 0x30 | `shrs` | r0 = signed32(stack) >> (r0 & 31); sp += 4 *(arithmetic shift)* |

Float operands are 32-bit IEEE 754 single-precision values stored as raw bit patterns in r0 and on the stack. `float_bits(f)` means the IEEE 754 bit pattern of `f`. `float(x)` means interpret the bit pattern `x` as IEEE 754.

**Stack-binary convention**: all `add`, `sub`, `mul`, `div`, `mod`, `shl`, `shr`, comparisons, and bitwise ops pop their left operand from the stack (`mem32[sp]`) and use r0 as right operand, writing the result to r0 and incrementing sp by 4.

**Signed vs unsigned arithmetic**: `lt`/`le`/`gt`/`ge`, `div`, `mod`, and `shr` treat operands as **unsigned** 32-bit values. The `lts`/`les`/`gts`/`ges`, `divs`, `mods`, and `shrs` variants treat operands as **signed** 32-bit (two's complement) values. The compiler emits signed variants when the C operand type is a signed integer type (`char`, `short`, `int`, `long`); unsigned variants are used for pointer comparisons and unsigned integer types.

**Store convention**: `sb`, `sw`, `sl` pop a 32-bit address from the stack and write r0 to that address.

**CPU builtin**: `putchar` (0x1e) is pre-declared as a global builtin symbol by `make_basic_types()` via `insert_builtin()` in `types.c`; the compiler emits the opcode directly without a call frame.

### Format 1 — Signed 8-bit Operand

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0x40 | `immb imm8` | r0 = sign_extend(imm8) |
| 0x41 | `adj imm8` | sp += imm8 |

`adj` is used to allocate (`adj -N`) or free (`adj +N`) local variable space within a scope.

### Format 2 — Unsigned 16-bit Operand

| Opcode | Mnemonic | Semantics |
|---|---|---|
| 0x80 | `immw imm16` | r0 = imm16 |
| 0x81 | `immwh imm16` | r0 = (r0 & 0xffff) \| (imm16 << 16) |
| 0x82 | `j imm16` | pc = imm16 |
| 0x83 | `jl imm16` | lr = pc; pc = imm16 |
| 0x84 | `jz imm16` | if (r0 == 0) pc = imm16 |
| 0x85 | `jnz imm16` | if (r0 != 0) pc = imm16 |
| 0x86 | `enter imm16` | mem32[sp-4] = lr; mem32[sp-8] = bp; bp = sp-8; sp -= imm16+8 |
| 0x87 | `lea imm16` | r0 = bp + sign_extend(imm16) |
| 0x88 | `ssp imm16` | sp = imm16 |
| 0x89 | `adjw imm16` | sp += sign_extend(imm16) |

**Loading a 32-bit constant**: use `immw` (lower 16 bits) followed by `immwh` (upper 16 bits). The compiler's `gen_imm(val)` emits this pair when `val > 0xffff`.

**Direct function call**: `jl` saves the return address in `lr`, then jumps. `enter` saves `lr` and `bp` on the stack and sets `bp` for the new frame. On return, `ret` restores the frame.

**Indirect function call**: load the function pointer into r0, then `jli` (same save/jump semantics as `jl` but target is r0 not an immediate).

## Memory Map

The top 256 bytes of the 16-bit address space are reserved for memory-mapped I/O (MMIO). The stack grows down from `0x1000` and global data is placed after code, both well below `0xFF00`.

| Address | Size | Register | Access |
|---------|------|----------|--------|
| `0xFF00` | 4 B  | Cycle counter | RO |
| `0xFF04–0xFFFF` | — | Reserved | — |

**Cycle counter (`0xFF00`):** 32-bit little-endian read-only register. Incremented once per instruction executed. Wraps at 2³²−1. Writes to the MMIO region are silently ignored.

```c
/* Read 32-bit cycle counter */
unsigned long t = *(volatile unsigned long *)0xFF00;
```

Both `sim_c` and `cpu3/sim.py` implement this peripheral.

## Assembly Syntax

```
.text=0             ; set text section origin to address 0
.data=N             ; set data section origin

label:              ; defines a label at current address
    mnemonic        ; zero-operand instruction
    mnemonic  val   ; instruction with operand (decimal or 0x hex)
    mnemonic  label ; instruction with label operand (resolved by assembler)

; Directives:
    byte  v1 v2 ...    ; emit one byte per value
    word  v1 v2 ...    ; emit one 16-bit word per value (labels allowed as values)
    long  v1 v2 ...    ; emit one 32-bit long per value
    allocb N           ; reserve N zero bytes
    allocw N           ; reserve N zero words (word-aligned)
    align              ; align to next word boundary
```

Comments start with `;`. Labels and instruction names are case-sensitive. The assembler resolves labels in `word` and `byte` directives (not only in instruction operands).

## Typical Assembly Output

```asm
.text=0
    ssp     0x1000    ; set stack pointer
    jl      main      ; call main
    halt

main:
    enter   0         ; save lr, bp; no extra locals at function level
    adj     -4        ; allocate 2 locals (int a, int b each 2 bytes)
    lea     -2
    push
    immw    0x000a    ; a = 10
    sw
    lea     -4
    push
    immw    0x000b    ; b = 11
    sw
    lea     -2
    lw
    push
    lea     -4
    lw
    add               ; a + b
    ret
    adj     4         ; reclaim locals
    ret               ; function return
```
