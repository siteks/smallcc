# CPU3 + smallcc Performance Analysis

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

**Current result: 0.044 CoreMark/MHz.**

To reproduce:

```bash
make smallcc
# compile coremark with smallcc, then:
./sim_c coremark/coremark.s
```

### Industry Baseline Comparison

| Processor / Runtime | CoreMark/MHz | Notes |
|---|---|---|
| ARM Cortex-M0 (ARMv6-M) | ~1.0 | In-order, 3-address load/store RISC |
| RISC-V RV32E (Ibex) | ~0.9 | Minimal 3-address RISC |
| Interpreted CPython | ~0.01 | Pure interpreter overhead |
| Interpreted Forth | ~0.1–0.3 | Stack machine, no JIT |
| Native Forth (compiled) | >1.0 | Direct-threaded, TOS-cached |
| JVM HotSpot (after JIT) | ~5–15 | Aggressive optimization, native code |
| **CPU3 + smallcc** | **0.044** | Stack machine, no TOS caching |

The gap to Cortex-M0 is approximately **25×**. Closing it entirely requires architectural
changes; substantial improvement is achievable with targeted ISA additions.

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

### Measured Encoding Density

From `coremark.s` (text section only):

- **8,319 instructions** in **14,746 bytes** = **1.77 bytes/instruction**

This is actually competitive with JVM bytecode (~1.2 bytes/instruction). The problem is
not encoding density — it is **instructions per operation**.

---

## 3. Instruction Mix Analysis

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
3 instructions on CPU3:

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

## 4. Critical Loop Analysis

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

---

## 5. ISA Optimization Opportunities

Each proposed instruction includes encoding, before/after comparison, and estimated savings.
Static counts are from `coremark.s`; dynamic multipliers are estimated from profiling
context (inner loop variables execute ~270× more often than static count suggests).

### 5a. `lw_local N` — Combined `lea + lw` (Format-1, 2 bytes)

Load a 2-byte local variable into r0 in one instruction.

**Encoding**: `opcode` (1 byte) + signed 8-bit bp offset (1 byte). Covers ±127 bytes,
sufficient for most function frames.

```
Before (4 bytes, 2 instr):   lea -N    [3 bytes]
                              lw        [1 byte]
After  (2 bytes, 1 instr):   lw_local -N
```

Similarly: `ll_local N` for 4-byte locals (`long`/`float`).

**Estimated static savings**:
- `lea+lw` occurrences: 641 → saves **641 instructions**
- `lea+ll` occurrences: 270 → saves **270 instructions**
- Total: **911 fewer instructions** (11.0% of 8,319)

**Estimated runtime savings**: 911 static instances × ~270 average dynamic multiplier ≈
**246,000 instructions** per CoreMark iteration removed.

### 5b. `push_local N` — Combined `push + lea + lw` (Format-1, 2 bytes)

Push a 2-byte local variable onto the expression stack in one instruction. This is the
operand-fetch pattern for the left-hand side of every binary operation.

**Encoding**: same as `lw_local` — opcode + signed 8-bit bp offset.

```
Before (7 bytes, 3 instr):   push      [1 byte]   (preceded by lea+lw)
                              lea -N    [3 bytes]
                              lw        [1 byte]
After  (2 bytes, 1 instr):   push_local -N
```

(The common ordering is `lea; lw; push` — `push_local` replaces all three.)

**Estimated static savings**: 416 occurrences → **832 fewer instructions** (two instructions
eliminated per occurrence, 10.0% of 8,319).

**Combined with 5a**: `lw_local` + `push_local` together save approximately **1,743
instructions static** (21.0%) and **~471,000 instructions** per CoreMark iteration.

### 5c. `inc_local N` / `dec_local N` — In-Place Local Increment (Format-1, 2 bytes)

Increment a 2-byte local variable in memory without using r0.

**Encoding**: opcode + signed 8-bit bp offset.

```
Before (8 instr, ~14 bytes):
    lea     -N
    lw
    push
    immw    0x0001
    add
    lea     -N
    push
    sw

After (2 bytes, 1 instr):   inc_local -N
```

**Estimated static savings**: ~47 occurrences of the `immw 1; add; [lea; push;] sw` tail
pattern → **329 fewer static instructions**. The dynamic savings are disproportionately
large since loop variables are incremented on every iteration: **potentially 100,000+
runtime instructions** per CoreMark iteration for the matrix loop's `k` and `i` variables.

### 5d. `sw_local N` — Store r0 to Local Variable (Format-1, 2 bytes)

Store the accumulator r0 into a 2-byte local variable, consuming the address from the
pattern `lea N; push; sw`.

**Encoding**: opcode + signed 8-bit bp offset.

```
Before (5 bytes, 3 instr):   lea -N    [3 bytes]
                              push      [1 byte]
                              sw        [1 byte]
After  (2 bytes, 1 instr):   sw_local -N
```

**Estimated static savings**: most assignment destinations in expression-heavy code; roughly
200–250 additional occurrences beyond what `inc_local` covers → **400–500 fewer
instructions** (4–5%).

Pairs with `push_local` to make `a = b + c` a 3-instruction sequence:
```asm
push_local -b_off   ; push b
lw_local   -c_off   ; r0 = c
add                 ; r0 = b + c
sw_local   -a_off   ; a = r0
```
vs. the current 10-instruction sequence.

### 5e. Compare-and-Branch: `beq_zero_local N, L` (Format-2 variant)

The dominant loop-exit pattern is: load a local, compare with a constant, branch. The
simplest useful variant tests a local against zero:

```
Before (6 instr):   lea -N; lw; jz/jnz L   (and the push+immw+eq that may precede)
After  (3 bytes):   bz_local -N, L          (opcode + 8-bit offset + 8-bit label)
```

A full 3-operand `beq_local N, K, L` (compare local with constant and branch) requires
a 5-byte or 6-byte encoding — still smaller than the current 15+ bytes.

**Implementation difficulty**: **High**. Requires a new instruction format, assembler
changes, and simulator changes. The assembler's `ptable` mechanism in `cpu3/cpu.py` must
be extended, and `sim_c.c` must handle the new opcode.

**Estimated savings**: 10–15% for state-machine and comparison-heavy code. Less benefit
for arithmetic-dominated kernels.

---

## 6. Two-Register (TOS2) Model

### 6a. The Fundamental Stack Machine Problem

In a pure 1-register stack machine, every binary operation requires the pattern:

```asm
; evaluate left operand → r0
push        ; spill left operand to memory
; evaluate right operand → r0
op          ; pop left, r0 = left op r0
```

The `push` + `op` pair exists purely to manage the expression stack — they do no useful
arithmetic. For `a + b` where both are already in registers on a RISC machine, `add r0,
r1, r2` is a single instruction. On CPU3 it is a minimum of 6 instructions (counting the
operand loads).

### 6b. TOS Register Caching (TOS1)

The classic optimization for stack machines: keep the **top of the expression stack** in
a register rather than in memory. The stack pointer only moves when a second value must be
saved.

This is universal in practical Forth implementations. Chuck Moore's cmForth and Brad
Rodriguez's "Moving Forth" series document TOS caching as eliminating 30–40% of stack
traffic in typical Forth code. For JVM interpreters, Ertl and Gregg ("The Structure and
Performance of Efficient Interpreters", 1997) quantify TOS caching at 15–30% speedup.

For CPU3, TOS1 would keep r0 perpetually valid as the top of the expression stack,
avoiding the `push`/`pop` round-trips for values that are consumed immediately. The `push`
instruction would become a "spill" that only fires when a second expression value is
needed concurrently.

### 6c. TOS2: Two-Register Model

**TOS2** keeps both r0 and a second register (call it r1) as the top two expression stack
slots. This directly maps to the operands of a binary operation:

```
TOS2 state before `add`:   r1 = left operand, r0 = right operand
After `add`:               r0 = r1 + r0, r1 = undefined
```

No memory access is needed at all for the common `a OP b` pattern when both operands are
register-resident.

JVM research quantifies the gain: the "two-slot buffer" model reduces stack memory traffic
by ~50% compared to no buffering for purely arithmetic bytecode sequences (referenced in
analyses using the Soot framework and in Alpern et al., "Efficient Implementation of Java
Interfaces", 2001).

For CPU3's matrix multiply inner loop, the multiply-accumulate `sum += a[i][k] * b[k][j]`
currently costs:
```asm
push        ; save a[i][k] (fetched with 14 instr above)
; fetch b[k][j] (another 14 instr)
mul         ; a[i][k] * b[k][j]
push        ; save product
lea  -sum   ; &sum
ll           ; load sum (4 bytes)
push
fadd         ; sum + product  (or ladd for integer)
lea  -sum
push
sl           ; store sum
```
With TOS2, the `push`/`pop` between the multiply and accumulate disappears.

### 6d. TOS2 Implementation Difficulty

TOS2 is not a simple optimization — it requires redesigning multiple layers:

1. **ABI change**: r1 must be declared caller-saved or callee-saved. Every function entry
   and exit must account for r1's state. The current ABI has no concept of r1.

2. **State machine in codegen**: the code generator must track whether "the expression TOS
   is in r1 (live)" or "the expression TOS is on the memory stack." This is a simple
   register allocator for r1 — still a significant engineering investment.

3. **Spill code at calls**: when r1 is live and a function call is made, r1 must be pushed
   before the call and restored after. For call-heavy code (like list operations), this
   partially defeats the purpose.

4. **New shuffle instructions**: the current `push`/`pop` operate between r0 and memory.
   A TOS2 architecture additionally needs:
   - `dup`: copy r0 → r1 (or push r0 to r1, shifting r1 to memory stack)
   - `swap`: exchange r0 and r1
   - `drop`: r0 = r1 (discard r0)
   This is the Forth "parameter stack" model.

5. **ISA growth**: 4–8 new instructions, simulator and assembler changes.

**Estimated gain**: 20–30% instruction count reduction for arithmetic-heavy kernels (matrix
multiply). Pointer-chasing code (`list_find`) is memory-latency bound and benefits less —
perhaps 10%.

**Recommendation**: TOS2 is the single highest-leverage architectural change available, but
requires a coordinated redesign of the ABI, ISA, and compiler backend. It is a v2.0
project, not an incremental improvement.

---

## 7. Compiler Optimizations (No ISA Changes Required)

### 7a. Switch Selector Caching

The most impactful purely-compiler fix. `gen_switchstmt` currently re-evaluates `ch[0]`
(the selector expression) for every case comparison. For a local variable selector:

```c
switch (state) { /* 7 cases */ }
```

The current code emits `lea; lw` (2 instructions) per case = 14 wasted instructions per
dispatch, plus 7 `push` instructions = **21 redundant instructions** per `switch`
evaluation.

Fix: evaluate the selector once, store in a compiler-allocated temporary local, then
compare against the temporary:

```c
// In gen_switchstmt:
int tmp_offset = alloc_temp_local(WORD_SIZE);  // adj -2, never freed until function exit
gen_expr(selector_node);
emit(IR_LEA, -tmp_offset);
emit(IR_PUSH, 0);
emit(IR_SW, 0);
// Then each case: lea tmp; lw; push; immw K; eq; jnz
```

**Estimated savings**: 21 instructions per dispatch × dispatch frequency ≈ **10–15%**
total reduction for the state-machine workload. This is a pure compiler change — no ISA
modification needed.

### 7b. Extended Store/Reload Peephole (Rule 9 Extensions)

The current Rule 9 handles `LEA N; PUSH; …expr…; SW` followed by `LEA N; LW` within
the same basic block. Extensions that would fire frequently:

- **Global variable reload**: `IMMW sym; PUSH; …; SW; IMMW sym; LW` — eliminate the
  reload after storing to a global.
- **Narrow pattern**: `LEA N; PUSH; SW; LEA N; LW` — the specific `a = expr; use(a)`
  pattern where the expression depth between store and reload is 0.
- **Through pointer arithmetic**: extend the depth tracker to look through
  `PUSH; IMMW; ADD` sequences (constant-offset pointer arithmetic).

**Estimated savings**: modest in isolation (~3–5%), but these rules compose with other
optimizations and are safe (no semantic change risk within a basic block).

### 7c. Peephole: Duplicate Local Load Elimination

When the same local is loaded twice consecutively (e.g. `a op a` or the second `lea -N;
lw` in a peephole window):

```asm
lea     -N
lw          ; first load of local N → r0
push
lea     -N  ; second load of local N
lw
```

Can be replaced with:

```asm
lea     -N
lw          ; load local N → r0
push        ; save r0
            ; (r0 still holds local N — no reload needed)
```

This fires when the same `LEA N; LW` sequence appears twice with only a `push` between
them, which is the `a == a` or `a + a` pattern.

### 7d. Constant Propagation Through Local Variables

Within a single basic block, if a local is assigned a compile-time constant and not
subsequently modified before a use, the load can be replaced with the immediate:

```asm
; Before: int n = 5; ... use n ...
lea     -N
push
immw    5
sw               ; n = 5
; ... later ...
lea     -N
lw               ; loads n (value is 5, but codegen doesn't know)

; After (with propagation):
lea     -N
push
immw    5
sw               ; n = 5
; ... later ...
immw    5        ; substitute the known constant
```

Particularly useful for loop bounds that are compile-time constants, allowing Rule 1
(constant folding) to fire on the loop termination comparison.

**Implementation**: a simple single-pass scan within each basic block, maintaining a
`local_offset → known_constant` map. Clear an entry on any `SW` or `SL` to the same
offset, or on any `JL`/`JLI` call (calls may modify memory unpredictably).

---

## 8. Encoding Density Comparison

| Architecture | Bytes/Instr | Instr per binary op | Instr per local load | Notes |
|---|---|---|---|---|
| CPU3 (current) | 1.77 | 4 min (push+load+load+op) | 2 (lea+lw) | 1-register stack |
| CPU3 with new instr | ~1.5 est | 2–3 est | 1 (lw_local) | With §5 additions |
| CPU3 with TOS2 | ~1.5 est | 1–2 | 1 | v2.0 redesign |
| ARM Thumb-2 | ~1.6 | 1 (3-addr) | 1 (LDR r0,[r1,#N]) | 16/32-bit mixed |
| RISC-V RV32I | 4.0 | 1 (3-addr) | 1 (LW r0,N(r1)) | 32-bit fixed |
| x86-64 | ~3.5 | 1 (mem operand) | 1 (MOV eax,[rbp-N]) | CISC with mem operands |
| JVM bytecode | ~1.2 | 3 (iload+iload+iadd) | 1 (iload_N) | Purpose-built stack ops |

CPU3's encoding density (1.77 bytes/instruction) is reasonable — similar to Thumb-2. The
performance gap is entirely explained by **instructions per operation**, not bytes per
instruction. The dedicated load-from-local JVM instruction (`iload_N`) shows exactly the
same approach proposed in §5a.

---

## 9. What a Real 1.0 CoreMark/MHz Processor Does

A Cortex-M0 (ARMv6-M, in-order, 1 cycle per instruction for most ops) achieves ~1.0
CoreMark/MHz. Key capabilities that explain the gap:

| Operation | Cortex-M0 | CPU3 (current) | Ratio |
|---|---|---|---|
| Load local variable | `LDR r0, [fp, #-N]` = 1 instr | `lea; lw` = 2 instr | 2× |
| Push local for binop | included in operand of `ADDS` | `push; lea; lw` = 3 instr | 3× |
| Increment local `i++` | `ADDS r2, r2, #1` = 1 instr | `lea; lw; push; immw 1; add; lea; push; sw` = 8 instr | 8× |
| Array access `a[i]` | `LDR r0, [r1, r2, LSL #1]` = 1 instr | `lea; push; lea; lw; push; immw; mul; add; lw` ≈ 9 instr | 9× |
| Binary op `a + b` (locals) | `LDR r0,[fp,#a]; ADDS r0,r0,[fp,#b]` = 2 instr | `lea; lw; push; lea; lw; add` = 6 instr | 3× |

The Cortex-M0 uses a **3-address load/store architecture**: operands are named registers,
computation uses register operands, and a dedicated load instruction fills those registers
from memory with an offset from a base register. This is the fundamental architectural
advantage.

A pure 1-register stack machine **cannot** match 3-address RISC in instructions-per-
operation without either:
1. Adding dedicated "load-and-push" instructions that collapse multiple operations (§5),
2. Caching the expression TOS in a second register (TOS2, §6), or
3. Both.

---

## 10. Prioritized Recommendations

### Implementation Roadmap

| Priority | Change | Effort | Estimated Instr Reduction | CoreMark/MHz Est. |
|---|---|---|---|---|
| 1 | `lw_local N` + `ll_local N` (§5a) | Low | ~11% | ~0.049 |
| 2 | `push_local N` (§5b) | Low | +10% cumulative | ~0.055 |
| 3 | Switch selector caching in codegen (§7a) | Low | +5–10% | ~0.058–0.061 |
| 4 | `inc_local N` / `dec_local N` (§5c) | Low | +5% static, large runtime gain | ~0.065 |
| 5 | `sw_local N` (§5d) | Low | +3–5% | ~0.068 |
| 6 | Extended store/reload peephole (§7b–d) | Medium | +5% | ~0.071 |
| 7 | `bz_local N, L` or `beq_local N, K, L` (§5e) | High | +10–15% | ~0.080 |
| 8 | TOS2 two-register model (§6) | Very High | +20–30% cumulative | ~0.100–0.110 |

### Effort Definitions

**Low**: new opcode(s) in `sim_c.c` and `cpu3/cpu.py`; new IR kind(s) in `codegen.c`;
new emission case(s) in `backend.c`. No architecture changes. 1–2 days per instruction.

**Medium**: peephole rules in `optimise.c`; may require new IR scan logic. 2–5 days.

**High**: new instruction format requiring assembler changes in `cpu3/assembler.py` and
`sim_c.c`; new encoding path in `backend.c`. 1–2 weeks.

**Very High**: ABI redesign, ISA redesign, codegen state machine rewrite. Multi-week
project with high regression risk.

### Fundamental Ceiling

Items 1–7 together: plausibly **40% instruction count reduction** → ~0.073 CoreMark/MHz.

Items 1–8 (with TOS2): plausibly **60% instruction count reduction** → ~0.11 CoreMark/MHz.

**Reaching 1.0 CoreMark/MHz is not achievable with the 1-register stack machine model.**
It requires matching the instructions-per-operation density of 3-address RISC, which
fundamentally needs either TOS2 + the §5 instructions (getting to ~0.1), or replacing
the accumulator model with a general-purpose register file (a CPU4 design).

The practical recommendation is:

1. **Implement §5a–5d and §7a immediately** (all Low effort, ~25–30% gain, estimated
   score ~0.058–0.065). These changes are independent, low-risk, and composable.
2. **Implement §7b–7d** (Medium effort, additional ~5%) to clean up the peephole.
3. **Evaluate TOS2** as a separate architectural initiative if score > 0.07 is required.

---

## Appendix: Verification

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
