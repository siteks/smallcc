# CPU3 + CPU4 + smallcc Performance Analysis

## 1. Baseline and Motivation

### What CoreMark Measures

CoreMark is an industry-standard embedded benchmark designed to exercise realistic workloads:

- **List processing**: linked-list find and sort operations with pointer chasing
- **Matrix multiply**: multiply-accumulate inner loops with 2D array indexing
- **State machine / CRC**: switch dispatch and bitwise operations
- **String search**: character-by-character pattern matching

### Scoring Formula

```
score = iterations / elapsed_seconds
```

`sim_c` counts exactly one cycle per instruction executed. The benchmark uses
`CLOCKS_PER_SEC = 1,000,000`, and the cycle counter is read via the MMIO register at
`0xFF00`. CoreMark/MHz = score / 1.0 (since 1 cycle = 1 "MHz" in simulation).

### Measured Scores

| Build | CoreMark/MHz | Notes |
|---|---|---|
| CPU3 -O2 | **0.445** | Stack machine, peephole optimiser |
| CPU4 -O0 | **0.458** | Raw RISC ISA benefit vs CPU3 stack machine |
| CPU4 -O1 | **0.503** | +10% from stack-IR peephole (Rules 1–9) |
| CPU4 -O2 | **0.623** | +24% from SSA-level optimisations (B2/B3/C1/D4) |
| CPU4 -O2 +E1–E5 | **0.696** | +12% from E1/E2/E3/E4/E5 (LEA+LOAD, ADDLI+LOAD, ALU+MOV, sign-loads, dead-MOV DCE) |
| CPU4 -O2 +F1/F2 | **0.712** | +2% from F1 (MOVI+MOV retarget), F2 (redundant SXW DCE), extended C2 (LOAD+SXW+MOV fusion) |
| CPU4 -O2 +discard-postinc | **0.744** | historical best; E3 had latent correctness bug (vA==VREG_START cases) masked by G1 register-pinning |
| CPU4 -O2 current (E3 guarded) | **0.683** | E3 disabled for accumulator-destination ALU; all CRCs correct |

To reproduce:

```bash
make smallcc
# compile coremark with smallcc, then:
./sim_c coremark/coremark.s      # CPU3
python3 cpu4/sim.py coremark/coremark4.s  # CPU4
```

### Industry Baseline Comparison

| Processor / Runtime | CoreMark/MHz | Notes |
|---|---|---|
| ARM Cortex-M0 (ARMv6-M) | 2.33 | In-order, 3-address load/store RISC |
| RISC-V RV32E (Ibex) | ~0.9 | Minimal 3-address RISC |
| Interpreted CPython | ~0.01 | Pure interpreter overhead |
| Interpreted Forth | ~0.1–0.3 | Stack machine, no JIT |
| Native Forth (compiled) | >1.0 | Direct-threaded, TOS-cached |
| JVM HotSpot (after JIT) | ~5–15 | Aggressive optimization, native code |
| **CPU3 + smallcc -O2** | **0.445** | Stack machine, peephole optimiser |
| **CPU4 + smallcc -O2** | **0.623** | 3-address RISC, SSA-level optimisations (B2/B3/C1/D4) |
| **CPU4 + smallcc -O2 (current)** | **0.683** | +E1–E5, F1, F2, discard-postinc; E3 guarded for correctness |

The gap to Cortex-M0 is approximately **3.4×** for the current build (2.33 / 0.683), down from
the ~58× gap that would exist with a naive 1-register stack machine (2.33 / 0.04).

---

## 2. CPU3 Architecture Recap

CPU3 is a 1-register accumulator/stack machine:

- **r0** is the sole accumulator and return-value register (32-bit)
- **sp**, **bp**, **lr**, **pc** are 16-bit control registers
- All ALU operations pop the **left operand from the stack** (`mem32[sp]`) and use r0 as
  the right operand, writing the result back to r0 and advancing sp by 4
- Three encoding formats: Format-0 (1 byte, opcode only), Format-1 (2 bytes, opcode +
  signed 8-bit immediate), Format-2 (3 bytes, opcode + unsigned 16-bit immediate)
- `int` and pointers are **2 bytes**; `long`, `float`, `double` are **4 bytes**
- No cache, no pipeline, no branch prediction — 1 instruction = 1 cycle

CPU4, a 3-address RISC successor with variable-width encoding, is described in §5.

### Measured Encoding Density

From `coremark.s` (text section only):

- **8,319 instructions** in **14,746 bytes** = **1.77 bytes/instruction**

This is actually competitive with JVM bytecode (~1.2 bytes/instruction). The problem is
not encoding density — it is **instructions per operation**.

---

## 3. CPU3 Instruction Mix

Static instruction counts from the text section of `coremark.s`:

| Instruction | Count | % | Category |
|---|---|---|---|
| `push`   | 1,615 | 19.4% | Stack move |
| `lea`    | 1,448 | 17.4% | Address calc |
| `immw`   |   1,046 | 12.6% | Immediate load |
| `lw`     |   809 |  9.7% | Load (2-byte) |
| `add`    |   510 |  6.1% | ALU |
| `ll`     |   366 |  4.4% | Load (4-byte) |
| `adj`    |   249 |  3.0% | Stack alloc |
| `pushw`  |   241 |  2.9% | Stack move |
| `jl`     |   230 |  2.8% | Call |
| `jnz`   |   181 |  2.2% | Branch |
| `sw`     |   178 |  2.1% | Store (2-byte) |
| `eq`     |   161 |  1.9% | Compare |
| `ret`    |   128 |  1.5% | Return |
| `jz`     |   119 |  1.4% | Branch |
| `sub`    |   113 |  1.4% | ALU |
| `mul`    |    97 |  1.2% | ALU |
| `sl`     |    92 |  1.1% | Store (4-byte) |
| `j`      |    78 |  0.9% | Jump |
| other    |   658 |  7.9% | — |
| **Total**| **8,319** | | |

### Category Summary

| Category | % of instructions |
|---|---|
| Stack moves (`push`, `pushw`, `adj`) | 25.3% |
| Address calculation (`lea`, `immw`) | 30.0% |
| Loads (`lw`, `ll`, `lb`) | 14.5% |
| ALU (`add`, `sub`, `mul`, `eq`, …) | 12.6% |
| Branches / calls / returns | 9.7% |
| Stores (`sw`, `sl`, `sb`) | 4.1% |
| Other | 3.8% |

### The Core Inefficiency

**Loading a local variable** — the most frequent primitive in any C program — takes
2 instructions on CPU3:

```asm
lea     -N       ; r0 = bp + (-N)   [3 bytes]
lw               ; r0 = mem16[r0]   [1 byte]
```

When the result is needed as a binary operand, a push is also required:

```asm
lea     -N       ; address
lw               ; load value
push             ; save left operand to memory stack
```

Key pattern frequencies (static):

| Pattern | Count | Instructions | Total instr |
|---|---|---|---|
| `lea + lw` (load local into r0) | 641 | 2 each | 1,282 |
| `lea + ll` (load long local into r0) | 270 | 2 each | 540 |
| `push + lea + lw` (push local as binary LHS) | 416 | 3 each | 1,248 |

Together these three patterns account for **3,070 instructions — 36.9% of the entire
program** — just to read local variables.

---

## 4. CPU3 Critical Loop Analysis

### 4a. `matrix_mul_matrix` — Inner Loop

Matrix operations dominate CoreMark scoring. The inner loop body is approximately
**71 instructions**, plus 12 for the loop counter increment and back-branch, totalling
**~83 instructions per iteration**.

#### Loop variable increment: `k++`

```asm
lea     -14      ; address of k
lw               ; load k
push             ; save k to stack
immw    0x0001   ; constant 1
add              ; k + 1
lea     -14      ; address of k (again)
push             ; prepare store target
sw               ; mem[k_addr] = k + 1
```

**8 instructions** to increment a local integer. A Cortex-M0 does this in 1 instruction
(`ADDS r2, r2, #1` with `r2` holding `k`).

#### Array element access: `a[i][k]`

```asm
lea     base_a   ; base address of a[][]
push
lea     -i_off
lw               ; i
push
immw    stride   ; sizeof(row)
mul              ; i * stride
add              ; a + i*stride
push
lea     -k_off
lw               ; k
push
immw    4        ; sizeof(long) = 4
mul              ; k * 4
add              ; a[i] + k*4
ll               ; load a[i][k]
```

**~14 instructions** for what `LDR r0, [r1, r2, LSL #2]` does in 1 instruction on ARM.

### 4b. `core_list_find` — Linked-List Traversal

The condition `list->info->idx != info->idx` requires a memory-indirect load chain:

```asm
; list->info->idx
lea     -2       ; &list (local pointer)
lw               ; list
push
immw    0x0002   ; offset of .info field
add              ; &list->info
lw               ; list->info (a pointer)
push
immw    0x0000   ; offset of .idx
add              ; &list->info->idx
lw               ; list->info->idx (the value)
```

**10 instructions** for one side of the comparison. A 3-address RISC handles this in
3–4 instructions using `LDR r1, [r0, #2]` (load indirect with offset).

There is no way to reduce this significantly without either register indirect addressing
or a multi-operand load instruction — both require ISA changes.

### 4c. `core_state_transition` — Switch Dispatch

The current codegen re-evaluates the selector expression for every `case` comparison.
With 7 cases and a `switch` on a local variable:

```asm
; case CORE_STATE_START (0):
lea     -4       ; &state
lw               ; state value
push
immw    0x0000   ; case constant
eq
jnz     Lcase0
; case CORE_STATE_INVALID (1):
lea     -4       ; &state  ← RELOADED
lw               ;         ← RELOADED
push
immw    0x0001
eq
jnz     Lcase1
; ... repeated for each case
```

The selector reload (`lea -4; lw`) costs 2 instructions per case. For 7 cases, this is
**14 redundant instructions per dispatch** (plus the push that precedes each comparison,
adding another 7 wasted instructions = **21 redundant instructions**).

This is purely a compiler deficiency — no ISA change is needed to fix it.

Equivalent CPU4 loops are in §7.

---

## 5. CPU4 Architecture

CPU4 is a 3-address RISC machine with a dense variable-width encoding. The architecture
directly addresses the three major inefficiencies identified in the CPU3 analysis above.

### Key Encoding Formats

| Format | Width | Innovation | CPU3 equivalent |
|---|---|---|---|
| **F2** | 2 bytes | `lw rx, [bp+N]` — bp-relative load in 1 instruction | `lea N; lw` = 2 instr, 4 bytes |
| **F1a** | 2 bytes | `add rd, rx, ry` — 3-address ALU, no push/pop | `push; add` = 2 instr, but needs prior LEA |
| **F3b** | 3 bytes | `llw rx, [ry+N]` — register-indirect load with imm10 | `push; immw N; add; lw` = 4 instr |
| **F1b** | 2 bytes | `inc rd` / `dec rd` — in-place register increment | `immw 1; add` = 2 instr (plus surrounding loads/stores) |
| **F3c** | 3 bytes | `immw rd, imm16` — load immediate to any register | `immw` always goes to r0 on CPU3 |

F2 (`lw`, `sw`, `ll`, `sl`) is the single largest win: the `lea + lw` pattern that
constitutes 15.4% of all CPU3 instructions collapses to 1 instruction.

### Compiler Optimisations at -O2

The CPU4 path applies SSA-level peephole passes after `lift_to_ssa()`:

| Pass | Effect |
|---|---|
| B2 compare-branch fusion | `push; immw K; eq; jnz L` → `ne rd, rx, ry; jz L` (4 instr → 2) |
| B3/C1 LEA→STORE forwarding | Eliminates the `push` preceding many stores |
| C2 LOAD+MOV fusion | Collapses `SSA_LOAD + SSA_MOV` when destination equals source |
| D4 add-immediate | `immw K; add` → `addli rx, ry, K` (F3b, 1 instruction) |
| E1 LEA+LOAD → bp-relative | `SSA_LEA rd=vA; SSA_LOAD rs1=vA` → F2 `lw`/`ll` (kills lea) |
| E2 ADDLI+LOAD → offset LOAD | `addli r0,r1,2; llw r0,r0,0` → `llw r0,r1,1` (F3b with imm) |
| E3 ALU+MOV → retarget ALU | `mul r0,r3,r0; or r3,r0,r0` → `mul r3,r3,r0` (kills mov) |
| E4 LOAD+SXW/SXB → sign-load | `lw rx,N; sxw rx` → `lwx rx,N` / `llw rx,ry,0; sxw rx` → `llwx` |
| E5 Dead MOV DCE | Kill `SSA_MOV` whose destination virtual register has no uses |

### Score Progression

```
CPU4 -O0                    → 0.458  (raw RISC ISA benefit vs CPU3 stack machine)
CPU4 -O1                    → 0.503  (+10%, stack-IR peephole: constant fold, dead-branch, adj merge)
CPU4 -O2                    → 0.623  (+24% on top of -O1, SSA-level: B2, B3/C1, C2, D4)
CPU4 -O2 +E1–E5             → 0.696  (+12% on top of -O2, SSA-level: E1/E2/E3/E4/E5)
CPU4 -O2 +F1/F2             → 0.712  (+2%, F1: MOVI+MOV retarget; F2: redundant SXW DCE; C2 extended)
CPU4 -O2 +discard-postinc   → 0.744  (historical; E3 had latent bug masked by G1)
CPU4 -O2 current (guarded)  → 0.683  (E3 vA==VREG_START cases disabled; all CRCs verified correct)
```

---

## 6. CPU4 Instruction Mix

Static instruction counts from `coremark4.s` (CPU4 -O2 build):

| Instruction | Count | % | Category |
|---|---|---|---|
| `or` (pseudo-mov) | 1,111 | 16.1% | Register copy |
| `immw` | 784 | 11.4% | F3c immediate load |
| `lw` | 631 | 9.2% | F2 bp-rel load (word) |
| `adjw` | 516 | 7.5% | Stack adjust |
| `lea` | 448 | 6.5% | F3c address (out-of-F2-range or large offset) |
| `sw` | 316 | 4.6% | F2 bp-rel store (word) |
| `ll` | 278 | 4.0% | F2 bp-rel load (long) |
| `addli` | 274 | 4.0% | F3b add-with-imm10 |
| `add` | 236 | 3.4% | F1a ALU |
| `slw` | 231 | 3.4% | F3b reg-indirect store (word) |
| `jz` | 220 | 3.2% | F3a conditional branch |
| `sxw` | 209 | 3.0% | F1b sign extend |
| `llw` | 185 | 2.7% | F3b reg-indirect load (word) |
| `jl` | 162 | 2.4% | F3a call |
| `j` | 151 | 2.2% | F3a jump |
| `lll` | 144 | 2.1% | F3b reg-indirect load (long) |
| `sl` | 135 | 2.0% | F2 bp-rel store (long) |
| `sll` | 122 | 1.8% | F3b reg-indirect store (long) |
| `mul` | 111 | 1.6% | F1a ALU |
| `and` | 85 | 1.2% | F1a ALU |
| `ret` | 86 | 1.2% | F0 return |
| other | ~312 | 4.5% | — |
| **Total** | **6,883** | | |

### Category Summary

| Category | % of instructions |
|---|---|
| Register copies (`or` pseudo-mov) | 16.1% |
| F2 bp-relative loads (`lw`, `ll`, `lb`) | 13.7% |
| Immediate loads (`immw`, `immwh`) | 11.5% |
| F3b reg-indirect loads (`llw`, `lll`, `llb`) | 5.2% |
| F3b reg-indirect stores (`slw`, `sll`, `slb`) | 5.3% |
| F2 bp-relative stores (`sw`, `sl`, `sb`) | 6.8% |
| F3b add-immediate (`addli`) | 4.0% |
| ALU (`add`, `sub`, `mul`, `and`, `ne`, …) | ~10.5% |
| Stack adjust (`adjw`) | 7.5% |
| Address (`lea`) | 6.5% |
| Sign extend (`sxw`, `sxb`) | 3.1% |
| Branches / calls / returns | 9.8% |

### Key Comparisons with CPU3

| Metric | CPU3 | CPU4 -O2 |
|---|---|---|
| `push` / `pop` | 25.3% of instructions | **0%** — stack machine eliminated |
| bp-relative loads | 14.5% (2 instr each) | 13.7% (1 instr each via F2) |
| `lea` for address calc | 17.4% (mostly for load/store) | 6.5% (out-of-range fallback only) |
| Register copies | — | 16.1% (`or` pseudo-mov — SSA residue) |

### Encoding Density

- **6,883 instructions** in an estimated **~17,000 bytes** = **~2.47 bytes/instruction**

CPU4 bytes/instruction is *higher* than CPU3 (F2=2B, F3b/F3c=3B vs many 1B CPU3 opcodes)
but **instructions per operation is halved**. Encoding density is the wrong metric.

---

## 7. CPU4 Critical Loop Analysis

### 7a. `matrix_mul_matrix` — Inner k-loop

#### Loop variable increment: `k++`

From `coremark4.s`, the k-loop tail (`_l298`):

```asm
ll      r1, -3      ; r1 = old k          [F2, 2 bytes]
ll      r3, -3      ; r3 = k — REDUNDANT  [F2, 2 bytes]
addli   r0, r3, 1   ; r0 = k + 1          [F3b, 3 bytes]
sl      r0, -3      ; k = k + 1           [F2, 2 bytes]
or      r0, r1, r1  ; r0 = old k — DEAD   [F1a, 2 bytes]
j       _l297       ; back-branch         [F3a, 3 bytes]
```

**5 instructions / 11 bytes** for the increment (plus 3 bytes back-branch).
Compare to CPU3's 8 instructions / ~14 bytes.

Remaining waste:
- The second `ll r3, -3` is **redundant** — old k is already in `r1`
- `or r0, r1, r1` is **dead** — the pre-increment value of `k` is unused after the store

Root cause: the stack IR emits `LOAD k; PUSH (save old k); LOAD k; MOVI 1; ADD; STORE k`.
The `PUSH` captures the old value (for post-increment semantics); the second `LOAD`
reloads k for the +1. A same-address consecutive-load elimination in `ssa_opt.c` would
collapse `ll r1, -3; ll r3, -3` → keep only `r1` and use it for both.

#### Array element access: `a[i][k]`

The k-indexed access still requires a multiply to compute the offset:

```asm
ll      r3, -1      ; i
ll      r0, 2       ; N (size param)
mul     r0, r3, r0  ; i * N          ← rd=r0, but result needed in r3
or      r3, r0, r0  ; r3 = i*N       ← could be: mul r3, r3, r0
ll      r0, -3      ; k
add     r0, r3, r0  ; i*N + k        ← rd=r0, but result needed in r3
or      r3, r0, r0  ; r3 = i*N+k    ← could be: add r3, r3, r0
immw    r0, 0x0001  ; stride = sizeof(short)/2
shl     r0, r3, r0  ; offset in bytes ← rd=r0, but result needed in r3
or      r3, r0, r0  ; r3 = offset    ← could be: shl r3, r3, r0
lw      r0, 7       ; &a (param)
add     r0, r3, r0  ; address
llw     r0, r0, 0   ; load a[i][k]   [F3b]
sxw     r0          ; sign-extend    ← could be: llwx r0, r0, 0 (1 instr)
```

**~16 instructions** per element (including `or` pseudo-moves). Three missed optimisations
are visible:

1. **op→mov pattern**: Each `ALUop r0, r3, r0; or r3, r0, r0` pair writes the result to
   r0 and then copies it to r3. The F1a encoding supports any destination register, so
   `mul r3, r3, r0` (writing directly to r3) would eliminate the `or` copy. The backend
   always chooses rd=r0 (the accumulator) and inserts a copy — fixing this requires
   `risc_backend.c` to select rd to match the consumer's expected source register.

2. **`llwx` not used**: `llw r0, r0, 0; sxw r0` (5 bytes, 2 instr) could be
   `llwx r0, r0, 0` (3 bytes, 1 instr) — the sign-extending F3b load. The backend
   currently never emits `llwx`/`llbx` for register-relative accesses.

### 7b. `core_list_find` — Pointer Chain

One level of `list->info->idx` (from `coremark4.s`, lines 1960–1966):

```asm
lw      r1, 4       ; r1 = list param    [F2, 2 bytes]
addli   r0, r1, 2   ; &list->info        [F3b, 3 bytes] ← redundant
llw     r1, r0, 0   ; r1 = list->info    [F3b, 3 bytes]
addli   r0, r1, 2   ; &list->info->idx   [F3b, 3 bytes] ← redundant
llw     r0, r0, 0   ; r0 = idx value     [F3b, 3 bytes]
sxw     r0          ; sign-extend        [F1b, 2 bytes]
```

**6 instructions / 16 bytes** vs CPU3's 10 instructions.

Missed optimisation: each `addli rx, ry, 2; llw rz, rx, 0` pair (6 bytes, 2 instr) is
equivalent to `llw rz, ry, 1` (3 bytes, 1 instr), because F3b `llw` scales its imm10 by
2 — `llw r1, r1, 1` loads `mem16[r1 + 1×2] = mem16[r1+2]`. The compiler emits the
explicit address computation instead of using the built-in F3b offset. Fixing this in
`risc_backend.c` (or `ssa.c`) would reduce each pointer-chain step from 2 instructions
to 1, matching ARM's `LDR r1, [r0, #2]`:

```asm
lw      r1, 4       ; r1 = list          [F2, 2 bytes]
llw     r1, r1, 1   ; r1 = list->info    [F3b, 3 bytes]  ← addli+llw folded
llwx    r0, r1, 1   ; r0 = idx (sxd)     [F3b, 3 bytes]  ← addli+llw+sxw folded
```

**3 instructions / 8 bytes** — same as ARM.

ARM: `LDR r1,[r0,#2]; LDR r0,[r1,#2]` = 2 instructions, 4 bytes.

### 7c. `core_state_transition` — Switch Dispatch

The B2 optimisation fuses compare-then-branch from 4 instructions to 2. Each case in the
switch body (from `coremark4.s`, lines 7835–7869) follows this pattern:

```asm
; per-case (5 instructions, 14 bytes):
lea     r1, -5      ; addr of local       [F3c, 3 bytes] ← unnecessary
llw     r1, r1, 0   ; load state value    [F3b, 3 bytes]
immw    r0, K       ; case constant       [F3c, 3 bytes]
ne      r0, r1, r0  ; compare (B2 fusion) [F1a, 2 bytes]
jz      _lN         ; branch if match     [F3a, 3 bytes]
```

**5 instructions / 14 bytes** per case vs CPU3's `lea; lw; push; immw K; eq; jnz` =
6 instructions.

Missed optimisation: `lea r1, -5; llw r1, r1, 0` (6 bytes, 2 instr) could be
`lw r1, -5` (2 bytes, 1 instr) — the F2 bp-relative word load. The local holding the
state value is within the F2 range. The backend emitted a `lea` + register-relative
`llw` instead of the direct F2 form, giving:

```asm
; corrected per-case (4 instructions, 11 bytes):
lw      r1, -5      ; load state value    [F2, 2 bytes]
immw    r0, K       ; case constant       [F3c, 3 bytes]
ne      r0, r1, r0  ; compare             [F1a, 2 bytes]
jz      _lN         ; branch              [F3a, 3 bytes]
```

The B2 fusion specifically eliminates the `push; eq` → `ne` step (saves 1 instruction per
case, and removes the intermediate 0/1 materialization). With the `lw` fix, each case
drops from 5 to 4 instructions.

---

## 8. CPU4 Remaining Optimisation Opportunities

| Inefficiency | Root cause | Fix location | Est. gain |
|---|---|---|---|
| Redundant load in post-increment (`ll r3, -3` after `ll r1, -3`) | Stack IR `PUSH; LOAD` idiom generates two loads | `ssa_opt.c` — same-address consecutive-load elim | 5–10% |
| Dead `or` pseudo-moves (1,111 instances, 16%) | SSA MOV not fully eliminated; C2 handles only adjacent pairs | `ssa_opt.c` — DCE pass after copy propagation | 5–8% |
| op→mov pattern: `mul r0,r3,r0; or r3,r0,r0` instead of `mul r3,r3,r0` | Backend always writes to rd=r0 (accumulator) then copies; F1a supports any rd | `risc_backend.c` — select rd to match consumer; eliminates ~3 `or` copies per array index | 3–5% |
| `addli rx,ry,N; llw rz,rx,0` instead of `llw rz,ry,N/2` | Backend emits explicit address add then load at offset 0; F3b imm10 can carry the offset directly | `risc_backend.c` / `ssa.c` — fold `SSA_ALU addli + SSA_LOAD` into single `SSA_LOAD` with non-zero imm | 3–5% on pointer-chain code |
| `llw rz,ry,0; sxw rz` instead of `llwx rz,ry,0` | Backend never emits `llwx`/`llbx` (sign-extending F3b loads) | `risc_backend.c` — emit `llwx`/`llbx` when followed by `sxw`/`sxb` on same register | 1–2% |
| `lea rx,-N; llw rx,rx,0` instead of `lw rx,-N` | Backend emits F3c `lea` + F3b register-relative `llw` for bp-relative word access within F2 range | `risc_backend.c` — `SSA_LOAD rs1==-2` path should emit F2 `lw`/`ll` before falling back to `lea+llw` | 2–4% |
| Array indexing: `mul` by power-of-2 stride | Strength-reduce (Rule 7) fires on stack IR, but SSA lift sees the result as a general multiply | Gate Rule 7 before SSA lift, or add SSA strength-reduce in `ssa_opt.c` | 3–5% |
| Switch: O(n) linear scan | No jump-table codegen | `codegen.c` + `risc_backend.c` | 5–10% on FSM code |
| Loop induction vars reload from memory each iteration | r6/r7 not used as dedicated loop registers | Register allocator (`regalloc.c`) with live-range analysis | 10–15% on loops |

### Implementation Roadmap

Current baseline: CPU4 -O2 at **0.623 CoreMark/MHz**. Estimated gains are speculative
and may be conservative; actual numbers will be measured after each implementation.

| Priority | Change | Effort | Fix location | Status |
|---|---|---|---|---|
| 1 | `lea+llw` → F2 `lw` for in-range bp-relative locals | Low | `ssa_opt.c` (E1) | **Done** |
| 2 | `addli rx,ry,N; llw rz,rx,0` → `llw rz,ry,N/2` (use F3b offset) | Low | `ssa_opt.c` (E2) | **Done** |
| 3 | op→mov: write ALU result directly to consumer register (avoid `or` copy) | Low | `ssa_opt.c` (E3) | **Partial** — disabled when ALU writes to accumulator (vA==VREG_START); root cause unknown |
| 4 | `llw+sxw` → `llwx` / `llb+sxb` → `llbx` (sign-extending F3b loads) | Low | `risc_backend.c` (E4) | **Done** |
| 5 | Dead `or` elimination: DCE in SSA | Low | `ssa_opt.c` (E5) | **Done** |
| 6 | Redundant consecutive-load elimination (same address `LOAD; LOAD`) | Medium | `ssa_opt.c` | — |
| 7 | Array stride strength-reduce at SSA level (mul by power-of-2 → shl) | Medium | `ssa_opt.c` | — |
| 8 | Jump table for dense switch | Medium | `codegen.c` + `risc_backend.c` | — |
| 9 | Loop induction variables via r6/r7 with live-range analysis | High | `regalloc.c` | — |

### Effort Definitions

**Low**: targeted pattern match in `risc_backend.c`, or a new single-pass rule in
`ssa_opt.c`. No architectural changes. 1–3 days per item.

**Medium**: new IR pattern detection in `ssa_opt.c` or `ssa.c`; may require tracking
virtual register provenance or introducing a new SSA pass. 1–2 weeks.

**High**: register allocator redesign in `regalloc.c`; ABI implications for callee-saved
r6/r7 (functions using them must save/restore). Multi-week project.

### Fundamental Ceiling

Items 1–5 are all Low effort and independent — implementing them first will establish the
true baseline before attempting the harder items.

Items 1–8 together: if gains compose, plausibly **35–45% improvement** →
~0.84–0.90 CoreMark/MHz.

Item 9 (loop induction): additional 10–15% → potentially ~0.95–1.0 CoreMark/MHz.

Reaching 2.33 CoreMark/MHz requires matching ARM's 1-instruction array access, which
needs ISA support for scaled-register addressing (`llw rx, [ry + rz*k]`) — a CPU5
architectural change.

---

## 9. Encoding Density Comparison

| Architecture | Bytes/Instr | Instr per binary op | Instr per local load | Notes |
|---|---|---|---|---|
| CPU3 (current) | 1.77 | 4 min (push+load+load+op) | 2 (lea+lw) | 1-register stack |
| **CPU4 -O2** | **~2.47** | **2–3** | **1 (F2 lw)** | 3-address RISC, SSA opts |
| ARM Thumb-2 | ~1.6 | 1 (3-addr) | 1 (LDR r0,[r1,#N]) | 16/32-bit mixed |
| RISC-V RV32I | 4.0 | 1 (3-addr) | 1 (LW r0,N(r1)) | 32-bit fixed |
| x86-64 | ~3.5 | 1 (mem operand) | 1 (MOV eax,[rbp-N]) | CISC with mem operands |
| JVM bytecode | ~1.2 | 3 (iload+iload+iadd) | 1 (iload_N) | Purpose-built stack ops |

CPU4's bytes/instruction ratio (2.47) is *higher* than CPU3 (1.77) because F2/F3b/F3c
encodings are 2–3 bytes each while many CPU3 opcodes are 1 byte. But **instructions per
operation is halved** — the `push`/`pop` overhead is gone entirely, and local variable
access requires one instruction instead of two. Encoding density is the wrong metric;
**instructions per operation** determines performance.

---

## 10. What a Real 2.33 CoreMark/MHz Processor Does

A Cortex-M0 (ARMv6-M, in-order, 1 cycle per instruction for most ops) achieves 2.33
CoreMark/MHz (ARM official figure). Key capabilities that explain the gap:

| Operation | Cortex-M0 | CPU3 | CPU4 -O2 |
|---|---|---|---|
| Load local variable | `LDR r0, [fp, #-N]` = 1 instr | `lea; lw` = 2 instr | `lw rx, [bp+N]` = **1 instr** ← closed |
| Push local for binop | included in operand of `ADDS` | `push; lea; lw` = 3 instr | eliminated (3-address ALU) |
| Increment local `i++` | `ADDS r2, r2, #1` = 1 instr | `lea; lw; push; immw 1; add; lea; push; sw` = 8 instr | `ll; addli; sl` = 3 useful + 2 wasted = 5 instr |
| Array access `a[i]` | `LDR r0, [r1, r2, LSL #1]` = 1 instr | `lea; push; lea; lw; push; immw; mul; add; lw` ≈ 9 instr | `ll i; ll N; mul; add; llw` ≈ 13 instr (w/ or copies) |
| Binary op `a + b` (locals) | `LDR r0,[fp,#a]; ADDS r0,r0,[fp,#b]` = 2 instr | `lea; lw; push; lea; lw; add` = 6 instr | `lw r0,N; lw r1,M; add r0,r0,r1` = **3 instr** |

CPU4 has closed the local-variable-load gap entirely. The remaining gap is primarily in
complex address computations (array indexing with non-constant strides) and the SSA
residue `or` pseudo-moves.

---

## Appendix: Verification


### CPU3 Instruction Counts

All instruction counts can be verified against `coremark.s`:

```bash
# Count push instructions in text section
grep -c '^\s*push$' coremark.s

# Count lea+lw pairs (approximate)
grep -c '^\s*lw$' coremark.s

# Count total instructions (non-directive, non-label, non-comment lines)
grep -E '^\s+[a-z]' coremark.s | wc -l
```

To observe loop structure, compile a representative fragment with annotations:

```bash
echo 'long mat_mul(long a[][3], long b[][3], int n) {
    long s = 0; int i,j,k;
    for(i=0;i<n;i++) for(j=0;j<n;j++) for(k=0;k<n;k++)
        s += a[i][k] * b[k][j];
    return s;
}' > mat.c
./smallcc -ann -O1 -o mat.s mat.c
cat mat.s
```

The annotation comments (`; --- bbN ---` and source line echoes) make the instruction
overhead of each source construct visible.

### CPU4 Instruction Counts

```bash
# Total instruction count (excluding data directives)
grep -E '^\s+[a-z]' coremark4.s | sed 's/^\s*//' | awk '{print $1}' | grep -v '^byte$\|^word$\|^align$' | wc -l

# F2 bp-relative loads
grep -cE '^\s+lw\s'  coremark4.s   # word
grep -cE '^\s+ll\s'  coremark4.s   # long
grep -cE '^\s+lb\s'  coremark4.s   # byte

# F2 bp-relative stores
grep -cE '^\s+sw\s'  coremark4.s
grep -cE '^\s+sl\s'  coremark4.s

# F3b register-indirect loads
grep -cE '^\s+llw\s' coremark4.s
grep -cE '^\s+lll\s' coremark4.s
grep -cE '^\s+llb\s' coremark4.s

# F3b register-indirect stores
grep -cE '^\s+slw\s|^\s+sll\s' coremark4.s

# Pseudo-moves (or), immw, addli, lea
grep -cE '^\s+or\s'    coremark4.s
grep -cE '^\s+immw\s'  coremark4.s
grep -cE '^\s+addli\s' coremark4.s
grep -cE '^\s+lea\s'   coremark4.s

# Calls and branches
grep -cE '^\s+jl\s'  coremark4.s
grep -cE '^\s+jz\s|^\s+jnz\s' coremark4.s

# Stack adjust
grep -cE '^\s+adjw\s' coremark4.s

# Confirm: push/pop should be zero
grep -cE '^\s+push$|^\s+pop$' coremark4.s
```
