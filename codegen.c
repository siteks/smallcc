
#include "smallcc.h"
#include "ir3.h"

/* Sentinel sym pointer for promotable-variable IR_LEA instructions.
 * braun_ssa() identifies promotable LEAs by pointer identity:
 *   p->sym == ir_promote_sentinel  → promotable scalar local / param
 *   p->sym == NULL                 → structural LEA (array fill, copy buffer)
 *   p->sym = other non-NULL ptr    → address-taken or aggregate variable */
const char ir_promote_sentinel[] = "";

// Codegen context instance
CodegenContext codegen_ctx;


typedef struct { int offset; bool is_param; } LocalAddr;
// Returns bp-relative byte offset for local variables and parameters.
// Returns offset == -1 for globals (use symbolic label) and local-statics (use _ls{id} label).
static LocalAddr find_local_addr(Node *node, const char *name)
{
    DBG_PRINT("%s scope id:%d depth:%d\n", __func__, node->st->scope_id, node->st->depth);
    Symbol *s = find_symbol_st(node->st, name, NS_IDENT);
    if (!s)
        src_error(node->line, node->col, "Symbol %s not found!", name);
    if (s->kind == SYM_STATIC_LOCAL || s->kind == SYM_GLOBAL || s->kind == SYM_STATIC_GLOBAL || s->kind == SYM_EXTERN)
        return (LocalAddr){ -1, false };
    DBG_PRINT(";find_local_addr ident:%s got offset %d\n", name, s->offset & 0xffff);
    return (LocalAddr){ s->offset, s->kind == SYM_PARAM };
}


static int new_strlit(char *data, int len)
{
    if (codegen_ctx.strlit_count >= MAX_STRLITS)
        error("too many string literals (max %d)", MAX_STRLITS);
    int id = codegen_ctx.label_counter++;
    codegen_ctx.strlits[codegen_ctx.strlit_count].id   = id;
    codegen_ctx.strlits[codegen_ctx.strlit_count].data = data;
    codegen_ctx.strlits[codegen_ctx.strlit_count].len  = len;
    codegen_ctx.strlit_count++;
    return id;
}

int new_label();

void reset_codegen(void)
{
    codegen_ctx.strlit_count      = 0;
    codegen_ctx.local_static_count = 0;
    codegen_ctx.loop_depth        = 0;
    codegen_ctx.label_table_size  = 0;
    codegen_ctx.ir_head           = NULL;
    codegen_ctx.ir_tail           = NULL;
    codegen_ctx.adj_depth         = 0;
    codegen_ctx.current_fn_ret_type = NULL;
    memset(codegen_ctx.break_labels, 0, sizeof(codegen_ctx.break_labels));
    memset(codegen_ctx.cont_labels,  0, sizeof(codegen_ctx.cont_labels));
    // 'codegen_ctx.label_counter' is NOT reset — monotonically increasing across TUs.
}

static int current_codegen_line = 0;  // source line stamped onto each IR instruction

static IRInst *ir_append(IROp op, int operand, const char *sym)
{
    IRInst *inst = arena_alloc(sizeof(IRInst));
    inst->op      = op;
    inst->operand = operand;
    inst->sym     = sym;
    inst->line    = current_codegen_line;
    inst->next    = NULL;
    if (codegen_ctx.ir_tail)
        codegen_ctx.ir_tail->next = inst;
    else
        codegen_ctx.ir_head = inst;
    codegen_ctx.ir_tail = inst;
    return inst;
}

// Insert IR_BB_START markers at every basic-block leader in the IR list.
// A leader is:
//   1. The first instruction in the list.
//   2. Any instruction that immediately follows a terminator
//      (unconditional/conditional branch or ret).
//   3. Any IR_LABEL / IR_SYMLABEL instruction (potential jump target).
//
// IR_JL and IR_JLI are calls, not terminators: execution continues at
// the instruction after the call, so they do not end a basic block.
//
// Consecutive IR_BB_START nodes are suppressed (only one is inserted
// between any two non-marker instructions).
void mark_basic_blocks(void)
{
    IRInst *head = codegen_ctx.ir_head;
    if (!head)
        return;

    // Helper: is this op a basic-block terminator?
#define IS_TERMINATOR(op) \
    ((op) == IR_J || (op) == IR_JZ || (op) == IR_JNZ || (op) == IR_RET)

    // Helper: does this op start a new BB regardless of what precedes it?
#define IS_LABEL(op) \
    ((op) == IR_LABEL || (op) == IR_SYMLABEL)

    // --- Insert marker before the very first instruction (if not already a marker) ---
    if (head->op != IR_BB_START)
    {
        IRInst *m = arena_alloc(sizeof(IRInst));
        m->op   = IR_BB_START;
        m->next = head;
        codegen_ctx.ir_head = m;
    }

    // --- Walk the list and insert markers where needed ---
    for (IRInst *p = codegen_ctx.ir_head; p && p->next; p = p->next)
    {
        IRInst *nxt = p->next;

        // Skip if already a marker — nothing to insert after a marker.
        if (p->op == IR_BB_START)
            continue;

        bool need_marker = IS_TERMINATOR(p->op) || IS_LABEL(nxt->op);

        // Suppress if next is already a BB_START.
        if (nxt->op == IR_BB_START)
            need_marker = false;

        if (need_marker)
        {
            IRInst *m = arena_alloc(sizeof(IRInst));
            m->op   = IR_BB_START;
            m->next = nxt;
            p->next = m;
            p = m;  // step over the fresh marker; loop increment moves to nxt
        }
    }

    // Update ir_tail to reflect any appended nodes.
    IRInst *last = codegen_ctx.ir_head;
    while (last->next)
        last = last->next;
    codegen_ctx.ir_tail = last;

#undef IS_TERMINATOR
#undef IS_LABEL
}

static void collect_labels(Node *node);
static void collect_labels_visitor(Node *child, void *ctx)
{
    (void)ctx;
    collect_labels(child);
}
static void collect_labels(Node *node)
{
    if (node->kind == ND_LABELSTMT)
    {
        if (codegen_ctx.label_table_size >= MAX_LABEL_TABLE)
            error("too many goto labels in function (max %d)", MAX_LABEL_TABLE);
        codegen_ctx.label_table[codegen_ctx.label_table_size].label_id = new_label();
        strncpy(codegen_ctx.label_table[codegen_ctx.label_table_size].name, node->u.labelstmt.name, 63);
        codegen_ctx.label_table_size++;
    }
    for_each_child(node, collect_labels_visitor, NULL);
}

//--------------------------------------------------------------------------------
// Pseudoinstructions
//
// Stack VM
//
// State:
//  pc
//  sp
//  bp
//  lr
//  r0  
//
//  imm     val     r0 = val
//  lb              r0 = *r0
//  lw              r0 = *r0
//  sb              **sp++ = r0
//  sw              **sp++ = r0
//
//  op              r0 = *sp++ op r0
//  push            *--sp = r0
//  pop             r0 = *sp++

//  jl      val     lr = pc; pc = val; 
//  enter   val     *--sp = lr; *--sp = bp; bp = sp; sp -= val;
//  return          sp = bp; bp = *sp++; pc = *sp++
//  lea     val     r0 = bp + val
//
//
// To store to local var
//  lea     offset
//  push
//  
//--------------------------------------------------------------------------------
int new_label() { return codegen_ctx.label_counter++; }


void gen_zeros(int bytes)
{
    for (int i = 0; i < bytes; i++)
        ir_append(IR_BYTE, 0, NULL);
}
void gen_bytes(char *data, int size)
{
    for (int i = 0; i < size; i++)
        ir_append(IR_BYTE, (unsigned char)data[i], NULL);
}
// Forward declarations needed by mutual recursion and out-of-order use.
void gen_expr(Node *node);
static void gen_expr_discard(Node *node);

// Short-circuit logical OR: if lhs != 0 result is 1 without evaluating rhs.
// Self-contained — does not push or pop; r0 holds the result on exit.
static void gen_logor_expr(Node *lhs, Node *rhs)
{
    int l_true = new_label();
    int l_end  = new_label();
    gen_expr(lhs);
    ir_append(IR_JNZ, l_true, NULL);   // lhs != 0 → skip rhs
    gen_expr(rhs);
    ir_append(IR_JNZ, l_true, NULL);
    ir_append(IR_IMM, 0, NULL);
    ir_append(IR_J, l_end, NULL);
    ir_append(IR_LABEL, l_true, NULL);
    ir_append(IR_IMM, 1, NULL);
    ir_append(IR_LABEL, l_end, NULL);
}

// Short-circuit logical AND: if lhs == 0 result is 0 without evaluating rhs.
// Self-contained — does not push or pop; r0 holds the result on exit.
static void gen_logand_expr(Node *lhs, Node *rhs)
{
    int l_false = new_label();
    int l_end   = new_label();
    gen_expr(lhs);
    ir_append(IR_JZ, l_false, NULL);   // lhs == 0 → skip rhs
    gen_expr(rhs);
    ir_append(IR_JZ, l_false, NULL);
    ir_append(IR_IMM, 1, NULL);
    ir_append(IR_J, l_end, NULL);
    ir_append(IR_LABEL, l_false, NULL);
    ir_append(IR_IMM, 0, NULL);
    ir_append(IR_LABEL, l_end, NULL);
}
void gen_st(int s)
{
    if (s == 1)      ir_append(IR_SB,    0, NULL);
    else if (s == 2) ir_append(IR_SW,    0, NULL);
    else if (s == 4) ir_append(IR_SL,    0, NULL);
    else             error("gen_st: unsupported size %d\n", s);
}
void gen_ld(int s)
{
    if (s == 1)      ir_append(IR_LB,    0, NULL);
    else if (s == 2) ir_append(IR_LW,    0, NULL);
    else if (s == 4) ir_append(IR_LL,    0, NULL);
    else             error("gen_ld: unsupported size %d\n", s);
}

/* Load with sign-extension for signed 16-bit types (int, short). */
static void gen_ld_t(Type *t)
{
    gen_ld(t->size);
    if (t->size == 2 && (t->base == TB_INT || t->base == TB_SHORT))
        ir_append(IR_SXW, 0, NULL);
}

static void emit_float_bytes(double val)
{
    float f = (float)val;
    char bytes[4];
    memcpy(bytes, &f, 4);
    gen_bytes(bytes, 4);
}

/* Emit integer data of size 2 or 4 bytes in little-endian order. */
static void emit_int_data(long long val, int size)
{
    if (size == 4) {
        char bytes[4];
        bytes[0] = (char)( val        & 0xff);
        bytes[1] = (char)((val >>  8) & 0xff);
        bytes[2] = (char)((val >> 16) & 0xff);
        bytes[3] = (char)((val >> 24) & 0xff);
        gen_bytes(bytes, 4);
    } else {
        ir_append(IR_WORD, (int)(val & 0xffff), NULL);
    }
}
void gen_imm_float(double val)
{
    float f = (float)val;
    unsigned int bits;
    memcpy(&bits, &f, sizeof(bits));
    ir_append(IR_IMM, (int)bits, NULL);
}
void gen_varaddr_from_ident(Node *node, const char *name)
{
    LocalAddr la = find_local_addr(node, name);
    if (la.offset < 0)
    {
        Symbol *sym = node->symbol;
        if (sym && sym->kind == SYM_STATIC_LOCAL)
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "_ls%d", sym->offset);
            ir_append(IR_IMM, 0, arena_strdup(buf));
        }
        else if (sym && sym->kind == SYM_STATIC_GLOBAL)
        {
            char buf[80];
            snprintf(buf, sizeof(buf), "_s%d_%s", sym->tu_index, sym->name);
            ir_append(IR_IMM, 0, arena_strdup(buf));
        }
        else
            ir_append(IR_IMM, 0, name);
    }
    else
    {
        Symbol *sym = find_symbol_st(node->st, name, NS_IDENT);
        /* A scalar local/param is promotable if its address never escapes.
         * Aggregates and function types are never promoted. */
        bool scalar = sym &&
                      !istype_array(sym->type) &&
                      sym->type->base != TB_STRUCT &&
                      sym->type->base != TB_FUNCTION;
        bool promote = scalar && sym && !sym->address_taken;
        const char *lea_sym = promote ? ir_promote_sentinel : name;

        if (la.is_param)
        {
            ir_append(IR_LEA, la.offset, lea_sym);
            // Struct params are passed by pointer (hidden copy ABI): dereference the pointer
            // to get the struct base address, making access transparent to all callers.
            if (sym && sym->type->base == TB_STRUCT)
                gen_ld(WORD_SIZE);
        }
        else
            ir_append(IR_LEA, -la.offset, lea_sym);
    }
}
void gen_varaddr(Node *node)
{
    if (node->kind != ND_IDENT)
        src_error(node->line, node->col, "Expecting local var got %s", nodestr(node->kind));
    gen_varaddr_from_ident(node, node->u.ident.name);
}
void gen_fill(int offset, int size)
{
    int step = 1;
    if (size %2 == 0)
        step = 2;
    for(int i = 0; i < size; i+=step)
    {
        ir_append(IR_LEA, offset + i, NULL);
        ir_append(IR_PUSH,  0, NULL);
        ir_append(IR_IMM, 0, NULL);
        gen_st(step);
    }
}

//--------------------------------------------------------------------------------
// Everything below here uses pseudoinstructions
//--------------------------------------------------------------------------------

// Forward declarations (bodies appear after gen_struct_copy is defined)
static int push_struct_arg(Node *arg, int copybuf_lea);

// Push function call args from a linked list (right-to-left onto stack).
// params: declared parameter list (NULL for unknown/variadic); used to truncate args that
//         are wider than their declared parameter type (e.g. ee_u32 arg → ee_s16 param).
// Returns total bytes pushed (not counting copy buffer allocations, which are tracked via adj_depth).
static int push_args_list(Node *first_arg, Param *params)
{
    // Collect args into a temporary array for right-to-left pushing
    Node *args[64];
    int n = 0;
    for (Node *a = first_arg; a; a = a->next)
    {
        if (n >= 64) error("Too many function arguments");
        args[n++] = a;
    }

    // Collect declared params into a parallel array (may be shorter than args for variadic fns)
    Param *param_arr[64];
    int np = 0;
    for (Param *p = params; p && np < 64; p = p->next)
        param_arr[np++] = p;

    // Phase 1 (right-to-left): allocate copy buffers for struct args via adj.
    //   This must happen before any pushw so that the param-pointer pushws are contiguous.
    //   Record the bp-relative LEA offset of each copy buffer.
    int copybuf_leas[64];
    for (int i = n - 1; i >= 0; i--)
    {
        Node *arg = args[i];
        if (arg->type && arg->type->base == TB_STRUCT)
        {
            int sz = arg->type->size;
            copybuf_leas[i] = -(codegen_ctx.adj_depth + sz);
            ir_append(IR_ADJ, -sz, NULL);
            codegen_ctx.adj_depth += sz;
        }
        else
            copybuf_leas[i] = 0;   // unused for non-struct args
    }

    // Phase 2 (right-to-left): copy each struct arg into its copy buffer.
    //   No adj changes here; only balanced push/pop pairs.
    for (int i = n - 1; i >= 0; i--)
    {
        Node *arg = args[i];
        if (arg->type && arg->type->base == TB_STRUCT)
            push_struct_arg(arg, copybuf_leas[i]);
    }

    // Phase 3 (right-to-left): push all param values (copy buffer ptrs or scalar values).
    int param_size = 0;
    for (int i = n - 1; i >= 0; i--)
    {
        Node *arg = args[i];
        if (arg->type && arg->type->base == TB_STRUCT)
        {
            // Push the copy buffer address as a 2-byte pointer param
            ir_append(IR_LEA, copybuf_leas[i], NULL);
            ir_append(IR_PUSHW, 0, NULL);
            param_size += WORD_SIZE;
        }
        else
        {
            gen_expr(arg);
            // For function call expressions, the value in r0 is the return value,
            // not the function pointer — use return type size for push decision.
            Type *push_type = arg->type;
            if (push_type && istype_function(push_type))
            {
                bool is_call = (arg->kind == ND_IDENT   && arg->u.ident.is_function)   ||
                               (arg->kind == ND_UNARYOP && arg->u.unaryop.is_function) ||
                               (arg->kind == ND_MEMBER  && arg->u.member.is_function);
                if (is_call && push_type->u.fn.ret)
                    push_type = push_type->u.fn.ret;
            }
            int s = push_type ? push_type->size : 0;
            if (s == 0) s = WORD_SIZE;  // function designator decays to pointer size
            int slot_size = (s == 4) ? 4 : WORD_SIZE;

            // When we know the declared parameter type and the arg slot size
            // differs from the param slot size, adjust:
            //   - Wider arg than param (e.g. ee_u32 passed to ee_s16): truncate.
            //   - Narrower arg than param (e.g. int passed to unsigned long): widen.
            if (i < np && param_arr[i] && param_arr[i]->type &&
                param_arr[i]->type->base != TB_STRUCT)
            {
                int ps = param_arr[i]->type->size;
                int param_slot = (ps >= 4) ? 4 : WORD_SIZE;
                if (slot_size == 4 && param_slot == WORD_SIZE)
                {
                    // Truncate 32-bit value to 16 bits: push; immw 0xffff; and
                    ir_append(IR_PUSH, 0, NULL);
                    ir_append(IR_IMM,  0xffff, NULL);
                    ir_append(IR_AND,  0, NULL);
                    slot_size = WORD_SIZE;
                }
                else if (slot_size == WORD_SIZE && param_slot == 4)
                {
                    // Widen 16-bit value to 32 bits for a 4-byte param.
                    // Sign-extend if the arg type is signed, otherwise
                    // the upper 16 bits are already 0 from immw.
                    if (push_type && (push_type->base == TB_INT ||
                                     push_type->base == TB_SHORT ||
                                     push_type->base == TB_CHAR))
                        ir_append(IR_SXW, 0, NULL);
                    slot_size = 4;
                }
            }

            if (slot_size == 4) { ir_append(IR_PUSH,  0, NULL); param_size += 4; }
            else                { ir_append(IR_PUSHW, 0, NULL); param_size += WORD_SIZE; }
        }
    }
    return param_size;
}

// Copy fields of a struct from src to dst.
// src_slot_bp and dst_slot_bp are bp-relative offsets pointing to stack slots
// that hold the base addresses of the source and destination structs respectively.
// These slots are assumed to be already on the stack (pushed before the call).
static void gen_copy_fields(int src_slot_bp, int dst_slot_bp, Field *fields, int base_off)
{
    for (Field *f = fields; f; f = f->next)
    {
        int off = base_off + f->offset;
        if (f->type->base == TB_STRUCT)
        {
            gen_copy_fields(src_slot_bp, dst_slot_bp,
                            f->type->u.composite.members, off);
        }
        else
        {
            // dst field addr: load dst base ptr, add offset
            ir_append(IR_LEA, dst_slot_bp, NULL);
            gen_ld(WORD_SIZE);
            ir_append(IR_PUSH, 0, NULL);
            ir_append(IR_IMM, off, NULL);
            ir_append(IR_ADD, 0, NULL);
            ir_append(IR_PUSH, 0, NULL);

            // src field value: load src base ptr, add offset, load value
            ir_append(IR_LEA, src_slot_bp, NULL);
            gen_ld(WORD_SIZE);
            ir_append(IR_PUSH, 0, NULL);
            ir_append(IR_IMM, off, NULL);
            ir_append(IR_ADD, 0, NULL);
            gen_ld(f->type->size);

            gen_st(f->type->size);
        }
    }
}

static void gen_struct_copy(int src_slot_bp, int dst_slot_bp, Type *st)
{
    gen_copy_fields(src_slot_bp, dst_slot_bp, st->u.composite.members, 0);
}

// Copy a struct arg into its pre-allocated copy buffer.
// copybuf_lea: bp-relative LEA offset of the copy buffer (allocated in phase 1 of push_args_list).
// At this point: sp = bp - adj_depth (no extra non-adj pushes in flight).
// The copy uses balanced push/pop pairs, leaving sp and adj_depth unchanged on exit.
static int push_struct_arg(Node *arg, int copybuf_lea)
{
    int adj_before = codegen_ctx.adj_depth;

    // Evaluate the source struct: r0 = src base address.
    // adj_depth may grow if gen_expr allocates a retbuf for a struct-returning call.
    gen_expr(arg);
    ir_append(IR_PUSH, 0, NULL);                          // stack: [src_addr]
    // sp = bp - adj_depth - 4 (adj_depth may have grown)
    int src_slot = -(codegen_ctx.adj_depth + 4);

    ir_append(IR_LEA, copybuf_lea, NULL);                 // r0 = copy buffer address
    ir_append(IR_PUSH, 0, NULL);                          // stack: [src_addr, dst_addr]
    int dst_slot = -(codegen_ctx.adj_depth + 8);

    gen_struct_copy(src_slot, dst_slot, arg->type);

    ir_append(IR_ADJ, 8, NULL);                           // pop the two address slots

    // Free any retbuf allocated by gen_expr(arg)
    int retbuf_sz = codegen_ctx.adj_depth - adj_before;
    if (retbuf_sz > 0)
    {
        ir_append(IR_ADJ, retbuf_sz, NULL);
        codegen_ctx.adj_depth -= retbuf_sz;
    }

    return 0;   // no param_size contribution here (pushw is done in phase 3 of push_args_list)
}

// Indirect call through function pointer variable: fp(args)
void gen_callfunction_via_ptr(Node *node)
{
    // Check if function pointer returns a struct
    Type *fn_type = node->symbol->type->u.ptr.pointee;
    Type *ret_type = fn_type->u.fn.ret;
    bool struct_ret = (ret_type && ret_type->base == TB_STRUCT);
    int retbuf_bp_off = 0;
    if (struct_ret)
    {
        int sz = ret_type->size;
        retbuf_bp_off = -(codegen_ctx.adj_depth + sz);
        codegen_ctx.adj_depth += sz;
        ir_append(IR_ADJ, -sz, NULL);
    }
    int adj_before_args = codegen_ctx.adj_depth;
    int param_size = push_args_list(node->ch[0], fn_type->u.fn.params);   // user args (right-to-left)
    if (struct_ret)
    {
        ir_append(IR_LEA, retbuf_bp_off, NULL);
        ir_append(IR_PUSHW, 0, NULL);
        param_size += WORD_SIZE;
    }
    gen_varaddr(node);
    gen_ld(node->symbol->type->size);
    ir_append(IR_JLI,   0, NULL);
    ir_append(IR_ADJ, param_size, NULL);
    // Free copy buffers allocated by push_struct_arg calls
    int copybuf_sz = codegen_ctx.adj_depth - adj_before_args;
    if (copybuf_sz > 0)
    {
        ir_append(IR_ADJ, copybuf_sz, NULL);
        codegen_ctx.adj_depth -= copybuf_sz;
    }
    if (struct_ret)
        ir_append(IR_LEA, retbuf_bp_off, NULL);
}

// Indirect call through dereferenced pointer: (*fp)(args)
void gen_callfunction_via_deref(Node *node)
{
    // Determine return type via operand's pointee function type
    Type *ptr_type = node->ch[0]->type;
    Type *fn_type  = (ptr_type && istype_ptr(ptr_type)) ? ptr_type->u.ptr.pointee : ptr_type;
    Type *ret_type = (fn_type && istype_function(fn_type)) ? fn_type->u.fn.ret : NULL;
    bool struct_ret = (ret_type && ret_type->base == TB_STRUCT);
    int retbuf_bp_off = 0;
    if (struct_ret)
    {
        int sz = ret_type->size;
        retbuf_bp_off = -(codegen_ctx.adj_depth + sz);
        codegen_ctx.adj_depth += sz;
        ir_append(IR_ADJ, -sz, NULL);
    }
    int adj_before_args = codegen_ctx.adj_depth;
    Param *deref_params = (fn_type && istype_function(fn_type)) ? fn_type->u.fn.params : NULL;
    int param_size = push_args_list(node->ch[1], deref_params);   // user args (right-to-left)
    if (struct_ret)
    {
        ir_append(IR_LEA, retbuf_bp_off, NULL);
        ir_append(IR_PUSHW, 0, NULL);
        param_size += WORD_SIZE;
    }
    gen_expr(node->ch[0]);                           // operand: function pointer value or address
    // For (*fp)(args): gen_expr(ND_IDENT "fp") already loads the 2-byte fp value.
    // For arr[i](args): ch[0] is ND_BINOP "+" (subscript address arithmetic), so gen_expr
    // produces the address of the array slot — load the function pointer from there.
    if (node->ch[0]->kind == ND_BINOP)
        gen_ld(WORD_SIZE);
    ir_append(IR_JLI,   0, NULL);
    ir_append(IR_ADJ, param_size, NULL);
    // Free copy buffers allocated by push_struct_arg calls
    int copybuf_sz = codegen_ctx.adj_depth - adj_before_args;
    if (copybuf_sz > 0)
    {
        ir_append(IR_ADJ, copybuf_sz, NULL);
        codegen_ctx.adj_depth -= copybuf_sz;
    }
    if (struct_ret)
        ir_append(IR_LEA, retbuf_bp_off, NULL);
}

void gen_callfunction(Node *node)
{
    // __putchar(c) is a CPU builtin
    if (node->symbol->kind == SYM_BUILTIN)
    {
        if (!node->ch[0] || node->ch[0]->next)
            src_error(node->line, node->col, "__putchar requires exactly 1 argument");
        gen_expr(node->ch[0]);
        ir_append(IR_PUTCHAR, 0, NULL);
        return;
    }
    // Check if function returns a struct (hidden retbuf ABI)
    Type *ret_type = node->symbol->type->u.fn.ret;
    bool struct_ret = (ret_type && ret_type->base == TB_STRUCT);
    int retbuf_bp_off = 0;  // bp-relative offset of the retbuf (only valid if struct_ret)
    if (struct_ret)
    {
        // Allocate retbuf on the caller's stack frame.
        int sz = ret_type->size;
        retbuf_bp_off = -(codegen_ctx.adj_depth + sz);
        codegen_ctx.adj_depth += sz;
        ir_append(IR_ADJ, -sz, NULL);
    }
    // Push the params on backwards, the offsets from the symbol table then work
    int adj_before_args = codegen_ctx.adj_depth;
    int param_size = push_args_list(node->ch[0], node->symbol->type->u.fn.params);   // user args (right-to-left)
    if (struct_ret)
    {
        // Push hidden retbuf pointer as the implicit first argument (pushed last,
        // so it occupies bp+FRAME_OVERHEAD in the callee's frame).
        ir_append(IR_LEA, retbuf_bp_off, NULL);
        ir_append(IR_PUSHW, 0, NULL);
        param_size += WORD_SIZE;
    }
    if (node->symbol->kind == SYM_STATIC_GLOBAL)
    {
        char mangled[80];
        snprintf(mangled, sizeof(mangled), "_s%d_%s",
                 node->symbol->tu_index, node->symbol->name);
        ir_append(IR_JL, 0, arena_strdup(mangled));
    }
    else
        ir_append(IR_JL, 0, node->symbol->name);
    // Reclaim pushed args (and hidden ptr if struct_ret)
    ir_append(IR_ADJ, param_size, NULL);
    // Free copy buffers allocated by push_struct_arg calls
    int copybuf_sz = codegen_ctx.adj_depth - adj_before_args;
    if (copybuf_sz > 0)
    {
        ir_append(IR_ADJ, copybuf_sz, NULL);
        codegen_ctx.adj_depth -= copybuf_sz;
    }
    if (struct_ret)
    {
        // r0 = address of retbuf (still allocated on caller's stack)
        ir_append(IR_LEA, retbuf_bp_off, NULL);
    }
}
static const char *node_ident_str(Node *node)
{
    if (!node) return "";
    if (node->kind == ND_IDENT)
        return node->u.ident.name ? node->u.ident.name : "";
    if (node->kind == ND_GOTOSTMT)
        return node->u.label ? node->u.label : "";
    if (node->kind == ND_LABELSTMT)
        return node->u.labelstmt.name ? node->u.labelstmt.name : "";
    return "";
}

void gen_offset(Node *node)
{
    // lhs is structure, rhs is member within
    ir_append(IR_IMM, node->u.member.offset, NULL);
}
void gen_addr(Node *node)
{
    if (node->kind == ND_IDENT)
    {
        gen_varaddr(node);
    }
    else if (node->kind == ND_UNARYOP && node->op_kind == TK_STAR)
    {
        gen_expr(node->ch[0]);   // operand
    }
    else if (node->kind == ND_MEMBER)
    {
        if (node->op_kind == TK_ARROW)
            gen_expr(node->ch[0]);    // base: load pointer value
        else
            gen_addr(node->ch[0]);    // base: struct base address
        ir_append(IR_PUSH,  0, NULL);
        gen_offset(node);
        ir_append(IR_ADD,   0, NULL);
    }
    else
        src_error(node->line, node->col, "Expecting lvalue");
}
typedef enum
{
    TC_INVALID = 0,
    TC_S1,   // char           (1-byte signed)
    TC_U1,   // uchar          (1-byte unsigned)
    TC_S2,   // short, int     (2-byte signed)
    TC_U2,   // ushort, uint, pointer, enum  (2-byte unsigned)
    TC_S4,   // long           (4-byte signed)
    TC_U4,   // ulong          (4-byte unsigned)
    TC_FP,   // float, double  (4-byte IEEE 754)
} TypeClass;

static const TypeClass type_class_table[] =
{
    [TB_VOID]     = TC_INVALID,
    [TB_CHAR]     = TC_S1,
    [TB_UCHAR]    = TC_U1,
    [TB_SHORT]    = TC_S2,
    [TB_USHORT]   = TC_U2,
    [TB_INT]      = TC_S2,
    [TB_UINT]     = TC_U2,
    [TB_LONG]     = TC_S4,
    [TB_ULONG]    = TC_U4,
    [TB_FLOAT]    = TC_FP,
    [TB_DOUBLE]   = TC_FP,
    [TB_POINTER]  = TC_U2,
    [TB_ARRAY]    = TC_INVALID,
    [TB_FUNCTION] = TC_INVALID,
    [TB_STRUCT]   = TC_INVALID,
    [TB_ENUM]     = TC_U2,
};

typedef enum
{
    COP_NOOP = 0,
    COP_SXB,    // IR_SXB
    COP_SXW,    // IR_SXW
    COP_T8,     // push; immw 0xff;   and
    COP_T16,    // push; immw 0xffff; and
    COP_ITOF_1, // IR_SXB; IR_ITOF
    COP_ITOF_2, // IR_SXW; IR_ITOF
    COP_ITOF_4, // IR_ITOF
    COP_FTOI_1, // IR_FTOI; push; immw 0xff;   and
    COP_FTOI_2, // IR_FTOI; push; immw 0xffff; and
    COP_FTOI_4, // IR_FTOI
} CastOp;

#define N   COP_NOOP
#define B   COP_SXB
#define W   COP_SXW
#define T8  COP_T8
#define T16 COP_T16
#define I1  COP_ITOF_1
#define I2  COP_ITOF_2
#define I4  COP_ITOF_4
#define F1  COP_FTOI_1
#define F2  COP_FTOI_2
#define F4  COP_FTOI_4

//                           dst→  INV  S1   U1   S2   U2   S4   U4   FP
static const CastOp cast_table[8][8] =
{
    /* src INV */ {                 N,   N,   N,   N,   N,   N,   N,   N  },
    /* src S1  */ {                 N,   N,   N,   B,   B,   B,   B,   I1 },
    /* src U1  */ {                 N,   N,   N,   N,   N,   T16, T16, I1 },
    /* src S2  */ {                 N,   T8,  T8,  N,   N,   W,   W,   I2 },
    /* src U2  */ {                 N,   T8,  T8,  N,   N,   T16, T16, I2 },
    /* src S4  */ {                 N,   T8,  T8,  T16, T16, N,   N,   I4 },
    /* src U4  */ {                 N,   T8,  T8,  T16, T16, N,   N,   I4 },
    /* src FP  */ {                 N,   F1,  F1,  F2,  F2,  F4,  F4,  N  },
};

#undef N
#undef B
#undef W
#undef T8
#undef T16
#undef I1
#undef I2
#undef I4
#undef F1
#undef F2
#undef F4

static void emit_cast_op(CastOp op)
{
    switch (op)
    {
    case COP_NOOP:   return;
    case COP_SXB:    ir_append(IR_SXB,  0, NULL); return;
    case COP_SXW:    ir_append(IR_SXW,  0, NULL); return;
    case COP_T8:
        ir_append(IR_PUSH, 0, NULL); ir_append(IR_IMM, 0xff,   NULL); ir_append(IR_AND, 0, NULL); return;
    case COP_T16:
        ir_append(IR_PUSH, 0, NULL); ir_append(IR_IMM, 0xffff, NULL); ir_append(IR_AND, 0, NULL); return;
    case COP_ITOF_1: ir_append(IR_SXB,  0, NULL); ir_append(IR_ITOF, 0, NULL); return;
    case COP_ITOF_2: ir_append(IR_SXW,  0, NULL); ir_append(IR_ITOF, 0, NULL); return;
    case COP_ITOF_4: ir_append(IR_ITOF, 0, NULL); return;
    case COP_FTOI_1:
        ir_append(IR_FTOI, 0, NULL); ir_append(IR_PUSH, 0, NULL); ir_append(IR_IMM, 0xff,   NULL); ir_append(IR_AND, 0, NULL); return;
    case COP_FTOI_2:
        ir_append(IR_FTOI, 0, NULL); ir_append(IR_PUSH, 0, NULL); ir_append(IR_IMM, 0xffff, NULL); ir_append(IR_AND, 0, NULL); return;
    case COP_FTOI_4: ir_append(IR_FTOI, 0, NULL); return;
    }
}

void gen_cast(Type *src, Type *dst)
{
    if (src == dst) return;
    TypeClass sc = (src->base < (int)(sizeof type_class_table / sizeof *type_class_table))
                   ? type_class_table[src->base] : TC_INVALID;
    TypeClass dc = (dst->base < (int)(sizeof type_class_table / sizeof *type_class_table))
                   ? type_class_table[dst->base] : TC_INVALID;
    emit_cast_op(cast_table[sc][dc]);
}
// Emit the arithmetic/comparison instruction for binary op `op`.
// Assumes lhs is already on the stack and rhs is in r0.
static void gen_arith_op(Token_kind op, bool is_float, bool is_signed)
{
    if (is_float)
    {
        switch (op)
        {
            case TK_PLUS:  ir_append(IR_FADD,  0, NULL); return;
            case TK_MINUS: ir_append(IR_FSUB,  0, NULL); return;
            case TK_STAR:  ir_append(IR_FMUL,  0, NULL); return;
            case TK_SLASH: ir_append(IR_FDIV,  0, NULL); return;
            case TK_LT:    ir_append(IR_FLT,   0, NULL);  return;
            case TK_LE:    ir_append(IR_FLE,   0, NULL);  return;
            case TK_GT:    ir_append(IR_FGT,   0, NULL);  return;
            case TK_GE:    ir_append(IR_FGE,   0, NULL);  return;
            /* No feq/fne on CPU3 — bit-pattern equality is correct for non-NaN IEEE 754 */
            case TK_EQ:    ir_append(IR_EQ,    0, NULL);  return;
            case TK_NE:    ir_append(IR_NE,    0, NULL);  return;
            default: error("fp op_kind %d not handled in codegen", op);
        }
    }
    switch (op)
    {
        case TK_PLUS:      ir_append(IR_ADD,   0, NULL);    return;
        case TK_MINUS:     ir_append(IR_SUB,   0, NULL);    return;
        case TK_STAR:      ir_append(IR_MUL,   0, NULL);    return;
        case TK_SLASH:     ir_append(is_signed ? IR_DIVS : IR_DIV, 0, NULL); return;
        case TK_LT:        ir_append(is_signed ? IR_LTS  : IR_LT,  0, NULL); return;
        case TK_LE:        ir_append(is_signed ? IR_LES  : IR_LE,  0, NULL); return;
        case TK_GT:        ir_append(is_signed ? IR_GTS  : IR_GT,  0, NULL); return;
        case TK_GE:        ir_append(is_signed ? IR_GES  : IR_GE,  0, NULL); return;
        case TK_EQ:        ir_append(IR_EQ,    0, NULL);     return;
        case TK_NE:        ir_append(IR_NE,    0, NULL);     return;
        case TK_SHIFTR:    ir_append(is_signed ? IR_SHRS : IR_SHR, 0, NULL); return;
        case TK_SHIFTL:    ir_append(IR_SHL,   0, NULL); return;
        case TK_BITOR:     ir_append(IR_OR,    0, NULL);  return;
        case TK_BITXOR:    ir_append(IR_XOR,   0, NULL); return;
        case TK_AMPERSAND: ir_append(IR_AND,   0, NULL); return;
        case TK_PERCENT:   ir_append(is_signed ? IR_MODS : IR_MOD, 0, NULL); return;
        default: error("op_kind %d not handled in codegen", op);
    }
}

void gen_expr(Node *node)
{
    // ===== Binary operators =====
    if (node->kind == ND_BINOP)
    {
        Node *lhs = node->ch[0];
        Node *rhs = node->ch[1];
        if (node->op_kind == TK_COMMA)
        {
            gen_expr(lhs);    // evaluate for side effects
            gen_expr(rhs);    // result is rhs
            return;
        }
        // Short-circuit operators must be handled before the lhs push.
        if (node->op_kind == TK_LOGOR)  { gen_logor_expr(lhs, rhs);  return; }
        if (node->op_kind == TK_LOGAND) { gen_logand_expr(lhs, rhs); return; }

        gen_expr(lhs);
        ir_append(IR_PUSH,  0, NULL);
        gen_expr(rhs);
        gen_arith_op(node->op_kind, istype_fp(lhs->type), istype_signed(lhs->type));
        return;
    }
    // ===== Literals =====
    else if (node->kind == ND_LITERAL)
    {
        if (node->u.literal.strval)
        {
            int sid = new_strlit(node->u.literal.strval, node->u.literal.strval_len);
            char buf[32];
            snprintf(buf, sizeof(buf), "_l%d", sid);
            ir_append(IR_IMM, 0, arena_strdup(buf));
        }
        else if (istype_fp(node->type))
            gen_imm_float(node->u.literal.fval);
        else
            ir_append(IR_IMM, (int)node->u.literal.ival, NULL);
    }
    // ===== Identifiers and function calls =====
    else if (node->kind == ND_IDENT)
    {
        Symbol *sym = node->symbol;
        if (sym->kind == SYM_ENUM_CONST)
        {
            // Enum constant: inline the integer value; no memory load.
            ir_append(IR_IMM, sym->offset, NULL);
        }
        else if (node->u.ident.is_function)
        {
            // Call context: the identifier appears as the callee of a call expression.
            if (istype_function(sym->type))
                gen_callfunction(node);          // direct call: myfunc(args)
            else if (istype_ptr(sym->type))
                gen_callfunction_via_ptr(node);  // indirect call via ptr: fp(args)
        }
        else
        {
            // Value context: load or address of the variable.
            gen_varaddr(node);
            // Don't fetch from address if array/function/struct (name IS the address).
            if (!istype_array(sym->type) && !istype_function(sym->type)
                && sym->type->base != TB_STRUCT)
                gen_ld_t(sym->type);
        }
    }
    // ===== Unary operators =====
    else if (node->kind == ND_UNARYOP)
    {
        Node *operand = node->ch[0];   // operand
        switch (node->op_kind)
        {
        case TK_PLUS:
            gen_expr(operand);
            break;
        case TK_MINUS:
            if (istype_fp(operand->type))
            {
                gen_imm_float(0.0);
                ir_append(IR_PUSH,  0, NULL);
                gen_expr(operand);
                ir_append(IR_FSUB,  0, NULL);
            }
            else
            {
                ir_append(IR_IMM, 0, NULL);
                ir_append(IR_PUSH,  0, NULL);
                gen_expr(operand);
                ir_append(IR_SUB,   0, NULL);
            }
            break;
        case TK_STAR:
            if (node->u.unaryop.is_function)
            {
                // (*fp)(args) — indirect call through dereferenced pointer
                gen_callfunction_via_deref(node);
            }
            else
            {
                gen_expr(operand);
                gen_ld_t(node->type);
            }
            break;
        case TK_AMPERSAND:
            gen_addr(operand);
            break;
        case TK_BANG:
            gen_expr(operand);
            ir_append(IR_PUSH,  0, NULL);
            ir_append(IR_IMM, 0, NULL);
            ir_append(IR_EQ,    0, NULL);
            break;
        case TK_TILDE:
            gen_expr(operand);
            ir_append(IR_PUSH,  0, NULL);
            ir_append(IR_IMM, 0xffffffff, NULL);
            ir_append(IR_XOR,   0, NULL);
            break;
        // Prefix increment/decrement: ++x / --x
        // Result: new value of x
        case TK_INC: case TK_DEC:
        {
            int is_inc = (node->op_kind == TK_INC);
            int sz = operand->type ? operand->type->size : 2;
            // For pointer types, stride = sizeof(*ptr); for scalars stride = 1
            int stride = 1;
            if (operand->type && (operand->type->base == TB_POINTER || operand->type->base == TB_ARRAY))
                stride = elem_type(operand->type)->size;
            gen_addr(operand);
            ir_append(IR_PUSH,  0, NULL);         // stack: [&x]
            gen_expr(operand);
            ir_append(IR_PUSH,  0, NULL);         // stack: [&x, x]
            ir_append(IR_IMM, stride, NULL);
            if (is_inc) ir_append(IR_ADD,   0, NULL); else ir_append(IR_SUB,   0, NULL);  // r0 = x±stride, stack: [&x]
            gen_st(sz);
            // r0 still holds x±stride (sw/sb/sl don't modify r0)
            break;
        }
        // Postfix increment/decrement: x++ / x--
        // Result: old value of x
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wswitch"
        case TK_POST_INC: case TK_POST_DEC:
        {
            int is_inc = (node->op_kind == TK_POST_INC);
            int sz = operand->type ? operand->type->size : 2;
            // For pointer types, stride = sizeof(*ptr); for scalars stride = 1
            int stride = 1;
            if (operand->type && (operand->type->base == TB_POINTER || operand->type->base == TB_ARRAY))
                stride = elem_type(operand->type)->size;
            gen_expr(operand);    // r0 = old_x
            ir_append(IR_PUSH,  0, NULL);         // stack: [old_x]
            gen_addr(operand);    // r0 = &x
            ir_append(IR_PUSH,  0, NULL);         // stack: [old_x, &x]
            gen_expr(operand);    // r0 = x (reload)
            ir_append(IR_PUSH,  0, NULL);         // stack: [old_x, &x, x]
            ir_append(IR_IMM, stride, NULL);
            if (is_inc) ir_append(IR_ADD,   0, NULL); else ir_append(IR_SUB,   0, NULL);  // r0 = x±stride, stack: [old_x, &x]
            gen_st(sz);
            // stack: [old_x], r0 = x±stride
            ir_append(IR_POP,   0, NULL);          // r0 = old_x, stack: []
            break;
        }
        #pragma clang diagnostic pop
        default: src_error(node->line, node->col, "unary op_kind %d not handled in codegen", node->op_kind);
        }
    }
    // ===== Assignment =====
    else if (node->kind == ND_ASSIGN)
    {
        Node *lhs = node->ch[0];
        Node *rhs = node->ch[1];
        if (lhs->type->base == TB_STRUCT)
        {
            // Struct assignment: copy field by field.
            int adj_before = codegen_ctx.adj_depth;
            gen_expr(rhs);                                            // r0 = rhs struct addr
            ir_append(IR_PUSH, 0, NULL);
            int src_slot = -(codegen_ctx.adj_depth + 4);             // slot for src ptr
            gen_addr(lhs);                                            // r0 = lhs struct addr
            ir_append(IR_PUSH, 0, NULL);
            int dst_slot = -(codegen_ctx.adj_depth + 8);             // slot for dst ptr
            gen_struct_copy(src_slot, dst_slot, lhs->type);
            ir_append(IR_ADJ, 8, NULL);                               // free the two pushed slots
            // Free any retbuf allocated by gen_expr(rhs)
            int retbuf_sz = codegen_ctx.adj_depth - adj_before;
            if (retbuf_sz > 0)
            {
                ir_append(IR_ADJ, retbuf_sz, NULL);
                codegen_ctx.adj_depth -= retbuf_sz;
            }
            gen_addr(lhs);                                            // r0 = lhs addr (expression result)
        }
        else
        {
            gen_addr(lhs);
            ir_append(IR_PUSH,  0, NULL);
            gen_expr(rhs);
            gen_st(lhs->type->size);
        }
    }
    // ===== Compound assignment =====
    else if (node->kind == ND_COMPOUND_ASSIGN)
    {
        // Evaluate LHS address exactly once, then: load old value, compute rhs, apply op, store.
        // Stack discipline:
        //   gen_addr(lhs)  → r0 = addr
        //   push           → [addr]
        //   gen_ld(sz)     → r0 = *addr  (lb/lw/ll uses r0; stack unchanged)
        //   push           → [addr, old_val]
        //   gen_expr(rhs)  → r0 = rhs
        //   op             → r0 = old_val op rhs; stack: [addr]
        //   store          → mem[addr] = r0; stack: []
        Node *lhs = node->ch[0];
        Node *rhs = node->ch[1];
        int sz = lhs->type ? lhs->type->size : 2;
        gen_addr(lhs);
        ir_append(IR_PUSH,  0, NULL);
        gen_ld(sz);
        ir_append(IR_PUSH,  0, NULL);
        gen_expr(rhs);
        gen_arith_op(node->op_kind, istype_fp(lhs->type), istype_signed(lhs->type));
        gen_st(sz);
    }
    // ===== Cast =====
    else if (node->kind == ND_CAST)
    {
        // ch[0]=type_decl (not evaluated), ch[1]=expr to cast
        gen_expr(node->ch[1]);
        gen_cast(node->ch[1]->type, node->type);
    }
    // ===== Member access =====
    else if (node->kind == ND_MEMBER && node->u.member.is_function)
    {
        // Call through a function-pointer struct member: s.fp(args) or s->fp(args)
        int param_size = push_args_list(node->ch[1], NULL);   // args list (fn-ptr member: param types unknown)
        gen_addr(node);
        gen_ld(WORD_SIZE);  // load function pointer (pointer-sized)
        ir_append(IR_JLI,   0, NULL);
        ir_append(IR_ADJ, param_size, NULL);
    }
    else if (node->kind == ND_MEMBER)
    {
        gen_addr(node);         // handles both . and ->
        if (node->type->base != TB_STRUCT && node->type->base != TB_ARRAY)
            gen_ld_t(node->type);
    }
    // ===== Ternary =====
    else if (node->kind == ND_TERNARY)
    {
        Node *cond  = node->ch[0];
        Node *then_ = node->ch[1];
        Node *else_ = node->ch[2];
        int l_else = new_label();
        int l_end  = new_label();
        gen_expr(cond);
        ir_append(IR_JZ, l_else, NULL);
        gen_expr(then_);
        ir_append(IR_J, l_end, NULL);
        ir_append(IR_LABEL, l_else, NULL);
        gen_expr(else_);
        ir_append(IR_LABEL, l_end, NULL);
    }
    // ===== VA_START / VA_ARG / VA_END =====
    else if (node->kind == ND_VA_START)
    {
        // va_start(ap, last_param)
        // ap = bp + last_param_offset + last_param_size
        Node *ap_node  = node->ch[0];   // ap
        Node *lp_node  = node->ch[1];   // last
        int param_off  = lp_node->symbol->offset;
        int param_size = lp_node->symbol->type->size;
        gen_addr(ap_node);                       // r0 = &ap
        ir_append(IR_PUSH,  0, NULL);                              // stack: [&ap]
        ir_append(IR_LEA, param_off + param_size, NULL);         // r0 = bp + first_vararg_offset
        ir_append(IR_SW,    0, NULL);                                // mem[&ap] = first_vararg_addr
    }
    else if (node->kind == ND_VA_ARG)
    {
        // va_arg(ap, T) where T has size s
        // Returns *old_ap, advances ap by s
        int s         = node->type->size;
        Node *ap_node = node->ch[0];   // ap
        // Save old ap value
        gen_addr(ap_node);    // r0 = &ap
        gen_ld(WORD_SIZE);    // r0 = ap (current vararg pointer)
        ir_append(IR_PUSH,  0, NULL);           // stack: [old_ap]
        // Advance ap: ap = ap + s
        gen_addr(ap_node);    // r0 = &ap
        ir_append(IR_PUSH,  0, NULL);           // stack: [old_ap, &ap]
        gen_addr(ap_node);    // r0 = &ap
        gen_ld(WORD_SIZE);    // r0 = ap
        ir_append(IR_PUSH,  0, NULL);           // stack: [old_ap, &ap, ap]
        int slot = s;
        ir_append(IR_IMM, slot, NULL);        // r0 = slot size
        ir_append(IR_ADD,   0, NULL);            // r0 = ap + slot; stack: [old_ap, &ap]
        ir_append(IR_SW,    0, NULL);             // mem[&ap] = ap + s; stack: [old_ap]
        // Load vararg from old_ap
        ir_append(IR_POP,   0, NULL);            // r0 = old_ap; stack: []
        gen_ld_t(node->type);                    // r0 = *(old_ap) = vararg value (sign-extends signed types)
    }
    else if (node->kind == ND_VA_END)
    {
        // va_end is a no-op
        (void)node;
    }
}
void gen_returnstmt(Node *node)
{
    Node *expr = node->ch[0];   // return expr (NULL if bare return)
    Type *ret  = codegen_ctx.current_fn_ret_type;
    if (expr && ret && ret->base == TB_STRUCT)
    {
        // Struct return: copy src struct to caller's retbuf.
        int adj_before = codegen_ctx.adj_depth;
        gen_expr(expr);                              // r0 = src struct addr
        ir_append(IR_PUSH, 0, NULL);
        int src_slot = -(codegen_ctx.adj_depth + 4);
        // Load the hidden retbuf ptr from bp+FRAME_OVERHEAD (callee's first param slot)
        ir_append(IR_LEA, FRAME_OVERHEAD, NULL);
        gen_ld(WORD_SIZE);                           // r0 = caller's retbuf addr
        ir_append(IR_PUSH, 0, NULL);
        int dst_slot = -(codegen_ctx.adj_depth + 8);
        gen_struct_copy(src_slot, dst_slot, ret);
        ir_append(IR_ADJ, 8, NULL);                  // free the two pushed slots
        // Free any retbuf allocated by gen_expr(expr)
        int retbuf_sz = codegen_ctx.adj_depth - adj_before;
        if (retbuf_sz > 0)
        {
            ir_append(IR_ADJ, retbuf_sz, NULL);
            codegen_ctx.adj_depth -= retbuf_sz;
        }
    }
    else if (expr)
    {
        gen_expr(expr);
    }
    ir_append(IR_RET, 0, NULL);
}
void gen_ifstmt(Node *node)
{
    // Structure is expr, stmt, [stmt]
    Node *cond = node->ch[0];
    Node *then_ = node->ch[1];
    Node *else_ = node->ch[2];
    int l_else = new_label();
    gen_expr(cond);
    ir_append(IR_JZ, l_else, NULL);
    gen_stmt(then_);
    if (else_)
    {
        int l_end = new_label();
        ir_append(IR_J, l_end, NULL);
        ir_append(IR_LABEL, l_else, NULL);
        gen_stmt(else_);
        ir_append(IR_LABEL, l_end, NULL);
    }
    else
        ir_append(IR_LABEL, l_else, NULL);
}
void gen_whilestmt(Node *node)
{
    if (codegen_ctx.loop_depth >= 64) error("too deeply nested");
    Node *cond = node->ch[0];
    Node *body = node->ch[1];
    int lloop   = new_label();
    int lbreak  = new_label();
    codegen_ctx.break_labels[codegen_ctx.loop_depth] = lbreak;
    codegen_ctx.cont_labels[codegen_ctx.loop_depth]  = lloop;
    codegen_ctx.loop_adj[codegen_ctx.loop_depth]     = codegen_ctx.adj_depth;
    codegen_ctx.loop_depth++;
    ir_append(IR_LABEL, lloop, NULL);
    gen_expr(cond);
    ir_append(IR_JZ, lbreak, NULL);
    gen_stmt(body);
    ir_append(IR_J, lloop, NULL);
    ir_append(IR_LABEL, lbreak, NULL);
    codegen_ctx.loop_depth--;
}
void gen_decl(Node *node);  // forward declaration
void gen_forstmt(Node *node)
{
    if (codegen_ctx.loop_depth >= 64) error("too deeply nested");
    Node *init = node->ch[0];
    Node *cond = node->ch[1];
    Node *inc  = node->ch[2];
    Node *body = node->ch[3];
    // If the init was a declaration, node->symtable holds the for-init scope.
    // CPU4: round up to even to keep sp 2-byte aligned.
    int for_init_sz = node->symtable ? node->symtable->size : 0;
    if (g_target_arch == 4 && (for_init_sz & 1)) for_init_sz++;
    if (node->symtable)
    {
        codegen_ctx.adj_depth += for_init_sz;
        ir_append(IR_ADJ, -for_init_sz, NULL);
    }
    if (init->kind == ND_DECLARATION)
        gen_decl(init);
    else if (init->kind != ND_EMPTY)
        gen_expr(init);
    int lloop  = new_label();
    int lcont  = new_label();
    int lbreak = new_label();
    codegen_ctx.break_labels[codegen_ctx.loop_depth] = lbreak;
    codegen_ctx.cont_labels[codegen_ctx.loop_depth]  = lcont;
    codegen_ctx.loop_adj[codegen_ctx.loop_depth]     = codegen_ctx.adj_depth;
    codegen_ctx.loop_depth++;
    ir_append(IR_LABEL, lloop, NULL);
    if (cond->kind != ND_EMPTY) {
        gen_expr(cond);
        ir_append(IR_JZ, lbreak, NULL);
    }
    gen_stmt(body);
    ir_append(IR_LABEL, lcont, NULL);
    if (inc->kind != ND_EMPTY)
        gen_expr_discard(inc);
    ir_append(IR_J, lloop, NULL);
    ir_append(IR_LABEL, lbreak, NULL);
    codegen_ctx.loop_depth--;
    if (node->symtable)
    {
        ir_append(IR_ADJ, for_init_sz, NULL);
        codegen_ctx.adj_depth -= for_init_sz;
    }
}
void gen_dowhilestmt(Node *node)
{
    if (codegen_ctx.loop_depth >= 64) error("too deeply nested");
    Node *body = node->ch[0];
    Node *cond = node->ch[1];
    int lloop  = new_label();
    int lcont  = new_label();
    int lbreak = new_label();
    codegen_ctx.break_labels[codegen_ctx.loop_depth] = lbreak;
    codegen_ctx.cont_labels[codegen_ctx.loop_depth]  = lcont;
    codegen_ctx.loop_adj[codegen_ctx.loop_depth]     = codegen_ctx.adj_depth;
    codegen_ctx.loop_depth++;
    ir_append(IR_LABEL, lloop, NULL);
    gen_stmt(body);
    ir_append(IR_LABEL, lcont, NULL);
    gen_expr(cond);
    ir_append(IR_JNZ, lloop, NULL);
    ir_append(IR_LABEL, lbreak, NULL);
    codegen_ctx.loop_depth--;
}
void gen_breakstmt(Node *node)
{
    if (codegen_ctx.loop_depth == 0)
        src_error(node->line, node->col, "break outside loop or switch");
    int lev     = codegen_ctx.loop_depth - 1;
    int cleanup = codegen_ctx.adj_depth - codegen_ctx.loop_adj[lev];
    if (cleanup > 0) ir_append(IR_ADJ, cleanup, NULL);
    ir_append(IR_J, codegen_ctx.break_labels[lev], NULL);
}
void gen_continuestmt(Node *node)
{
    /* Walk up the loop/switch stack to find the nearest true loop.
       switch sets cont_labels[depth] = -1; only loops set a real label. */
    for (int i = codegen_ctx.loop_depth - 1; i >= 0; i--)
    {
        if (codegen_ctx.cont_labels[i] >= 0)
        {
            int cleanup = codegen_ctx.adj_depth - codegen_ctx.loop_adj[i];
            if (cleanup > 0) ir_append(IR_ADJ, cleanup, NULL);
            ir_append(IR_J, codegen_ctx.cont_labels[i], NULL);
            return;
        }
    }
    src_error(node->line, node->col, "continue outside loop");
}
/* Generate a post-increment/decrement as a pure side effect (result discarded).
 * Equivalent to pre-increment: avoids saving/restoring the old value. */
static void gen_postinc_stmt(Node *node)
{
    Node *operand = node->ch[0];
    int sz = operand->type ? operand->type->size : 2;
    int stride = 1;
    if (operand->type && (operand->type->base == TB_POINTER ||
                          operand->type->base == TB_ARRAY))
        stride = elem_type(operand->type)->size;
    gen_addr(operand);
    ir_append(IR_PUSH, 0, NULL);
    gen_expr(operand);
    ir_append(IR_PUSH, 0, NULL);
    ir_append(IR_IMM, stride, NULL);
    if (node->op_kind == TK_POST_INC) ir_append(IR_ADD, 0, NULL);
    else                               ir_append(IR_SUB, 0, NULL);
    gen_st(sz);
}

/* Generate an expression whose result is discarded.
 * For top-level post-increment/decrement, use pre-increment semantics to avoid
 * the wasteful save/restore of the old value. */
static void gen_expr_discard(Node *node)
{
    if (node->kind == ND_UNARYOP &&
        (node->op_kind == TK_POST_INC || node->op_kind == TK_POST_DEC))
        gen_postinc_stmt(node);
    else
        gen_expr(node);
}

void gen_exprstmt(Node *node)
{
    int adj_before = codegen_ctx.adj_depth;
    gen_expr_discard(node->ch[0]);   // expr, result discarded
    // If a struct-returning call left a retbuf on the stack, reclaim it.
    int retbuf_sz = codegen_ctx.adj_depth - adj_before;
    if (retbuf_sz > 0)
    {
        ir_append(IR_ADJ, retbuf_sz, NULL);
        codegen_ctx.adj_depth -= retbuf_sz;
    }
}
bool is_constexpr(Node *n)
{
    if (n->kind == ND_LITERAL) return true;
    // Enum constants are compile-time integers
    if (n->kind == ND_IDENT && n->symbol && n->symbol->kind == SYM_ENUM_CONST) return true;
    // Composite expressions are constexpr only if all operands are.
    if (n->kind == ND_BINOP)
        return is_constexpr(n->ch[0]) && is_constexpr(n->ch[1]);
    if (n->kind == ND_UNARYOP)
        return is_constexpr(n->ch[0]);   // operand
    // Cast of a constexpr is also a constexpr (e.g. (ee_u16)0xd4b0)
    if (n->kind == ND_CAST)
        return is_constexpr(n->ch[1]);   // ch[1] = expression being cast
    return false;
}
int count_constexpr(Node *n)
{
    if (is_constexpr(n))
        return 1;
    int count = 0;
    if (n->kind == ND_INITLIST)
    {
        for (Node *c = n->ch[0]; c; c = c->next)   // items list
            count += count_constexpr(c);
    }
    return count;
}
void gen_inits(Node *n, Symbol *s, int vaddr, int offset, int depth)
{
    // Compute the element size (innermost) and row stride at current depth
    int elem_sz = array_elem_type(s->type)->size;
    // stride at this depth = size of elem at depth levels down
    Type *arr_at = s->type;
    for (int _d = 0; _d < depth && arr_at->base == TB_ARRAY; _d++)
        arr_at = arr_at->u.arr.elem;
    int row_stride = (arr_at->base == TB_ARRAY) ? arr_at->u.arr.elem->size : elem_sz;

    // Recurse through initlist
    int ptr = offset;
    for (Node *item = n->ch[0]; item; item = item->next)   // items list
        if (is_constexpr(item))
        {
            ir_append(IR_LEA, vaddr + ptr, NULL);
            ir_append(IR_PUSH,  0, NULL);
            gen_expr(item);
            gen_st(elem_sz);
            ptr += elem_sz;
        }
        else
        {
            // Going deeper, we need to set the offset to be the start of the row
            int new_offset = offset;
            if (ptr != offset)
                // If we've done anything on this row, advance to next row
                new_offset = offset + row_stride;
            gen_inits(item, s, vaddr, new_offset, depth + 1);
            // Now out of level, we move on a whole row
            offset = new_offset + row_stride;
            DBG_PRINT("out of inits, now inc offset to %d\n", offset);
            ptr = offset;
        }
}

int int_constexpr(Node *n);
void gen_mem_inits(char *data, Node *n, Symbol *s, int vaddr, int offset, int depth)
{
    // Compute element size and row stride at current depth
    int elem_sz = array_elem_type(s->type)->size;
    Type *arr_at = s->type;
    for (int _d = 0; _d < depth && arr_at->base == TB_ARRAY; _d++)
        arr_at = arr_at->u.arr.elem;
    int row_stride = (arr_at->base == TB_ARRAY) ? arr_at->u.arr.elem->size : elem_sz;

    // Recurse through initlist
    int ptr = offset;
    for (Node *item = n->ch[0]; item; item = item->next)   // items list
        if (is_constexpr(item))
        {
            int val = int_constexpr(item);
            data[vaddr + ptr]       = val & 0xff;
            if (elem_sz > 1)
                data[vaddr + ptr + 1]   = (val >> 8) & 0xff;
            ptr += elem_sz;
        }
        else
        {
            // Going deeper, we need to set the offset to be the start of the row
            int new_offset = offset;
            if (ptr != offset)
                // If we've done anything on this row, advance to next row
                new_offset = offset + row_stride;
            gen_mem_inits(data, item, s, vaddr, new_offset, depth + 1);
            // Now out of level, we move on a whole row
            offset = new_offset + row_stride;
            DBG_PRINT("out of inits, now inc offset to %d\n", offset);
            ptr = offset;
        }
}

int int_unaryop(Node *n)
{
    int i = int_constexpr(n->ch[0]);   // operand
    if (n->op_kind == TK_MINUS)
        return -i;
    return i;
}
int int_binop(Node *n)
{
    int i = int_constexpr(n->ch[0]);   // lhs
    int j = int_constexpr(n->ch[1]);   // rhs
    switch (n->op_kind)
    {
        case TK_PLUS:  return i + j;
        case TK_MINUS: return i - j;
        case TK_STAR:  return i * j;
        case TK_SLASH: return i / j;
        default:       return 0;
    }
}
int int_constexpr(Node *n)
{
    // Calculate an int constexpr
    if (n->kind == ND_UNARYOP)
        return int_unaryop(n);
    if (n->kind == ND_BINOP)
        return int_binop(n);
    if (n->kind == ND_IDENT && n->symbol && n->symbol->kind == SYM_ENUM_CONST)
        return n->symbol->offset;
    if (n->kind == ND_CAST)
        return int_constexpr(n->ch[1]);   // evaluate the expression being cast
    return (int)n->u.literal.ival;
}

// Write struct field initializers for a local struct.
// vaddr: lea argument for struct base (negative, e.g. -4 for bp-4)
// st: the struct/union Type
static void gen_struct_inits(Node *n, int vaddr, Type *st)
{
    Field *f = st->u.composite.members;
    for (Node *child = n->ch[0]; child && f; child = child->next, f = f->next)   // items list
    {
        if (child->kind == ND_INITLIST)
        {
            if (f->type->base == TB_STRUCT)
                gen_struct_inits(child, vaddr + f->offset, f->type);
        }
        else if (is_constexpr(child))
        {
            ir_append(IR_LEA, vaddr + f->offset, NULL);
            ir_append(IR_PUSH,  0, NULL);
            gen_expr(child);
            gen_st(f->type->size);
        }
    }
}

// Fill a byte buffer with struct field initializers for a global struct.
// base: byte offset of struct base within the data buffer
// st: the struct/union Type
static void gen_struct_mem_inits(char *data, Node *n, Type *st, int base)
{
    Field *f = st->u.composite.members;
    for (Node *child = n->ch[0]; child && f; child = child->next, f = f->next)   // items list
    {
        if (child->kind == ND_INITLIST)
        {
            if (f->type->base == TB_STRUCT)
                gen_struct_mem_inits(data, child, f->type, base + f->offset);
        }
        else if (is_constexpr(child))
        {
            int val = int_constexpr(child);
            data[base + f->offset] = val & 0xff;
            if (f->type->size > 1)
                data[base + f->offset + 1] = (val >> 8) & 0xff;
            if (f->type->size > 2)
            {
                data[base + f->offset + 2] = (val >> 16) & 0xff;
                data[base + f->offset + 3] = (val >> 24) & 0xff;
            }
        }
    }
}

/* Return the assembly label for an address-of-global expression, or NULL.
   Handles &var, (type *)&var, static globals, and static locals. */
static const char *get_addr_of_label(Node *expr)
{
    while (expr->kind == ND_CAST) expr = expr->ch[1];
    if (expr->kind != ND_UNARYOP || expr->op_kind != TK_AMPERSAND)
        return NULL;
    Node *child = expr->ch[0];
    if (child->kind != ND_IDENT) return NULL;
    Symbol *sym = child->symbol;
    if (!sym) return child->u.ident.name;
    if (sym->kind == SYM_STATIC_LOCAL) {
        char buf[32];
        snprintf(buf, sizeof(buf), "_ls%d", sym->offset);
        return arena_strdup(buf);
    }
    if (sym->kind == SYM_STATIC_GLOBAL) {
        char buf[80];
        snprintf(buf, sizeof(buf), "_s%d_%s", sym->tu_index, sym->name);
        return arena_strdup(buf);
    }
    return sym->name;
}

/* Collect symbolic (address-of) fields in a struct initialiser, recursively. */
typedef struct { int offset; const char *sym; } SymField;
#define MAX_SYM_FIELDS 64
static void collect_sym_fields(Node *n, Type *st, int base,
                               SymField *out, int *count)
{
    Field *f = st->u.composite.members;
    for (Node *child = n->ch[0]; child && f; child = child->next, f = f->next) {
        if (child->kind == ND_INITLIST && f->type->base == TB_STRUCT)
            collect_sym_fields(child, f->type, base + f->offset, out, count);
        else {
            const char *label = get_addr_of_label(child);
            if (label && *count < MAX_SYM_FIELDS) {
                out[*count].offset = base + f->offset;
                out[*count].sym    = label;
                (*count)++;
            }
        }
    }
}

void gen_initlist(Node *n, Symbol *s)
{
    // Address of variable to be initialised is on the stack
    Type *t = s->type;

    int constexpr = count_constexpr(n);
    if (n->st->depth)
    {
        if (t->base == TB_STRUCT)
        {
            int vaddr = -find_local_addr(n, s->name).offset;
            gen_fill(vaddr, t->size);
            gen_struct_inits(n, vaddr, t);
        }
        else if (!array_dimensions(t))
        {
            // Scalar object, just take the first element of the list. There should be
            // only one element
            if (constexpr != 1)
                error("Should be exactly one constexpr in initialiser\n");
            Node *l;
            for(l = n; !is_constexpr(l); l = l->ch[0]);   // items list head
            gen_varaddr_from_ident(n, s->name);
            ir_append(IR_PUSH,  0, NULL);
            gen_expr(l);
            ir_append(IR_SW,    0, NULL);
        }
        else if (constexpr)
        {
            // Fill the whole variable with zero, then add in init values.
            // Optimisation would be to create a data area and copy the data over..
            //
            // Offset here is what you apply to lea to get address
            int vaddr = -find_local_addr(n, s->name).offset;
            gen_fill(vaddr, t->size);
            gen_inits(n, s, vaddr, 0, 0);
        }
    }
    else
    {
        if (t->base == TB_STRUCT)
        {
            /* Check for symbolic (address-of-global) fields that can't be
               represented in a flat byte buffer. */
            SymField sym_fields[MAX_SYM_FIELDS];
            int n_sym = 0;
            collect_sym_fields(n, t, 0, sym_fields, &n_sym);

            char *data = arena_alloc(t->size);
            gen_struct_mem_inits(data, n, t, 0);

            if (n_sym > 0)
            {
                /* Emit byte-by-byte, replacing 2-byte spans at symbolic
                   field offsets with IR_WORD label references. */
                int pos = 0;
                for (int si = 0; si < n_sym; si++)
                {
                    /* Emit bytes before this symbolic field */
                    for (; pos < sym_fields[si].offset; pos++)
                        ir_append(IR_BYTE, (unsigned char)data[pos], NULL);
                    /* Emit symbolic address as a word */
                    ir_append(IR_WORD, 0, sym_fields[si].sym);
                    pos += 2; /* pointer is 2 bytes */
                }
                /* Emit remaining bytes after the last symbolic field */
                for (; pos < t->size; pos++)
                    ir_append(IR_BYTE, (unsigned char)data[pos], NULL);
            }
            else
            {
                gen_bytes(data, t->size);
            }
        }
        else if (!array_dimensions(t))
        {
            // Scalar object, just take the first element of the list. There should be
            // only one element
            if (constexpr != 1)
                error("Should be exactly one constexpr in initialiser\n");
            Node *l;
            for(l = n; !is_constexpr(l); l = l->ch[0]);   // items list head
            if (istype_fp(t))
                emit_float_bytes(l->u.literal.fval);
            else
                emit_int_data(int_constexpr(l), t->size);
        }
        else if (constexpr)
        {
            /* Check if any element is a string-literal pointer (symbolic address).
               The byte-buffer approach can't hold label references, so emit each
               element individually in that case. */
            Type *elem_t = t;
            while (elem_t->base == TB_ARRAY) elem_t = elem_t->u.arr.elem;
            bool has_strlit_ptr = false;
            if (istype_ptr(elem_t))
            {
                for (Node *item = n->ch[0]; item; item = item->next)
                {
                    Node *lit = item;
                    while (lit->kind == ND_CAST) lit = lit->ch[1];
                    if (lit->kind == ND_LITERAL && lit->u.literal.strval) { has_strlit_ptr = true; break; }
                }
            }

            if (has_strlit_ptr)
            {
                /* Emit each element: string literal (or cast thereof) → word label;
                   integer constexpr → word value */
                for (Node *item = n->ch[0]; item; item = item->next)
                {
                    /* Strip casts to get to the underlying literal */
                    Node *lit = item;
                    while (lit->kind == ND_CAST) lit = lit->ch[1];
                    if (lit->kind == ND_LITERAL && lit->u.literal.strval)
                    {
                        int sid = new_strlit(lit->u.literal.strval, lit->u.literal.strval_len);
                        char buf[32];
                        snprintf(buf, sizeof(buf), "_l%d", sid);
                        ir_append(IR_WORD, 0, arena_strdup(buf));
                    }
                    else
                    {
                        emit_int_data(int_constexpr(item), elem_t->size);
                    }
                }
            }
            else
            {
                char *data = arena_alloc(t->size);
                gen_mem_inits(data, n, s, 0, 0, 0);
                gen_bytes(data, t->size);
            }
        }
    }
}

void gen_decl(Node *node)
{
    if (node->line) current_codegen_line = node->line;
    if (node->u.declaration.sclass == SC_TYPEDEF) return;
    if (node->u.declaration.sclass == SC_EXTERN)  return;  // extern decl → no data emission
    // The symbol table has all the details needed for declarations.
    // Space is reserved at the start of the compound statement

    // Iterate over declarators, generating code for initialisations
    for (Node *n = node->ch[1]; n; n = n->next)   // decls list
    {
        // Different treatment if at scope 0
        if (n->st->depth)
        {
            if (n->kind == ND_DECLARATOR && n->symbol && n->symbol->kind == SYM_STATIC_LOCAL)
            {
                if (codegen_ctx.local_static_count >= MAX_LOCAL_STATICS)
                    error("too many local static variables (max %d)", MAX_LOCAL_STATICS);
                codegen_ctx.local_statics[codegen_ctx.local_static_count++] = (LocalStaticEntry){
                    n->symbol->offset, n->symbol, n
                };
                continue;
            }
            if (n->kind == ND_DECLARATOR && n->ch[1] != NULL)   // init
            {
                // This is an initialiser.
                // Get ident from symbol table
                if (!n->symbol)
                    error("Missing symbol!\n");
                Node *init = n->ch[1];   // init
                if (init->kind == ND_INITLIST)
                {
                    gen_initlist(init, n->symbol);
                }
                else if (init->u.literal.strval && istype_array(n->symbol->type))
                {
                    // Local char array init from string literal: store bytes one by one
                    int vaddr = -find_local_addr(n, n->symbol->name).offset;
                    char *str = init->u.literal.strval;
                    int   len = init->u.literal.strval_len + 1;
                    for (int i = 0; i < len; i++)
                    {
                        ir_append(IR_LEA, vaddr + i, NULL);
                        ir_append(IR_PUSH,  0, NULL);
                        ir_append(IR_IMM, (unsigned char)str[i], NULL);
                        ir_append(IR_SB,    0, NULL);
                    }
                }
                else if (n->symbol->type->base == TB_STRUCT)
                {
                    // Struct initializer from expression (e.g. struct P p = make(3, 7))
                    int adj_before = codegen_ctx.adj_depth;
                    gen_expr(init);                                   // r0 = src struct addr
                    ir_append(IR_PUSH, 0, NULL);
                    int src_slot = -(codegen_ctx.adj_depth + 4);
                    gen_varaddr_from_ident(n, n->symbol->name);       // r0 = dst addr
                    ir_append(IR_PUSH, 0, NULL);
                    int dst_slot = -(codegen_ctx.adj_depth + 8);
                    gen_struct_copy(src_slot, dst_slot, n->symbol->type);
                    ir_append(IR_ADJ, 8, NULL);
                    // Free any retbuf allocated by gen_expr(init)
                    int retbuf_sz = codegen_ctx.adj_depth - adj_before;
                    if (retbuf_sz > 0)
                    {
                        ir_append(IR_ADJ, retbuf_sz, NULL);
                        codegen_ctx.adj_depth -= retbuf_sz;
                    }
                }
                else
                {
                    gen_varaddr_from_ident(n, n->symbol->name);
                    ir_append(IR_PUSH,  0, NULL);
                    gen_expr(init);
                    gen_st(n->symbol->type->size);
                }
            }
        }
        else
        {
            // Global, we always make codegen_ctx.label_counter and allocate space
            if (n->kind == ND_DECLARATOR)
            {
                // This is an initialiser.
                // Get ident from symbol table
                if (!n->symbol)
                    error("Missing symbol!\n");
                // Skip bare function prototypes — no data to emit
                if (istype_function(n->symbol->type)) continue;
                ir_append(IR_ALIGN, 0, NULL);
                if (n->symbol->kind == SYM_STATIC_GLOBAL)
                {
                    char mangled[80];
                    snprintf(mangled, sizeof(mangled), "_s%d_%s",
                             n->symbol->tu_index, n->symbol->name);
                    ir_append(IR_SYMLABEL, 0, arena_strdup(mangled));
                }
                else
                    ir_append(IR_SYMLABEL, 0, n->symbol->name);
                // If there is no init, we make space

                if (n->ch[1] != NULL)   // init
                {
                    // Initialiser
                    Node *init = n->ch[1];   // init
                    if (init->kind == ND_INITLIST)
                    {
                        gen_initlist(init, n->symbol);
                    }
                    else if (init->u.literal.strval && istype_array(n->symbol->type))
                    {
                        // char s[] = "hello" — emit bytes directly
                        gen_bytes(init->u.literal.strval, init->u.literal.strval_len + 1);
                    }
                    else if (init->u.literal.strval)
                    {
                        // char *p = "hello" — emit pointer to deferred string data
                        int sid = new_strlit(init->u.literal.strval, init->u.literal.strval_len);
                        char buf[32];
                        snprintf(buf, sizeof(buf), "_l%d", sid);
                        ir_append(IR_WORD, 0, arena_strdup(buf));
                    }
                    else
                    {
                        if (istype_fp(n->symbol->type))
                            emit_float_bytes(init->u.literal.fval);
                        else
                            emit_int_data(int_constexpr(init), n->symbol->type->size);
                    }
                }
                else
                {
                    gen_zeros(n->symbol->type->size);
                }
            }
        }
    }
}
void gen_compstmt(Node *node)
{
    // Make space on stack for this scope's locals.
    // CPU4: round up to even so sp stays 2-byte aligned (F2 bp-relative instructions
    // require bp to be even; odd adjw would make callee bp odd).
    int sz = node->symtable->size;
    if (g_target_arch == 4 && (sz & 1)) sz++;
    codegen_ctx.adj_depth += sz;
    ir_append(IR_ADJ, -sz, NULL);
    for (Node *n = node->ch[0]; n; n = n->next)   // stmts list
    {
        if (n->kind == ND_DECLARATION)
            gen_decl(n);
        else
            gen_stmt(n);
    }
    // Release the space back
    ir_append(IR_ADJ, sz, NULL);
    codegen_ctx.adj_depth -= sz;
}
static int find_label_id(char *name)
{
    for (int i = 0; i < codegen_ctx.label_table_size; i++)
        if (!strcmp(name, codegen_ctx.label_table[i].name))
            return codegen_ctx.label_table[i].label_id;
    error("label '%s' not found\n", name);
    return -1;
}
void gen_labelstmt(Node *node)
{
    ir_append(IR_LABEL, find_label_id(node->u.labelstmt.name), NULL);
    if (node->ch[0])   // stmt
        gen_stmt(node->ch[0]);
}
void gen_gotostmt(Node *node)
{
    ir_append(IR_J, find_label_id(node->u.label), NULL);
}
void gen_switchstmt(Node *node)
{
    if (codegen_ctx.loop_depth >= 64) error("too deeply nested");
    Node *selector = node->ch[0];
    Node *body     = node->ch[1]; // ND_COMPSTMT
    int lbreak     = new_label();
    int ldefault   = -1;
    // Phase 1: assign codegen_ctx.label_counter to cases and emit comparisons
    for (Node *ch = body->ch[0]; ch; ch = ch->next)   // stmts list
    {
        if (ch->kind == ND_CASESTMT) {
            int lcase              = new_label();
            ch->u.casestmt.label_id = lcase;
            gen_expr(selector);
            ir_append(IR_PUSH,  0, NULL);
            ir_append(IR_IMM, (int)ch->u.casestmt.value, NULL);
            ir_append(IR_EQ,    0, NULL);
            ir_append(IR_JNZ, lcase, NULL);
        } else if (ch->kind == ND_DEFAULTSTMT) {
            int ldef                   = new_label();
            ch->u.defaultstmt.label_id = ldef;
            ldefault                   = ldef;
        }
    }
    if (ldefault >= 0) ir_append(IR_J, ldefault, NULL);
    else               ir_append(IR_J, lbreak, NULL);
    // Phase 2: emit body with case codegen_ctx.label_counter
    codegen_ctx.break_labels[codegen_ctx.loop_depth] = lbreak;
    codegen_ctx.cont_labels[codegen_ctx.loop_depth]  = -1;
    codegen_ctx.loop_adj[codegen_ctx.loop_depth]     = codegen_ctx.adj_depth;
    codegen_ctx.loop_depth++;
    // CPU4: round up to even to keep sp 2-byte aligned.
    int sw_sz = body->symtable->size;
    if (g_target_arch == 4 && (sw_sz & 1)) sw_sz++;
    codegen_ctx.adj_depth += sw_sz;
    ir_append(IR_ADJ, -sw_sz, NULL);
    for (Node *ch = body->ch[0]; ch; ch = ch->next)   // stmts list
    {
        if (ch->kind == ND_CASESTMT)
            ir_append(IR_LABEL, ch->u.casestmt.label_id, NULL);
        else if (ch->kind == ND_DEFAULTSTMT)
            ir_append(IR_LABEL, ch->u.defaultstmt.label_id, NULL);
        else if (ch->kind == ND_DECLARATION)
            gen_decl(ch);
        else
            gen_stmt(ch);
    }
    ir_append(IR_ADJ, sw_sz, NULL);
    codegen_ctx.adj_depth -= sw_sz;
    codegen_ctx.loop_depth--;
    ir_append(IR_LABEL, lbreak, NULL);
}
void gen_stmt(Node *node)
{
    if (node->line) current_codegen_line = node->line;
    switch(node->kind)
    {
    case ND_EXPRSTMT:    gen_exprstmt(node);    return;
    case ND_COMPSTMT:    gen_compstmt(node);    return;
    case ND_IFSTMT:      gen_ifstmt(node);      return;
    case ND_WHILESTMT:   gen_whilestmt(node);   return;
    case ND_FORSTMT:     gen_forstmt(node);     return;
    case ND_DOWHILESTMT: gen_dowhilestmt(node); return;
    case ND_SWITCHSTMT:  gen_switchstmt(node);  return;
    case ND_BREAKSTMT:   gen_breakstmt(node);   return;
    case ND_CONTINUESTMT:gen_continuestmt(node);return;
    case ND_LABELSTMT:   gen_labelstmt(node);   return;
    case ND_GOTOSTMT:    gen_gotostmt(node);    return;
    case ND_CASESTMT:
    case ND_DEFAULTSTMT:
    case ND_EMPTY:                              return;
    case ND_RETURNSTMT:  gen_returnstmt(node);  return;
    case ND_STMT:        gen_stmt(node->ch[0]); return;   // body
    default:;
    }
}
/* Pre-scan the function AST to mark every local/param symbol whose address
 * is taken with '&'.  Called once per function before gen_ir runs.
 * Walks ch[0..3] and ->next recursively, stripping implicit ND_CAST nodes
 * that insert_coercions may have placed around the & operand. */
static void mark_address_taken(Node *n)
{
    if (!n) return;
    if (n->kind == ND_UNARYOP && n->op_kind == TK_AMPERSAND) {
        Node *op = n->ch[0];
        /* Strip implicit casts inserted by insert_coercions */
        while (op && op->kind == ND_CAST) op = op->ch[1];
        if (op && op->kind == ND_IDENT && op->symbol) {
            Symbol *s = op->symbol;
            if (s->kind == SYM_LOCAL || s->kind == SYM_PARAM)
                s->address_taken = true;
        }
    }
    for (int i = 0; i < 4; i++) mark_address_taken(n->ch[i]);
    mark_address_taken(n->next);
}

void gen_function(Node *node)
{
    Node *first_decl = node->ch[1];   // decls list head
    Node *func_body  = node->ch[2];   // func_body
    Symbol *fsym = first_decl->symbol;
    if (fsym->kind == SYM_STATIC_GLOBAL)
    {
        char buf[80];
        snprintf(buf, sizeof(buf), "_s%d_%s", fsym->tu_index, fsym->name);
        ir_append(IR_SYMLABEL, 0, arena_strdup(buf));
    }
    else
        ir_append(IR_SYMLABEL, 0, fsym->name);
    codegen_ctx.label_table_size = 0;
    collect_labels(func_body);
    mark_address_taken(func_body);
    ir_append(IR_ENTER, 0, NULL);
    codegen_ctx.adj_depth = 0;
    codegen_ctx.current_fn_ret_type = fsym->type->u.fn.ret;
    gen_stmt(func_body);
    ir_append(IR_RET,   0, NULL);
}
// Emitted once across all TUs, before the first global variable.
static bool globals_start_emitted = false;

void gen_ir(Node *node, int tu_index)
{

    // Pass 1: emit function definitions (text area)
    for (Node *c = node->ch[0]; c; c = c->next)   // decls list
    {
        if (c->kind == ND_DECLARATION && c->u.declaration.is_func_defn)
            gen_function(c);
    }
    // Pass 2: emit global variable declarations (data area)
    if (!globals_start_emitted) {
        ir_append(IR_SYMLABEL, 0, "_globals_start");
        globals_start_emitted = true;
    }
    for (Node *c = node->ch[0]; c; c = c->next)   // decls list
    {
        if (c->kind == ND_DECLARATION && !c->u.declaration.is_func_defn)
            gen_decl(c);
    }
    // Pass 2b: local static data collected during pass 1 (function codegen).
    for (int i = 0; i < codegen_ctx.local_static_count; i++)
    {
        LocalStaticEntry *e = &codegen_ctx.local_statics[i];
        Node *n = e->decl_node;
        ir_append(IR_ALIGN, 0, NULL);
        {
            char buf[32];
            snprintf(buf, sizeof(buf), "_ls%d", e->id);
            ir_append(IR_SYMLABEL, 0, arena_strdup(buf));
        }
        Node *init = n->ch[1];   // init
        if (init != NULL)
        {
            if (init->kind == ND_INITLIST)
                gen_initlist(init, e->sym);
            else if (init->u.literal.strval && istype_array(e->sym->type))
                gen_bytes(init->u.literal.strval, init->u.literal.strval_len + 1);
            else if (init->u.literal.strval)
            {
                int sid = new_strlit(init->u.literal.strval, init->u.literal.strval_len);
                char sbuf[32];
                snprintf(sbuf, sizeof(sbuf), "_l%d", sid);
                ir_append(IR_WORD, 0, arena_strdup(sbuf));
            }
            else if (istype_fp(e->sym->type))
                emit_float_bytes(init->u.literal.fval);
            else
                emit_int_data(int_constexpr(init), e->sym->type->size);
        }
        else
            gen_zeros(e->sym->type->size);
    }
    // Pass 3: emit deferred string literal data
    for (int i = 0; i < codegen_ctx.strlit_count; i++)
    {
        ir_append(IR_ALIGN, 0, NULL);
        ir_append(IR_LABEL, codegen_ctx.strlits[i].id, NULL);
        gen_bytes(codegen_ctx.strlits[i].data, codegen_ctx.strlits[i].len + 1);
    }
}

