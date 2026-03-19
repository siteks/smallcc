# RISC Backend: Retargeting the Stack IR

## 1. The Question

The existing smallcc IR is shaped around the CPU3 stack machine. Can it be retargeted
to a 2- or 3-address RISC ISA — without redesigning the IR — and if so, does the
resulting code approach RISC instruction densities?

The short answer: yes, with a backend that decodes the expression stack into physical
registers. The gain is substantial (3–4×), but reaching 1.0 CoreMark/MHz also requires
ISA features (load-with-offset, pipeline) that go beyond the backend alone.

---

## 2. The Coupling Problem

The IR is not an abstract value graph. It encodes CPU3 stack-machine semantics directly:

```
IR_PUSH          → push          (sp -= 4; mem[sp] = r0)
IR_ADD           → add           (r0 = mem[sp] + r0; sp += 4)
IR_LEA -N        → lea -N        (r0 = bp - N)
IR_LW            → lw            (r0 = mem16[r0])
```

`IR_ADD` does not mean "add two values". It means "pop the stack top and add it to r0".
The expression stack in memory IS the temporary-value store; there is no register
allocation.

A naïve 1:1 IR→RISC translation is impossible: RISC ISAs have no `push`/`pop` stack ops
for expression temporaries. A RISC backend must decode the stack protocol and convert
it back into register operations — which is equivalent to performing register allocation
in the backend.

---

## 3. Two Approaches to a RISC Backend

**Approach A — Backend register allocation (current IR unchanged)**

The backend maintains a "virtual register file" that mirrors the expression stack.
When it sees `IR_PUSH`, instead of emitting `push`, it notes that r0's value should
be held in the next scratch register. When it sees `IR_ADD`, it knows scratch[d] holds
the left operand and r0 holds the right → emit `ADD r0, scratch[d], r0`.

This keeps all existing compiler phases intact. Only `backend.c` changes.

**Approach B — Register-based IR layer**

Add a new 3-address IR between `gen_ir()` and the backend. The existing `gen_ir()` is
rewritten to emit register IR directly (`LOAD rd, [bp, -N]`; `ADD rd, rs1, rs2`; etc.).
The backend becomes a trivial 1:1 translation. A linear-scan register allocator maps
virtual registers to physical registers.

This is how LLVM, GCC, and Cranelift work. It produces better code but requires
rewriting the most complex component of the compiler.

---

## 4. Approach A: Backend Stack → Register Decoding

The backend walks the IR list and maintains a small register file for the expression
stack. When the virtual stack depth is D, `scratch[D-1]` holds the pushed value and r0
holds the current accumulator.

| IR sequence | CPU3 emitted | RISC emitted | Registers |
|---|---|---|---|
| `IR_LEA -N; IR_LW` | `lea -N; lw` | `LDR r0, [bp, -N]` | r0 |
| `IR_PUSH` | `push` | `MOV r1, r0` (virtual push to scratch[0]) | r0, r1 |
| `IR_ADD` | `add` | `ADD r0, r1, r0` | r0 |
| `IR_LEA -N; IR_LW; IR_PUSH; IR_LEA -M; IR_LW; IR_ADD` | 6 instrs | `LDR r1, [bp,-N]; LDR r0, [bp,-M]; ADD r0, r1, r0` | r0, r1 |
| `IR_LEA -N; IR_PUSH; IR_IMM offset; IR_ADD; IR_LW` | 5 instrs | `LDR r0, [r_local, #offset]` (struct field) | r0 |

When the virtual stack exceeds available scratch registers, the backend spills to the
real memory stack — the same as today, but only for expressions deeper than 3–4 levels.
In practice, Sethi-Ullman reordering (already active) keeps expression stack depth ≤ 3
for all typical C idioms.

### Key expression idioms

**`a + b` (two locals)**

```
CPU3 (6 instructions):          RISC Approach A (3 instructions):
  lea   -2                        LDR r1, [bp, -2]
  lw                              LDR r0, [bp, -4]
  push                            ADD r0, r1, r0
  lea   -4
  lw
  add
```

**`k++` (loop counter)**

```
CPU3 (8 instructions):          RISC Approach A (2 instructions):
  lea   -2                        LDR r0, [bp, -2]
  push                            ADD r0, r0, #1    (then STR back)
  lea   -2
  lw
  push
  immw  1
  add
  sw
```

**`p->field` (struct field via pointer)**

```
CPU3 (5 instructions):          RISC Approach A (2 instructions):
  lea   -2                        LDR r1, [bp, -2]
  lw                              LDR r0, [r1, #offset]
  push
  immw  offset
  add
  lw
```
This requires load-with-offset (`LDR rd, [rs, #N]`) — see §7.

**`a[i]` (array subscript, int array)**

```
CPU3 (9 instructions):          RISC Approach A (4 instructions):
  lea   -10                       LDR r2, [bp, -10]    ; base
  push                            LDR r0, [bp, -2]     ; index
  immw  2                         LSL r0, r0, #1       ; stride=2: shift
  push                            LDR r0, [r2, r0]     ; load [base + offset]
  lea   -2
  lw
  mul
  add
  lw
```
With `-O2`, the `immw 2; mul` → `immw 1; shl` (Rule 7) and constant indices fold
completely (Rule 1). The RISC backend benefits from the same IR reductions before
decoding the stack.

---

## 5. What Peephole Can Recover on the IR Side

The existing peephole optimiser runs before the backend and reduces stack depth:

- **Rule 9** (store/reload elimination): when the backend sees `IR_SW` followed by
  `IR_LEA N; IR_LW` from the same address, it discards the reload — a primitive form
  of register reuse.
- **Rules 1, 7** (constant folding, strength reduction): collapse constant subexpressions,
  reducing the number of IR nodes the backend must process.
- **Rule 4/5** (adj merging): collapse `adj -N; adj +N` → nothing, eliminating dead
  scope-allocation pairs.

New peephole rules could collapse `IR_PUSH; IR_binary_op` pairs into a two-operand
tag, hinting to the backend that a specific register assignment is needed. But IR-level
peephole cannot assign register names — it can only simplify depth. The register
assignment step requires the backend.

**Conclusion**: IR peephole reduces instruction count before the backend, but the
register-allocation step that achieves RISC density must happen in the backend (Approach A)
or in a redesigned IR layer (Approach B).

---

## 6. What RISC ISA Is Needed

Minimum instruction classes for efficient retargeting:

| Class | Example | Replaces |
|---|---|---|
| Load-with-offset | `LDR rd, [rs, #N]` | `lea N; lw` |
| Store-with-offset | `STR rs, [rd, #N]` | `lea N; push; sw` |
| 3-address ALU | `ADD rd, rs1, rs2` | `push; add` |
| Register-immediate ALU | `ADD rd, rs, #N` | `push; immw N; add` |
| Shift-immediate | `LSL rd, rs, #N` | `push; immw N; shl` |
| Register-indirect branch | (for indirect calls) | `jli` |

A minimal physical register file for this use case:

| Register | Role |
|---|---|
| r0 | Return value / accumulator |
| r1–r3 | Expression scratch (caller-saved) |
| r4–r7 | Loop variables, saved values (callee-saved) |
| bp | Frame pointer |
| sp | Stack pointer |

With 3 scratch registers (r1–r3), expression stack depths up to 3 are handled entirely
in registers. Sethi-Ullman guarantees this covers essentially all C expressions in
practice.

---

## 7. Load-with-Offset Is the Critical Instruction

The single most impactful RISC feature is `LDR rd, [rs, #offset]`. It collapses the
two most frequent IR patterns:

**Local variable load** (`lea -N; lw` → `LDR r0, [bp, -N]`): 2 instructions → 1.

**Struct field access** (`ptr; push; immw offset; add; lw` → `LDR r0, [r1, #offset]`):
5 instructions → 1.

At the instruction frequencies observed in the CoreMark trace:

| Pattern | Count | Saving per match | Total saving |
|---|---|---|---|
| `lea + lw` (local/param load) | ~641 | 1 instruction | ~641 |
| `push + lea + lw` (pushed address + load) | ~416 | 2 instructions | ~832 |
| `push + immw + add + lw` (struct field) | ~284 | 3 instructions | ~852 |
| **Total** | | | **~2,325 instructions** |

This is a ~30% reduction in total instruction count from a single ISA feature, before
any other RISC improvements are applied.

Store-with-offset (`STR rs, [rd, #N]`) gives a comparable saving on the write side
(`lea; push; sw` → 1 instruction).

---

## 8. Approach B: Register IR Layer

For reference, adding a proper 3-address IR would look like:

```c
// New IR opcodes (additions to IROp)
IR_REG_LOAD,    // rd = mem[rs + offset]   (load-with-offset)
IR_REG_STORE,   // mem[rd + offset] = rs
IR_REG_ADD,     // rd = rs1 + rs2
IR_REG_ADDI,    // rd = rs + imm
IR_REG_MOV,     // rd = rs
```

`gen_ir()` would emit these instead of stack ops. The backend becomes a trivial 1:1
translation:

```
IR_REG_LOAD  r0, bp, -4   →   LDR r0, [bp, -4]
IR_REG_ADD   r0, r1, r0   →   ADD r0, r1, r0
```

A linear-scan register allocator maps virtual registers to physical registers. Virtual
register pressure is bounded by expression depth (≤3 for typical C), so spill is rare.

**Tradeoff vs Approach A**:
- Approach A reuses existing `gen_ir()` and `codegen.c` unchanged; only `backend.c`
  grows a stack-decoding phase.
- Approach B requires rewriting `gen_ir()` — the most complex component — but achieves
  better code quality (closer to Approach B column in §9 table) and is the correct
  long-term path.
- Both approaches leave the front-end (tokeniser, parser, type system) and the peephole
  optimiser completely unchanged.

---

## 9. Expected Instruction Counts

| Kernel | CPU3 now | RISC (Approach A) | RISC (Approach B) |
|---|---|---|---|
| `a + b` (locals) | 6 | 3 | 2 |
| `k++` (loop counter) | 8 | 2 | 2 |
| `p->field` (struct access) | 5 | 2 | 1 |
| `a[i]` (array, int) | 9 | 4 | 2 |
| `switch` (7 cases, per dispatch) | 28 | 10 | 7 |
| Matrix inner loop body | ~83 | ~25 | ~15 |

Approach A's instruction count for the matrix inner loop can be estimated by applying
the load-with-offset saving (~2,325 total) proportionally. The matrix kernel accounts
for roughly 30% of the CoreMark trace, giving ~700 saved instructions in the matrix
path, or ~30 instructions per inner-loop iteration (83 → ~53). With 3-address ALU
eliminating the remaining push/add pairs, the realistic estimate is ~25.

Approach B's ~15 reflects a near-optimal register schedule with no memory-stack
temporaries for expressions of depth ≤ 3.

---

## 10. CoreMark/MHz Projection

| Configuration | Est. CoreMark/MHz | vs. baseline |
|---|---|---|
| Current (CPU3, stack IR) | 0.044 | 1× |
| RISC Approach A + load-with-offset | 0.13–0.18 | 3–4× |
| RISC Approach B (register IR) | 0.22–0.31 | 5–7× |
| True 1.0 target | 1.0 | 23× |

**The residual gap to 1.0**: the `list_find` benchmark (linked-list pointer chasing) is
memory-access bound, not instruction-count bound. Each node dereference requires a
load, and loads on an in-order machine with no cache stall for an effective IPC well
below 1.0 regardless of instruction count. True 1.0 CoreMark/MHz requires both the
RISC backend AND ILP (out-of-order execution or a data cache).

The projection assumes:
- Load-with-offset eliminates the `lea+lw` pairs (~2,325 instructions saved in the
  CoreMark trace)
- 3-address ALU eliminates `push + binary_op` pairs for depths ≤ 3
- No pipeline improvement (IPC ≈ 1, memory latency = 1 cycle)

---

## 11. What Doesn't Change

Regardless of which backend approach is taken:

- **Front-end** (tokeniser, parser, type system, `insert_coercions`, `label_su`): fully
  backend-neutral, unchanged.
- **IR structure** (`IRInst`, `IROp`): the existing opcodes remain valid; Approach A adds
  no new opcodes; Approach B adds register-addressed opcodes alongside the stack ops.
- **Peephole optimiser**: its rules continue to run before the backend. Rules 1–9
  still apply and reduce the IR before the backend's stack-decoding phase sees it.
- **Switch-selector caching**: any fix that avoids re-evaluating the switch selector
  per case saves instructions in the IR, and those savings carry through to both RISC
  approaches unchanged.
- **Multi-TU compilation**, **static symbol mangling**, **cross-TU extern resolution**:
  all independent of the backend.

---

## 12. Recommendation

**Approach A** is the pragmatic path for the near term:

1. Add `LDR rd, [rs, #N]` / `STR rs, [rd, #N]` (load/store-with-offset) to the target
   ISA. This is the single highest-ROI change (~2,325 saved instructions per CoreMark run).

2. Implement a backend stack-decoder in `backend.c`: maintain a 3-element scratch register
   array; on `IR_PUSH` assign r0 to `scratch[depth++]`; on `IR_ADD` etc. emit
   `ADD r0, scratch[--depth], r0`; on `IR_LEA + IR_LW` emit `LDR r0, [bp, offset]`.

3. Run the existing peephole before the backend pass (already the case). Add a Rule 10
   that folds `IR_LEA N; IR_LW` and `IR_LEA N; IR_PUSH` into tagged 2-operand forms to
   help the backend's pattern matching.

Estimated effort: ~300 lines in `backend.c`, ~50 lines in `sim_c.c`/`cpu.py` for the
new ISA instructions. No changes to `codegen.c`, `parser.c`, or `types.c`.

**Approach B** (register IR) is the correct long-term path to match true RISC
performance. It requires rewriting `gen_ir()` and adding a lightweight register
allocator, but achieves near-optimal code and removes the fundamental impedance mismatch
between the stack-shaped IR and register-based targets. When the compiler matures to
the point where instruction quality is the primary concern, Approach B is the right
investment.
