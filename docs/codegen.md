# Code Generator (`codegen.c`) and Backend (`backend.c`)

`codegen.c` walks the annotated AST and builds a flat linked list of `IRInst` nodes. `backend.c` then walks that list and emits assembly text to the output file (stdout or `-o` target). This split makes `backend.c` the retargeting point: to support a new CPU, replace `backend_emit_asm` while keeping the IR unchanged.

## Stack Frame Layout

```
  bp+8 + 2*(n-1)   param n   (last param, 2-byte int/pointer)
       ...
  bp+8             param 0   (first param)   ← sp immediately before jl
  bp+4             return address (lr)
  bp+0             saved bp                  ← bp set here by enter
  bp-2             local 0
  bp-4             local 1
       ...
  bp-2k            local k                   ← sp after all adj in nested scopes
```

`enter word` executes: `mem[sp-4] = lr; mem[sp-8] = bp; bp = sp-8; sp -= word + 8`.

`ret` executes: `sp = bp; bp = mem[sp]; pc = mem[sp+4]; sp += 8`.

Locals within a compound statement are allocated with `adj -size` on entry and reclaimed with `adj +size` on exit. Nested compound statements chain their adj pairs.

## Variable Addressing

`gen_varaddr_from_ident(node, name)`:
- **Global (non-static)**: `immw label_name` (symbolic address)
- **Global (static)**: `immw _s{tu_index}_{name}` (mangled to be TU-private)
- **Local static**: `immw _ls{id}` (persistent data-section label; `id` from `local_static_counter`)
- **Parameter**: `lea positive_offset` (above bp)
- **Local**: `lea -offset` (below bp)

After `gen_varaddr`, r0 holds the **address** of the variable. For non-array, non-pointer variables, `gen_ld(size)` is immediately called to load the value; arrays and pointers leave r0 as the address.

## Expression Idioms

**Binary operation** (`ND_BINOP`):
```asm
; gen_expr(lhs) → r0 = lhs
push                ; stack[sp] = lhs
; gen_expr(rhs) → r0 = rhs
op                  ; r0 = stack[sp] op r0; sp += 4
```

`gen_arith_op(op, is_float, is_signed)` selects the assembly instruction. For `<`, `<=`, `>`, `>=`, `/`, `%`, and `>>`, **signed** integer types (`char`, `short`, `int`, `long`) emit the signed variants (`lts`, `les`, `gts`, `ges`, `divs`, `mods`, `shrs`); unsigned types and pointers use the unsigned variants (`lt`, `le`, `gt`, `ge`, `div`, `mod`, `shr`). Floating-point operands use the `f`-prefixed instructions.

**Assignment** (`ND_ASSIGN`):
```asm
; gen_addr(lhs) → r0 = address of lhs
push
; gen_expr(rhs) → r0 = value
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
immw member_offset
add
lb/lw/ll            ; load member value
```

**Call through struct member function pointer** (`ND_MEMBER`, `is_function=true`; covers `s.fp(args)` and `s->fp(args)`):
```asm
; push arguments right-to-left (children[2..])
; gen_expr(arg[n-1]) → pushw/push
; ...
; gen_expr(arg[0]) → pushw/push
; gen_addr(node) → r0 = address of fn-ptr field (struct_base + member_offset)
lw                  ; load function pointer value (2 bytes)
jli                 ; indirect call
adj   param_total_bytes
```

**Direct function call** (`ND_IDENT` with `is_function=true` and function type):
```asm
; push arguments right-to-left (last arg first)
; gen_expr(arg[n-1]) → pushw/push
; ...
; gen_expr(arg[0]) → pushw/push
jl    func_name
adj   param_total_bytes   ; caller cleans up parameters
```
Function designators used as values (e.g. `fp = myfunc`) emit only `immw func_name` (no call).

**Indirect call through function pointer variable** (`fp(args)` where `fp` has `ptr(fn)` type):
```asm
; push arguments right-to-left
; gen_expr(arg[n-1]) → pushw/push
; ...
; gen_expr(arg[0]) → pushw/push
lea   fp_offset     ; or immw fp for globals
lw                  ; load function pointer value into r0
jli                 ; indirect call: lr = pc; pc = r0
adj   param_total_bytes
```

**Indirect call through dereferenced pointer** (`(*fp)(args)`):
```asm
; push arguments right-to-left (children[1..n-1])
; gen_expr(children[0]) → r0 = function address (evaluating the pointer expr)
jli
adj   param_total_bytes
```

**`putchar(c)`** — special CPU builtin, handled directly in `gen_callfunction`:
```asm
; gen_expr(c) → r0
putchar             ; opcode 0x1e; writes r0 & 0xff to stdout; does not modify r0
```

**Logical OR** (`||`):
```asm
; gen_expr(lhs)
jnz  L1         ; short-circuit: if lhs != 0, result is 1
; gen_expr(rhs)
jnz  L1
immw 0
j    L2
L1: immw 1
L2:
```

**Logical AND** (`&&`):
```asm
; gen_expr(lhs)
jz   L1         ; short-circuit: if lhs == 0, result is 0
; gen_expr(rhs)
jz   L1
immw 1
j    L2
L1: immw 0
L2:
```

**Break/continue label stacks** (`codegen.c` file scope):
```c
static int break_labels[64];   // target label for break at each loop nesting depth
static int cont_labels[64];    // target label for continue at each loop nesting depth
static int loop_depth = 0;     // current nesting depth (incremented on loop entry)
```
`gen_whilestmt`, `gen_forstmt`, `gen_dowhilestmt`, and `gen_switchstmt` push their break/continue targets before generating the body and pop them after. `gen_breakstmt`/`gen_continuestmt` emit `j break_labels[loop_depth-1]` / `j cont_labels[loop_depth-1]`.

**Switch dispatch** (`gen_switchstmt`): two-phase — phase 1 re-evaluates the selector expression for each `case` label, emitting `push; eq; jnz lcase` comparisons; phase 2 emits the compound-statement body verbatim, inserting `gen_label` at each `ND_CASESTMT`/`ND_DEFAULTSTMT` node.

**Goto/label resolution** (`gen_function`): before emitting a function body, `collect_labels()` walks the AST and calls `new_label()` for every `ND_LABELSTMT`, storing `(name, id)` pairs in `label_table[64]`. Both `gen_gotostmt` and `gen_labelstmt` look up the name in this table, so forward jumps resolve correctly.

## Cast Generation (`gen_cast`)

Casts are no-ops when src and dst have the same size. Otherwise:

| From → To | Instructions |
|---|---|
| char → int (sign-extend) | `sxb` |
| short → int (sign-extend) | `sxw` |
| long/ulong → int (truncate) | `push; immw 0xffff; and` |
| long/ulong → char (truncate) | `push; immw 0xff; and` |
| int → char (truncate) | `push; immw 0xff; and` |
| uchar/ushort → larger | zero-extend (no-op, top bits assumed 0) |
| int/ptr → long (zero-extend) | `push; immw 0; push; …` (builds 32-bit) |
| int-like → float/double | `sxb` or `sxw` (sign-extend to 32 bits) then `itof` |
| float/double → int-like | `ftoi` then `push; immw mask; and` if size < 4 |

## Global Variable Initialization

**Scalars**: emits `word value` (or `long` for 4-byte integer types) in the data section. Float scalars emit 4 bytes of IEEE 754 representation via `gen_bytes`.

**Arrays** (global): `gen_mem_inits()` recursively fills a `char data[]` byte buffer, then `gen_bytes()` emits one `byte` directive per byte.

**Arrays** (local): `gen_fill(vaddr, size)` zeroes all bytes, then `gen_inits()` recursively emits `lea; push; immw; sw/sl` sequences for non-zero initializers.

**Structs**: `gen_struct_inits` (local) and `gen_struct_mem_inits` (global) write each field; nested structs recurse. Partial initializers leave remaining fields zeroed.

## Sethi-Ullman Numbering (`label_su`)

`label_su(root)` (`parser.c`) runs as an AST pass between `insert_coercions` and `gen_ir`. It assigns a stack-depth label to every expression node and, for commutative binary operators with pure operands, reorders children so the heavier subtree evaluates first.

### Motivation

On a single-accumulator stack machine, evaluating `a OP b` always follows:

```
gen_expr(lhs)   → r0 = lhs
push            → save lhs to stack
gen_expr(rhs)   → r0 = rhs
op              → r0 = stack[sp] op r0; sp += 4
```

For commutative operators (`+ * & | ^ == !=`), either child can go first. Evaluating the heavier child first (the one that requires more stack slots) reduces peak stack depth and — critically — moves constant subtrees to the left where the `-O1` constant-folder can fire more often.

### Label Rules

| Node kind | `su_label` |
|---|---|
| `ND_LITERAL`, `ND_IDENT` | `0` (load into r0, no stack) |
| `ND_UNARYOP`, `ND_MEMBER`, `ND_CAST` | `su_label` of the single expression child |
| `ND_TERNARY` | `max(cond, then, else)` |
| `ND_BINOP` (commutative, after optional swap) | `l == r ? l+1 : max(l, r)` |
| `ND_BINOP` (non-commutative) | `max(l, r+1)` |
| Comma / `&&` / `\|\|` | `0` (evaluation order is semantically fixed) |

**Swap condition**: if `su(right) > su(left)` and both children are `is_pure_expr`, swap `ch[0]` ↔ `ch[1]`. Purity is conservative: literals, non-call identifiers, non-mutating unary/binary ops, and casts qualify; assignments, calls, and `++`/`--` do not.

The pass uses the existing `post_order_walk` infrastructure so children are fully labelled before their parent is processed.

## IR Construction (`gen_ir`) and Emission (`backend_emit_asm`)

`gen_ir(node, tu_index)` sets `current_tu` then makes three passes over the top-level declaration list for the current TU, appending `IRInst` nodes to `codegen_ctx.ir_head`:

1. **Pass 1**: append IR for function definitions (text area).
2. **Pass 2**: append IR for global variable declarations (data area). `extern` declarations and bare function prototypes are skipped. Static globals use the mangled label `_s{tu_index}_{name}`.
3. **Pass 2b**: append IR for local static variable data collected during pass 1 (`_ls{id}:` labels). `local_static_counter` is monotonically increasing across TUs; labels are assigned at symbol-table build time.
4. **Pass 3**: append IR for deferred string literal data (`_l{id}:` labels), each as a sequence of `IR_BYTE` nodes followed by a null terminator. Label IDs are monotonically increasing across TUs.

After `gen_ir` returns, the caller invokes `backend_emit_asm(codegen_ctx.ir_head)` to walk the IR list and write assembly text. Two additional passes run between construction and emission:

```
gen_ir(node, tu)          →  IR list (raw)
mark_basic_blocks()       →  IR list + BB_START markers
peephole(opt_level)       →  IR list, compacted   (only when -O1/-O2)
backend_emit_asm(ir_head) →  assembly text
```

## Basic-Block Markers (`mark_basic_blocks`)

`mark_basic_blocks()` (`codegen.c`) inserts `IR_BB_START` sentinel nodes into the IR list immediately after `gen_ir` completes. These nodes emit no assembly; they exist solely to delimit basic blocks for the optimiser.

A **basic-block leader** is any instruction where a new basic block must begin:

1. The very first instruction in the list.
2. Any instruction that immediately follows a terminator (`IR_J`, `IR_JZ`, `IR_JNZ`, `IR_RET`).
3. Any `IR_LABEL` or `IR_SYMLABEL` instruction (a potential jump target).

`IR_JL` and `IR_JLI` are **calls**, not terminators — execution continues at the instruction after the call — so they do not end a basic block.

`mark_basic_blocks` walks the list with a single pass, inserting an `IR_BB_START` node before each leader (suppressing duplicates). It updates `codegen_ctx.ir_tail` afterward.

The result is a list whose structure looks like:

```
IR_BB_START
IR_SYMLABEL "func"
IR_ENTER  N
...instructions...
IR_JZ     L3
IR_BB_START
IR_LABEL  L3
...instructions...
IR_RET
IR_BB_START
...
```

`backend_emit_asm` skips `IR_BB_START` and `IR_NOP` nodes silently.

## Peephole Optimiser (`optimise.c`)

Enabled by the `-O<N>` compiler flag (`-O` alone implies `-O1`). Called as `peephole(opt_level)` after `mark_basic_blocks()`.

### Design

`IR_BB_START` is a **hard barrier**: no pattern fires across it, so the optimiser never moves code between basic blocks. Within a block, `IR_NOP` (the deleted-instruction marker) and `IR_COMMENT` nodes are treated as **transparent**: the `next_inbb()` helper skips them when building a lookahead window, so intervening deletions from earlier rules do not prevent later rules from firing.

The pass repeats until the IR stops changing (capped at 20 iterations), then `compact_ir()` relinks the list to physically remove all `IR_NOP` nodes and updates `ir_tail`.

### `-O1` Rules

| # | Pattern (same basic block) | Replacement | Notes |
|---|---|---|---|
| 1 | `IMM K1; PUSH; IMM K2; binop` | `IMM (K1 op K2)` | Constant-fold arithmetic and comparisons |
| 2 | `IMM K; SXB` | `IMM sext8(K)` | Constant sign-extend byte |
| 2 | `IMM K; SXW` | `IMM sext16(K)` | Constant sign-extend word |
| 3 | `J/JZ/JNZ Lx` → `BB_START` → `LABEL Lx` | kill branch | Dead jump to immediately following label |
| 4 | `ADJ N; ADJ M` | `ADJ N+M` (or delete if sum=0) | Merge adjacent stack adjustments |
| 5 | `ADJ 0` | delete | Zero stack adjustment |
| 9 | `LEA N; PUSH; …expr…; SW/SB/SL` followed by `LEA N; LW/LB/LL` | delete reload | Store/reload elimination: r0 still holds the stored value |

Rule 1 covers all foldable binary ops: `add sub mul and or xor shl shr eq ne lt le gt ge`. Symbolic immediates (label addresses) are never folded.

Rules 4 and 5 together collapse the paired `adj -N` / `adj +N` that surround empty or dead scopes (two passes: merge → zero → delete).

Rule 3 handles both unconditional jumps and conditional branches whose target is the immediately following label (the branch is always taken to the same place as fall-through regardless of condition).

### `-O2` Additional Rules

| # | Pattern | Replacement | Notes |
|---|---|---|---|
| 6 | `PUSH; IMM 1; MUL` | delete all three | Multiply by 1 is identity |
| 7 | `PUSH; IMM 2^k; MUL` | `PUSH; IMM k; SHL` | Strength-reduce power-of-2 multiply to shift; Rule 1 can then fold if the pushed value is also a constant |
| 8 | `PUSH; IMM 0; ADD/SUB` | delete all three | Add/subtract zero is identity |

Rule 9 scans forward from the address push, tracking expression-stack depth across binary ops, pushes, and pops. When depth reaches 0 at a store (`SW/SB/SL`), r0 still holds the value just stored. If the immediately following instructions reload from the same address (matching `LEA N; LW/LB/LL` or `IMM sym; LW/LB/LL`), those two instructions are killed. The scan aborts at `JL`/`JLI` (calls may clobber memory) and at BB boundaries. A 64-instruction limit prevents O(n²) worst case.

Rule 7 is particularly effective for array indexing: stride-scale multiplications (always a power of 2 on this target since `sizeof(int) = 2`) are converted to shifts, and when the array index is a compile-time constant the shift is immediately constant-folded by Rule 1, collapsing the entire address computation.

### Example: constant array access

Source: `int a[5]; a[0]=10; a[1]=20; a[2]=30; return a[1]+a[2];`

Unoptimised address computation for `a[1]`:
```asm
lea     -10          ; base of a
push
immw    0x0002       ; stride (sizeof int)
push
immw    0x0001       ; index
mul                  ; 2 * 1
add                  ; base + offset
```
After `-O2` (Rule 1 folds `immw 2; push; immw 1; mul` → `immw 2`; Rule 8 has no effect here since 2 ≠ 0):
```asm
lea     -10
push
immw    0x0002
add
```
For `a[0]` the index is 0, so Rule 1 folds `2*0` → `immw 0`, and then Rule 8 eliminates `push; immw 0; add`, leaving only `lea -10` as the address.

## Annotation Mode (`-ann`)

Passing `-ann` to the compiler enables inline source-code and basic-block comments in the assembly output. This is implemented across `codegen.c`, `backend.c`, and `smallcc.c`.

### Source Line Tracking

Every `IRInst` carries an `int line` field (0 = unknown). A static `current_codegen_line` in `codegen.c` is updated at the top of `gen_stmt()` and `gen_decl()` using `node->line`, which is set by the parser to the line of the first token of each statement or declaration. `ir_append()` stamps `current_codegen_line` onto every new instruction.

**Statement line assignment**: `stmt()` sets `node->line = token_ctx.current->line` immediately after `new_node()`, using the not-yet-consumed token of the current statement (e.g. the `if` keyword) rather than the previously consumed token (e.g. `{` on the preceding line).

### Assembly Output

`backend_emit_asm` maintains `prev_line` (last emitted source line) and `bb_idx` (current BB counter). For each IR instruction:

1. If `flag_annotate` is set, `p->line != 0`, and `p->line != prev_line`, and `p->op` is not `IR_BB_START`/`IR_NOP`/`IR_COMMENT`: emit the source line as a comment (`; trimmed source text`) and update `prev_line`.
2. At `IR_BB_START` nodes: emit `; --- bbN ---` and increment `bb_idx`.

`set_ann_source(src)` (called once per TU in `smallcc.c` before `gen_ir`) builds a line-pointer index: it `strdup`s the preprocessed source and indexes the start of each line into `ann_lines[]`. `emit_src_comment(line)` uses this index to print the trimmed content of line N.

### Example output (`-ann -O1`)

```asm
; --- bb0 ---
fact:
    enter   0
    adj     -2
; if (n <= 1) return 1;
    lea     2
    lw
    push
    immw    0x0001
    le
    jz      _l4
; --- bb1 ---
    immw    0x0001
    ret
; --- bb2 ---
_l4:
; return n * fact(n - 1);
    lea     2
    lw
    push
    lea     2
    lw
    push
    immw    0x0001
    sub
    jl      fact
    adj     2
    mul
    ret
```
