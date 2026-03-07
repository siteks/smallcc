

#include "mycc.h"


extern Symbol_table *symbol_table;
char *scope_str(Scope sc);


int find_local_addr(Node *node, char *name)
{
    // Get the offset from bp of the local variable in node.
    // Go to scope in node, then search for variable name in that scope,
    // and successively enclosing scopes, until found.
    Scope sc = node->scope;
    fprintf(stderr, "%s scope is %s\n", __func__, scope_str(sc));
    Symbol_table *st = symbol_table;
    if (sc.depth)
    {
        for(int d = 1; d <= sc.depth; d++)
        {
            int index = sc.indices[d - 1] - 1;
            st = st->children[index];
        }
    }
    // st now pointing to symbol table at scope.
    Symbol *s;
    bool found = false;
    while(!found)
    { 
        fprintf(stderr, "Searching in scope:%s\n", scope_str(st->scope));
        for(s = st->idents; s; s = s->next)
        {
            fprintf(stderr, "Ident:%s\n", s->name);
            if (!strcmp(name, s->name))
            {
                found = true;
                break;
            }
        }
        if (found)
            break;
        // Not found in this scope, go to enclosing scope
        fprintf(stderr, "Not found, going to enclosing scope\n");
        if (st->parent)
            st = st->parent;
    }
    if (!found)
    {
        error("Symbol %s not found!\n", name);
    }
    if (s->is_local_static)
        return -1;

    // Compute global_offset dynamically: sum of all non-global ancestor scope sizes.
    // This is correct even for C99-style mixed declarations, where the parse-time
    // st->global_offset may have been captured before later siblings were added.
    int global_offset = 0;
    Symbol_table *ancestor = st->parent;
    while (ancestor && ancestor->scope.depth > 0)
    {
        global_offset += ancestor->size;
        ancestor = ancestor->parent;
    }
    int offset = global_offset + s->offset;
    if (s->is_param)
    {
        // FIXME!! This is very hacky, but we set bit 30 to indicate that 
        // this is a param, and the offset is in the opposite direction to a
        // local
        offset |= 0x40000000;
    }
    if (!st->scope.depth)
        // If at top scope, addresses are labels
        offset = -1;
    printf(";%s ident:%s at scope:%s %s got offset %d\n", __func__, name, scope_str(st->scope), 
        s->is_param ? "is param" : "", offset&0xffff);
    return offset;
}


static int labels;
static int break_labels[64];
static int cont_labels[64];
static int loop_depth = 0;
static int current_tu = 0;

typedef struct { int id; char *data; int len; } StrLit;
static StrLit strlits[512];
static int    strlit_count = 0;

typedef struct { int id; Symbol *sym; Node *decl_node; } LocalStaticEntry;
static LocalStaticEntry local_statics[512];
static int              local_static_count = 0;

static int new_strlit(char *data, int len)
{
    int id = labels++;
    strlits[strlit_count].id   = id;
    strlits[strlit_count].data = data;
    strlits[strlit_count].len  = len;
    strlit_count++;
    return id;
}

int new_label();

typedef struct
{
    char name[64];
    int  id;
} LabelEntry;
static LabelEntry label_table[64];
static int        label_table_size;

void reset_codegen(void)
{
    strlit_count      = 0;
    local_static_count = 0;
    loop_depth        = 0;
    label_table_size  = 0;
    memset(break_labels, 0, sizeof(break_labels));
    memset(cont_labels,  0, sizeof(cont_labels));
    // 'labels' is NOT reset — monotonically increasing across TUs.
}

static void collect_labels(Node *node)
{
    if (node->kind == ND_LABELSTMT)
    {
        label_table[label_table_size].id = new_label();
        strncpy(label_table[label_table_size].name, node->val, 63);
        label_table_size++;
    }
    for (int i = 0; i < node->child_count; i++)
        collect_labels(node->children[i]);
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
int new_label()                 { return labels++; }
void gen_label(int label)       { printf("_l%d:\n", label); }
void gen_symlabel(char *name)   { printf("%s:\n", name); }
void gen_text()                 { printf(".text\n"); }
void gen_data()                 { /* printf(".data\n");*/ }
void gen_align()                { printf("    align\n"); }
void gen_word(int d)            { printf("    word 0x%04x\n", d & 0xffff); }
void gen_jz(int label)          { printf("    jz      _l%d\n", label); }
void gen_jnz(int label)         { printf("    jnz     _l%d\n", label); }
void gen_j(int label)           { printf("    j       _l%d\n", label); }
void gen_push()                 { printf("    push\n"); }
void gen_pop()                  { printf("    pop\n"); }
void gen_pushw()                { printf("    pushw\n"); }
void gen_popw()                 { printf("    popw\n"); }
void gen_jl(char *label)        { printf("    jl %s\n", label); }
void gen_jli()                  { printf("    jli\n"); }
void gen_adj(int offset)        { printf("    adj %d\n", offset); }
void gen_add()                  { printf("    add\n"); }
void gen_sub()                  { printf("    sub\n"); }
void gen_mul()                  { printf("    mul\n"); }
void gen_div()                  { printf("    div\n"); }
void gen_mod()                  { printf("    mod\n"); }
void gen_eq()                   { printf("    eq\n"); }
void gen_ne()                   { printf("    ne\n"); }
void gen_lt()                   { printf("    lt\n"); }
void gen_le()                   { printf("    le\n"); }
void gen_gt()                   { printf("    gt\n"); }
void gen_ge()                   { printf("    ge\n"); }
void gen_shiftl()               { printf("    shl\n"); }
void gen_shiftr()               { printf("    shr\n"); }
void gen_bitor()                { printf("    or\n"); }
void gen_bitand()               { printf("    and\n"); }
void gen_bitxor()               { printf("    xor\n"); }
void gen_lb()                   { printf("    lb\n"); }
void gen_lw()                   { printf("    lw\n"); }
void gen_ll()                   { printf("    ll\n"); }
void gen_sb()                   { printf("    sb\n"); }
void gen_sw()                   { printf("    sw\n"); }
void gen_sl()                   { printf("    sl\n"); }
void gen_sxb()                  { printf("    sxb\n"); }
void gen_sxw()                  { printf("    sxw\n"); }
void gen_fadd()                 { printf("    fadd\n"); }
void gen_fsub()                 { printf("    fsub\n"); }
void gen_fmul()                 { printf("    fmul\n"); }
void gen_fdiv()                 { printf("    fdiv\n"); }
void gen_flt()                  { printf("    flt\n"); }
void gen_fle()                  { printf("    fle\n"); }
void gen_fgt()                  { printf("    fgt\n"); }
void gen_fge()                  { printf("    fge\n"); }
void gen_itof()                 { printf("    itof\n"); }
void gen_ftoi()                 { printf("    ftoi\n"); }
void gen_assign()               { }
void gen_lea(int o)             { printf("    lea     %d\n", o); }

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
    printf("    immw    0x%04x\n", val);
    if (val & 0xffff0000)
        printf("    immwh   0x%04x\n", val>>16);
}
void gen_pushi(int val)
{
    gen_imm(val);
    printf("    push\n");
}
void gen_logor()
{
    int l1 = new_label();
    int l2 = new_label();
    gen_jnz(l1);
    gen_pop();
    gen_jnz(l1);
    gen_j(l2);
    gen_label(l1);
    gen_imm(1);
    gen_label(l2);

}
void gen_logand()
{
    int l1 = new_label();
    int l2 = new_label();
    gen_jz(l1);
    gen_pop();
    gen_jz(l1);
    gen_imm(1);
    gen_label(l1);
}
void gen_st(int s)
{
    if (s == 1) gen_sb();
    if (s == 2) gen_sw();
    if (s == 4) gen_sl();
}
void gen_ld(int s)
{
    if (s == 1) gen_lb();
    if (s == 2) gen_lw();
    if (s == 4) gen_ll();
}

void gen_imm_float(double val)
{
    float f = (float)val;
    unsigned int bits;
    memcpy(&bits, &f, sizeof(bits));
    printf("    immw    0x%04x\n", bits & 0xffff);
    printf("    immwh   0x%04x\n", (bits >> 16) & 0xffff);
}
void gen_varaddr_from_ident(Node *node, char *name)
{
    int offset = find_local_addr(node, name);
    if (offset < 0)
    {
        Symbol *sym = node->symbol;
        if (sym && sym->is_local_static)
            printf("    immw    _ls%d\n", sym->offset);
        else if (sym && sym->is_static)
            printf("    immw    _s%d_%s\n", sym->tu_index, sym->name);
        else
            printf("    immw    %s\n", name);
    }
    else
        if (offset & 0x40000000)
            // This is a function parameter
            printf("    lea     %d\n", offset & 0xffff);
        else
            // This is a local variable
            printf("    lea     %d\n", -(offset & 0xffff));
}
void gen_varaddr(Node *node)
{
    if (node->kind != ND_IDENT)
        error("Expecting local var got %s", nodestr(node->kind));
    
    gen_varaddr_from_ident(node, node->val);
}
void gen_preamble(int words)
{
    printf(";Reserve space for %d words\n", words);
    printf("    enter   %d\n", words);
}
void gen_postamble()
{
    printf(";%s\n", __func__);
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

// Indirect call through function pointer variable: fp(args)
// node is the ND_IDENT for fp; all children are arguments.
void gen_callfunction_via_ptr(Node *node)
{
    int param_size = 0;
    for (int i = node->child_count - 1; i >= 0; i--)
    {
        gen_expr(node->children[i]);
        int s = node->children[i]->type->size;
        if (s == 2) gen_pushw(); else gen_push();
        param_size += s;
    }
    // Load function pointer value; gen_varaddr uses bp-relative lea,
    // which is safe even after pushing arguments.
    gen_varaddr(node);
    gen_ld(node->symbol->type->size);
    gen_jli();
    gen_adj(param_size);
}

// Indirect call through dereferenced pointer: (*fp)(args)
// node is UNARYOP "*" with is_function=true.
// node->children[0] = the pointer expression; children[1..] = arguments.
void gen_callfunction_via_deref(Node *node)
{
    int param_size = 0;
    for (int i = node->child_count - 1; i >= 1; i--)
    {
        gen_expr(node->children[i]);
        int s = node->children[i]->type->size;
        if (s == 2) gen_pushw(); else gen_push();
        param_size += s;
    }
    // Evaluate pointer expression — yields function address in r0.
    gen_expr(node->children[0]);
    gen_jli();
    gen_adj(param_size);
}

void gen_callfunction(Node *node)
{
    // putchar(c) is a CPU builtin
    if (!strcmp(node->symbol->name, "putchar"))
    {
        if (node->child_count != 1) error("putchar requires 1 argument\n");
        gen_expr(node->children[0]);
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
    int param_size = 0;
    for(int i = node->child_count - 1; i >= 0; i--)
    {
        gen_expr(node->children[i]);
        int s = node->children[i]->type->size;
        // Function designators decay to pointer-sized values when passed as args
        if (s == 0) s = 2;
        if (s == 2)
            gen_pushw();
        else
            gen_push();
        param_size += s;
    }
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
void gen_offset(Node *node)
{
    printf(";%s %s %s offset:%d\n", __func__, nodestr(node->kind), node->val, node->offset);
    // lhs is structure, rhs is member within
    gen_imm(node->offset);
}
void gen_addr(Node *node)
{
    printf(";%s %s %s\n", __func__, nodestr(node->kind), node->val);
    if (node->kind == ND_IDENT)
    {
        gen_varaddr(node);
    }
    else if (node->kind == ND_UNARYOP && !strcmp(node->val, "*"))
    {
        gen_expr(node->children[0]);
    }
    else if (node->kind == ND_MEMBER)
    {
        if (!strcmp(node->val, "->"))
            gen_expr(node->children[0]);    // load pointer value
        else
            gen_addr(node->children[0]);    // struct base address
        gen_push();
        gen_offset(node);
        gen_add();
    }
    else
        error("Expecting lvalue\n");
}
void gen_cast(Type2 *src, Type2 *dst)
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
    if (istype_intlike(src) && (istype_float(dst) || istype_double(dst)))
    {
        if (src->size == 1) gen_sxb();
        else if (src->size == 2) gen_sxw();
        gen_itof();
        return;
    }
    if ((istype_float(src) || istype_double(src)) && istype_intlike(dst))
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
    if ((istype_float(src) || istype_double(src)) && (istype_float(dst) || istype_double(dst)))
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
    printf(";%s %s %s\n", __func__, nodestr(node->kind), node->val);
    if (node->kind == ND_BINOP)
    {
        if (!strcmp(node->val, ","))
        {
            gen_expr(node->children[0]);    // evaluate for side effects
            gen_expr(node->children[1]);    // result is rhs
            return;
        }
        gen_expr(node->children[0]);
        gen_push();
        gen_expr(node->children[1]);
        if (istype_float(node->children[0]->type) || istype_double(node->children[0]->type))
        {
            if (!strcmp(node->val, "+"))   { gen_fadd(); return; }
            if (!strcmp(node->val, "-"))   { gen_fsub(); return; }
            if (!strcmp(node->val, "*"))   { gen_fmul(); return; }
            if (!strcmp(node->val, "/"))   { gen_fdiv(); return; }
            if (!strcmp(node->val, "<"))   { gen_flt();  return; }
            if (!strcmp(node->val, "<="))  { gen_fle();  return; }
            if (!strcmp(node->val, ">"))   { gen_fgt();  return; }
            if (!strcmp(node->val, ">="))  { gen_fge();  return; }
        }
        if (!strcmp(node->val, "+"))    {gen_add(); return;}
        if (!strcmp(node->val, "-"))    {gen_sub(); return;}
        if (!strcmp(node->val, "*"))    {gen_mul(); return;}
        if (!strcmp(node->val, "/"))    {gen_div(); return;}
        if (!strcmp(node->val, "<"))    {gen_lt(); return;}
        if (!strcmp(node->val, "<="))   {gen_le(); return;}
        if (!strcmp(node->val, ">"))    {gen_gt(); return;}
        if (!strcmp(node->val, ">="))   {gen_ge(); return;}
        if (!strcmp(node->val, "=="))   {gen_eq(); return;}
        if (!strcmp(node->val, "!="))   {gen_ne(); return;}
        if (!strcmp(node->val, ">>"))   {gen_shiftr(); return;}
        if (!strcmp(node->val, "<<"))   {gen_shiftl(); return;}
        if (!strcmp(node->val, "||"))   {gen_logor(); return;}
        if (!strcmp(node->val, "&&"))   {gen_logand(); return;}
        if (!strcmp(node->val, "|"))    {gen_bitor(); return;}
        if (!strcmp(node->val, "^"))    {gen_bitxor(); return;}
        if (!strcmp(node->val, "&"))    {gen_bitand(); return;}
        if (!strcmp(node->val, "%"))    {gen_mod(); return;}
        error("%s not handled in codegen", node->val);
    }
    else if (node->kind == ND_LITERAL)
    {
        if (node->strval)
        {
            int sid = new_strlit(node->strval, node->strval_len);
            printf("    immw    _l%d\n", sid);
        }
        else if (istype_float(node->type) || istype_double(node->type))
            gen_imm_float(node->fval);
        else
            gen_imm((int)strtol(node->val, 0, 0));
    }
    else if (node->kind == ND_IDENT)
    {
        Symbol *sym = node->symbol;
        if (istype_function(sym->type) && node->is_function)
        {
            // Direct named call: myfunc(args)
            gen_callfunction(node);
        }
        else if (istype_function(sym->type) && !node->is_function)
        {
            // Function name used as a value: fp = myfunc
            gen_varaddr_from_ident(node, sym->name);
        }
        else if (istype_ptr(sym->type)
                 && sym->type->u.ptr.pointee->base == TB2_FUNCTION
                 && node->is_function)
        {
            // Indirect call through function pointer variable: fp(args)
            gen_callfunction_via_ptr(node);
        }
        else if (node->symbol->is_enum_const)
        {
            gen_imm(node->symbol->offset);
        }
        else
        {
            gen_varaddr(node);
            // Don't fetch from address if array (array name IS the address)
            if (!istype_array(node->symbol->type))
                gen_ld(node->symbol->type->size);
        }
    }
    else if (node->kind == ND_UNARYOP)
    {
        if (!strcmp(node->val, "+"))    
        {
            gen_expr(node->children[0]);
        }      
        else if (!strcmp(node->val, "-"))
        {
            if (istype_float(node->children[0]->type) || istype_double(node->children[0]->type))
            {
                gen_imm_float(0.0);
                gen_push();
                gen_expr(node->children[0]);
                gen_fsub();
            }
            else
            {
                gen_imm(0);
                gen_push();
                gen_expr(node->children[0]);
                gen_sub();
            }
        }
        else if (!strcmp(node->val, "*"))
        {
            if (node->is_function)
            {
                // (*fp)(args) — indirect call through dereferenced pointer
                gen_callfunction_via_deref(node);
            }
            else
            {
                gen_expr(node->children[0]);
                gen_ld(elem_type(node->type)->size);
            }
        }
        else if (!strcmp(node->val, "&"))
        {
            gen_addr(node->children[0]);
        }
        else if (!strcmp(node->val, "!"))
        {
            gen_expr(node->children[0]);
            gen_push();
            gen_imm(0);
            gen_eq();
        }
        else if (!strcmp(node->val, "~"))
        {
            gen_expr(node->children[0]);
            gen_push();
            gen_imm(0xffffffff);
            gen_bitxor();
        }
        // Prefix increment/decrement: ++x / --x
        // Result: new value of x
        else if (!strcmp(node->val, "++") || !strcmp(node->val, "--"))
        {
            int is_inc = !strcmp(node->val, "++");
            Node *child = node->children[0];
            int sz = child->type ? child->type->size : 2;
            gen_addr(child);
            gen_push();         // stack: [&x]
            gen_expr(child);
            gen_push();         // stack: [&x, x]
            gen_imm(1);
            if (is_inc) gen_add(); else gen_sub();  // r0 = x±1, stack: [&x]
            switch (sz)
            {
                case 1: gen_sb(); break;
                case 2: gen_sw(); break;
                default: gen_sl(); break;
            }
            // r0 still holds x±1 (sw/sb/sl don't modify r0)
        }
        // Postfix increment/decrement: x++ / x--
        // Result: old value of x
        else if (!strcmp(node->val, "post++") || !strcmp(node->val, "post--"))
        {
            int is_inc = !strcmp(node->val, "post++");
            Node *child = node->children[0];
            int sz = child->type ? child->type->size : 2;
            gen_expr(child);    // r0 = old_x
            gen_push();         // stack: [old_x]
            gen_addr(child);    // r0 = &x
            gen_push();         // stack: [old_x, &x]
            gen_expr(child);    // r0 = x (reload)
            gen_push();         // stack: [old_x, &x, x]
            gen_imm(1);
            if (is_inc) gen_add(); else gen_sub();  // r0 = x±1, stack: [old_x, &x]
            switch (sz)
            {
                case 1: gen_sb(); break;
                case 2: gen_sw(); break;
                default: gen_sl(); break;
            }
            // stack: [old_x], r0 = x±1
            gen_pop();          // r0 = old_x, stack: []
        }
    }
    else if (node->kind == ND_ASSIGN)
    {
        gen_addr(node->children[0]);
        gen_push();
        gen_expr(node->children[1]);
        switch(node->children[0]->type->size)
        {
            case 1:     gen_sb(); break;
            case 2:     gen_sw(); break;
            default:    gen_sl(); break;
        }
    }
    else if (node->kind == ND_CAST)
    {
        // The type conversion is defined by the type of the expression in child 1 and the 
        // type of the cast
        gen_expr(node->children[1]);
        gen_cast(node->children[1]->type, node->type);
    }
    else if (node->kind == ND_MEMBER && node->is_function)
    {
        // Call through a function-pointer struct member: s.fp(args) or s->fp(args)
        int param_size = 0;
        for (int i = node->child_count - 1; i >= 2; i--)
        {
            gen_expr(node->children[i]);
            int s = node->children[i]->type->size;
            if (s == 2) gen_pushw(); else gen_push();
            param_size += s;
        }
        gen_addr(node);     // r0 = address of the fn-ptr field
        gen_ld(2);          // r0 = fn ptr value (pointers are 2 bytes)
        gen_jli();
        gen_adj(param_size);
    }
    else if (node->kind == ND_MEMBER)
    {
        gen_addr(node);         // handles both . and ->
        gen_ld(node->type->size);
    }
    else if (node->kind == ND_TERNARY)
    {
        int l_else = new_label();
        int l_end  = new_label();
        gen_expr(node->children[0]);
        gen_jz(l_else);
        gen_expr(node->children[1]);
        gen_j(l_end);
        gen_label(l_else);
        gen_expr(node->children[2]);
        gen_label(l_end);
    }
    else if (node->kind == ND_VA_START)
    {
        // va_start(ap, last_param)
        // ap = bp + last_param_offset + last_param_size
        Node *ap_node  = node->children[0];
        Node *lp_node  = node->children[1];
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
        Node *ap_node = node->children[0];
        // Save old ap value
        gen_addr(ap_node);    // r0 = &ap
        gen_ld(2);            // r0 = ap (current vararg pointer)
        gen_push();           // stack: [old_ap]
        // Advance ap: ap = ap + s
        gen_addr(ap_node);    // r0 = &ap
        gen_push();           // stack: [old_ap, &ap]
        gen_addr(ap_node);    // r0 = &ap
        gen_ld(2);            // r0 = ap
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
    if (node->child_count)
    {
        // Must have an expression
        gen_expr(node->children[0]);
    }
    gen_return();

}
void gen_ifstmt(Node *node)
{
    printf(";%s\n", __func__);
    // Structure is expr, stmt, [stmt]
    gen_expr(node->children[0]);
    int label = new_label();
    gen_jz(label);
    gen_stmt(node->children[1]);
    gen_label(label);
    if (node->child_count == 3)
    {
        printf(";else clause\n");
        gen_stmt(node->children[2]);
    }
    
}
void gen_whilestmt(Node *node)
{
    printf(";%s\n", __func__);
    // structure is expr, stmt
    int lloop   = new_label();
    int lbreak  = new_label();
    break_labels[loop_depth] = lbreak;
    cont_labels[loop_depth]  = lloop;
    loop_depth++;
    gen_label(lloop);
    gen_expr(node->children[0]);
    gen_jz(lbreak);
    gen_stmt(node->children[1]);
    gen_j(lloop);
    gen_label(lbreak);
    loop_depth--;
}
void gen_decl(Node *node);  // forward declaration
void gen_forstmt(Node *node)
{
    printf(";%s\n", __func__);
    // children: [init, cond, inc, body]
    // If the init was a declaration, node->symtable holds the for-init scope.
    if (node->symtable)
        gen_adj(-node->symtable->size);
    if (node->children[0]->kind == ND_DECLARATION)
        gen_decl(node->children[0]);
    else if (node->children[0]->kind != ND_EMPTY)
        gen_expr(node->children[0]);
    int lloop  = new_label();
    int lcont  = new_label();
    int lbreak = new_label();
    break_labels[loop_depth] = lbreak;
    cont_labels[loop_depth]  = lcont;
    loop_depth++;
    gen_label(lloop);
    if (node->children[1]->kind != ND_EMPTY) {
        gen_expr(node->children[1]);
        gen_jz(lbreak);
    }
    gen_stmt(node->children[3]);
    gen_label(lcont);
    if (node->children[2]->kind != ND_EMPTY)
        gen_expr(node->children[2]);
    gen_j(lloop);
    gen_label(lbreak);
    loop_depth--;
    if (node->symtable)
        gen_adj(node->symtable->size);
}
void gen_dowhilestmt(Node *node)
{
    printf(";%s\n", __func__);
    // children: [body, cond]
    int lloop  = new_label();
    int lcont  = new_label();
    int lbreak = new_label();
    break_labels[loop_depth] = lbreak;
    cont_labels[loop_depth]  = lcont;
    loop_depth++;
    gen_label(lloop);
    gen_stmt(node->children[0]);
    gen_label(lcont);
    gen_expr(node->children[1]);
    gen_jnz(lloop);
    gen_label(lbreak);
    loop_depth--;
}
void gen_breakstmt(Node *node)
{
    gen_j(break_labels[loop_depth - 1]);
}
void gen_continuestmt(Node *node)
{
    gen_j(cont_labels[loop_depth - 1]);
}
void gen_exprstmt(Node *node)
{
    // printf(";%s\n", __func__);
    gen_expr(node->children[0]);
}
bool is_constexpr(Node *n)
{
    // TODO this isnt rigorous
    return n->kind == ND_LITERAL || n->kind == ND_BINOP || n->kind == ND_IDENT || n->kind == ND_UNARYOP;
}
int count_constexpr(Node *n)
{
    if (is_constexpr(n))
        return 1;
    int count = 0;
    for(int i = 0; i < n->child_count; i++)
        count += count_constexpr(n->children[i]);
    return count;
}
void gen_inits(Node *n, Symbol *s, int vaddr, int offset, int depth)
{
    printf(";%s %s vaddr:%d os:%d depth:%d\n", __func__, nodestr(n->kind),vaddr,offset, depth);
    // Compute the element size (innermost) and row stride at current depth
    int elem_sz = array_elem_type(s->type)->size;
    // stride at this depth = size of elem at depth levels down
    Type2 *arr_at = s->type;
    for (int _d = 0; _d < depth && arr_at->base == TB2_ARRAY; _d++)
        arr_at = arr_at->u.arr.elem;
    int row_stride = (arr_at->base == TB2_ARRAY) ? arr_at->u.arr.elem->size : elem_sz;

    // Recurse through initlist
    int ptr = offset;
    for(int i = 0; i < n->child_count; i++)
        if (is_constexpr(n->children[i]))
        {
            gen_lea(vaddr + ptr);
            gen_push();
            gen_expr(n->children[i]);
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
            gen_inits(n->children[i], s, vaddr, new_offset, depth + 1);
            // Now out of level, we move on a whole row
            offset = new_offset + row_stride;
            fprintf(stderr, "out of inits, now inc offset to %d\n", offset);
            ptr = offset;
        }
}

int int_constexpr(Node *n);
void gen_mem_inits(char *data, Node *n, Symbol *s, int vaddr, int offset, int depth)
{
    printf(";%s %s os:%d depth:%d\n", __func__, nodestr(n->kind),offset, depth);
    // Compute element size and row stride at current depth
    int elem_sz = array_elem_type(s->type)->size;
    Type2 *arr_at = s->type;
    for (int _d = 0; _d < depth && arr_at->base == TB2_ARRAY; _d++)
        arr_at = arr_at->u.arr.elem;
    int row_stride = (arr_at->base == TB2_ARRAY) ? arr_at->u.arr.elem->size : elem_sz;

    // Recurse through initlist
    int ptr = offset;
    for(int i = 0; i < n->child_count; i++)
        if (is_constexpr(n->children[i]))
        {
            int val = int_constexpr(n->children[i]);
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
            gen_mem_inits(data, n->children[i], s, vaddr, new_offset, depth + 1);
            // Now out of level, we move on a whole row
            offset = new_offset + row_stride;
            fprintf(stderr, "out of inits, now inc offset to %d\n", offset);
            ptr = offset;
        }
}

int int_unaryop(Node *n)
{
    int i = int_constexpr(n->children[0]);
    if (!strcmp(n->val, "-"))
        return -i;
    return i;
}
int int_binop(Node *n)
{
    int i = int_constexpr(n->children[0]);
    int j = int_constexpr(n->children[1]);
    if (!strcmp(n->val, "+")) return i + j;
    if (!strcmp(n->val, "-")) return i - j;
    if (!strcmp(n->val, "*")) return i * j;
    if (!strcmp(n->val, "/")) return i / j;
    return 0;
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
    return strtol(n->val, 0, 0);
}

// Write struct field initializers for a local struct.
// vaddr: lea argument for struct base (negative, e.g. -4 for bp-4)
// st: the struct/union Type2
static void gen_struct_inits(Node *n, int vaddr, Type2 *st)
{
    Field *f = st->u.composite.members;
    for (int i = 0; i < n->child_count && f; i++, f = f->next)
    {
        Node *child = n->children[i];
        if (child->kind == ND_INITLIST)
        {
            if (f->type->base == TB2_STRUCT)
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
// st: the struct/union Type2
static void gen_struct_mem_inits(char *data, Node *n, Type2 *st, int base)
{
    Field *f = st->u.composite.members;
    for (int i = 0; i < n->child_count && f; i++, f = f->next)
    {
        Node *child = n->children[i];
        if (child->kind == ND_INITLIST)
        {
            if (f->type->base == TB2_STRUCT)
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
    Type2 *t = s->type;

    int constexpr = count_constexpr(n);
    if (n->scope.depth)
    {
        if (t->base == TB2_STRUCT)
        {
            int vaddr = -find_local_addr(n, s->name);
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
            for(l = n; !is_constexpr(l); l = l->children[0]);
            // gen_varaddr_from_ident(n, n->symbol->name);
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
            int vaddr = -find_local_addr(n, s->name);
            gen_fill(vaddr, t->size);
            gen_inits(n, s, vaddr, 0, 0);
        }
    }
    else
    {
        if (t->base == TB2_STRUCT)
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
            for(l = n; !is_constexpr(l); l = l->children[0]);
            if (istype_float(t) || istype_double(t))
            {
                float f = (float)l->fval;
                char bytes[4];
                memcpy(bytes, &f, 4);
                gen_bytes(bytes, 4);
            }
            else
            {
                gen_word(int_constexpr(l));
            }
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
    printf(";%s %s\n", __func__, node->val);
    if (node->sclass == TK_TYPEDEF) return;
    if (node->sclass == TK_EXTERN)  return;  // extern decl → no data emission
    // The symbol table has all the details needed for declarations.
    // Space is reserved at the start of the compound statement

    // Iterate over declarators, generating code for initialisations
    for(int i = 0; i < node->child_count; i++)
    {
        Node *n = node->children[i];
        // Different treatment if at scope 0
        if (n->scope.depth)
        {
            if (n->kind == ND_DECLARATOR && n->symbol && n->symbol->is_local_static)
            {
                local_statics[local_static_count++] = (LocalStaticEntry){
                    n->symbol->offset, n->symbol, n
                };
                continue;
            }
            if (n->kind == ND_DECLARATOR && n->child_count == 2)
            {
                // This is an initialiser.
                // Get ident from symbol table
                if (!n->symbol)
                    error("Missing symbol!\n");
                if (n->children[1]->kind == ND_INITLIST)
                {
                    gen_initlist(n->children[1], n->symbol);
                }
                else if (n->children[1]->strval && istype_array(n->symbol->type))
                {
                    // Local char array init from string literal: store bytes one by one
                    int vaddr = -find_local_addr(n, n->symbol->name);
                    char *str = n->children[1]->strval;
                    int   len = n->children[1]->strval_len + 1;
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
                    gen_expr(n->children[1]);
                    gen_st(n->symbol->type->size);
                }
            }
        }
        else
        {
            // Global, we always make labels and allocate space
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

                if (n->child_count == 2)
                {
                    // Initialiser
                    if (n->children[1]->kind == ND_INITLIST)
                    {
                        gen_initlist(n->children[1], n->symbol);
                    }
                    else if (n->children[1]->strval && istype_array(n->symbol->type))
                    {
                        // char s[] = "hello" — emit bytes directly
                        gen_bytes(n->children[1]->strval, n->children[1]->strval_len + 1);
                    }
                    else if (n->children[1]->strval)
                    {
                        // char *p = "hello" — emit pointer to deferred string data
                        int sid = new_strlit(n->children[1]->strval, n->children[1]->strval_len);
                        printf("    word    _l%d\n", sid);
                    }
                    else
                    {
                        if (istype_float(n->symbol->type) || istype_double(n->symbol->type))
                        {
                            float f = (float)n->children[1]->fval;
                            char bytes[4];
                            memcpy(bytes, &f, 4);
                            gen_bytes(bytes, 4);
                        }
                        else
                        {
                            int val = int_constexpr(n->children[1]);
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
    for(int i = 0; i < node->child_count; i++)
    {
        Node *n = node->children[i];
        if (n->kind == ND_DECLARATION)
            gen_decl(n);
        else
            gen_stmt(n);
    }
    // Release the space back
    gen_adj(node->symtable->size);
}
void gen_labelstmt(Node *node)
{
    for (int i = 0; i < label_table_size; i++)
    {
        if (!strcmp(node->val, label_table[i].name))
        {
            gen_label(label_table[i].id);
            if (node->child_count)
                gen_stmt(node->children[0]);
            return;
        }
    }
    error("label '%s' not found\n", node->val);
}
void gen_gotostmt(Node *node)
{
    for (int i = 0; i < label_table_size; i++)
    {
        if (!strcmp(node->val, label_table[i].name))
        {
            gen_j(label_table[i].id);
            return;
        }
    }
    error("goto: label '%s' not found\n", node->val);
}
void gen_decl(Node *node);
void gen_switchstmt(Node *node)
{
    printf(";%s\n", __func__);
    Node *selector = node->children[0];
    Node *body     = node->children[1]; // ND_COMPSTMT
    int lbreak     = new_label();
    int ldefault   = -1;
    // Phase 1: assign labels to cases and emit comparisons
    for (int i = 0; i < body->child_count; i++) {
        Node *ch = body->children[i];
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
    // Phase 2: emit body with case labels
    break_labels[loop_depth] = lbreak;
    cont_labels[loop_depth]  = -1;
    loop_depth++;
    gen_adj(-body->symtable->size);
    for (int i = 0; i < body->child_count; i++) {
        Node *ch = body->children[i];
        if (ch->kind == ND_CASESTMT || ch->kind == ND_DEFAULTSTMT)
            gen_label(ch->offset);
        else if (ch->kind == ND_DECLARATION)
            gen_decl(ch);
        else
            gen_stmt(ch);
    }
    gen_adj(body->symtable->size);
    loop_depth--;
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
    case ND_STMT:        gen_stmt(node->children[0]); return;
    default:;
    }
}
void gen_param_list(Node *node)
{
    gen_preamble(0);
}
void gen_function(Node *node)
{
    printf(";%s\n", __func__);
    Symbol *fsym = node->children[0]->symbol;
    if (fsym->is_static)
        printf("_s%d_%s:\n", fsym->tu_index, fsym->name);
    else
        printf("%s:\n", fsym->name);
    label_table_size = 0;
    collect_labels(node->children[1]);
    gen_param_list(node->children[0]);
    gen_stmt(node->children[1]);
    gen_return();
}
void gen_globals(Node *node)
{

}
void gen_code(Node *node, int tu_index)
{
    current_tu = tu_index;
    printf(";%s\n", __func__);
    
    gen_globals(node);

    // Do two passes, one to get the globals allocated space, then the functions
    // gen_text();
    for(int i = 0; i < node->child_count; i++)
    {
        if (node->children[i]->kind == ND_DECLARATION && node->children[i]->is_func_defn)
            gen_function(node->children[i]);
    }
    // gen_data();
    for(int i = 0; i < node->child_count; i++)
    {
        if (node->children[i]->kind == ND_DECLARATION && !node->children[i]->is_func_defn)
            gen_decl(node->children[i]);
    }
    // Pass 2b: local static data collected during pass 1 (function codegen).
    for (int i = 0; i < local_static_count; i++)
    {
        LocalStaticEntry *e = &local_statics[i];
        Node *n = e->decl_node;
        gen_align();
        printf("_ls%d:\n", e->id);
        if (n->child_count == 2)
        {
            Node *init = n->children[1];
            if (init->kind == ND_INITLIST)
                gen_initlist(init, e->sym);
            else if (init->strval && istype_array(e->sym->type))
                gen_bytes(init->strval, init->strval_len + 1);
            else if (init->strval)
            {
                int sid = new_strlit(init->strval, init->strval_len);
                printf("    word    _l%d\n", sid);
            }
            else if (istype_float(e->sym->type) || istype_double(e->sym->type))
            {
                float f = (float)n->children[1]->fval;
                char bytes[4];
                memcpy(bytes, &f, 4);
                gen_bytes(bytes, 4);
            }
            else
            {
                int val = int_constexpr(n->children[1]);
                gen_word(val);
            }
        }
        else
            gen_zeros(e->sym->type->size);
    }
    // Pass 3: emit deferred string literal data
    for (int i = 0; i < strlit_count; i++)
    {
        gen_align();
        gen_label(strlits[i].id);
        gen_bytes(strlits[i].data, strlits[i].len + 1);
    }
}

