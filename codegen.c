
#include <stdarg.h>
#include "mycc.h"


const char *scope_str(Scope sc);
Symbol_table *find_scope(Node *node);

// Codegen context instance
CodegenContext codegen_ctx;


typedef struct { int offset; bool is_param; } LocalAddr;
// Returns bp-relative byte offset for local variables and parameters.
// Returns offset == -1 for globals (use symbolic label) and local-statics (use _ls{id} label).
static LocalAddr find_local_addr(Node *node, const char *name)
{
    DBG_PRINT("%s scope is %s\n", __func__, scope_str(node->st->scope));
    Symbol_table *st = find_scope(node);
    Symbol *s;
    bool found = false;
    while (!found)
    {
        DBG_PRINT("Searching in scope:%s\n", scope_str(st->scope));
        for (s = st->idents; s; s = s->next)
        {
            DBG_PRINT("Ident:%s\n", s->name);
            if (!strcmp(name, s->name)) { found = true; break; }
        }
        if (found) break;
        DBG_PRINT("Not found, going to enclosing scope\n");
        if (st->parent) st = st->parent;
    }
    if (!found)
        error("Symbol %s not found!\n", name);
    if (s->is_local_static || !st->scope.depth)
        return (LocalAddr){ -1, false };
    DBG_PRINT(";find_local_addr ident:%s got offset %d\n", name, s->offset & 0xffff);
    return (LocalAddr){ s->offset, s->is_param };
}


static int new_strlit(char *data, int len)
{
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
    memset(codegen_ctx.break_labels, 0, sizeof(codegen_ctx.break_labels));
    memset(codegen_ctx.cont_labels,  0, sizeof(codegen_ctx.cont_labels));
    // 'codegen_ctx.label_counter' is NOT reset — monotonically increasing across TUs.
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

// Format specifiers:
//   %s  - string operand (instruction name or label name)
//   %d  - decimal integer
//   %x  - hex integer (4-digit for word values)
//   %l  - label number (auto-prefixed with _l)
//   %i  - immediate value (outputs immw/immwh pair if > 16 bits)
//   %%  - literal %
void emit(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    const char *p = fmt;
    int needs_indent = 1;
    while (*p)
    {
        if (*p != '%')
        {
            if (needs_indent)
            {
                printf("    ");
                needs_indent = 0;
            }
            putchar(*p);
            p++;
            continue;
        }
        p++;
        switch (*p)
        {
            case 's':
                printf("%s", va_arg(args, char*));
                break;
            case 'd':
                printf("%d", va_arg(args, int));
                break;
            case 'x':
                printf("0x%04x", va_arg(args, int) & 0xffff);
                break;
            case 'l':
                printf("_l%d", va_arg(args, int));
                break;
            case 'i':
            {
                int val = va_arg(args, int);
                printf("immw    0x%04x", val & 0xffff);
                if (val & 0xffff0000)
                    printf("\n    immwh   0x%04x", ((unsigned)val >> 16) & 0xffff);
                break;
            }
            case '%':
                putchar('%');
                break;
            default:
                putchar(*p);
                break;
        }
        p++;
    }
    printf("\n");

    va_end(args);
}

void gen_label(int label)       { printf("_l%d:\n", label); }
void gen_symlabel(const char *name) { printf("%s:\n", name); }
void gen_align()                { emit("align"); }
void gen_word(int d)            { printf("    word 0x%04x\n", d & 0xffff); }
void gen_jz(int label)          { emit("jz %l", label); }
void gen_jnz(int label)         { emit("jnz %l", label); }
void gen_j(int label)           { emit("j %l", label); }
void gen_push()                 { emit("push"); }
void gen_pop()                  { emit("pop"); }
void gen_pushw()                { emit("pushw"); }
void gen_popw()                 { emit("popw"); }
void gen_jl(const char *label)  { emit("jl %s", label); }
void gen_jli()                  { emit("jli"); }
void gen_adj(int offset)        { emit("adj %d", offset); }
void gen_add()                  { emit("add"); }
void gen_sub()                  { emit("sub"); }
void gen_mul()                  { emit("mul"); }
void gen_div()                  { emit("div"); }
void gen_mod()                  { emit("mod"); }
void gen_eq()                   { emit("eq"); }
void gen_ne()                   { emit("ne"); }
void gen_lt()                   { emit("lt"); }
void gen_le()                   { emit("le"); }
void gen_gt()                   { emit("gt"); }
void gen_ge()                   { emit("ge"); }
void gen_shiftl()               { emit("shl"); }
void gen_shiftr()               { emit("shr"); }
void gen_bitor()                { emit("or"); }
void gen_bitand()               { emit("and"); }
void gen_bitxor()               { emit("xor"); }
void gen_lb()                   { emit("lb"); }
void gen_lw()                   { emit("lw"); }
void gen_ll()                   { emit("ll"); }
void gen_sb()                   { emit("sb"); }
void gen_sw()                   { emit("sw"); }
void gen_sl()                   { emit("sl"); }
void gen_sxb()                  { emit("sxb"); }
void gen_sxw()                  { emit("sxw"); }
void gen_fadd()                 { emit("fadd"); }
void gen_fsub()                 { emit("fsub"); }
void gen_fmul()                 { emit("fmul"); }
void gen_fdiv()                 { emit("fdiv"); }
void gen_flt()                  { emit("flt"); }
void gen_fle()                  { emit("fle"); }
void gen_fgt()                  { emit("fgt"); }
void gen_fge()                  { emit("fge"); }
void gen_itof()                 { emit("itof"); }
void gen_ftoi()                 { emit("ftoi"); }
void gen_lea(int o)             { emit("lea %d", o); }

void gen_zeros(int bytes)
{
    for(int i = bytes; i >= 8; i-= 8)
        printf("    byte    0 0 0 0 0 0 0 0\n");
    if (bytes % 8)
    {
        printf("    byte    ");
        for(int i = 0; i < bytes % 8; i++)
            printf("0 ");
        printf("\n");
    }
}
void gen_bytes(char *data, int size)
{
    for(int i = 0; i < size; i++)
    {
        if (i % 8 == 0)
            printf("    byte    ");
        printf("0x%02x ", data[i]);
        if (i % 8 == 7)
            printf("\n");
    }
    if (size % 8)
        printf("\n");
}
void gen_imm(int val)
{
    emit("%i", val);
}
void gen_pushi(int val)
{
    emit("%i", val);
    emit("push");
}
// Forward declaration needed by gen_logor_expr / gen_logand_expr below.
void gen_expr(Node *node);

// Short-circuit logical OR: if lhs != 0 result is 1 without evaluating rhs.
// Self-contained — does not push or pop; r0 holds the result on exit.
static void gen_logor_expr(Node *lhs, Node *rhs)
{
    int l_true = new_label();
    int l_end  = new_label();
    gen_expr(lhs);
    gen_jnz(l_true);   // lhs != 0 → skip rhs
    gen_expr(rhs);
    gen_jnz(l_true);
    gen_imm(0);
    gen_j(l_end);
    gen_label(l_true);
    gen_imm(1);
    gen_label(l_end);
}

// Short-circuit logical AND: if lhs == 0 result is 0 without evaluating rhs.
// Self-contained — does not push or pop; r0 holds the result on exit.
static void gen_logand_expr(Node *lhs, Node *rhs)
{
    int l_false = new_label();
    int l_end   = new_label();
    gen_expr(lhs);
    gen_jz(l_false);   // lhs == 0 → skip rhs
    gen_expr(rhs);
    gen_jz(l_false);
    gen_imm(1);
    gen_j(l_end);
    gen_label(l_false);
    gen_imm(0);
    gen_label(l_end);
}
void gen_st(int s)
{
    if (s == 1)      gen_sb();
    else if (s == 2) gen_sw();
    else if (s == 4) gen_sl();
    else             error("gen_st: unsupported size %d\n", s);
}
void gen_ld(int s)
{
    if (s == 1)      gen_lb();
    else if (s == 2) gen_lw();
    else if (s == 4) gen_ll();
    else             error("gen_ld: unsupported size %d\n", s);
}

static void emit_float_bytes(double val)
{
    float f = (float)val;
    char bytes[4];
    memcpy(bytes, &f, 4);
    gen_bytes(bytes, 4);
}
void gen_imm_float(double val)
{
    float f = (float)val;
    unsigned int bits;
    memcpy(&bits, &f, sizeof(bits));
    printf("    immw    0x%04x\n", bits & 0xffff);
    printf("    immwh   0x%04x\n", (bits >> 16) & 0xffff);
}
void gen_varaddr_from_ident(Node *node, const char *name)
{
    LocalAddr la = find_local_addr(node, name);
    if (la.offset < 0)
    {
        Symbol *sym = node->symbol;
        if (sym && sym->is_local_static)
            printf("    immw    _ls%d\n", sym->offset);
        else if (sym && sym->is_static)
            printf("    immw    _s%d_%s\n", sym->tu_index, sym->name);
        else
            printf("    immw    %s\n", name);
    }
    else if (la.is_param)
        // Function parameter: above bp
        printf("    lea     %d\n", la.offset);
    else
        // Local variable: below bp
        printf("    lea     %d\n", -la.offset);
}
void gen_varaddr(Node *node)
{
    if (node->kind != ND_IDENT)
        error("Expecting local var got %s", nodestr(node->kind));
    
    gen_varaddr_from_ident(node, node->u.ident.name);
}
void gen_preamble(void)
{
    printf("    enter   0\n");
}
void gen_fill(int offset, int size)
{
    int step = 1;
    if (size %2 == 0)
        step = 2;
    for(int i = 0; i < size; i+=step)
    {
        gen_lea(offset + i);
        gen_push();
        gen_imm(0);
        gen_st(step);
    }
}

//--------------------------------------------------------------------------------
// Everything below here uses pseudoinstructions
//--------------------------------------------------------------------------------

void gen_expr(Node *node);
void gen_varaddr(Node *node);

// Push function call args from a linked list (right-to-left onto stack).
// Returns total bytes pushed.
static int push_args_list(Node *first_arg)
{
    // Collect args into a temporary array for right-to-left pushing
    Node *args[64];
    int n = 0;
    for (Node *a = first_arg; a; a = a->next)
    {
        if (n >= 64) error("Too many function arguments\n");
        args[n++] = a;
    }
    int param_size = 0;
    for (int i = n - 1; i >= 0; i--)
    {
        gen_expr(args[i]);
        int s = args[i]->type->size;
        if (s == 0) s = WORD_SIZE;  // function designator decays to pointer size
        if (s == 2) gen_pushw(); else gen_push();
        param_size += s;
    }
    return param_size;
}


// Indirect call through function pointer variable: fp(args)
void gen_callfunction_via_ptr(Node *node)
{
    int param_size = push_args_list(node->u.ident.args);
    gen_varaddr(node);
    gen_ld(node->symbol->type->size);
    gen_jli();
    gen_adj(param_size);
}

// Indirect call through dereferenced pointer: (*fp)(args)
void gen_callfunction_via_deref(Node *node)
{
    int param_size = push_args_list(node->u.unaryop.args);
    gen_expr(node->u.unaryop.operand);
    gen_jli();
    gen_adj(param_size);
}

void gen_callfunction(Node *node)
{
    // putchar(c) is a CPU builtin
    if (!strcmp(node->symbol->name, "putchar"))
    {
        if (!node->u.ident.args || node->u.ident.args->next)
            error("putchar requires exactly 1 argument\n");
        gen_expr(node->u.ident.args);
        printf("    putchar\n");
        return;
    }
    // We have to call the function. This means setting up the
    // parameters on the stack.
    //
    // Call structure is thus:
    //  4n  param n
    //      ..
    //  12  param 1
    //  8   param 0     <- sp on entry
    //  4   link
    //  0   old bp      <- bp set to this
    //  -4  local 0
    //  -8  local 1
    //  ..
    //  -4n  local n     <- sp set to this after entry
    //
    // Parameters are push onto the stack. On entry, the lr, holding the
    // return address is saved, and the old bp is saved. The sp is then adjusted
    // by the space required for this local scope variables. These are accessed 
    // through the bp with an offset from the symbol table. Each time we enter
    // a new scope, the sp is adjusted to make space for the new locals, each
    // time the end of a scope is reached, the sp is adjusted back the other way
    // to reclaim the space.
    //
    // At the end of the function, ret sets the sp to bp, old bp is restored, then
    // pc is set to saved link address, returning to the caller.
    //
    // Push the params on backwards, the offsets from the symbol table then work
    int param_size = push_args_list(node->u.ident.args);
    if (node->symbol->is_static)
    {
        char mangled[80];
        snprintf(mangled, sizeof(mangled), "_s%d_%s",
                 node->symbol->tu_index, node->symbol->name);
        gen_jl(mangled);
    }
    else
        gen_jl(node->symbol->name);
    // For the function postamble, we need to adjust the stack by the size of the
    // push parameters
    gen_adj(param_size);

}
static const char *node_ident_str(Node *node)
{
    if (!node) return "";
    if (node->kind == ND_IDENT)
        return node->u.ident.name ? node->u.ident.name : "";
    if (node->kind == ND_LABELSTMT || node->kind == ND_GOTOSTMT)
        return node->u.label ? node->u.label : "";
    return "";
}

void gen_offset(Node *node)
{
    printf(";%s %s %s offset:%d\n", __func__, nodestr(node->kind), node_ident_str(node), node->offset);
    // lhs is structure, rhs is member within
    gen_imm(node->offset);
}
void gen_addr(Node *node)
{
    printf(";%s %s %s\n", __func__, nodestr(node->kind), node_ident_str(node));
    if (node->kind == ND_IDENT)
    {
        gen_varaddr(node);
    }
    else if (node->kind == ND_UNARYOP && node->op_kind == TK_STAR)
    {
        gen_expr(node->u.unaryop.operand);
    }
    else if (node->kind == ND_MEMBER)
    {
        if (node->op_kind == TK_ARROW)
            gen_expr(node->u.member.base);    // load pointer value
        else
            gen_addr(node->u.member.base);    // struct base address
        gen_push();
        gen_offset(node);
        gen_add();
    }
    else
        error("Expecting lvalue\n");
}
void gen_cast(Type *src, Type *dst)
{
    //                  dest
    //          uc  c   s   us  e   i   u   l   ul  p   f   d
    //      uc  .   .   z   z   z   z   z   z   z   z 
    //      c   .   .   s   s   s   s   s   s   s   s      
    //  s   s   m   m   .   .   .   .   .   s   s   .        
    //  s   us  m   m   .   .   .   .   .   z   z   .        
    //  r   e   m   m   .   .   .   .   .   z   z   .        
    //  c   i   m   m   .   .   .   .   .   s   s   .        
    //      u   m   m   .   .   .   .   .   z   z   .        
    //      l   tm  tm  t   t   t   t   t   .   .   t         
    //      ul  tm  tm  t   t   t   t   t   .   .   t         
    //      p   m   m   .   .   .   .   .   z   z   .        
    //      f                                           .   .
    //      d                                           .   . 

    // src is in reg
    // dst is in reg


    // Value in reg is in src type, needs to be converted to dst type
    if (src == dst)
        return;
    if (istype_intlike(src) && istype_fp(dst))
    {
        if (src->size == 1) gen_sxb();
        else if (src->size == 2) gen_sxw();
        gen_itof();
        return;
    }
    if (istype_fp(src) && istype_intlike(dst))
    {
        gen_ftoi();
        if (dst->size < 4)
        {
            gen_push();
            gen_imm(dst->size == 1 ? 0xff : 0xffff);
            gen_bitand();
        }
        return;
    }
    if (src->size == 1 && dst->size == 1)
        return;
    if (src->size == 2 && dst->size == 2)
        return;
    if (istype_uchar(src) && dst->size == 2)
        // Zero extend, assume already zero in top 8 bits
        return;
    if (istype_fp(src) && istype_fp(dst))
        // floats and doubles are the same
        return;
    if ((istype_long(src) || istype_ulong(src)) && dst->size == 2)
    {
        // Truncate
        gen_push();
        gen_imm(0xffff);
        gen_bitand();
        return;
    }
    if ((istype_long(src) || istype_ulong(src)) && dst->size == 1)
    {
        // Truncate
        gen_push();
        gen_imm(0xff);
        gen_bitand();
        return;
    }
    if (src->size == 2 && dst->size == 1)
    {
        // Mask top 8 bits. 
        // The high word is first on the stack, highest address (little endian)
        gen_push();
        gen_imm(0xff);
        gen_bitand();
        return;
    }
    if (istype_char(src) && dst->size == 2)
    {
        // Sign extend
        gen_sxb();
        return;
    }
    if (istype_char(src) && (istype_long(dst) || istype_ulong(dst)))
    {
        gen_sxb();
        return;
    }
    if ((istype_short(src) || istype_int(src)) && (istype_long(dst) || istype_ulong(dst)))
    {
        gen_sxw();
        return;
    }
    if ((istype_uchar(src) || istype_uint(src) || istype_enum(src) || istype_ptr(src)) 
        && (istype_long(dst) || istype_ulong(dst)))
    {
        // Zero extend
        gen_push();
        gen_imm(0xffff);
        gen_bitand();
        return;
    }
}
void gen_expr(Node *node)
{
    printf(";%s %s %s\n", __func__, nodestr(node->kind), node_ident_str(node));
    // ===== Binary operators =====
    if (node->kind == ND_BINOP)
    {
        Node *lhs = node->u.binop.lhs;
        Node *rhs = node->u.binop.rhs;
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
        gen_push();
        gen_expr(rhs);
        if (istype_fp(lhs->type))
        {
            switch (node->op_kind)
            {
                case TK_PLUS:  gen_fadd(); return;
                case TK_MINUS: gen_fsub(); return;
                case TK_STAR:  gen_fmul(); return;
                case TK_SLASH: gen_fdiv(); return;
                case TK_LT:    gen_flt();  return;
                case TK_LE:    gen_fle();  return;
                case TK_GT:    gen_fgt();  return;
                case TK_GE:    gen_fge();  return;
                default: break;
            }
        }
        switch (node->op_kind)
        {
            case TK_PLUS:      gen_add();    return;
            case TK_MINUS:     gen_sub();    return;
            case TK_STAR:      gen_mul();    return;
            case TK_SLASH:     gen_div();    return;
            case TK_LT:        gen_lt();     return;
            case TK_LE:        gen_le();     return;
            case TK_GT:        gen_gt();     return;
            case TK_GE:        gen_ge();     return;
            case TK_EQ:        gen_eq();     return;
            case TK_NE:        gen_ne();     return;
            case TK_SHIFTR:    gen_shiftr(); return;
            case TK_SHIFTL:    gen_shiftl(); return;
            case TK_BITOR:     gen_bitor();  return;
            case TK_BITXOR:    gen_bitxor(); return;
            case TK_AMPERSAND: gen_bitand(); return;
            case TK_PERCENT:   gen_mod();    return;
            default: error("BINOP op_kind %d not handled in codegen", node->op_kind);
        }
    }
    // ===== Literals =====
    else if (node->kind == ND_LITERAL)
    {
        if (node->strval)
        {
            int sid = new_strlit(node->strval, node->strval_len);
            printf("    immw    _l%d\n", sid);
        }
        else if (istype_fp(node->type))
            gen_imm_float(node->fval);
        else
            gen_imm((int)node->ival);
    }
    // ===== Identifiers and function calls =====
    else if (node->kind == ND_IDENT)
    {
        Symbol *sym = node->symbol;
        if (sym->is_enum_const)
        {
            // Enum constant: inline the integer value; no memory load.
            gen_imm(sym->offset);
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
            // Don't fetch from address if array/function (name IS the address).
            if (!istype_array(sym->type) && !istype_function(sym->type))
                gen_ld(sym->type->size);
        }
    }
    // ===== Unary operators =====
    else if (node->kind == ND_UNARYOP)
    {
        // Use u.unaryop.operand directly
        Node *operand = node->u.unaryop.operand;
        switch (node->op_kind)
        {
        case TK_PLUS:
            gen_expr(operand);
            break;
        case TK_MINUS:
            if (istype_fp(operand->type))
            {
                gen_imm_float(0.0);
                gen_push();
                gen_expr(operand);
                gen_fsub();
            }
            else
            {
                gen_imm(0);
                gen_push();
                gen_expr(operand);
                gen_sub();
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
                gen_ld(elem_type(node->type)->size);
            }
            break;
        case TK_AMPERSAND:
            gen_addr(operand);
            break;
        case TK_BANG:
            gen_expr(operand);
            gen_push();
            gen_imm(0);
            gen_eq();
            break;
        case TK_TILDE:
            gen_expr(operand);
            gen_push();
            gen_imm(0xffffffff);
            gen_bitxor();
            break;
        // Prefix increment/decrement: ++x / --x
        // Result: new value of x
        case TK_INC: case TK_DEC:
        {
            int is_inc = (node->op_kind == TK_INC);
            int sz = operand->type ? operand->type->size : 2;
            gen_addr(operand);
            gen_push();         // stack: [&x]
            gen_expr(operand);
            gen_push();         // stack: [&x, x]
            gen_imm(1);
            if (is_inc) gen_add(); else gen_sub();  // r0 = x±1, stack: [&x]
            gen_st(sz);
            // r0 still holds x±1 (sw/sb/sl don't modify r0)
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
            gen_expr(operand);    // r0 = old_x
            gen_push();         // stack: [old_x]
            gen_addr(operand);    // r0 = &x
            gen_push();         // stack: [old_x, &x]
            gen_expr(operand);    // r0 = x (reload)
            gen_push();         // stack: [old_x, &x, x]
            gen_imm(1);
            if (is_inc) gen_add(); else gen_sub();  // r0 = x±1, stack: [old_x, &x]
            gen_st(sz);
            // stack: [old_x], r0 = x±1
            gen_pop();          // r0 = old_x, stack: []
            break;
        }
        #pragma clang diagnostic pop
        default: error("unary op_kind %d not handled in codegen", node->op_kind);
        }
    }
    // ===== Assignment =====
    else if (node->kind == ND_ASSIGN)
    {
        Node *lhs = node->u.binop.lhs;
        Node *rhs = node->u.binop.rhs;
        gen_addr(lhs);
        gen_push();
        gen_expr(rhs);
        gen_st(lhs->type->size);
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
        Node *lhs = node->u.compound_assign.lhs;
        Node *rhs = node->u.compound_assign.rhs;
        int sz = lhs->type ? lhs->type->size : 2;
        gen_addr(lhs);
        gen_push();
        gen_ld(sz);
        gen_push();
        gen_expr(rhs);
        if (istype_fp(lhs->type))
        {
            switch (node->op_kind)
            {
                case TK_PLUS:  gen_fadd(); break;
                case TK_MINUS: gen_fsub(); break;
                case TK_STAR:  gen_fmul(); break;
                case TK_SLASH: gen_fdiv(); break;
                default: error("fp compound op_kind %d not handled", node->op_kind);
            }
        }
        else
        {
            switch (node->op_kind)
            {
                case TK_PLUS:      gen_add();    break;
                case TK_MINUS:     gen_sub();    break;
                case TK_STAR:      gen_mul();    break;
                case TK_SLASH:     gen_div();    break;
                case TK_PERCENT:   gen_mod();    break;
                case TK_AMPERSAND: gen_bitand(); break;
                case TK_BITOR:     gen_bitor();  break;
                case TK_BITXOR:    gen_bitxor(); break;
                case TK_SHIFTL:    gen_shiftl(); break;
                case TK_SHIFTR:    gen_shiftr(); break;
                default: error("compound op_kind %d not handled", node->op_kind);
            }
        }
        gen_st(sz);
    }
    // ===== Cast =====
    else if (node->kind == ND_CAST)
    {
        // The type conversion is defined by the type of the expression in child 1 and the
        // type of the cast
        // Use u.cast.expr directly
        gen_expr(node->u.cast.expr);
        gen_cast(node->u.cast.expr->type, node->type);
    }
    // ===== Member access =====
    else if (node->kind == ND_MEMBER && node->u.member.is_function)
    {
        // Call through a function-pointer struct member: s.fp(args) or s->fp(args)
        int param_size = push_args_list(node->u.member.args);
        gen_addr(node);
        gen_ld(WORD_SIZE);  // load function pointer (pointer-sized)
        gen_jli();
        gen_adj(param_size);
    }
    else if (node->kind == ND_MEMBER)
    {
        gen_addr(node);         // handles both . and ->
        gen_ld(node->type->size);
    }
    // ===== Ternary =====
    else if (node->kind == ND_TERNARY)
    {
        Node *cond  = node->u.ternary.cond;
        Node *then_ = node->u.ternary.then_;
        Node *else_ = node->u.ternary.else_;
        int l_else = new_label();
        int l_end  = new_label();
        gen_expr(cond);
        gen_jz(l_else);
        gen_expr(then_);
        gen_j(l_end);
        gen_label(l_else);
        gen_expr(else_);
        gen_label(l_end);
    }
    // ===== VA_START / VA_ARG / VA_END =====
    else if (node->kind == ND_VA_START)
    {
        // va_start(ap, last_param)
        // ap = bp + last_param_offset + last_param_size
        Node *ap_node  = node->u.vastart.ap;
        Node *lp_node  = node->u.vastart.last;
        int param_off  = lp_node->symbol->offset;
        int param_size = lp_node->symbol->type->size;
        gen_addr(ap_node);                       // r0 = &ap
        gen_push();                              // stack: [&ap]
        gen_lea(param_off + param_size);         // r0 = bp + first_vararg_offset
        gen_sw();                                // mem[&ap] = first_vararg_addr
    }
    else if (node->kind == ND_VA_ARG)
    {
        // va_arg(ap, T) where T has size s
        // Returns *old_ap, advances ap by s
        int s         = node->type->size;
        Node *ap_node = node->u.vaarg.ap;
        // Save old ap value
        gen_addr(ap_node);    // r0 = &ap
        gen_ld(WORD_SIZE);    // r0 = ap (current vararg pointer)
        gen_push();           // stack: [old_ap]
        // Advance ap: ap = ap + s
        gen_addr(ap_node);    // r0 = &ap
        gen_push();           // stack: [old_ap, &ap]
        gen_addr(ap_node);    // r0 = &ap
        gen_ld(WORD_SIZE);    // r0 = ap
        gen_push();           // stack: [old_ap, &ap, ap]
        gen_imm(s);           // r0 = s
        gen_add();            // r0 = ap + s; stack: [old_ap, &ap]
        gen_sw();             // mem[&ap] = ap + s; stack: [old_ap]
        // Load vararg from old_ap
        gen_pop();            // r0 = old_ap; stack: []
        gen_ld(s);            // r0 = *(old_ap) = vararg value
    }
    else if (node->kind == ND_VA_END)
    {
        // va_end is a no-op
        (void)node;
    }
}
void gen_return()
{
    printf("    ret\n");
}
void gen_returnstmt(Node *node)
{
    printf(";%s\n", __func__);
    Node *expr = node->u.returnstmt.expr;
    if (expr)
    {
        // Must have an expression
        gen_expr(expr);
    }
    gen_return();

}
void gen_ifstmt(Node *node)
{
    printf(";%s\n", __func__);
    // Structure is expr, stmt, [stmt]
    Node *cond = node->u.ifstmt.cond;
    Node *then_ = node->u.ifstmt.then_;
    Node *else_ = node->u.ifstmt.else_;
    int l_else = new_label();
    gen_expr(cond);
    gen_jz(l_else);
    gen_stmt(then_);
    if (else_)
    {
        int l_end = new_label();
        gen_j(l_end);
        gen_label(l_else);
        printf(";else clause\n");
        gen_stmt(else_);
        gen_label(l_end);
    }
    else
        gen_label(l_else);
}
void gen_whilestmt(Node *node)
{
    printf(";%s\n", __func__);
    if (codegen_ctx.loop_depth >= 64) error("too deeply nested");
    Node *cond = node->u.whilestmt.cond;
    Node *body = node->u.whilestmt.body;
    int lloop   = new_label();
    int lbreak  = new_label();
    codegen_ctx.break_labels[codegen_ctx.loop_depth] = lbreak;
    codegen_ctx.cont_labels[codegen_ctx.loop_depth]  = lloop;
    codegen_ctx.loop_depth++;
    gen_label(lloop);
    gen_expr(cond);
    gen_jz(lbreak);
    gen_stmt(body);
    gen_j(lloop);
    gen_label(lbreak);
    codegen_ctx.loop_depth--;
}
void gen_decl(Node *node);  // forward declaration
void gen_forstmt(Node *node)
{
    printf(";%s\n", __func__);
    if (codegen_ctx.loop_depth >= 64) error("too deeply nested");
    Node *init = node->u.forstmt.init;
    Node *cond = node->u.forstmt.cond;
    Node *inc  = node->u.forstmt.inc;
    Node *body = node->u.forstmt.body;
    // If the init was a declaration, node->symtable holds the for-init scope.
    if (node->symtable)
        gen_adj(-node->symtable->size);
    if (init->kind == ND_DECLARATION)
        gen_decl(init);
    else if (init->kind != ND_EMPTY)
        gen_expr(init);
    int lloop  = new_label();
    int lcont  = new_label();
    int lbreak = new_label();
    codegen_ctx.break_labels[codegen_ctx.loop_depth] = lbreak;
    codegen_ctx.cont_labels[codegen_ctx.loop_depth]  = lcont;
    codegen_ctx.loop_depth++;
    gen_label(lloop);
    if (cond->kind != ND_EMPTY) {
        gen_expr(cond);
        gen_jz(lbreak);
    }
    gen_stmt(body);
    gen_label(lcont);
    if (inc->kind != ND_EMPTY)
        gen_expr(inc);
    gen_j(lloop);
    gen_label(lbreak);
    codegen_ctx.loop_depth--;
    if (node->symtable)
        gen_adj(node->symtable->size);
}
void gen_dowhilestmt(Node *node)
{
    printf(";%s\n", __func__);
    if (codegen_ctx.loop_depth >= 64) error("too deeply nested");
    Node *body = node->u.dowhile.body;
    Node *cond = node->u.dowhile.cond;
    int lloop  = new_label();
    int lcont  = new_label();
    int lbreak = new_label();
    codegen_ctx.break_labels[codegen_ctx.loop_depth] = lbreak;
    codegen_ctx.cont_labels[codegen_ctx.loop_depth]  = lcont;
    codegen_ctx.loop_depth++;
    gen_label(lloop);
    gen_stmt(body);
    gen_label(lcont);
    gen_expr(cond);
    gen_jnz(lloop);
    gen_label(lbreak);
    codegen_ctx.loop_depth--;
}
void gen_breakstmt(Node *node)
{
    if (codegen_ctx.loop_depth == 0)
        error("break outside loop or switch\n");
    gen_j(codegen_ctx.break_labels[codegen_ctx.loop_depth - 1]);
}
void gen_continuestmt(Node *node)
{
    if (codegen_ctx.loop_depth == 0)
        error("continue outside loop\n");
    gen_j(codegen_ctx.cont_labels[codegen_ctx.loop_depth - 1]);
}
void gen_exprstmt(Node *node)
{
    gen_expr(node->u.exprstmt.expr);
}
bool is_constexpr(Node *n)
{
    if (n->kind == ND_LITERAL) return true;
    // Enum constants are compile-time integers
    if (n->kind == ND_IDENT && n->symbol && n->symbol->is_enum_const) return true;
    // Composite expressions are constexpr only if all operands are.
    if (n->kind == ND_BINOP)
        return is_constexpr(n->u.binop.lhs) && is_constexpr(n->u.binop.rhs);
    if (n->kind == ND_UNARYOP)
        return is_constexpr(n->u.unaryop.operand);
    return false;
}
int count_constexpr(Node *n)
{
    if (is_constexpr(n))
        return 1;
    int count = 0;
    if (n->kind == ND_INITLIST)
    {
        for (Node *c = n->u.initlist.items; c; c = c->next)
            count += count_constexpr(c);
    }
    return count;
}
void gen_inits(Node *n, Symbol *s, int vaddr, int offset, int depth)
{
    printf(";%s %s vaddr:%d os:%d depth:%d\n", __func__, nodestr(n->kind),vaddr,offset, depth);
    // Compute the element size (innermost) and row stride at current depth
    int elem_sz = array_elem_type(s->type)->size;
    // stride at this depth = size of elem at depth levels down
    Type *arr_at = s->type;
    for (int _d = 0; _d < depth && arr_at->base == TB_ARRAY; _d++)
        arr_at = arr_at->u.arr.elem;
    int row_stride = (arr_at->base == TB_ARRAY) ? arr_at->u.arr.elem->size : elem_sz;

    // Recurse through initlist
    int ptr = offset;
    for (Node *item = n->u.initlist.items; item; item = item->next)
        if (is_constexpr(item))
        {
            gen_lea(vaddr + ptr);
            gen_push();
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
    printf(";%s %s os:%d depth:%d\n", __func__, nodestr(n->kind),offset, depth);
    // Compute element size and row stride at current depth
    int elem_sz = array_elem_type(s->type)->size;
    Type *arr_at = s->type;
    for (int _d = 0; _d < depth && arr_at->base == TB_ARRAY; _d++)
        arr_at = arr_at->u.arr.elem;
    int row_stride = (arr_at->base == TB_ARRAY) ? arr_at->u.arr.elem->size : elem_sz;

    // Recurse through initlist
    int ptr = offset;
    for (Node *item = n->u.initlist.items; item; item = item->next)
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
    int i = int_constexpr(n->u.unaryop.operand);
    if (n->op_kind == TK_MINUS)
        return -i;
    return i;
}
int int_binop(Node *n)
{
    int i = int_constexpr(n->u.binop.lhs);
    int j = int_constexpr(n->u.binop.rhs);
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
    if (n->kind == ND_IDENT && n->symbol && n->symbol->is_enum_const)
        return n->symbol->offset;
    return (int)n->ival;
}

// Write struct field initializers for a local struct.
// vaddr: lea argument for struct base (negative, e.g. -4 for bp-4)
// st: the struct/union Type
static void gen_struct_inits(Node *n, int vaddr, Type *st)
{
    Field *f = st->u.composite.members;
    for (Node *child = n->u.initlist.items; child && f; child = child->next, f = f->next)
    {
        if (child->kind == ND_INITLIST)
        {
            if (f->type->base == TB_STRUCT)
                gen_struct_inits(child, vaddr + f->offset, f->type);
        }
        else if (is_constexpr(child))
        {
            gen_lea(vaddr + f->offset);
            gen_push();
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
    for (Node *child = n->u.initlist.items; child && f; child = child->next, f = f->next)
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

void gen_initlist(Node *n, Symbol *s)
{
    // Address of variable to be initialised is on the stack
    Type *t = s->type;

    int constexpr = count_constexpr(n);
    if (n->st->scope.depth)
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
            for(l = n; !is_constexpr(l); l = l->u.initlist.items);
            gen_varaddr_from_ident(n, s->name);
            gen_push();
            gen_expr(l);
            gen_sw();
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
            char *data = calloc(1, t->size);
            gen_struct_mem_inits(data, n, t, 0);
            gen_bytes(data, t->size);
        }
        else if (!array_dimensions(t))
        {
            // Scalar object, just take the first element of the list. There should be
            // only one element
            if (constexpr != 1)
                error("Should be exactly one constexpr in initialiser\n");
            Node *l;
            for(l = n; !is_constexpr(l); l = l->u.initlist.items);
            if (istype_fp(t))
                emit_float_bytes(l->fval);
            else
                gen_word(int_constexpr(l));
        }
        else if (constexpr)
        {
            char *data = calloc(1, t->size);
            gen_mem_inits(data, n, s, 0, 0, 0);
            gen_bytes(data, t->size);
        }

    }
}

void gen_decl(Node *node)
{
    printf(";%s declaration\n", __func__);
    if (node->u.declaration.sclass == TK_TYPEDEF) return;
    if (node->u.declaration.sclass == TK_EXTERN)  return;  // extern decl → no data emission
    // The symbol table has all the details needed for declarations.
    // Space is reserved at the start of the compound statement

    // Iterate over declarators, generating code for initialisations
    for (Node *n = node->u.declaration.decls; n; n = n->next)
    {
        // Different treatment if at scope 0
        if (n->st->scope.depth)
        {
            if (n->kind == ND_DECLARATOR && n->symbol && n->symbol->is_local_static)
            {
                codegen_ctx.local_statics[codegen_ctx.local_static_count++] = (LocalStaticEntry){
                    n->symbol->offset, n->symbol, n
                };
                continue;
            }
            if (n->kind == ND_DECLARATOR && n->u.declarator.init != NULL)
            {
                // This is an initialiser.
                // Get ident from symbol table
                if (!n->symbol)
                    error("Missing symbol!\n");
                Node *init = n->u.declarator.init;
                if (init->kind == ND_INITLIST)
                {
                    gen_initlist(init, n->symbol);
                }
                else if (init->strval && istype_array(n->symbol->type))
                {
                    // Local char array init from string literal: store bytes one by one
                    int vaddr = -find_local_addr(n, n->symbol->name).offset;
                    char *str = init->strval;
                    int   len = init->strval_len + 1;
                    for (int i = 0; i < len; i++)
                    {
                        gen_lea(vaddr + i);
                        gen_push();
                        gen_imm((unsigned char)str[i]);
                        gen_sb();
                    }
                }
                else
                {
                    gen_varaddr_from_ident(n, n->symbol->name);
                    gen_push();
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
                gen_align();
                if (n->symbol->is_static)
                {
                    char mangled[80];
                    snprintf(mangled, sizeof(mangled), "_s%d_%s",
                             n->symbol->tu_index, n->symbol->name);
                    gen_symlabel(mangled);
                }
                else
                    gen_symlabel(n->symbol->name);
                // If there is no init, we make space

                if (n->u.declarator.init != NULL)
                {
                    // Initialiser
                    Node *init = n->u.declarator.init;
                    if (init->kind == ND_INITLIST)
                    {
                        gen_initlist(init, n->symbol);
                    }
                    else if (init->strval && istype_array(n->symbol->type))
                    {
                        // char s[] = "hello" — emit bytes directly
                        gen_bytes(init->strval, init->strval_len + 1);
                    }
                    else if (init->strval)
                    {
                        // char *p = "hello" — emit pointer to deferred string data
                        int sid = new_strlit(init->strval, init->strval_len);
                        printf("    word    _l%d\n", sid);
                    }
                    else
                    {
                        if (istype_fp(n->symbol->type))
                            emit_float_bytes(init->fval);
                        else
                        {
                            int val = int_constexpr(init);
                            gen_word(val);
                        }
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
    printf(";%s\n", __func__);
    // Make space on stack for this scope's locals
    gen_adj(-node->symtable->size);
    for (Node *n = node->u.compstmt.stmts; n; n = n->next)
    {
        if (n->kind == ND_DECLARATION)
            gen_decl(n);
        else
            gen_stmt(n);
    }
    // Release the space back
    gen_adj(node->symtable->size);
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
    gen_label(find_label_id(node->u.labelstmt.name));
    if (node->u.labelstmt.stmt)
        gen_stmt(node->u.labelstmt.stmt);
}
void gen_gotostmt(Node *node)
{
    gen_j(find_label_id(node->u.label));
}
void gen_switchstmt(Node *node)
{
    printf(";%s\n", __func__);
    if (codegen_ctx.loop_depth >= 64) error("too deeply nested");
    Node *selector = node->u.switchstmt.selector;
    Node *body     = node->u.switchstmt.body; // ND_COMPSTMT
    int lbreak     = new_label();
    int ldefault   = -1;
    // Phase 1: assign codegen_ctx.label_counter to cases and emit comparisons
    for (Node *ch = body->u.compstmt.stmts; ch; ch = ch->next)
    {
        if (ch->kind == ND_CASESTMT) {
            int lcase   = new_label();
            ch->offset  = lcase;
            gen_expr(selector);
            gen_push();
            gen_imm((int)ch->ival);
            gen_eq();
            gen_jnz(lcase);
        } else if (ch->kind == ND_DEFAULTSTMT) {
            int ldef   = new_label();
            ch->offset = ldef;
            ldefault   = ldef;
        }
    }
    if (ldefault >= 0) gen_j(ldefault);
    else               gen_j(lbreak);
    // Phase 2: emit body with case codegen_ctx.label_counter
    codegen_ctx.break_labels[codegen_ctx.loop_depth] = lbreak;
    codegen_ctx.cont_labels[codegen_ctx.loop_depth]  = -1;
    codegen_ctx.loop_depth++;
    gen_adj(-body->symtable->size);
    for (Node *ch = body->u.compstmt.stmts; ch; ch = ch->next)
    {
        if (ch->kind == ND_CASESTMT || ch->kind == ND_DEFAULTSTMT)
            gen_label(ch->offset);
        else if (ch->kind == ND_DECLARATION)
            gen_decl(ch);
        else
            gen_stmt(ch);
    }
    gen_adj(body->symtable->size);
    codegen_ctx.loop_depth--;
    gen_label(lbreak);
}
void gen_stmt(Node *node)
{
    printf(";%s\n", __func__);
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
    case ND_STMT:        gen_stmt(node->u.stmt_wrap.body); return;
    default:;
    }
}
void gen_param_list(Node *node)
{
    gen_preamble();
}
void gen_function(Node *node)
{
    printf(";%s\n", __func__);
    Node *first_decl = node->u.declaration.decls;
    Node *func_body  = node->u.declaration.func_body;
    Symbol *fsym = first_decl->symbol;
    if (fsym->is_static)
        printf("_s%d_%s:\n", fsym->tu_index, fsym->name);
    else
        printf("%s:\n", fsym->name);
    codegen_ctx.label_table_size = 0;
    collect_labels(func_body);
    gen_param_list(first_decl);
    gen_stmt(func_body);
    gen_return();
}
void gen_code(Node *node, int tu_index)
{
    printf(";%s\n", __func__);

    // Pass 1: emit function definitions (text area)
    for (Node *c = node->u.program.decls; c; c = c->next)
    {
        if (c->kind == ND_DECLARATION && c->u.declaration.is_func_defn)
            gen_function(c);
    }
    // Pass 2: emit global variable declarations (data area)
    for (Node *c = node->u.program.decls; c; c = c->next)
    {
        if (c->kind == ND_DECLARATION && !c->u.declaration.is_func_defn)
            gen_decl(c);
    }
    // Pass 2b: local static data collected during pass 1 (function codegen).
    for (int i = 0; i < codegen_ctx.local_static_count; i++)
    {
        LocalStaticEntry *e = &codegen_ctx.local_statics[i];
        Node *n = e->decl_node;
        gen_align();
        printf("_ls%d:\n", e->id);
        Node *init = n->u.declarator.init;
        if (init != NULL)
        {
            if (init->kind == ND_INITLIST)
                gen_initlist(init, e->sym);
            else if (init->strval && istype_array(e->sym->type))
                gen_bytes(init->strval, init->strval_len + 1);
            else if (init->strval)
            {
                int sid = new_strlit(init->strval, init->strval_len);
                printf("    word    _l%d\n", sid);
            }
            else if (istype_fp(e->sym->type))
                emit_float_bytes(init->fval);
            else
            {
                int val = int_constexpr(init);
                gen_word(val);
            }
        }
        else
            gen_zeros(e->sym->type->size);
    }
    // Pass 3: emit deferred string literal data
    for (int i = 0; i < codegen_ctx.strlit_count; i++)
    {
        gen_align();
        gen_label(codegen_ctx.strlits[i].id);
        gen_bytes(codegen_ctx.strlits[i].data, codegen_ctx.strlits[i].len + 1);
    }
}

