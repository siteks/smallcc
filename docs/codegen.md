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

## IR Construction (`gen_ir`) and Emission (`backend_emit_asm`)

`gen_ir(node, tu_index)` sets `current_tu` then makes three passes over the top-level declaration list for the current TU, appending `IRInst` nodes to `codegen_ctx.ir_head`:

1. **Pass 1**: append IR for function definitions (text area).
2. **Pass 2**: append IR for global variable declarations (data area). `extern` declarations and bare function prototypes are skipped. Static globals use the mangled label `_s{tu_index}_{name}`.
3. **Pass 2b**: append IR for local static variable data collected during pass 1 (`_ls{id}:` labels). `local_static_counter` is monotonically increasing across TUs; labels are assigned at symbol-table build time.
4. **Pass 3**: append IR for deferred string literal data (`_l{id}:` labels), each as a sequence of `IR_BYTE` nodes followed by a null terminator. Label IDs are monotonically increasing across TUs.

After `gen_ir` returns, the caller invokes `backend_emit_asm(codegen_ctx.ir_head)` to walk the IR list and write assembly text. An optimisation pass (`optimise_ir`) could be inserted between the two calls.
