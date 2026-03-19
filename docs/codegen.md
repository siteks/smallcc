# Code Generator, IR, and Backends

`codegen.c` walks the annotated AST and builds a flat linked list of `IRInst` (stack IR) nodes.
From there the pipeline splits by target architecture:

```
gen_ir()              [codegen.c]    AST → stack IR list
mark_basic_blocks()   [codegen.c]    insert IR_BB_START sentinels
peephole(opt_level)   [optimise.c]   constant fold, dead branch, adj merge, store/reload elim

── CPU3 ──────────────────────────────────────────────────────────────────────
backend_emit_asm()    [backend.c]    stack IR → CPU3 assembly

── CPU4 ──────────────────────────────────────────────────────────────────────
lift_to_ssa()         [ssa.c]        stack IR → SSA IR (virtual registers)
ssa_peephole()        [ssa_opt.c]    identity-move elimination
regalloc()            [regalloc.c]   virtual registers → physical r0–r7
risc_backend_emit()   [risc_backend.c]  SSA IR → CPU4 assembly
```

The stack IR is identical for both targets; the split happens after `peephole`. `backend.c`
and `risc_backend.c` are independent emitters that consume the same upstream IR.

---

## Stack IR

### `IRInst` Struct

```c
typedef struct IRInst {
    IROp          op;       // operation kind
    int           operand;  // integer operand (imm, offset, label id, byte count)
    const char   *sym;      // symbolic operand (label name, symbol name); NULL if unused
    int           line;     // source line number (0 = unknown); set by ir_append()
    struct IRInst *next;
} IRInst;
```

`ir_append(op, operand, sym)` allocates a new node, stamps `current_codegen_line`, and appends
it to `codegen_ctx.ir_head/ir_tail`.

### `IROp` Enum

| Group | Operations |
|---|---|
| Immediate load | `IR_IMM` — r0 = operand (or &sym when sym != NULL) |
| Stack | `IR_PUSH`, `IR_PUSHW`, `IR_POP`, `IR_POPW` |
| Arithmetic | `IR_ADD`, `IR_SUB`, `IR_MUL`, `IR_DIV`, `IR_MOD` |
| Bitwise | `IR_SHL`, `IR_SHR`, `IR_AND`, `IR_OR`, `IR_XOR` |
| Signed variants | `IR_DIVS`, `IR_MODS`, `IR_SHRS` |
| Comparisons (unsigned) | `IR_EQ`, `IR_NE`, `IR_LT`, `IR_LE`, `IR_GT`, `IR_GE` |
| Comparisons (signed) | `IR_LTS`, `IR_LES`, `IR_GTS`, `IR_GES` |
| Float arithmetic | `IR_FADD`, `IR_FSUB`, `IR_FMUL`, `IR_FDIV` |
| Float comparisons | `IR_FLT`, `IR_FLE`, `IR_FGT`, `IR_FGE` |
| Float/int conversion | `IR_ITOF`, `IR_FTOI` |
| Sign extension | `IR_SXB`, `IR_SXW` |
| Memory load | `IR_LB`, `IR_LW`, `IR_LL` (byte / word / long) |
| Memory store | `IR_SB`, `IR_SW`, `IR_SL` |
| Address | `IR_LEA` — r0 = bp + operand |
| Control flow | `IR_J`, `IR_JZ`, `IR_JNZ`, `IR_JL`, `IR_JLI`, `IR_RET` |
| Frame | `IR_ENTER`, `IR_ADJ` |
| Labels | `IR_LABEL` (numeric, operand = id), `IR_SYMLABEL` (sym = name) |
| Data directives | `IR_WORD`, `IR_BYTE`, `IR_ALIGN` |
| Special | `IR_PUTCHAR`, `IR_COMMENT`, `IR_BB_START`, `IR_NOP` |

Binary ops (`IR_ADD` … `IR_GES`, `IR_FADD` … `IR_FGE`) follow the stack-binary convention:
left operand is popped from the stack (`mem[sp]`), right operand is in r0, result written to r0,
sp advanced by 4.

---

## IR Construction (`gen_ir`)

`gen_ir(node, tu_index)` sets `current_tu` then makes four passes over the top-level declaration
list for the current TU:

1. **Pass 1** — function definitions (text area). Each function body emits `IR_SYMLABEL`,
   `IR_ENTER`, per-scope `IR_ADJ` pairs, expression IR, and `IR_RET`. Local static variables
   encountered here are collected for pass 2b.
2. **Pass 2** — global variable declarations (data area). `extern` declarations and bare
   function prototypes are skipped. Static globals use the mangled label `_s{tu_index}_{name}`.
3. **Pass 2b** — local static variable data (`_ls{id}:` labels). `local_static_counter` is
   monotonically increasing across TUs; label IDs are assigned at symbol-table build time.
4. **Pass 3** — deferred string literal data (`_l{id}:` labels), each as a sequence of
   `IR_BYTE` nodes followed by a null-terminator byte. Label IDs are monotonically increasing
   across TUs.

---

## Basic-Block Markers (`mark_basic_blocks`)

`mark_basic_blocks()` inserts `IR_BB_START` sentinel nodes immediately after `gen_ir` completes.
These nodes emit no assembly; they exist solely to delimit basic blocks for the optimiser.

A **basic-block leader** is:
1. The very first instruction.
2. Any instruction immediately following a terminator (`IR_J`, `IR_JZ`, `IR_JNZ`, `IR_RET`).
3. Any `IR_LABEL` or `IR_SYMLABEL` (a potential jump target).

`IR_JL` and `IR_JLI` are calls (not terminators) — execution continues at the next instruction.

---

## Stack-IR Peephole (`optimise.c`)

Enabled by `-O<N>` (`-O` alone implies `-O1`). Called as `peephole(opt_level)` after
`mark_basic_blocks`. `IR_BB_START` is a hard barrier — no pattern fires across it.
`IR_NOP`/`IR_COMMENT` are transparent within a block. The pass repeats until stable (max 20
iterations), then `compact_ir()` removes all `IR_NOP` nodes.

### `-O1` Rules

| # | Pattern | Replacement | Notes |
|---|---|---|---|
| 1 | `IMM K1; PUSH; IMM K2; binop` | `IMM (K1 op K2)` | Constant-fold arithmetic and comparisons |
| 2 | `IMM K; SXB/SXW` | `IMM sext(K)` | Constant sign-extend |
| 3 | `J/JZ/JNZ Lx` → `BB_START` → `LABEL Lx` | kill branch | Dead jump to immediately following label |
| 4 | `ADJ N; ADJ M` | `ADJ N+M` (or delete if 0) | Merge adjacent stack adjustments |
| 5 | `ADJ 0` | delete | Zero adjustment |
| 9 | `LEA N; PUSH; <expr>; SW/SB/SL` + `LEA N; LW/LB/LL` | kill reload | Store/reload elim; gated on CPU3 only |

Rule 1 covers: `add sub mul and or xor shl shr eq ne lt le gt ge`. Symbolic immediates are
never folded. Rules 4+5 together collapse `adj -N`/`adj +N` pairs around empty scopes.

### `-O2` Additional Rules

| # | Pattern | Replacement | Notes |
|---|---|---|---|
| 6 | `PUSH; IMM 1; MUL` | delete | Multiply by 1 |
| 7 | `PUSH; IMM 2^k; MUL` | `PUSH; IMM k; SHL` | Strength-reduce; Rule 1 then folds constant shifts |
| 8 | `PUSH; IMM 0; ADD/SUB` | delete | Add/subtract zero |

Rule 7 is particularly effective for array indexing: stride-scale multiplications (always a
power of 2 since `sizeof(int) = 2`) convert to shifts, which constant-fold when the index is
compile-time constant, collapsing the entire address computation to a single `IMM`.

Rule 9 scans forward from the address push, tracking expression-stack depth across pushes, pops,
and binary ops. When depth reaches 0 at a store, r0 still holds the stored value. If the
immediately following instructions reload from the same address, they are killed. Aborts at
`JL`/`JLI` (calls may clobber memory) and at BB boundaries. 64-instruction scan limit.

---

## CPU3 Backend (`backend.c`)

`backend_emit_asm(ir_head)` walks the stack IR list and writes CPU3 assembly text. It is the
sole difference between the two targets from the perspective of `smallcc.c`.

### Stack Frame Layout

```
  bp+8 + 2*(n-1)   param n   (last param)
       ...
  bp+8             param 0   (first param)  ← sp immediately before jl
  bp+4             return address (lr)
  bp+0             saved bp                 ← bp set here by enter
  bp-2             local 0
  bp-4             local 1
       ...
  bp-2k            local k
```

`enter N`: `mem[sp-4]=lr; mem[sp-8]=bp; bp=sp-8; sp-=N+8`.
`ret`: `sp=bp; bp=mem[sp]; pc=mem[sp+4]; sp+=8`.

Locals within a compound statement are allocated with `adj -size` on entry and reclaimed with
`adj +size` on exit. Nested compound statements chain their adj pairs.

### Variable Addressing

`gen_varaddr_from_ident(node, name)` emits an address into r0:

| Variable kind | IR emitted | Result in r0 |
|---|---|---|
| Global (non-static) | `IR_IMM sym=label_name` | symbolic address |
| Global (static) | `IR_IMM sym=_s{tu}_{name}` | mangled label address |
| Local static | `IR_IMM sym=_ls{id}` | persistent data-section label |
| Parameter | `IR_LEA +offset` | bp + positive offset |
| Local | `IR_LEA -offset` | bp - offset |

For non-array, non-pointer variables, `gen_ld(size)` immediately follows with `IR_LB/LW/LL`
to load the value. Arrays and pointers leave r0 as the address.

### Expression Idioms

**Binary operation** (`ND_BINOP`):
```asm
; gen_expr(lhs) → r0 = lhs value
push
; gen_expr(rhs) → r0 = rhs value
add/sub/mul/…       ; r0 = mem[sp] op r0; sp += 4
```

`gen_arith_op(op, is_float, is_signed)` selects the mnemonic. Signed integer types (`char`,
`short`, `int`, `long`) use signed variants (`lts les gts ges divs mods shrs`); unsigned types
and pointers use unsigned variants. Float operands use `f`-prefixed instructions.

**Assignment** (`ND_ASSIGN`):
```asm
; gen_addr(lhs) → r0 = address of lhs
push
; gen_expr(rhs) → r0 = value to store
sb/sw/sl            ; mem[stack[sp]] = r0; sp += 4
```

**Pointer/array dereference** (`ND_UNARYOP "*"`, non-call):
```asm
; gen_expr(child) → r0 = address to load from
lb/lw/ll            ; r0 = mem[r0]  (size from elem_type(node->type)->size)
```

**Struct member access** (`ND_MEMBER`, `is_function=false`):
```asm
; gen_addr(struct_expr) → r0 = struct base address
push
immw    member_offset
add
lb/lw/ll            ; load member value
```

**Direct function call**:
```asm
; push arguments right-to-left (last arg pushed first)
jl      func_name
adj     param_total_bytes   ; caller cleans up parameters
```

**Indirect call through function pointer variable** (`fp(args)`):
```asm
; push arguments right-to-left
lea     fp_offset           ; or immw fp for globals
lw                          ; load function pointer into r0
jli                         ; lr = pc; pc = r0
adj     param_total_bytes
```

**Logical OR** (`||`):
```asm
; gen_expr(lhs)
jnz  L1             ; short-circuit
; gen_expr(rhs)
jnz  L1
immw 0
j    L2
L1: immw 1
L2:
```

**Logical AND** (`&&`) — symmetric with `jz` for false short-circuit.

**Break/continue**: `break_labels[64]` and `cont_labels[64]` stacks (indexed by `loop_depth`)
store the target label for the enclosing loop or switch. `gen_breakstmt`/`gen_continuestmt`
emit `IR_J break_labels[loop_depth-1]` / `IR_J cont_labels[loop_depth-1]`.

**Switch dispatch**: two-phase — phase 1 re-evaluates the selector for each case and emits
`push; eq; jnz Lcase`; phase 2 emits the body, inserting `IR_LABEL` at each case/default node.

**Goto/label**: `collect_labels()` pre-scans the function body AST before codegen, assigning
numeric IDs to all `ND_LABELSTMT` nodes. Both forward and backward `goto` then resolve by name.

### Cast Generation (`gen_cast`)

No-op when src and dst sizes match. Otherwise:

| From → To | Instructions |
|---|---|
| char → int (sign-extend) | `sxb` |
| short → int (sign-extend) | `sxw` |
| long/ulong → int (truncate) | `push; immw 0xffff; and` |
| long/ulong → char (truncate) | `push; immw 0xff; and` |
| int → char (truncate) | `push; immw 0xff; and` |
| uchar/ushort → larger | zero-extend (no-op; top bits assumed 0) |
| int/ptr → long (zero-extend) | `push; immw 0; push; …` (builds 32-bit pair) |
| int-like → float/double | sign-extend to 32 bits (`sxb`/`sxw`), then `itof` |
| float/double → int-like | `ftoi`, then `push; immw mask; and` if size < 4 |

### Global Variable Initialization

**Scalars**: emit `word value` (or `long` for 4-byte types) in the data section. Float scalars
emit 4 bytes of IEEE 754 representation via `gen_bytes`.

**Arrays** (global): `gen_mem_inits()` fills a byte buffer recursively, then `gen_bytes()` emits
one `IR_BYTE` per byte. Exception — pointer arrays with string-literal initializers emit
`IR_WORD label` per element (a byte buffer cannot hold symbolic addresses).

**Arrays** (local): `gen_fill(vaddr, size)` zeroes all bytes, then `gen_inits()` emits
`lea; push; immw; sw/sl` sequences for non-zero initializers.

**Structs**: `gen_struct_inits` (local) and `gen_struct_mem_inits` (global) write each field;
nested structs recurse. Partial initializers zero-fill the remainder.

### Sethi-Ullman Numbering (`label_su`)

Runs as an AST pass (in `parser.c`) between `insert_coercions` and `gen_ir`. Assigns a
stack-depth label to every expression node; for commutative binary operators with pure operands,
reorders children so the heavier subtree evaluates first. This reduces peak stack depth and moves
constant subtrees left where Rule 1 constant-folds them.

| Node kind | `su_label` |
|---|---|
| `ND_LITERAL`, `ND_IDENT` | 0 |
| `ND_UNARYOP`, `ND_MEMBER`, `ND_CAST` | su_label of the single expression child |
| `ND_TERNARY` | max(cond, then, else) |
| `ND_BINOP` commutative (after optional swap) | l==r ? l+1 : max(l,r) |
| `ND_BINOP` non-commutative | max(l, r+1) |
| Comma / `&&` / `\|\|` | 0 (evaluation order is fixed) |

Swap condition: if `su(right) > su(left)` and both children are pure (`is_pure_expr`), swap
`ch[0]` ↔ `ch[1]`. Pure = literals, non-call identifiers, non-mutating ops, casts. Impure =
assignments, calls, `++`/`--`.

### Annotation Mode (`-ann`)

Every `IRInst` carries a `line` field stamped by `ir_append()` from `current_codegen_line`.
`backend_emit_asm` maintains `prev_line` and `bb_idx`; when `flag_annotate` is set it emits a
`; source text` comment on the first instruction from each new source line, and `; --- bbN ---`
at each `IR_BB_START`. `set_ann_source(src)` builds a line-pointer index into the preprocessed
source text.

---

## CPU4 Backend

After `peephole`, the CPU4 path runs four additional passes before emitting assembly.

### SSA IR

#### `SSAInst` Struct

```c
typedef struct SSAInst {
    SSAOp       op;       // operation kind
    IROp        alu_op;   // SSA_ALU/SSA_ALU1: original IR opcode (IR_ADD, IR_LTS, …)
    int         rd;       // destination: VREG_START+ = virtual, 0–7 = physical, -1 = none, -2 = bp-relative
    int         rs1;      // source 1 (same encoding as rd)
    int         rs2;      // source 2 (same encoding as rd)
    int         imm;      // immediate value, bp byte offset, or label id
    int         size;     // 1/2/4 for SSA_LOAD/SSA_STORE
    const char *sym;      // symbolic name: call target, label name
    int         line;     // source line (carried from the originating IR node)
    struct SSAInst *next;
} SSAInst;
```

#### Virtual Register Namespace

`VREG_START = 8`. Before register allocation all destinations use virtual registers:

| Value | Meaning |
|---|---|
| -2 | bp-relative addressing (not a register; used with SSA_LOAD/SSA_STORE) |
| -1 | no destination |
| 0–7 | physical register (only valid after `regalloc()`) |
| 8 (VREG_START+0) | virtual accumulator → maps to r0 |
| 9 (VREG_START+1) | virtual depth-1 temp → maps to r1 |
| 10 (VREG_START+2) | virtual depth-2 temp → maps to r2 |
| 11+ | additional depths → r3, r4, … |

#### `SSAOp` Enum

| Op | Description |
|---|---|
| `SSA_MOVI` | rd = imm (integer constant) |
| `SSA_MOVSYM` | rd = &sym (label address) |
| `SSA_LEA` | rd = bp + imm (bp-relative address; no load) |
| `SSA_MOV` | rd = rs1 (register copy) |
| `SSA_LOAD` | rd = mem[rs1 + imm] (size bytes); rs1==-2 means bp-relative |
| `SSA_STORE` | mem[rd + imm] = rs1 (size bytes); rd==-2 means bp-relative |
| `SSA_ALU` | rd = rs1 alu_op rs2 (three-register; alu_op = IR_ADD, IR_LTS, IR_FADD, …) |
| `SSA_ALU1` | rd = op(rs1) (single-register; alu_op = IR_SXB, IR_SXW, IR_ITOF, IR_FTOI) |
| `SSA_ADJ` | sp += imm |
| `SSA_ENTER` | enter N (frame setup) |
| `SSA_RET` | ret |
| `SSA_CALL` | jl sym (direct call) |
| `SSA_CALLR` | jlr (indirect call through r0) |
| `SSA_J` | unconditional jump to label imm |
| `SSA_JZ` | jump to label imm if r0 == 0 |
| `SSA_JNZ` | jump to label imm if r0 != 0 |
| `SSA_LABEL` | numeric label (imm = id) |
| `SSA_SYMLABEL` | symbolic label (sym = name) |
| `SSA_WORD` | data word directive |
| `SSA_BYTE` | data byte directive |
| `SSA_ALIGN` | alignment directive |
| `SSA_PUTCHAR` | putchar opcode (r0 implicit) |
| `SSA_COMMENT` | annotation comment |

---

### Pass 1: `lift_to_ssa()` (`ssa.c`)

Converts the flat stack IR list to SSA IR with virtual registers. The key insight is that the
stack machine's expression stack maps cleanly to a small set of virtual registers.

#### Virtual Stack Model

```c
static int vs_reg[MAX_VDEPTH];   // virtual register at each stack depth
static int vs_size[MAX_VDEPTH];  // push size (2 or 4 bytes) per slot
static int vs_depth;             // current expression stack depth
static int vsp;                  // running (sp - bp) offset for scope adj tracking
```

`vs_reg[0]` is always `VREG_START+0` (the accumulator, maps to r0). When a `IR_PUSH` is
processed, depth increments and `vs_reg[depth]` becomes `VREG_START+depth` (maps to r1, r2, …).
`IR_POP` decrements depth. Sethi-Ullman labelling ensures depth never exceeds 3 for well-formed
C expressions, so r0–r3 suffice.

#### Stack IR → SSA Mapping

| Stack IR pattern | SSA output | Notes |
|---|---|---|
| `IR_IMM` (integer) | `SSA_MOVI rd=v0, imm=K` | accumulator |
| `IR_IMM` (symbolic) | `SSA_MOVSYM rd=v0, sym=S` | label address |
| `IR_LEA N; IR_LW/LB/LL` | `SSA_LOAD rd=v0, rs1=-2, imm=N, size` | peephole: look ahead for load; bp-relative F2 path |
| `IR_LW/LB/LL` (standalone) | `SSA_LOAD rd=v0, rs1=v0, imm=0, size` | register-relative (r0 is the address) |
| `IR_SW/SB/SL` | `SSA_STORE rd=v{d-1}, rs1=v0, imm=0, size` | address in vs_reg[depth-1] |
| `IR_LEA N; IR_SW/SB/SL` | `SSA_STORE rd=-2, rs1=v0, imm=N, size` | bp-relative store |
| `IR_PUSH/PUSHW` | `SSA_MOV rd=v{d+1}, rs1=v0`; depth++ | save accumulator to next depth slot |
| `IR_POP/POPW` | `SSA_MOV rd=v0, rs1=v{d}`; depth-- | restore accumulator |
| `IR_ADD` (etc.) | `SSA_ALU rd=v0, rs1=v{d-1}, rs2=v0, alu_op=IR_ADD`; depth-- | pop left from stack slot |
| `IR_SXB/SXW` | `SSA_ALU1 rd=v0, rs1=v0, alu_op=IR_SXB/SXW` | sign-extend accumulator |
| `IR_ITOF/FTOI` | `SSA_ALU1 rd=v0, rs1=v0, alu_op=IR_ITOF/FTOI` | float conversion |
| `IR_JL sym` | flush args + `SSA_CALL sym` | see call flushing below |
| `IR_JLI` | flush args + `SSA_CALLR` | indirect call |
| `IR_ENTER N` | `SSA_ENTER imm=N`; vsp = -N | frame setup; initialise vsp |
| `IR_ADJ N` | `SSA_ADJ imm=N` (scope adj) or post-call arg cleanup (see below) | |
| `IR_J/JZ/JNZ L` | `SSA_J/JZ/JNZ imm=L`; record vsp for forward-branch fixup | |
| `IR_LABEL L` | `SSA_LABEL imm=L`; apply recorded vsp from branch | |
| `IR_SYMLABEL S` | `SSA_SYMLABEL sym=S`; reset all state (function entry) | |
| `IR_RET` | `SSA_RET` | |

The lifter peeks one instruction ahead when it sees `IR_LEA`: if the next op is a load or
store, it collapses the pair into a single `SSA_LOAD`/`SSA_STORE` with `rs1=-2` (bp-relative),
enabling the F2 encode path in the backend.

#### Call Flushing (`flush_for_call_n`)

Before every `IR_JL`/`IR_JLI`, argument registers and outer expression temporaries must be
partitioned. The argument values pushed most recently occupy the bottom of the virtual stack;
any temporaries from an enclosing expression sit above them.

1. **Detect the boundary**: the call site knows the argument count (`n`); the bottom `n` virtual
   stack slots are arguments; slots above are outer temporaries.
2. **Spill outer temporaries** to memory at higher sp offsets with `SSA_STORE rd=-2, imm=vsp`.
   Record the spill locations in `spill_reg[]`/`spill_off[]`/`spill_sz[]`.
3. **Flush arguments** to the stack frame below the current sp with `SSA_STORE` and emit
   `SSA_ADJ` to move sp to the new call frame boundary.
4. **Post-call restore**: after the caller-side `IR_ADJ +N` (argument cleanup), reload the
   spilled outer temporaries from their saved locations and emit a second `SSA_ADJ` to reclaim
   the spill area.

This preserves r1–r3 across a call when there are outer expression temporaries, at the cost
of a spill/reload per temporary.

#### Forward-Branch vsp Fixup

`adj` scope-exit instructions from one branch arm must not affect the `vsp` seen by the merge
point at the target label. When a branch (`IR_J/JZ/JNZ`) is processed, the current `vsp` is
recorded in a fixup table keyed by label id. When the target `IR_LABEL` is reached, `vsp` is
restored from the table entry. This prevents scope-cleanup adjustments from one arm from
corrupting the sp model in subsequent blocks.

---

### Pass 2: `ssa_peephole()` (`ssa_opt.c`)

Single pass over the SSA list before register allocation. Currently:

- **Identity-move elimination**: `SSA_MOV rd=A, rs1=A` (destination equals source) is marked
  dead by setting `op = -1`. `risc_backend_emit` skips nodes with `op < 0`.

The pass operates on virtual registers. Future passes (copy propagation, constant propagation,
dead-code elimination, compare-branch fusion, LEA→STORE forwarding) slot in here.

---

### Pass 3: `regalloc()` (`regalloc.c`)

Trivial depth-based mapping — no live-range analysis required:

```c
static int alloc_reg(int vreg) {
    if (vreg < VREG_START) return vreg;   /* -2, -1, or already physical */
    return vreg - VREG_START;             /* v8→r0, v9→r1, v10→r2, … */
}
```

`regalloc()` walks the SSA list and rewrites every `rd`, `rs1`, `rs2` field through
`alloc_reg()`. After this pass all virtual registers ≥ `VREG_START` become physical registers
r0–r7. The `-2` bp-relative sentinel and `-1` no-destination sentinel are preserved unchanged.

Correctness relies on the Sethi-Ullman guarantee that expression depth ≤ 3 (r0–r2 suffice for
all typical C expressions). r3 handles the rare depth-3 case. r4–r7 are not currently assigned
by the allocator (reserved for future use or callee-saved conventions).

---

### Pass 4: `risc_backend_emit()` (`risc_backend.c`)

Walks the post-regalloc SSA list and emits CPU4 assembly text. Nodes with `op < 0` (killed by
`ssa_peephole`) are skipped.

#### Instruction Selection

| SSAOp | Condition | CPU4 emission |
|---|---|---|
| `SSA_MOVI` | imm fits in 16 bits | `immw rd, imm` |
| `SSA_MOVI` | imm > 0xffff | `immw rd, lo16; immwh rd, hi16` |
| `SSA_MOVSYM` | — | `immw rd, sym` |
| `SSA_LEA` | — | `lea rd, imm` (F3c; bp + signed 16-bit offset) |
| `SSA_MOV` | rd != rs1 | `or rd, rs1, rs1` (F1a pseudo `mov`) |
| `SSA_LOAD` | rs1 == -2, in F2 range | `lw/lb/ll rd, [bp+imm7]` (F2, 2 bytes) |
| `SSA_LOAD` | rs1 == -2, out of F2 range | `lea scratch, imm; llw/llb/lll rd, [scratch+0]` |
| `SSA_LOAD` | rs1 >= 0 | `llw/llb/lll rd, [rs1+imm]` (F3b, 3 bytes) |
| `SSA_STORE` | rd == -2, in F2 range | `sw/sb/sl rs1, [bp+imm7]` (F2, 2 bytes) |
| `SSA_STORE` | rd == -2, out of F2 range | `lea scratch, imm; slw/slb/sll rs1, [scratch+0]` |
| `SSA_STORE` | rd >= 0 | `slw/slb/sll rs1, [rd+imm]` (F3b, 3 bytes) |
| `SSA_ALU` | most ops | `add/sub/mul/… rd, rs1, rs2` (F1a, 2 bytes) |
| `SSA_ALU` | `le`/`ge` | `gt/lt rd, rs2, rs1; immw tmp, 0; eq rd, rd, tmp` (3 instructions) |
| `SSA_ALU` | `les`/`ges` | `gts/lts rd, rs2, rs1; immw tmp, 0; eq rd, rd, tmp` |
| `SSA_ALU` | `fle`/`fge` | `fgt/flt rd, rs2, rs1; immw tmp, 0; eq rd, rd, tmp` |
| `SSA_ALU1` | `sxb`/`sxw` | `sxb/sxw rd` (F1b, 2 bytes; in-place) |
| `SSA_ALU1` | `itof`/`ftoi` | `itof`/`ftoi` (F0; r0 implicit) |
| `SSA_ENTER` | — | `enter N` (F3a) |
| `SSA_ADJ` | imm != 0 | `adjw imm` (F3a; always adjw, no 8-bit adj in CPU4) |
| `SSA_RET` | — | `ret` (F0) |
| `SSA_CALL` | — | `jl sym` (F3a) |
| `SSA_CALLR` | — | `jlr` (F0; r0 holds target) |
| `SSA_J/JZ/JNZ` | — | `j/jz/jnz _lN` (F3a) |
| `SSA_LABEL` | — | `_lN:` |
| `SSA_SYMLABEL` | — | `sym:` |
| `SSA_WORD/BYTE/ALIGN` | — | `word`/`byte`/`align` directives |
| `SSA_PUTCHAR` | — | `putchar` (F0) |

#### F2 Range Checks

F2 bp-relative instructions use a scaled 7-bit signed immediate:

| Access size | Scale | F2 byte-offset range |
|---|---|---|
| byte (`lb`/`sb`/`lbx`) | ×1 | −64 … +63 bytes from bp |
| word (`lw`/`sw`/`lwx`) | ×2 | −128 … +126 bytes from bp (even) |
| long (`ll`/`sl`) | ×4 | −256 … +252 bytes from bp (mult of 4) |

When a bp-relative access falls outside the F2 range, the backend falls back to
`lea rd, imm` (F3c, 3 bytes) + `llw/slw rd, [rd+0]` (F3b, 3 bytes) = 6 bytes.

#### `le`/`ge` Expansion

The F1a instruction set omits `le`/`ge` (they are pseudo-ops implemented by operand-swap).
For the compare-into-register case (where a 0/1 result is needed in a register rather than
a direct branch), the backend expands `le rd, rs1, rs2` as:

```asm
gt   rd, rs1, rs2    ; rd = (rs1 > rs2) ? 1 : 0
immw tmp, 0
eq   rd, rd, tmp     ; rd = (rd == 0) ? 1 : 0  — i.e. !(rs1 > rs2)
```

When compare-branch fusion (optimisation B2) is implemented, `le`/`ge` followed by
`jz`/`jnz` will collapse to a single F3b `bgt`/`blt` instruction instead.

#### Annotation

When `-ann` is set, `rb_emit_src_comment(line)` emits a `; source text` comment before the
first instruction from each new source line, using the same `ann_lines[]` index built by
`set_ann_source()` in `smallcc.c`. Comments are suppressed at label nodes.
