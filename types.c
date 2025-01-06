

#include "mycc.h"


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





char *get_typename(Node *node)
{
    // TODO get the struct, union, enum, or typedef identifier
    return "";
}
Type *new_type(Node *node, Node *child)
{
    // Construct a new type, the node must be a ND_DECLARATION
    Type *ty        = calloc(1, sizeof(Type));
    ty->typespec    = node->typespec;
    ty->typequal    = node->typequal;
    ty->sclass      = node->sclass;
    if (ty->typespec == TK_STRUCT || ty->typespec == TK_UNION 
    || ty->typespec == TK_ENUM || ty->typespec == TK_TYPEDEF)
    {
        ty->name    = get_typename(node);
    }
    return ty;
}
bool is_named_type(Token_kind tk)
{
    return      tk == TK_STRUCT 
            ||  tk == TK_UNION 
            ||  tk == TK_ENUM 
            ||  tk == TK_TYPEDEF;
}
void calc_tsize(Type *t)
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
    if (!t->derived[0])
    {
        // Base type
        t->size         = s;
        t->elem_size    = t->size;
    }
    else if (t->derived[0] == '*')
    {
        // This is a pointer
        t->is_pointer   = true;
        t->size         = 2;
        t->elem_size    = t->size;
    }
    else if (t->derived[0] == '(')
    {
        // This is a function
        t->is_function  = true;
        t->size         = 2;
        t->elem_size    = t->size;
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
            t->elem_size = 4;
        else
            t->elem_size = s;
        t->size = t->elements * t->elem_size;
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
    fprintf(stderr, "%s ts:%s:\n", __func__, ts);
    Type *last = 0;
    for(Type *t = types; t; t = t->next)
    {
        last = t;
        if (    node->typespec == t->typespec
            &&  node->typequal == t->typequal
            &&  node->sclass == t->sclass
            &&  !strcmp(ts, t->derived))
        {
            // Check if named type is the same
            // if (is_named_type(t->typespec))
            // {
            //     // TODO
            // }
            // if we're here, we have found a matching type, so return
            fprintf(stderr, "%sFound type\n", __func__);
            return t;
        }
    }
    // Didn't find type in table, create a new one
    Type *t     = calloc(1, sizeof(Type));
    t->typespec = node->typespec;
    t->typequal = node->typequal;
    t->sclass   = node->sclass;
    t->derived  = calloc(1, strlen(ts) + 1);
    strcpy(t->derived, ts);
    // Work out the dimensions and sizes
    calc_tsize(t);
    fprintf(stderr, "%s ts:%s: fts:%s:\n", __func__, ts, fulltype_str(t));
    if (last)
        last->next  = t;
    else
        types = t;
    return t;
}

static int offset;
int calc_struct_tsize(Type *t)
{
    fprintf(stderr, "%s\n", __func__);
    for(int i = 0; i < t->num_members; i++)
    {
        Type *m = t->members[i];
        if (m->typespec & TB_STRUCT)
            calc_struct_tsize(m);
        else
        {
            calc_tsize(m);
            m->offset   = offset;
            offset      += m->size;
        }
    }
    return offset;
}
Type *insert_struct_type(Type *t)
{
    fprintf(stderr, "%s\n", __func__);
    Type *last = 0;
    for(Type *i = types; i; i = i->next)
        last = i;
    if (last)
        last->next  = t;
    else
        types = t;
    // We need to fill in the sizes and calculate the member offsets

    offset = 0;
    int size = calc_struct_tsize(t);
    t->size = size;
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

void make_basic_types()
{
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

bool is_struct_or_union(Type_base t)
{
    return (t & TB_STRUCT) || (t & TB_UNION);
}

char *typebase_str(Type_base tb)
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

static char ts[1024];
char *typespec_str(Type_base tb)
{
    char *p = ts;
    ts[0] = 0;
    for(int i = TB_UNION; i > 0;  i>>= 1)
    {
        if (tb & i)
            p = stpcpy(p, typebase_str(tb & i));
    }
    return ts;
}


