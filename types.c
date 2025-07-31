

#include "mycc.h"

// Type table. This is a DAG
Type    *types;


// Basic types, set to point to entries in the type table
Type    *t_void;
Type    *t_char;
Type    *t_uchar;
Type    *t_short;
Type    *t_ushort;
Type    *t_enum;
Type    *t_int;
Type    *t_uint;
Type    *t_long;
Type    *t_ulong;
Type    *t_float;
Type    *t_double;


static int scope_depth;
static int scope_indices[100];



Symbol_table    *symbol_table;
Symbol_table    *last_symbol_table;
Symbol_table    *curr_scope_st;



static Symbol *insert_tag(Node *node, char *ident);
static Symbol *new_symbol(Type *type, char *ident, int offset);
static Symbol *insert_ident(Node *node, Type *type, char *ident, bool is_param);


static char sbuf[1024];
static char *get_typename(Node *node)
{
    // TODO get the struct, union, enum, or typedef identifier
    return "";
}
static Type *new_type(Node *node, Node *child)
{
    // Construct a new type, the node must be a ND_DECLARATION
    Type *ty        = calloc(1, sizeof(Type));
    ty->typespec    = node->typespec;
    ty->typequal    = node->typequal;
    ty->sclass      = node->sclass;
    if (ty->typespec == TK_STRUCT || ty->typespec == TK_UNION 
    || ty->typespec == TK_ENUM || ty->typespec == TK_TYPEDEF)
    {
        ty->tag    = get_typename(node);
    }
    return ty;
}
static bool is_named_type(Type_base tb)
{
    return      (tb & TB_STRUCT)
            ||  (tb & TB_UNION)
            // ||  (tb & TK_ENUM);
            // ||  tk == TK_TYPEDEF;
            ;
}
static int align(int val, int size)
{
    if (size == 2) return (val & 1) ? (val & ~1) + 2 : val;
    if (size == 4) return (val & 3) ? (val & ~3) + 4 : val;
    return val;
}
static void calc_tsize(Type *t)
{
    // To work out what size a type is:
    //
    //  If there are no characters in the derivation, the size is the base type size.
    //
    //  If first character is '*' this is a pointer, all other
    //  characters do not change the size, which is 2.
    //
    //  If first character is '[' this is an array. The number of
    //  elements will be the product of all the constants inside 
    //  successive brackets []. If a pointer '*' is encountered,
    //  brackets after are ignored, the elem size is 2, otherwise
    //  the elem size is the base type size
    fprintf(stderr, "%s %s\n", __func__, fulltype_str(t));
    int s = 0;
    switch((int)t->typespec)
    {
        case TB_VOID:               s = 2; break;
        case TB_CHAR:
        case TB_SIGNED|TB_CHAR:
        case TB_UNSIGNED|TB_CHAR:   s = 1; break;
        case TB_SHORT:      
        case TB_SIGNED|TB_SHORT:      
        case TB_UNSIGNED|TB_SHORT:  s = 2; break;
        case TB_INT:        
        case TB_SIGNED|TB_INT:        
        case TB_UNSIGNED|TB_INT:    s = 2; break;
        case TB_LONG:       
        case TB_SIGNED|TB_LONG:       
        case TB_UNSIGNED|TB_LONG:   s = 4; break;
        case TB_FLOAT:              s = 4; break;
        case TB_DOUBLE:             s = 4; break;
        case TB_SIGNED:             s = 2; break;
        case TB_UNSIGNED:           s = 2; break;
        default:                    s = 2; break;
    }
    if (!t->derived || !t->derived[0])
    {
        // Base type
        t->size         = s;
        t->elem_size    = t->size;
        t->align        = s;
    }
    else if (t->derived[0] == '*')
    {
        // This is a pointer
        t->is_pointer   = true;
        t->size         = 2;
        t->elem_size    = t->size;
        t->align        = 2;
    }
    else if (t->derived[0] == '(')
    {
        // This is a function
        t->is_function  = true;
        t->size         = 2;
        t->elem_size    = t->size;
        t->align        = 2;
    }
    else if (t->derived[0] == '[')
    {
        // This is an array, work out the dimensions
        t->is_array     = true;
        char *p         = t->derived;
        t->dimensions   = 0;
        while(*p && *p != '*')
        {
            if (*p == '[')
            {
                p++;
                char *q;
                int d = strtol(p, &q, 0);
                if (!t->dimensions)
                    t->elements = d;
                else
                    t->elements *= d;
                t->dimensions++;
                t->dim_sizes = (int**)realloc(t->dim_sizes, t->dimensions * sizeof(int**));
                t->dim_sizes[t->dimensions - 1] = calloc(1, sizeof(int));
                *t->dim_sizes[t->dimensions - 1] = d;

                t->elems_per_row = (int**)realloc(t->elems_per_row, t->dimensions * sizeof(int**));
                t->elems_per_row[t->dimensions - 1] = calloc(1, sizeof(int));
                if (*q == ']')
                    p = q;
            }
            p++;
        }
        if (p && *p == '*')
            t->elem_size = 2;
        else
            t->elem_size = s;
        t->size     = t->elements * t->elem_size;
        t->align    = t->elem_size;
        int num_elems = 1;
        for(int i = t->dimensions - 1; i >= 0; i--)
        {
            *t->elems_per_row[i] = num_elems;
            num_elems *= *t->dim_sizes[i];
        }
    }
}

Type *insert_type(Node *node, char *ts)
{
    // Search type table, return location if found, otherwise insert
    fprintf(stderr, "%s ts:%s: typetag:%s\n", __func__, ts, node->typetag);
    Type *last = 0;
    bool found = false;
    for(Type *p = types; p; p = p->next)
    {
        last = p;

        // First check if there is a base type appropriate, if not, insert it
        if (    node->typespec == p->typespec
            &&  node->typequal == p->typequal
            &&  node->sclass == p->sclass
            &&  (!is_named_type(p->typespec) 
                || (node->typetag && p->tag && !strcmp(node->typetag, p->tag))))
        {
            found = true;
            fprintf(stderr, "%s found matching basetype\n", __func__);
            break;
        }
    }
    Type *t = 0;
    if (!found)
    {
        // Insert the base type at the end
        t           = calloc(1, sizeof(Type));
        t->typespec = node->typespec;
        t->typequal = node->typequal;
        t->sclass   = node->sclass;
        if (node->typetag)
            t->tag      = strdup(node->typetag);
        if (last)
            last->next  = t;
        else
            types = t;
        calc_tsize(t);
        fprintf(stderr, "%s inserting basetype %s\n", __func__, fulltype_str(t));
    }
    else
    {
        t = last;
    }
    if (!ts || !strcmp(ts, ""))
    {
        // derived string is empty, the type is a base type so just return now
        fprintf(stderr, "%s base type %s %016llx already exists %s\n", __func__, fulltype_str(t), (unsigned long long)t, t->tag);
        return t;
    }
    // If we get here, this is a derived type of the base type now in t. Search for it in the types
    for(Type *p = types; p; p = p->next)
    {
        last = p;
        if (p->derived && !strcmp(p->derived, ts) && p->basetype == t)
        {
            fprintf(stderr, "%s derived type %s %016llx already exists %s\n", __func__, fulltype_str(t), (unsigned long long)t, t->tag);
            return p;
        }
    }
    Type *dt        = calloc(1, sizeof(Type));
    dt->basetype    = t;
    dt->derived     = strdup(ts);
    last->next      = dt;
    calc_tsize(dt);
    fprintf(stderr, "%s ts:%s: fts:%s: %016llx\n", __func__, ts, fulltype_str(t), (unsigned long long)t);
    return dt;

}

static int offset;
static int max_align;
static int calc_struct_tsize(Type *t)
{
    fprintf(stderr, "%s\n", __func__);
    for(int i = 0; i < t->num_members; i++)
    {
        Type *m = t->members[i];
        if (m->typespec & TB_STRUCT)
        {
            int s   = calc_struct_tsize(m);
            m->size = s;
        }
        else
        {
            calc_tsize(m);
            if (m->align > max_align)
                max_align = m->align;
            offset      = align(offset, m->size);
            m->offset   = offset;
            offset      += m->size;
        }
    }
    return offset;
}
static Type *insert_struct_type(Type *t)
{
    fprintf(stderr, "%s\n", __func__);
    Type *last = 0;
    
    // We need to fill in the sizes and calculate the member offsets
    offset      = 0;
    max_align   = 0;
    int size    = calc_struct_tsize(t);
    t->size     = size;
    t->align    = max_align;


    for(Type *i = types; i; i = i->next)
    {
        if (t == i)
            return t;
        last = i;
    }
    if (last)
        last->next  = t;
    else
        types = t;

    return t;
}


Type *elem_type(Type *t)
{
    // If type is a pointer or an array, strip that
    // part of the type off and return the type of the
    // array elements, or the type pointed to. We do this 
    // by removing '[xx]' or '*' from the derived string.
    //
    // If the type so formed is not present in the type
    // table, insert it.
    if (t->derived[0] != '[' && t->derived[0] != '*')
        // This isn't an array or pointer derived, just
        // return the type
        return t; 

    Node *n = new_node(ND_CAST, 0, 0);
    n->typespec = t->typespec;
    n->typetag  = t->tag;
    if (t->derived[0] == '*')
        return insert_type(n, t->derived + 1);
    
    // Must be array, find closing bracket
    char *p;
    for(p = t->derived + 1; *p && *p != ']'; p++);
    if (!*p)
        error("Invalid type derivation\n");
    return insert_type(n, p + 1);
}


Type *find_type(char *name)
{
    // TODO find a typedef by name
    return 0;
}
bool istype_float(Type *t)  { return t == t_float; }
bool istype_double(Type *t) { return t == t_double; }
bool istype_char(Type *t)   { return t == t_char; }
bool istype_uchar(Type *t)  { return t == t_uchar; }
bool istype_short(Type *t)  { return t == t_short; }
bool istype_ushort(Type *t) { return t == t_ushort; }
bool istype_enum(Type *t)   { return t == t_enum; }
bool istype_int(Type *t)    { return t == t_int; }
bool istype_uint(Type *t)   { return t == t_uint; }
bool istype_long(Type *t)   { return t == t_long; }
bool istype_ulong(Type *t)  { return t == t_ulong; }
bool istype_intlike(Type *t)
{
    return  t == t_char || t == t_uchar
        ||  t == t_int || t == t_uint
        ||  t == t_long || t == t_ulong;
        
}
bool istype_ptr(Type *t)        { return t->is_pointer;}
bool istype_array(Type *t)      { return t->is_array;}
bool istype_function(Type *t)   { return t->is_function;}
// Add basic types to type table. 
void make_basic_types()
{
    fprintf(stderr, "%s\n", __func__);
    Node *n = new_node(ND_CAST, 0, 0);
    n->typespec = TB_VOID;              t_void      = insert_type(n, "");
    n->typespec = TB_CHAR;              t_char      = insert_type(n, "");
    n->typespec = TB_UNSIGNED|TB_CHAR;  t_uchar     = insert_type(n, "");
    n->typespec = TB_SHORT;             t_short     = insert_type(n, "");
    n->typespec = TB_UNSIGNED|TB_SHORT; t_ushort    = insert_type(n, "");
    n->typespec = TB_ENUM;              t_enum      = insert_type(n, "");
    n->typespec = TB_INT;               t_int       = insert_type(n, "");
    n->typespec = TB_UNSIGNED|TB_INT;   t_uint      = insert_type(n, "");
    n->typespec = TB_LONG;              t_long      = insert_type(n, "");
    n->typespec = TB_UNSIGNED|TB_LONG;  t_ulong     = insert_type(n, "");
    n->typespec = TB_FLOAT;             t_float     = insert_type(n, "");
    n->typespec = TB_DOUBLE;            t_double    = insert_type(n, "");
}
Type_base to_typespec(Token_kind tk)
{
    switch(tk)
    {
        case TK_VOID:       return TB_VOID;
        case TK_CHAR:       return TB_CHAR;
        case TK_SHORT:      return TB_SHORT;
        case TK_ENUM:       return TB_ENUM;
        case TK_INT:        return TB_INT;
        case TK_LONG:       return TB_LONG;
        case TK_FLOAT:      return TB_FLOAT;
        case TK_DOUBLE:     return TB_DOUBLE;
        case TK_SIGNED:     return TB_SIGNED;
        case TK_UNSIGNED:   return TB_UNSIGNED;
        case TK_STRUCT:     return TB_STRUCT;
        case TK_UNION:      return TB_UNION;
        default:            return 0;
    }
}
static bool is_struct_or_union(Type_base t)
{
    return (t & TB_STRUCT) || (t & TB_UNION);
}
static char *typebase_str(Type_base tb)
{
    return 
        tb == TB_VOID       ? "void " :
        tb == TB_CHAR       ? "char " :
        tb == TB_SHORT      ? "short " :
        tb == TB_ENUM       ? "enum " :
        tb == TB_INT        ? "int " :
        tb == TB_LONG       ? "long " :
        tb == TB_FLOAT      ? "float " :
        tb == TB_DOUBLE     ? "double " :
        tb == TB_SIGNED     ? "signed " :
        tb == TB_UNSIGNED   ? "unsigned " :
        tb == TB_STRUCT     ? "struct " :
        tb == TB_UNION      ? "union " :
                              "";
}
char *typespec_str(Type_base tb)
{
    char *p = sbuf;
    sbuf[0] = 0;
    for(int i = TB_UNION; i > 0;  i>>= 1)
    {
        if (tb & i)
            p = stpcpy(p, typebase_str(tb & i));
    }
    return sbuf;
}
static char *nspace_str(Namespace ns)
{
    return 
        ns == NS_IDENT      ? "ident " :
        ns == NS_TAG        ? "tag " :
        ns == NS_MEMBER     ? "member" :
        ns == NS_LABEL      ? "label" : 
                              "UNKNOWN ";
}
static Type *add_member(Type *parent, Type *child)
{
    fprintf(stderr, "%s :%s: :%s: fieldname:%s\n", __func__, fulltype_str(parent), fulltype_str(child), child->fieldname);
    parent->num_members++;
    parent->members = (Type **)realloc(parent->members, parent->num_members * sizeof(Type *));
    parent->members[parent->num_members - 1] = child;
    return child;
}
static Type *generate_struct_type(Node *n, int depth)
{
    fprintf(stderr, "%s\n", __func__);


    if (!(n->kind == ND_DECLARATION && n->child_count >= 1 && n->children[0]->kind == ND_STRUCT))
        error("Should be struct declaration!\n");


    Type *top = calloc(1, sizeof(Type));

    top->typespec   = TB_STRUCT;
    top->typequal   = n->typequal;
    top->derived    = strdup("");
    top->sclass     = n->sclass;
    top->tag        = n->children[0]->children[0]->val;
    n = n->children[0];


    char *tagname = n->children[0]->val;
    Symbol *tag = find_symbol(n, tagname, NS_TAG);
    if (!tag)
    {
        print_type_table();
        print_symbol_table(symbol_table, 0);
        error("Tag %s not found\n", tagname);
    }
    else if (tag->type == t_void)
    {
        // There is an incomplete tag
        fprintf(stderr, "%s this tag is incomplete, defining\n", __func__);
        for(int i = 1; i < n->child_count; i++)
        {
            // Declaration
            Node *d = n->children[i];
            if (d->kind != ND_DECLARATION)
                error("Should be struct member declaration!\n");
            // There may be many declarators, each is a struct field, 
            // or there may be another struct
            for (int j = 0; j < d->child_count; j++)
            {
                Node *m = d->children[j];
                Type *t;
                if (m->kind == ND_STRUCT)
                {
                    // Recurse into struct
                    t = generate_struct_type(d, depth + 1);
                    m->type = t;
                    // t->tag = tagname;
                }
                else
                {
                    // Construct the type of the member
                    char *ts = tstr_compact(m);
                    Type *t = insert_type(m, ts);

                    // t           = calloc(1, sizeof(Type));
                    if (d->children[0]->kind == ND_STRUCT)
                        // If the field is an anonymous struct, point to it
                        t->basetype = d->children[0]->type;
                    // t->typespec = d->typespec;
                    // t->typequal = d->typequal;
                    // t->sclass   = d->sclass;
                    // t->derived  = strdup(tstr_compact(m));
                    // t->fieldname= get_decl_ident(m);
                    m->type     = t;
                    fprintf(stderr, "%s struct declarator %s%s\n", __func__, fulltype_str(t), t->fieldname);
                }
                add_member(top, t);
            }
        }
    }
    else if (n->children[0]->child_count > 1)
    {
        // If the the struct has more than just a tag ident, 
        // it is a redefinition and illegal
        // print_type_table();
        // print_symbol_table(symbol_table, 0);
        error("Redefinition of tag %s\n", tagname);
    }
    else
    {
        fprintf(stderr, "%s declarator reference to already defined tag\n", __func__);
        return tag->type;
    }
    return top;
}



// This is the main entrypoint to add to the symbol and type tables.
// The 
void add_types_and_symbols(Node *node, bool is_param, int depth)
{
    // This is a declaration node, get all the derived types and 
    // variables. Put variable in symbol table structured to represent scope.
    // Put type in type table.
    //
    // A declaration node will have either declarators, or a struct with declarators
    // underneath.


    fprintf(stderr, "%s\n", __func__);
    if (node->kind != ND_DECLARATION)
        error("Node should be ND_DECLARATION\n");
    char *ts = 0;
    char *ident = 0;
    Type *t = 0;
    for(int i = 0; i < node->child_count; i++)
    {
        Node *n = node->children[i];
        // if (n->kind == ND_DECLARATOR && !n->is_struct)
        if (n->kind == ND_DECLARATOR && !depth)
        {
            ts      = tstr_compact(n);
            ident   = get_decl_ident(n);
            fprintf(stderr, "%s declarator and not struct %s %s %s\n", __func__, typespec_str(n->typespec), ts, ident);
        }
        else if (n->kind == ND_STRUCT)
        {
            if (n->child_count == 1)
            {
                // This is either an incomplete struct definition, or
                // a reference to an already defined struct tag. If the declaration
                // has only the struct child, it is the first
                if (node->child_count == 1)
                {
                    // Incomplete struct
                    fprintf(stderr, "%s incomplete struct, this should be a placeholder\n", __func__);
                    t = generate_struct_type(node, 0);
                    insert_struct_type(t);
                    n->symbol->type = t;
                    node->type      = t;
                    node->typetag   = t->tag;

                }
                else
                {
                    // Existing tag with declarator
                    Symbol *s       = find_symbol(node, n->children[0]->val, NS_TAG);
                    node->type      = s->type;
                    node->typetag   = n->children[0]->val;
                }
            }
            else if (n->child_count > 1)
            {
                // First child is the structure tag, if anon, it will have
                // had a tag created for it
                n->symbol = insert_tag(node, n->children[0]->val);
                if (!n->struct_depth)
                {
                    // Generate a struct type only at the top level of a struct
                    fprintf(stderr, "%s creating struct or get type of tag\n", __func__);
                    t = generate_struct_type(node, 0);
                    insert_struct_type(t);
                    n->symbol->type = t;
                    node->type      = t;
                    node->typetag   = t->tag;
                }
            }
        }
        if (ts)
        {
            // Its an actual declarator
            fprintf(stderr, "%s found %s %s typetag:%s\n", __func__, ts, ident, node->typetag);
            Type *ty = insert_type(node, ts);
            // ty is pointer to type in type table
            node->type = ty;
            // See if this is a function, if so, get the return type
            if (ts[0] == '(')
            {
                fprintf(stderr, "%s this is a function, setting return type\n", __func__);
                Type *ty = insert_type(node, ts + 2);
                node->return_type = ty;
            }


            if (ident)
            {
                n->symbol = insert_ident(node, ty, ident, is_param);

                if (!n->is_struct && node->children[0]->kind == ND_STRUCT)
                {
                    // get type pointer given tag
                    char *tag = node->children[0]->children[0]->val;
                    Symbol *s = find_symbol(node, tag, NS_TAG);
                    fprintf(stderr, "%s setting type to tag:%s %016llx\n", __func__, tag, (unsigned long long)s->type);
                    // // TODO this forces calculation of struct size
                    // insert_struct_type(s->type);
                    n->symbol->type = s->type;
                }
                // n->symbol->type = t;
            }
        }
    }
}
// --------------------------------------------------------------------
// Symbol table
//
// --------------------------------------------------------------------



static Symbol_table *find_scope(Node *node)
{
    // Given a node, return the symbol table in scope. We use scope
    // as a way to represent position in symbol table. 
    // FIXME why not just use the pointer to symbol table?
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
    return st;
}
Symbol *find_symbol(Node *n, char *name, Namespace nspace)
{
    // Search for name in symbol table of given namespace.
    //
    // Go to scope in node, then search for symbol name in that 
    // scope, and successively enclosing scopes, until found.
    Symbol_table *st = find_scope(n);
    Symbol *s;
    bool found = false;
    while(!found)
    { 
        fprintf(stderr, "Searching in scope:%s\n", scope_str(st->scope));
        s =     nspace == NS_IDENT  ? st->idents 
            :   nspace == NS_TAG    ? st->tags
            :   nspace == NS_MEMBER ? st->members
            :                         st->labels;
        for(; s; s = s->next)
        {
            fprintf(stderr, "Name:%s\n", s->name);
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
        else
            break;
    }
    if (!found)
    {
        print_type_table();
        print_symbol_table(symbol_table, 0);
        error("%s %s not found!\n", nspace_str(nspace), name);
    }
    return s;
}
static Symbol_table *new_st_scope()
{
    // Add a new scope to the symbol table
    curr_scope_st->child_count++;
    curr_scope_st->children = (Symbol_table **)realloc(curr_scope_st->children, 
                                curr_scope_st->child_count * sizeof(Symbol_table));
    Symbol_table *nt = calloc(1, sizeof(Symbol_table));
    curr_scope_st->children[curr_scope_st->child_count - 1] = nt;
    Scope *sc       = calloc(1, sizeof(Scope));
    set_scope(sc);
    nt->scope       = *sc;
    nt->parent      = curr_scope_st;
    curr_scope_st   = nt;
    return curr_scope_st;
}
Symbol_table *enter_new_scope(bool use_last_scope)
{
    if (use_last_scope)
    {
        scope_depth++;
        curr_scope_st = last_symbol_table;
        return last_symbol_table;
    }
    scope_indices[scope_depth++]++;
    scope_indices[scope_depth]      = 0;
    last_symbol_table   = new_st_scope();
    curr_scope_st       = last_symbol_table;
    return last_symbol_table;
}
void leave_scope()
{
    scope_depth--;
    curr_scope_st = curr_scope_st->parent;
}
void set_scope(Scope *sc)
{
    sc->depth   = scope_depth;
    sc->indices = calloc(scope_depth, sizeof(int));
    memcpy(sc->indices, scope_indices, scope_depth * sizeof(int));
}
char *scope_str(Scope sc)
{
    static char buf[1024];
    int l = 0;
    int d = sc.depth;
    l = sprintf(buf, "%s:%d.", sc.scope_type == ST_COMPSTMT ? "compstmt" : "struct", d);
    for(int i = 0; i < d; i++)
        l += sprintf(buf + l, "%d%c", sc.indices[i], i < d - 1 ? '.' : ' ');
    return buf;
}
static Symbol *insert_tag(Node *node, char *ident)
{
    fprintf(stderr, "%s %s\n", __func__, ident);
    // Find scope in table, the scope structure already exists from the parse process
    Scope sc = node->scope;
    Symbol_table *st = symbol_table;
    if (sc.depth)
    {
        for(int d = 1; d <= sc.depth; d++)
        {
            int index = sc.indices[d - 1] - 1;
            st = st->children[index];
        }
    }
    // st now points to the current scope
    Symbol *s = 0, *ls = 0;
    for(s = st->tags; s; s = s->next)
    {
        ls      = s;
        if (!strcmp(s->name, ident))
        {
            fprintf(stderr, "%s struct tag already exists in symbol table\n",__func__);
            return s;
        }
    }
    Symbol *n   = new_symbol(t_void, ident, 0);
    if (!ls)
        st->tags    = n;
    else
        ls->next    = n;
    return n;
}
static Symbol *new_symbol(Type *type, char *ident, int offset)
{
    Symbol *n   = calloc(1, sizeof(Symbol));
    n->name     = ident;
    n->type     = type;
    n->offset   = offset;
    return n;
}
static Symbol *insert_ident(Node *node, Type *type, char *ident, bool is_param)
{
    fprintf(stderr, "%s %s %s\n", __func__, fulltype_str(type), ident);
    // If we are inserting a symbol that is a parameter, we need to use a 
    // different offset, since this has been pushed onto the stack. We need to
    // insert the symbols backwards, while pushing them onto the stack in 
    // forwards order. Parameters can only occur at scope depth 1.

    // if (node->is_struct)
    //     // Symbols inside structs don't appear in the symbol table 
    //     return 0;

    Scope sc = node->scope;
    // Find scope in table, the scope structure already exists from the parse process
    Symbol_table *st = symbol_table;
    int go = 0; 
    // TODO refactor this, unify repeated logic
    if (sc.depth)
    {
        for(int d = 1; d <= sc.depth; d++)
        {
            if (d > 1)
                // Don't include size from globals
                go += st->size;
            int index = sc.indices[d - 1] - 1;
            st = st->children[index];
        }
    }
    else
    {
        // Treat global scope differently, we are not allocating downwards in stack space
        // but upwards in data space
        int offset  = st->size;
        st->size += type->size;
        Symbol *n   = new_symbol(type, ident, offset);
        Symbol *s = 0, *ls = 0;
        for(s = st->idents; s; s = s->next)
            ls      = s;
        if (!ls)
            st->idents = n;
        else
            ls->next = n;

        return n;
    }
    st->global_offset = go;
    // We are now at the correct scope, insert the symbol at the end of the list,
    // with offset within the current scope. This is the size of the current type,
    // added to the previous offset for local scopes. (TODO alignment)

    // Global offset is offset within the function, whatever scope level we are at.
    // This is the enclosing scopes
    Symbol *s = 0, *ls = 0;
    int offset          = 0;
    int param_offset    = 8; // There needs to be space for the link and base regs
    int last_size = 0;
    for(s = st->idents; s; s = s->next)
    {
        if (s->is_param)
        {
            param_offset = s->offset;
            last_size = s->type->size;
        }
        else
            offset  = s->offset;
        ls      = s;
    }
    // ls now points to last symbol in list
    // Move offset to make space for this type
    offset          += type->size;
    param_offset    += last_size;
    if (!is_param)
        // Size is used for allocating space on stack for this
        // scope, we don't include parameters
        st->size        += type->size;
    Symbol *n       = new_symbol(type, ident, is_param ? param_offset : offset);
    n->is_param     = is_param;
    if (!ls)
        st->idents = n;
    else
        ls->next    = n;
    return n;
}


int find_offset(Type *t, char *field, Type **it)
{
    // Type should be pointing to a struct, traverse to find
    // the field. Only look at the current level of a nested 
    // struct, the MEMBER parsing of the tree deals with this
    *it = 0;
    for(int i = 0; i < t->num_members; i++)
    {
        Type *m = t->members[i];
        if (m->fieldname && !strcmp(field, m->fieldname))
        {
            *it = m->basetype;
            return m->offset;
        }
    }
    return -1;
}
static void print_type_table_entry(int depth, Type *t)
{
    for(int i = 0; i < depth; i++)
        fprintf(stderr, "  ");

    fprintf(stderr, "%016llx %-40s %-10s %-10s ", (unsigned long long)t, fulltype_str(t), t->tag, t->fieldname);
    fprintf(stderr, "size:%d offset:%04x align:%d ", t->size, t->offset, t->align);
    if (t->num_members)
    {
        for(int i = 0; i < t->num_members; i++)
        {
            fprintf(stderr, "\n");
            print_type_table_entry(depth + 1, t->members[i]);
        }
    }
    if (istype_array(t))
    {
        fprintf(stderr, "%d", t->elem_size);
        for(int i = 0; i < t->dimensions; i++)
        {
            fprintf(stderr, "[%d]", *t->dim_sizes[i]);
        }
    }
    if (!depth)
        fprintf(stderr, "\n");
    
}
void print_type_table()
{
    Type *t;
    fprintf(stderr, "------ Type table ------                                  Tag        Fieldname\n");
    for(t = types; t; t = t->next)
    {
        print_type_table_entry(0, t);
    }
}
void print_symbol_table(Symbol_table *s, int depth)
{
    if (depth==0)fprintf(stderr, "------ Symbol table ------\n");
    for(int i = 0; i < depth; i++) 
        fprintf(stderr, "  ");
    fprintf(stderr, "Scope:%-20s   0x%04x 0x%04x\n", scope_str(s->scope), s->size, s->global_offset);
    Symbol *sm;
    for(sm = s->idents; sm; sm = sm->next)
    {
        for(int i = 0; i < depth; i++) 
            fprintf(stderr, "  ");
        fprintf(stderr, "%016llx %-10s %s 0x%02x %s\n", (unsigned long long)sm->type, sm->name, 
            sm->is_param ? "param" : depth ? "local" : "global", sm->offset, fulltype_str(sm->type));
    }
    for(sm = s->tags; sm; sm = sm->next)
    {
        for(int i = 0; i < depth; i++) 
            fprintf(stderr, "  ");
        fprintf(stderr, "%016llx %-10s tag\n", (unsigned long long)sm->type, sm->name);
    }
    for(sm = s->members; sm; sm = sm->next)
    {
        for(int i = 0; i < depth; i++) 
            fprintf(stderr, "  ");
        fprintf(stderr, "%016llx %-10s 0x%02x %s\n", (unsigned long long)sm->type, sm->name, 
            sm->offset, fulltype_str(sm->type));
    }
    // FIXME labels to go here
    for(int i = 0; i < s->child_count; i++)
    {
        print_symbol_table(s->children[i], depth + 1);
    }
}

static char *typestr(Node *node)
{
    sbuf[0] = 0;
    if (node->kind == ND_DECLARATOR)
        dcl(node);
    return sbuf;
}

static char buf[1024];
static char *fts(Type *t, char *p)
{
    if (t->basetype)
        p += sprintf(p, "%016llx ", (unsigned long long) t->basetype);
    p += sprintf(p, "%s%s%s", type_token_str(t->sclass), type_token_str(t->typequal), typespec_str(t->typespec));
    if (t->num_members)
        for(int i = 0; i < t->num_members; i++)
            p = fts(t->members[i], p);
    if (t->derived && t->derived[0])
        p += sprintf(p, "%s", t->derived);
    return p;
}
char *fulltype_str(Type *t)
{
    char *p = buf;
    fts(t, p);
    return buf;

}

char *curr_scope_str()
{

    sprintf(buf, "%d:", scope_depth); 
    for(int i = 0; i < scope_depth; i++)
        sprintf(buf,"%d.", scope_indices[i]);
    return buf;
}