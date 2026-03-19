# CPU4 Optimisation Opportunities

This document catalogues identified optimisation opportunities for the CPU4 backend, grouped by
where in the compilation pipeline the fix lives. It is a reference for future implementation work
and does not describe any currently-implemented optimisations beyond what is noted in §1.

---

## 1. Current Pipeline and Existing Optimisations

### Pipeline diagram

```
C source
   │
   ▼
preprocess()          [preprocess.c]
tokenise()            [tokeniser.c]
program()             [parser.c]
resolve_symbols()     [parser.c]
derive_types()        [parser.c]
insert_coercions()    [parser.c]
label_su()            [parser.c]      ← Sethi-Ullman reordering
finalize_local_offsets() [types.c]
   │
   ▼  Stack IR (IRInst list)
gen_ir()              [codegen.c]
mark_basic_blocks()   [codegen.c]
peephole(opt_level)   [optimise.c]    ← O1/O2 stack-IR rules (CPU3 + partial CPU4)
   │
   ▼  (CPU4 only from here)
lift_to_ssa()         [ssa.c]         ← flatten stack IR to SSA-like virtual-register form
ssa_peephole()        [ssa_opt.c]     ← identity-move elimination only
regalloc()            [regalloc.c]    ← linear-scan register allocation
risc_backend_emit()   [risc_backend.c]← virtual registers → CPU4 assembly
```

### What already exists

**`optimise.c` — stack IR peephole (rules 1–9)**

All rules fire only within a single basic block (hard barrier at `IR_BB_START`).

| Rule | Pattern | Replacement | Arch guard |
|------|---------|-------------|------------|
| 1 | `IMM K1; PUSH; IMM K2; binop` | `IMM (K1 op K2)` | none |
| 2 | `IMM K; SXB/SXW` | `IMM sext(K)` | none |
| 3 | `J/JZ/JNZ Lx` then `LABEL Lx` | kill branch | none |
| 4 | `ADJ N; ADJ M` | `ADJ N+M` (or delete if 0) | none |
| 5 | `ADJ 0` | delete | none |
| 6 | `PUSH; IMM 1; MUL` | delete (identity) | none (O2) |
| 7 | `PUSH; IMM 2^k; MUL` | `PUSH; IMM k; SHL` | none (O2) |
| 8 | `PUSH; IMM 0; ADD/SUB` | delete (identity) | none (O2) |
| 9 | `LEA N; PUSH; <expr>; SW/SB/SL` + `LEA N; LW/LB/LL` | kill reload | **CPU3 only** |

**`ssa_opt.c` — SSA-level peephole**

- Identity-move elimination: `SSA_MOV rd=v, rs1=v` (destination equals source) is deleted.

**`risc_backend.c` — inline backend micro-optimisations**

- `adjw 0` is skipped (zero stack adjustment elided at emit time).
- Identity `MOV rx, rx` (same source and destination after regalloc) is skipped.

---

## 2. Missed Opportunities — Grouped by Fix Location

### A. Stack-IR peephole (`optimise.c`) — easy wins

#### A1. Store/reload elimination (Rule 9) is gated on CPU3 only

**Pattern:**
```
LEA N                ; address of local variable
PUSH
<expression>         ; some IR for the RHS
SW / SB / SL         ; store r0 to mem[stack[sp]]
; ... immediately after (no intervening call or BB boundary) ...
LEA N                ; same address
LW / LB / LL         ; reload
```

**What happens:** Rule 9 already implements this elimination — it scans forward from the `LEA`,
tracks expression-stack depth, and when it reaches the store checks whether the next two
instructions reload from the same address. If so, it kills the reload (r0 still holds the
stored value).

**Why it doesn't fire for CPU4:** The rule body is guarded:
```c
if (g_target_arch == 3) { /* rule 9 */ }
```

**Fix:** Remove or generalise the arch guard so rule 9 fires for both CPU3 and CPU4 stack IR.
The IR operand shapes are identical for both targets at this stage of the pipeline; the guard is
an over-conservative holdover.

**Payoff:** One `LEA + LW` eliminated per compound-assign or store-then-read pattern. Common in
loops (`i += 1`, `a[i] = x; use(a[i])`).

---

### B. SSA-level peephole (`ssa_opt.c`) — medium wins

#### B1. SSA_ADJ merging

**Pattern:** After `lift_to_ssa`, scope entry/exit and `flush_for_call_n` can emit adjacent
`SSA_ADJ` nodes:
```
SSA_ADJ  -4
SSA_ADJ  -2
```
These are never re-merged by the existing stack-IR Rule 4 (which runs before `lift_to_ssa`).

**Fix:** Add a single-pass loop in `ssa_opt.c` (before or after the identity-move pass) that
merges consecutive `SSA_ADJ` nodes:
```
if cur.op == SSA_ADJ && next.op == SSA_ADJ:
    cur.imm += next.imm
    delete next
    if cur.imm == 0: delete cur
```

**Payoff:** Low — reduces instruction count only when scope management happens to produce adjacent
adjustments. Zero semantic risk.

#### B2. Compare-branch fusion

**Pattern:** Every `if`/`while`/`for` condition generates:
```
SSA_ALU  rd=v1, op=LTS/LT/EQ/NE/…, rs1=v2, rs2=v3   ; compare
SSA_JZ   target                                         ; branch if false
```
or `SSA_JNZ` for the inverted sense.

**CPU4 ISA support:** F3b branch instructions (`blt`, `bgt`, `blts`, `bgts`, `beq`, `bne`) take
two register operands and a label target in 3 bytes. They subsume the compare + branch pair.

**Fix:** In `ssa_opt.c`, scan for `SSA_ALU(compare op) + SSA_JZ/JNZ` pairs where the compare
result (`v1`) is only consumed by the immediately following branch and has no other uses. Fuse
the pair into a new `SSA_BRANCH` node carrying `(op, rs1, rs2, target, sense)`. In
`risc_backend.c`, emit the appropriate F3b instruction.

The `le`/`ge` cases have no direct F3b opcode but map to `bgt`/`blt` with swapped operands
(equivalent to the assembler pseudo-ops already defined for CPU4).

**Payoff:** One F1a compare instruction (2 bytes) eliminated per conditional branch. High-value
in tight loops with comparisons.

#### B3. LEA→STORE forwarding (F3b store → F2 store downgrade)

**Pattern:**
```
SSA_LEA  rd=v1, imm=N          ; r_v1 = bp + N
SSA_STORE rd=v1, rs1=v2, imm=0 ; mem[r_v1 + 0] = r_v2  →  F3b slw (3 bytes)
```

**Observation:** The address is `bp + N`. If `N` is in the F2 store immediate range for the
relevant size (word: N/2 fits in 6 bits, i.e. `|N| ≤ 126`; byte: `|N| ≤ 63`; long: N/4 fits,
i.e. `|N| ≤ 252`), the pair can be replaced by a single F2 store:
```
SSA_STORE rd=-2, rs1=v2, imm=N  ; mem[bp + N] = r_v2  →  F2 sw (2 bytes)
```
where `rd == -2` is the existing sentinel in `ssa.c` for bp-relative addressing. The `SSA_LEA`
is eliminated when its only use was this store.

**Fix:** In `ssa_opt.c`, after the identity-move pass, scan for `SSA_LEA + SSA_STORE(rd=LEA_result)`
pairs. When `N` is in F2 range and `v1` has exactly one use (the store), rewrite the store to
`rd=-2, imm=N` and delete the LEA.

**Payoff:** High. Every assignment to a local variable currently costs `lea (3B) + slw (3B) = 6B`.
After this fix: `sw (2B)`. 67% code-size reduction for the store half of every local assignment.
Combined with a matching load optimisation (see C2), local variable accesses shrink from
`lea+lw` and `lea+sw` pairs to single F2 instructions.

#### B4. `le`/`ge` expansion reduction (via branch fusion)

**Observation:** The `le` (less-than-or-equal) comparison currently expands to three SSA
instructions:
```
SSA_ALU  rd=v1, op=GT,   rs1=a, rs2=b   ; a > b
SSA_MOVI rd=v2, imm=0
SSA_ALU  rd=v3, op=EQ,   rs1=v1, rs2=v2 ; !(a > b)
```
(7 bytes of emitted code). When immediately followed by `SSA_JZ target`, the fusion from B2
would replace the entire sequence with a single `bgt rs_b, rs_a, target` (3 bytes, operands
swapped), a 7× reduction for this pattern.

**Fix:** Implement B2. The `le`/`ge` → `bgt`/`blt` mapping is handled by the assembler's
pseudo-op table and requires no special-casing in the compiler beyond the fusion itself.

---

### C. Backend (`risc_backend.c`) — targeted fixes

#### C1. F2 stores currently unreachable for normal assignments

**Observation:** `SSA_STORE` emission in `risc_backend.c` has two paths:
- `rd == -2`: emits F2 `sw`/`sb`/`sl` (2 bytes, bp-relative).
- `rd >= 0`: emits F3b `slw`/`slb`/`sll` (3 bytes, register-indirect).

For normal local-variable assignments, `ssa.c` always sets `rd = vs_reg[depth]` (a virtual
register number ≥ 0) because it generates `SSA_LEA` first and passes the result register to
`SSA_STORE`. The F2 path is only reached for the spill/argument stores emitted by
`flush_for_call_n`, which explicitly use `rd = -2`.

**Fix:** This is the downstream effect of B3. Implementing the LEA→STORE forwarding pass in
`ssa_opt.c` will rewrite normal-assignment stores to `rd=-2`, causing the existing F2 emit path
to fire without any changes to `risc_backend.c`.

#### C2. Load into non-r0 (fuse `SSA_LOAD + SSA_MOV`)

**Pattern (after regalloc):**
```
SSA_LOAD rd=v8,  rs1=-2, imm=N    ; lw r0, N/2   (F2, loads into r0)
SSA_MOV  rd=v9,  rs1=v8           ; or r1, r0, r0 (F1a, move r0→r1)
```
Two instructions (4 bytes) to load a value into a non-r0 register.

**CPU4 ISA support:** F2 load (`lw`, `lb`, `ll`) has an explicit 3-bit `rd` field; loading
directly into r1 (or any r0–r7) is valid.

**Fix:** Either:
- **In `ssa_opt.c`**: detect `SSA_LOAD(rd=A) + SSA_MOV(rd=B, rs1=A)` pairs where `A` has
  exactly one use (the MOV). Replace with `SSA_LOAD(rd=B, rs1=..., imm=...)` and delete the MOV.
- **In `risc_backend.c`**: peek at the next instruction. If it is an identity move from the load
  destination, fold the load destination to the move destination and skip the move.

The `ssa_opt.c` approach is cleaner and works before regalloc (virtual registers), but requires
tracking use counts. The `risc_backend.c` peek approach works post-regalloc and is trivial to
implement without use-count tracking.

**Payoff:** Medium. Eliminates one instruction per load-into-non-r0, which occurs when a loaded
value is used as the second operand of a binary op (rhs of the stack binary convention).

---

## 3. Bigger / Structural Opportunities

### D1. Long/float local alignment for F2 access

**Problem:** `finalize_local_offsets()` in `types.c` assigns bp-relative offsets to locals by
packing them sequentially without alignment padding. A `long` or `float` local (4 bytes) may
end up at an odd or 2-byte-aligned offset.

F2 `ll`/`sl` (long load/store) require the byte offset to be a multiple of 4. When an offset
is not 4-byte aligned, the backend falls back to the 2-instruction sequence:
```
SSA_LEA  rd=v1, imm=N        ; lea (3 bytes)
SSA_LOAD rd=v2, rs1=v1, imm=0 ; lll (3 bytes)  ← F3b, 6 bytes total
```
instead of:
```
SSA_LOAD rd=v2, rs1=-2, imm=N/4  ; ll  (2 bytes)  ← F2, requires N % 4 == 0
```

**Fix:** In `finalize_local_offsets()`, when advancing `local_offset` for a symbol whose type
has `align == 4` (i.e. `long`, `float`, `double`), round the current offset up to the next
multiple of 4 before assigning.

```c
// pseudocode change in finalize_local_offsets():
if (sym->type->align == 4)
    st->local_offset = do_align(st->local_offset, 4);
sym->offset = st->local_offset;
st->local_offset += sym->type->size;
```

**Payoff:** High for float/long-heavy code. Every `float` or `long` local access shrinks from
6 bytes to 2 bytes. No change to correctness for well-aligned locals (alignment rounding may
shift some following locals by 0–2 bytes, increasing frame size by at most 2 bytes per `long`/
`float` local — negligible).

### D2. Callee-saved registers r6/r7 (call-site spill reduction)

**Problem:** `flush_for_call_n` in `ssa.c` spills all live expression temporaries to memory
before every function call, because r0–r3 (and r4–r5 used for scratch) are treated as
caller-saved. A call inside a loop that has live loop-counter temporaries causes spill/reload
on every iteration.

**Opportunity:** Designate r6 and r7 as callee-saved. Functions that use them must save/restore
them in their prologue/epilogue (`push r6; push r7` / `pop r7; pop r6`). Callers can then leave
outer-loop temporaries in r6/r7 across calls without spilling.

**Required changes:**
1. `regalloc.c`: reserve r6/r7 for long-lived values (loop counters, frequently-read variables).
   The current linear-scan allocator would need to distinguish caller- vs callee-saved register
   classes.
2. `risc_backend.c`: emit save/restore in function prologues when r6 or r7 is used.
3. `flush_for_call_n` in `ssa.c`: skip spilling values already allocated to r6/r7.

**Payoff:** High for loops with function calls. The change is a calling-convention extension —
no ABI compatibility issue since this is a single-compiler single-target system.

**Effort:** Large. Requires coordinating regalloc, prologue/epilogue emission, and spill logic.

### D3. Direct F3b branch instructions for all comparisons

**Observation:** The CPU4 ISA's F3b comparison-branch instructions (`blt rs1, rs2, target`,
`bgt`, `blts`, `bgts`, `beq`, `bne`) subsume the `compare + branch` pair entirely. They are
defined in the ISA but not currently emitted by the backend for user-code conditions.

**Relationship to B2:** Implementing compare-branch fusion (B2) is the prerequisite. Once
`SSA_BRANCH` nodes exist, `risc_backend.c` can emit F3b branches directly. The `le`/`ge`/`lts`/
`gts` cases map via operand-swap pseudo-ops already in the assembler.

**Payoff:** Combined with B2, every conditional in the program shrinks by one instruction.

### D4. Add-immediate instructions are unused

**Problem:** The backend always emits `immw r0, K; add rd, rs, r0` (F2 + F1a = 5 bytes,
2 instructions) for any addition of a compile-time constant K.

**ISA support** (all now implemented in `cpu4/cpu.py` and `sim_c.c`):
- **F1b `inc rx`** / **`dec rx`** (2 bytes, subopcodes 0x02/0x03): `rx = rx ± 1`. Most compact
  for loop counters. K=1 case goes from 5 bytes to 2 bytes.
- **F2 `addi rx, imm7`** (2 bytes): `rx = rx + sxt7(imm7)`. In-place (source == dest).
  Covers `|K| ≤ 63`. Suited for `i += stride`, `p += sizeof(T)` (small K).
- **F3b `addli rx, ry, imm10`** (3 bytes): `rx = ry + sxt10(imm10)`. Separate source/dest.
  Covers `|K| ≤ 511`. Suited for bp-relative address computations and struct field offsets.

**Fix:** In `ssa_opt.c`, recognise `SSA_MOVI(rd=vK, imm=K) + SSA_ALU(op=ADD, rs1=vA, rs2=vK)`
pairs where `vK` has exactly one use (the ADD). Replace with a new `SSA_ALU_IMM(op=ADD,
rd=vR, rs1=vA, imm=K)` node. In `risc_backend.c`, emit:
- `inc`/`dec` (F1b, 2B) when K = ±1 and destination == source
- `addi rd, K` (F2, 2B) when destination == source and K fits in imm7
- `addli rd, rs, K` (F3b, 3B) when destination ≠ source and K fits in imm10
- fall back to `immw + add` (5B) for larger K

**Payoff:** Medium-to-high. Loop counters (`i++`, K=1) go from 5 bytes to 2 bytes (F1b `inc`).
Struct field address computation (small positive K) goes from 5 bytes to 2–3 bytes. Pointer
bumps in `memcpy`-style loops similarly benefit.

---

## 4. Priority / Effort Matrix

| ID | Description | Payoff | Effort | Fix location |
|----|-------------|--------|--------|--------------|
| A1 | Remove CPU3 guard on Rule 9 (store/reload elimination) | medium | trivial | `optimise.c`: delete arch guard |
| B1 | `SSA_ADJ` merge pass | low | trivial | `ssa_opt.c`: add merge loop |
| D1 | Align `long`/`float` locals to 4 bytes | high | small | `types.c` `finalize_local_offsets` |
| C2 | Load-MOV fusion (`SSA_LOAD + SSA_MOV` → single load) | medium | small | `ssa_opt.c` or `risc_backend.c` |
| B3/C1 | LEA→STORE forwarding (unlock F2 stores) | high | medium | `ssa_opt.c`: LEA+STORE rewrite |
| B2/D3 | Compare-branch fusion → F3b branch instructions | high | medium | `ssa_opt.c` + `risc_backend.c` |
| B4 | `le`/`ge` expansion reduction (depends on B2) | high | none extra | automatic via B2 |
| D4 | Add-immediate instructions (`addi`/`addli`/`inc`/`dec`) | medium | medium | `ssa_opt.c` + `risc_backend.c` |
| D2 | Callee-saved r6/r7 (reduce call-site spills) | high | large | `regalloc.c` + `ssa.c` + `risc_backend.c` |

**Recommended implementation order:**

1. **A1** — zero risk, immediate payoff for any CPU4 code using compound assignment.
2. **D1** — small isolated change in `types.c`; halves code size for every `float`/`long` local.
3. **B1** — trivial cleanup with no semantic risk.
4. **B3/C1** — the highest-value single change; transforms every local-variable store.
5. **C2** — complements B3 on the load side.
6. **B2/D3/B4** — implement together (B2 enables D3 and B4 automatically).
7. **D4** — independent of the above; most useful after loop-counter patterns are identified.
8. **D2** — a larger project best done after the above wins are measured.
