

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
        for(s = st->symbols; s; s = s->next)
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
    int offset = st->global_offset + s->offset;
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
int new_label()
{
    return labels++;
}
void gen_label(int label)
{
    printf("_l%d:\n", label);
}
void gen_symlabel(char *name)
{
    printf("%s:\n", name);
}
void gen_text()
{
    printf(".text\n");
}
void gen_data()
{
    // printf(".data\n");
}
void gen_align()
{
    printf("    align\n");
}
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
void gen_word(int d)
{
    printf("    word 0x%04x\n", d & 0xffff);
}
void gen_jz(int label)
{
    printf("    jz      _l%d\n", label);
}
void gen_jnz(int label)
{
    printf("    jnz     _l%d\n", label);
}
void gen_j(int label)
{
    printf("    j       _l%d\n", label);
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
void gen_push()
{
    printf("    push\n");
}
void gen_pop()
{
    printf("    pop\n");
}
void gen_pushw()
{
    printf("    pushw\n");
}
void gen_popw()
{
    printf("    popw\n");
}
void gen_jl(char *label)
{
    printf("    jl %s\n", label);
}
void gen_adj(int offset)
{
    printf("    adj %d\n", offset);
}
void gen_add()
{
    printf("    add\n");
}
void gen_sub()
{
    printf("    sub\n");
}
void gen_mul()
{
    printf("    mul\n");
}

void gen_div()
{
    printf("    div\n");
}
void gen_eq()
{
    printf("    eq\n");
}
void gen_ne()
{
    printf("    ne\n");
}
void gen_lt()
{
    printf("    lt\n");
}
void gen_le()
{
    printf("    le\n");
}
void gen_gt()
{
    printf("    gt\n");
}
void gen_ge()
{
    printf("    ge\n");
}
void gen_shiftl()
{
    printf("    shl\n");
}
void gen_shiftr()
{
    printf("    shr\n");
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
void gen_bitor()
{
    printf("    or\n");
}
void gen_bitand()
{
    printf("    and\n");
}
void gen_bitxor()
{
    printf("    xor\n");
}
void gen_lb()
{
    printf("    lb\n");
}
void gen_lw()
{
    printf("    lw\n");
}
void gen_ll()
{
    printf("    ll\n");
}
void gen_sb()
{
    printf("    sb\n");
}
void gen_sw()
{
    printf("    sw\n");
}
void gen_sl()
{
    printf("    sl\n");
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
void gen_sxb()
{
    printf("    sxb\n");
}
void gen_sxw()
{
    printf("    sxw\n");
}
void gen_assign()
{
}
void gen_lea(int o)
{
    printf("    lea     %d\n", o);
}
void gen_varaddr_from_ident(Node *node, char *name)
{
    int offset = find_local_addr(node, name);
    if (offset < 0)
        // Global, the name is a label
        printf("    immw    %s\n", name);
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
void gen_callfunction(Node *node)
{
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
        if (s == 2)
            gen_pushw();
        else
            gen_push();
        param_size += s;
    }
    gen_jl(node->symbol->name);
    // For the function postamble, we need to adjust the stack by the size of the
    // push parameters
    gen_adj(param_size);

}
void gen_offset(Node *node)
{
    printf(";%s %s %s\n", __func__, nodestr(node->kind), node->val);
    // lhs is structure, rhs is member within
    

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
        gen_addr(node->children[0]);
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
        gen_expr(node->children[0]);
        gen_push();
        gen_expr(node->children[1]);
        if (!strcmp(node->val, "+"))    {gen_add(); return;}
        if (!strcmp(node->val, "-"))    {gen_sub(); return;}
        if (!strcmp(node->val, "*"))    {gen_mul(); return;}
        if (!strcmp(node->val, "/"))    {gen_div(); return;}
        if (!strcmp(node->val, "<"))    {gen_lt(); return;}
        if (!strcmp(node->val, "<="))   {gen_le(); return;}
        if (!strcmp(node->val, ">"))    {gen_gt(); return;}
        if (!strcmp(node->val, ">="))   {gen_ge(); return;}
        if (!strcmp(node->val, "=="))   {gen_eq(); return;}
        if (!strcmp(node->val, ">>"))   {gen_shiftr(); return;}
        if (!strcmp(node->val, "<<"))   {gen_shiftl(); return;}
        if (!strcmp(node->val, "||"))   {gen_logor(); return;}
        if (!strcmp(node->val, "&&"))   {gen_logand(); return;}
        if (!strcmp(node->val, "|"))    {gen_bitor(); return;}
        if (!strcmp(node->val, "^"))    {gen_bitxor(); return;}
        if (!strcmp(node->val, "&"))    {gen_bitand(); return;}
        error("%s not handled in codegen", node->val);
    }
    else if (node->kind == ND_LITERAL)
    {
        gen_imm((int)strtol(node->val, 0, 0));
    }
    else if (node->kind == ND_IDENT)
    {
        // This could be a function call
        if (istype_function(node->symbol->type))
        {
            gen_callfunction(node);
        }
        else
        {
            gen_varaddr(node);
            // Don't fetch from address if pointer or array
            if (!istype_array(node->symbol->type) && !istype_ptr(node->symbol->type))
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
            gen_imm(0);
            gen_push();
            gen_expr(node->children[0]);
            gen_sub();
        }
        else if (!strcmp(node->val, "*"))
        {
            gen_expr(node->children[0]);
            // TODO!! size
            gen_ld(node->type->elem_size);
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
    int lloop   = labels++;
    int lbreak  = labels++;
    gen_label(lloop);
    gen_expr(node->children[0]);
    gen_jz(lbreak);
    gen_stmt(node->children[1]);
    gen_j(lloop);
    gen_label(lbreak);
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
    // Recurse through initlist
    int ptr = offset;
    for(int i = 0; i < n->child_count; i++)
        if (is_constexpr(n->children[i]))
        {
            gen_lea(vaddr + ptr);
            gen_push();
            gen_expr(n->children[i]);
            gen_st(s->type->elem_size);
            ptr += s->type->elem_size;
        }
        else
        {
            // Going deeper, we need to set the offset to be the start of the row
            int new_offset = offset;
            if (ptr != offset)
                // If we've done anything on this row, advance to next row
                new_offset = offset + *s->type->elems_per_row[depth] * s->type->elem_size;
            gen_inits(n->children[i], s, vaddr, new_offset, depth + 1);
            // Now out of level, we move on a whole row
            offset =  new_offset + *s->type->elems_per_row[depth] * s->type->elem_size;
            fprintf(stderr, "out of inits, now inc offset to %d\n", offset);
            ptr = offset;
        }
}

int int_constexpr(Node *n);
void gen_mem_inits(char *data, Node *n, Symbol *s, int vaddr, int offset, int depth)
{
    printf(";%s %s os:%d depth:%d\n", __func__, nodestr(n->kind),offset, depth);
    // Recurse through initlist
    int ptr = offset;
    for(int i = 0; i < n->child_count; i++)
        if (is_constexpr(n->children[i]))
        {
            int val = int_constexpr(n->children[i]);
            data[vaddr + ptr]       = val & 0xff;
            data[vaddr + ptr + 1]   = (val >> 8) & 0xff;
            ptr += s->type->elem_size;
        }
        else
        {
            // Going deeper, we need to set the offset to be the start of the row
            int new_offset = offset;
            if (ptr != offset)
                // If we've done anything on this row, advance to next row
                new_offset = offset + *s->type->elems_per_row[depth] * s->type->elem_size;
            gen_mem_inits(data, n->children[i], s, vaddr, new_offset, depth + 1);
            // Now out of level, we move on a whole row
            offset =  new_offset + *s->type->elems_per_row[depth] * s->type->elem_size;
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
    return strtol(n->val, 0, 0);
}
void gen_initlist(Node *n, Symbol *s)
{
    // Address of variable to be initialised is on the stack
    Type *t = s->type;

    int constexpr = count_constexpr(n);
    if (n->scope.depth)
    {
        if (!t->dimensions)
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
        if (!t->dimensions)
        {
            // Scalar object, just take the first element of the list. There should be 
            // only one element
            if (constexpr != 1)
                error("Should be exactly one constexpr in initialiser\n");
            Node *l;
            for(l = n; !is_constexpr(l); l = l->children[0]);
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
    printf(";%s %s\n", __func__, node->val);
    // The symbol table has all the details needed for declarations.
    // Space is reserved at the start of the compound statement

    // Iterate over declarators, generating code for initialisations
    for(int i = 0; i < node->child_count; i++)
    {
        Node *n = node->children[i];
        // Different treatment if at scope 0
        if (n->scope.depth)
        {
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
                // TODO string initialiser goes here
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
                gen_align();
                gen_symlabel(n->symbol->name);
                // If there is no init, we make space

                if (n->child_count == 2)
                {
                    // Initialiser
                    if (n->children[1]->kind == ND_INITLIST)
                    {
                        gen_initlist(n->children[1], n->symbol);
                    }
                    // TODO string initialiser goes here
                    else
                    {
                        int val = int_constexpr(n->children[1]);
                        gen_word(val);
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
void gen_stmt(Node *node)
{
    printf(";%s\n", __func__);
    switch(node->kind)
    {
    case ND_EXPRSTMT:   gen_exprstmt(node); return;
    case ND_COMPSTMT:   gen_compstmt(node); return;
    case ND_IFSTMT:     gen_ifstmt(node); return;
    case ND_WHILESTMT:  gen_whilestmt(node); return;
    case ND_RETURNSTMT: gen_returnstmt(node); return;
    case ND_STMT:       gen_stmt(node->children[0]); return;
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
    printf("%s:\n", node->children[0]->symbol->name);
    gen_param_list(node->children[0]);
    gen_stmt(node->children[1]);
    gen_return();
}
void gen_globals(Node *node)
{

}
void gen_code(Node *node)
{
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
}

