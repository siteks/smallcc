

#include "mycc.h"

// Type system context instance
TypeContext    type_ctx;

static int     next_scope_id = 0;

// Legacy pointer aliases for convenience (these still work because macros define t_*)

Symbol        *insert_tag(Symbol_table *st, char *ident);
static Symbol *insert_typedef(Node *node, Type *type, const char *ident);
static Symbol *new_symbol(Type *type, const char *ident, int offset);
static Symbol *insert_ident(Node *node, Type *type, const char *ident, bool is_param, StorageClass sclass);
static Type   *generate_struct_type(Node *decl_node, DeclParseState ds, int depth);
static Type   *type_from_declarator(Node *decl, Type *base);
static Type   *type_from_direct_decl(Node *dd, Type *base);
static Type   *typespec_to_base(Decl_spec typespec);

// ---------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------
static int     do_align(int val, int size)
{
    return (val + (size - 1)) & ~(size - 1);
}

static void append_symbol(Symbol_table *st, Symbol *sym)
{
    if (!st->symbols_tail)
        st->symbols = sym;
    else
        st->symbols_tail->next = sym;
    st->symbols_tail = sym;
}

static void append_type(Type *t)
{
    if (!type_ctx.type_list_tail)
        type_ctx.type_list = t;
    else
        type_ctx.type_list_tail->next = t;
    type_ctx.type_list_tail = t;
}

// ---------------------------------------------------------------
// Factory functions — intern types in global list
// ---------------------------------------------------------------
Type *get_basic_type(Type_base base)
{
    // Basic scalar types have no derived fields; check base alone.
    for (Type *p = type_ctx.type_list; p; p = p->next)
        if (p->base == base && base <= TB_DOUBLE)
            return p;

    Type *t = arena_alloc(sizeof(Type));
    t->base = base;
    switch (base)
    {
    case TB_VOID:
        t->size  = 0;
        t->align = 1;
        break;
    case TB_CHAR:
    case TB_UCHAR:
        t->size  = 1;
        t->align = 1;
        break;
    case TB_SHORT:
    case TB_USHORT:
        t->size  = 2;
        t->align = 2;
        break;
    case TB_INT:
    case TB_UINT:
        t->size  = 2;
        t->align = 2;
        break;
    case TB_LONG:
    case TB_ULONG:
        t->size  = 4;
        t->align = 4;
        break;
    case TB_FLOAT:
        t->size  = 4;
        t->align = 4;
        break;
    case TB_DOUBLE:
        t->size  = 4;
        t->align = 4;
        break;
    default:
        t->size  = 2;
        t->align = 2;
        break;
    }
    append_type(t);
    return t;
}

Type *get_pointer_type(Type *pointee)
{
    for (Type *p = type_ctx.type_list; p; p = p->next)
        if (p->base == TB_POINTER && p->u.ptr.pointee == pointee)
            return p;

    Type *t          = arena_alloc(sizeof(Type));
    t->base          = TB_POINTER;
    t->u.ptr.pointee = pointee;
    t->size          = 2;
    t->align         = 2;
    append_type(t);
    return t;
}

Type *get_array_type(Type *elem, int count)
{
    for (Type *p = type_ctx.type_list; p; p = p->next)
        if (p->base == TB_ARRAY && p->u.arr.elem == elem && p->u.arr.count == count)
            return p;

    Type *t        = arena_alloc(sizeof(Type));
    t->base        = TB_ARRAY;
    t->u.arr.elem  = elem;
    t->u.arr.count = count;
    t->size        = elem->size * count;
    t->align       = elem->align;
    append_type(t);
    return t;
}

static bool params_match(Param *a, Param *b)
{
    while (a && b)
    {
        if (a->type != b->type)
            return false;
        a = a->next;
        b = b->next;
    }
    return a == NULL && b == NULL;
}

Type *get_function_type(Type *ret, Param *params, bool is_variadic)
{
    for (Type *p = type_ctx.type_list; p; p = p->next)
        if (p->base == TB_FUNCTION && p->u.fn.ret == ret && p->u.fn.is_variadic == is_variadic &&
            params_match(p->u.fn.params, params))
            return p;

    Type *t             = arena_alloc(sizeof(Type));
    t->base             = TB_FUNCTION;
    t->u.fn.ret         = ret;
    t->u.fn.params      = params;
    t->u.fn.is_variadic = is_variadic;
    t->size             = 2;
    t->align            = 2;
    append_type(t);
    return t;
}

Type *get_struct_type(Symbol *tag, Field *members, bool is_union)
{
    for (Type *p = type_ctx.type_list; p; p = p->next)
        if (p->base == TB_STRUCT && p->u.composite.tag == tag)
            return p;

    Type *t                 = arena_alloc(sizeof(Type));
    t->base                 = TB_STRUCT;
    t->u.composite.tag      = tag;
    t->u.composite.members  = members;
    t->u.composite.is_union = is_union;
    append_type(t);
    return t;
}

Type *get_enum_type(Symbol *tag)
{
    for (Type *p = type_ctx.type_list; p; p = p->next)
        if (p->base == TB_ENUM && p->u.enu.tag == tag)
            return p;
    Type *t      = arena_alloc(sizeof(Type));
    t->base      = TB_ENUM;
    t->u.enu.tag = tag;
    t->size      = 2;
    t->align     = 2;
    append_type(t);
    return t;
}

// ---------------------------------------------------------------
// Direct AST → Type conversion (replaces tstr_compact + apply_derivation).
//
// type_from_declarator: walk an ND_DECLARATOR node.
//   pointer stars on this level are applied to 'base' first (innermost),
//   then the direct-decl's array/function suffixes wrap outward.
//
// type_from_direct_decl: walk an ND_DIRECT_DECL node.
//   Suffixes are applied right-to-left so the leftmost is outermost.
//   A grouped inner declarator (e.g. (*fp)) is handled by recursing.
// ---------------------------------------------------------------
static Type *type_from_declarator(Node *decl, Type *base)
{
    Type *t = base;
    for (int i = 0; i < decl->u.declarator.pointer_level; i++)
        t = get_pointer_type(t);
    return type_from_direct_decl(decl->ch[0], t);   // direct_decl
}

static Type *type_from_direct_decl(Node *dd, Type *base)
{
    // ch[0]=name (ND_IDENT or ND_DECLARATOR for grouped inner declarators).
    // ch[1]=suffixes linked list (ND_ARRAY_DECL and ND_FUNC_DECL nodes).
    Node *name = dd->ch[0];

    // Collect suffixes for right-to-left processing
    Node *suf[32];
    int   nsuf = 0;
    for (Node *s = dd->ch[1]; s; s = s->next)   // suffixes list
        suf[nsuf++] = s;

    // Apply array/function suffixes right-to-left (rightmost = innermost)
    Type *t = base;
    for (int i = nsuf - 1; i >= 0; i--)
    {
        Node *s = suf[i];
        if (s->kind == ND_ARRAY_DECL)
        {
            int count = (s->ch[0] && s->ch[0]->kind == ND_LITERAL)   // size
                            ? (int)s->ch[0]->u.literal.ival
                            : 0;
            t         = get_array_type(t, count);
        }
        else if (s->kind == ND_FUNC_DECL)
        {
            bool variadic = s->ch[0] && s->ch[0]->u.ptype_list.is_variadic;   // params
            t             = get_function_type(t, NULL, variadic);
        }
    }

    // Grouped inner declarator (e.g. (*fp)): recurse with accumulated type as new base
    if (name && name->kind == ND_DECLARATOR)
        return type_from_declarator(name, t);
    return t;
}

// ---------------------------------------------------------------
// typespec_to_base
// ---------------------------------------------------------------
static Type *typespec_to_base(Decl_spec typespec)
{
    switch ((int)typespec)
    {
    case DS_VOID:
        return t_void;
    case DS_CHAR:
    case DS_SIGNED | DS_CHAR:
        return t_char;
    case DS_UNSIGNED | DS_CHAR:
        return t_uchar;
    case DS_SHORT:
    case DS_SIGNED | DS_SHORT:
        return t_short;
    case DS_UNSIGNED | DS_SHORT:
        return t_ushort;
    case DS_INT:
    case DS_SIGNED | DS_INT:
    case DS_SIGNED:
        return t_int;
    case DS_UNSIGNED | DS_INT:
    case DS_UNSIGNED:
        return t_uint;
    case DS_LONG:
    case DS_SIGNED | DS_LONG:
        return t_long;
    case DS_UNSIGNED | DS_LONG:
        return t_ulong;
    case DS_FLOAT:
        return t_float;
    case DS_DOUBLE:
        return t_double;
    case DS_ENUM:
        return t_int;
    default:
        return t_int;
    }
}

// Exported: build Type from a declaration-context node (ND_DECLARATION or similar).
// Determines the base type from ds, then applies the first declarator child
// if present (used by type_name() for casts/sizeof).
Type *type2_from_decl_node(Node *node, DeclParseState ds)
{
    Type *base;
    if (ds.typespec & DS_TYPEDEF)
    {
        base = ds.typedef_type;
    }
    else if (ds.typespec & (DS_STRUCT | DS_UNION))
    {
        base = (node->type && node->type != t_void) ? node->type : t_void;
    }
    else if (ds.typespec & DS_ENUM)
    {
        base = (node->type && node->type->base == TB_ENUM) ? node->type : t_int;
    }
    else
    {
        base = typespec_to_base(ds.typespec);
    }
    // A declarator may be present (type_name() for casts/sizeof builds ND_DECLARATION
    // with a declarator stored as the first entry in u.declaration.decls).
    Node *first_decl = node->ch[1];   // decls list head
    if (first_decl && first_decl->kind == ND_DECLARATOR)
        return type_from_declarator(first_decl, base);
    return base;
}

// ---------------------------------------------------------------
// Array helpers
// ---------------------------------------------------------------
int array_dimensions(Type *t)
{
    int d = 0;
    while (t && t->base == TB_ARRAY)
    {
        d++;
        t = t->u.arr.elem;
    }
    return d;
}

Type *array_elem_type(Type *t)
{
    while (t && t->base == TB_ARRAY)
        t = t->u.arr.elem;
    return t;
}

Type *elem_type(Type *t)
{
    if (!t)
        return 0;
    if (t->base == TB_ARRAY)
        return t->u.arr.elem;
    if (t->base == TB_POINTER)
        return t->u.ptr.pointee;
    return t;
}

// ---------------------------------------------------------------
// Type checks
// ---------------------------------------------------------------

// ---------------------------------------------------------------
// Per-TU reset and extern symbol injection
// ---------------------------------------------------------------
void reset_types_state(void)
{
    // Carry over SYM_EXTERN symbols from the previous TU's global scope.
    // These are the cross-TU globals marked by harvest_globals.
    // Statics, putchar, and unresolved externs are excluded and not preserved.
    Symbol *carry = NULL, *carry_tail = NULL;
    Symbol *prev = type_ctx.symbol_table ? type_ctx.symbol_table->symbols : NULL;
    for (Symbol *s = prev; s; s = s->next)
    {
        if (s->ns != NS_IDENT || s->kind != SYM_EXTERN)
            continue;
        if (!carry) carry = s;
        else        carry_tail->next = s;
        carry_tail = s;
    }
    if (carry_tail)
        carry_tail->next = NULL;

    type_ctx.symbol_table               = arena_alloc(sizeof(Symbol_table));
    type_ctx.symbol_table->symbols      = carry;
    type_ctx.symbol_table->symbols_tail = carry_tail;
    type_ctx.curr_scope_st         = type_ctx.symbol_table;
    type_ctx.last_symbol_table     = type_ctx.symbol_table;
    type_ctx.scope_depth           = 0;
    type_ctx.scope_count           = 0;
    next_scope_id                  = 0;
}

void insert_extern_sym(const char *name, Type *type)
{
    // If already present, mark it as an extern reference for subsequent TUs.
    for (Symbol *s = type_ctx.symbol_table->symbols; s; s = s->next)
    {
        if (s->ns == NS_IDENT && !strcmp(s->name, name))
        {
            s->kind = SYM_EXTERN;
            return;
        }
    }
    // Not present — add a new extern entry (e.g. injected from outside).
    Symbol *sym = arena_alloc(sizeof(Symbol));
    sym->name   = name;
    sym->type   = type;
    sym->kind   = SYM_EXTERN;
    sym->ns     = NS_IDENT;
    append_symbol(type_ctx.symbol_table, sym);
}

// ---------------------------------------------------------------
// make_basic_types
// ---------------------------------------------------------------
static void insert_builtin(char *name, Type *type)
{
    for (Symbol *s = type_ctx.symbol_table->symbols; s; s = s->next)
        if (s->ns == NS_IDENT && !strcmp(s->name, name))
            return;
    Symbol *sym = arena_alloc(sizeof(Symbol));
    sym->name   = name;
    sym->type   = type;
    sym->offset = 0;
    sym->ns     = NS_IDENT;
    sym->kind   = SYM_BUILTIN;
    append_symbol(type_ctx.symbol_table, sym);
}

void make_basic_types()
{
    DBG_FUNC();
    t_void   = get_basic_type(TB_VOID);
    t_char   = get_basic_type(TB_CHAR);
    t_uchar  = get_basic_type(TB_UCHAR);
    t_short  = get_basic_type(TB_SHORT);
    t_ushort = get_basic_type(TB_USHORT);
    t_int    = get_basic_type(TB_INT);
    t_uint   = get_basic_type(TB_UINT);
    t_long   = get_basic_type(TB_LONG);
    t_ulong  = get_basic_type(TB_ULONG);
    t_float  = get_basic_type(TB_FLOAT);
    t_double = get_basic_type(TB_DOUBLE);

    // Builtin functions pre-declared in global scope
    Param *p = arena_alloc(sizeof(Param));
    p->type  = t_int;
    insert_builtin("putchar", get_function_type(t_int, p, false));

    // Register va_list as a typedef for int so the normal typedef path handles it.
    Symbol *va = arena_alloc(sizeof(Symbol));
    va->name   = "va_list";
    va->type   = t_int;
    va->ns     = NS_TYPEDEF;
    va->kind   = SYM_GLOBAL;
    append_symbol(type_ctx.symbol_table, va);
}

// ---------------------------------------------------------------
// find_offset
// ---------------------------------------------------------------
int find_offset(Type *t, char *field, Type **it)
{
    if (!t || t->base != TB_STRUCT)
        return -1;
    *it = 0;
    for (Field *f = t->u.composite.members; f; f = f->next)
    {
        if (f->name && !strcmp(field, f->name))
        {
            *it = f->type;
            return f->offset;
        }
    }
    return -1;
}

// ---------------------------------------------------------------
// Struct layout calculation
// ---------------------------------------------------------------
static void calc_struct_layout(Type *st)
{
    int off       = 0;
    int max_align = 1;
    for (Field *f = st->u.composite.members; f; f = f->next)
    {
        int al = f->type->align ? f->type->align : 1;
        if (st->u.composite.is_union)
        {
            f->offset = 0;
            if (f->type->size > off)
                off = f->type->size;
        }
        else
        {
            off       = do_align(off, al);
            f->offset = off;
            off += f->type->size;
        }
        if (al > max_align)
            max_align = al;
    }
    st->align = max_align;
    st->size  = st->u.composite.is_union ? off : do_align(off, max_align);
}

// ---------------------------------------------------------------
// generate_struct_type
// ---------------------------------------------------------------
static Type *generate_struct_type(Node *decl_node, DeclParseState ds, int depth)
{
    DBG_FUNC();
    Node *struct_node = decl_node->ch[0];   // spec
    if (!struct_node || struct_node->kind != ND_STRUCT)
        error("Should be struct declaration!\n");

    char   *tagname = struct_node->ch[0]->u.ident.name;   // tag node name
    Symbol *tag     = find_symbol_st(decl_node->st, tagname, NS_TAG);
    if (!tag)
    {
#ifdef DEBUG_ENABLED
        print_type_table();
        print_symbol_table(type_ctx.symbol_table, 0);
#endif
        error("Tag %s not found\n", tagname);
    }

    if (tag->type == t_void)
    {
        // Build Field list from struct member declarations
        DBG_PRINT("%s defining struct %s\n", __func__, tagname);
        Field  *fields   = NULL;
        Field **last_ptr = &fields;

        for (Node *d = struct_node->ch[1]; d; d = d->next)   // members list
        {
            if (d->kind != ND_DECLARATION)
                continue;

            bool has_nested_struct =
                (d->ch[0] != NULL && d->ch[0]->kind == ND_STRUCT &&
                 d->ch[0]->ch[1] != NULL);   // spec exists and has members list

            for (Node *m = d->ch[1]; m; m = m->next)   // decls list
            {
                if (m->kind != ND_DECLARATOR)
                    continue;

                Type *base;
                if (d->u.declaration.typespec & (DS_STRUCT | DS_UNION))
                {
                    if (has_nested_struct)
                    {
                        DeclParseState member_ds = { 0 };
                        member_ds.typespec       = d->u.declaration.typespec;
                        member_ds.sclass         = d->u.declaration.sclass;
                        base                     = generate_struct_type(d, member_ds, depth + 1);
                    }
                    else
                    {
                        Node   *sn       = d->ch[0];   // spec
                        char   *stag     = sn->ch[0]->u.ident.name;   // tag name
                        Symbol *stag_sym = find_symbol_st(d->st, stag, NS_TAG);
                        base             = (stag_sym && stag_sym->type) ? stag_sym->type : t_void;
                    }
                }
                else
                {
                    base = typespec_to_base(d->u.declaration.typespec);
                }

                Type       *mty   = type_from_declarator(m, base);
                const char *fname = get_decl_ident(m);

                Field      *f     = arena_alloc(sizeof(Field));
                f->name           = fname ? arena_strdup(fname) : "";
                f->type           = mty;
                *last_ptr         = f;
                last_ptr          = &f->next;

                DBG_PRINT("%s  member '%s' type %s\n", __func__, f->name, fulltype_str(mty));
            }
        }

        // Create struct Type entry
        Type *st                 = arena_alloc(sizeof(Type));
        st->base                 = TB_STRUCT;
        st->u.composite.tag      = tag;
        st->u.composite.members  = fields;
        st->u.composite.is_union = !!(ds.typespec & DS_UNION);
        calc_struct_layout(st);
        append_type(st);
        tag->type = st;
        return st;
    }
    else if (struct_node->ch[1] != NULL)   // members list exists
    {
        error("Redefinition of tag %s\n", tagname);
    }
    DBG_PRINT("%s referencing existing struct %s\n", __func__, tagname);
    return tag->type;
}

// ---------------------------------------------------------------
// add_types_and_symbols
// ---------------------------------------------------------------
void add_types_and_symbols(Node *node, DeclParseState ds, bool is_param, bool is_struct_member)
{
    DBG_FUNC();
    if (node->kind != ND_DECLARATION)
        error("Node should be ND_DECLARATION\n");

    // Store ds fields into the node for later use by generate_struct_type
    node->u.declaration.typespec = ds.typespec;
    node->u.declaration.sclass   = ds.sclass;

    // First pass: handle struct definitions and determine base type
    Node *spec                   = node->ch[0];   // spec
    if (spec && spec->kind == ND_STRUCT)
    {
        bool has_body   = (spec->ch[1] != NULL);   // members list
        bool standalone = (node->ch[1] == NULL);   // decls list
        if (!has_body)
        {
            if (standalone)
            {
                // Standalone incomplete struct (e.g. "struct foo;")
                DBG_PRINT("%s incomplete struct\n", __func__);
                Type *t = generate_struct_type(node, ds, 0);
                if (spec->symbol)
                    spec->symbol->type = t;
                node->type = t;
            }
            else
            {
                // "struct foo" as type specifier with a declarator
                Symbol *s  = find_symbol(node, spec->ch[0]->u.ident.name, NS_TAG);   // tag name
                node->type = s->type;
            }
        }
        else
        {
            // Full struct definition with body
            spec->symbol = insert_tag(node->st, spec->ch[0]->u.ident.name);   // tag name
            if (!is_struct_member)
            {
                DBG_PRINT("%s creating struct type\n", __func__);
                Type *t            = generate_struct_type(node, ds, 0);
                spec->symbol->type = t;
                node->type         = t;
            }
        }
    }

    // Determine base type for declarators
    Type *base;
    if (ds.typespec & DS_TYPEDEF)
    {
        base = ds.typedef_type ? ds.typedef_type : node->type;
    }
    else if (ds.typespec & (DS_STRUCT | DS_UNION))
    {
        base = (node->type && node->type->base == TB_STRUCT) ? node->type : t_void;
    }
    else if (ds.typespec & DS_ENUM)
    {
        base = (node->type && node->type->base == TB_ENUM) ? node->type : t_int;
    }
    else
    {
        base = typespec_to_base(ds.typespec);
    }

    // Second pass: process every ND_DECLARATOR child
    bool first_decl = true;
    for (Node *n = node->ch[1]; n; n = n->next)   // decls list
    {
        // Skip declarators for struct members: they are built
        // by generate_struct_type() directly, not by this pass.
        if (n->kind != ND_DECLARATOR || is_struct_member)
            continue;

        const char *ident = get_decl_ident(n);
        Type       *ty    = type_from_declarator(n, base);
        DBG_PRINT("%s declarator ident:'%s' type:%s\n", __func__, ident ? ident : "", fulltype_str(ty));
        // char s[] = "hello" — fix up zero-size array from string literal init
        if (ty->base == TB_ARRAY && ty->u.arr.count == 0 && n->ch[1] &&   // init
            n->ch[1]->u.literal.strval)
            ty = get_array_type(ty->u.arr.elem, n->ch[1]->u.literal.strval_len + 1);
        if (first_decl)
        {
            node->type = ty;
            first_decl = false;
        }
        DBG_PRINT("%s final type %s\n", __func__, fulltype_str(ty));

        if (ident)
        {
            if (ds.sclass == SC_TYPEDEF)
                n->symbol = insert_typedef(node, ty, ident);
            else
                n->symbol = insert_ident(node, ty, ident, is_param, ds.sclass);

            // For struct-typed variables, ensure symbol points to struct type
            if (node->ch[0] && node->ch[0]->kind == ND_STRUCT)   // spec
            {
                DBG_PRINT("%s setting sym type from tag\n", __func__);
                n->symbol->type = ty;    // ty already wraps struct base
            }
        }
    }
}

// ---------------------------------------------------------------
// Symbol table
// ---------------------------------------------------------------
// Probe lookup: walk scope chain from st, return NULL if not found.
Symbol *find_symbol_st(Symbol_table *st, const char *name, Namespace nspace)
{
    for (; st; st = st->parent)
    {
        DBG_PRINT("Searching in scope id:%d depth:%d\n", st->scope_id, st->depth);
        for (Symbol *s = st->symbols; s; s = s->next)
        {
            if (s->ns != nspace)
                continue;
            DBG_PRINT("Name:%s\n", s->name);
            if (!strcmp(name, s->name))
                return s;
        }
        DBG_PRINT("Not found, going to enclosing scope\n");
    }
    return NULL;
}

// Fatal lookup: error if the symbol is not found.
Symbol *find_symbol(Node *n, const char *name, Namespace nspace)
{
    Symbol *s = find_symbol_st(n->st, name, nspace);
    if (!s)
    {
#ifdef DEBUG_ENABLED
        print_type_table();
        print_symbol_table(type_ctx.symbol_table, 0);
#endif
        error("%s %s not found!\n", nspace == NS_IDENT ? "ident" : "tag", name);
    }
    return s;
}

static Symbol_table *new_st_scope()
{
    Symbol_table *nt                            = arena_alloc(sizeof(Symbol_table));
    nt->depth                                   = type_ctx.scope_depth;
    nt->scope_type                              = ST_COMPSTMT;
    nt->scope_id                                = next_scope_id++;
    nt->parent                                  = type_ctx.curr_scope_st;
    nt->param_offset                            = FRAME_OVERHEAD;
    // Register in flat scope list
    type_ctx.scope_list[type_ctx.scope_count++] = nt;
    type_ctx.curr_scope_st                      = nt;
    return type_ctx.curr_scope_st;
}

Symbol_table *enter_new_scope(void)
{
    type_ctx.scope_depth++;
    type_ctx.last_symbol_table = new_st_scope();
    type_ctx.curr_scope_st     = type_ctx.last_symbol_table;
    return type_ctx.last_symbol_table;
}

Symbol_table *reenter_last_scope(void)
{
    type_ctx.scope_depth++;
    type_ctx.curr_scope_st = type_ctx.last_symbol_table;
    return type_ctx.last_symbol_table;
}

void leave_scope()
{
    type_ctx.scope_depth--;
    type_ctx.curr_scope_st = type_ctx.curr_scope_st->parent;
}

Symbol *insert_tag(Symbol_table *st, char *ident)
{
    DBG_PRINT("%s %s\n", __func__, ident);
    for (Symbol *s = st->symbols; s; s = s->next)
        if (s->ns == NS_TAG && !strcmp(s->name, ident))
        {
            DBG_PRINT("%s tag already exists\n", __func__);
            return s;
        }
    Symbol *sym = new_symbol(t_void, ident, 0);
    sym->ns     = NS_TAG;
    append_symbol(st, sym);
    return sym;
}

Symbol *insert_enum_const(Symbol_table *st, Type *ety, char *ident, int value)
{
    DBG_PRINT("%s %s = %d\n", __func__, ident, value);
    Symbol *sym = new_symbol(ety, ident, value);
    sym->kind   = SYM_ENUM_CONST;
    // ns already NS_IDENT from new_symbol
    append_symbol(st, sym);
    return sym;
}

static Symbol *insert_typedef(Node *node, Type *type, const char *ident)
{
    DBG_PRINT("%s %s\n", __func__, ident);
    Symbol_table *st = node->st;
    for (Symbol *s = st->symbols; s; s = s->next)
        if (s->ns == NS_TYPEDEF && !strcmp(s->name, ident))
            return s;
    Symbol *sym = new_symbol(type, ident, 0);
    sym->ns     = NS_TYPEDEF;
    append_symbol(st, sym);
    return sym;
}

bool is_typedef_name(char *name)
{
    return find_symbol_st(type_ctx.curr_scope_st, name, NS_TYPEDEF) != NULL;
}

Type *find_typedef_type(char *name)
{
    Symbol *s = find_symbol_st(type_ctx.curr_scope_st, name, NS_TYPEDEF);
    if (s)
        return s->type;
    error("typedef '%s' not found\n", name);
    return NULL;
}

static Symbol *new_symbol(Type *type, const char *ident, int offset)
{
    Symbol *n = arena_alloc(sizeof(Symbol));
    n->name   = ident;
    n->type   = type;
    n->offset = offset;
    n->ns     = NS_IDENT;    // default; callers override for NS_TAG/NS_TYPEDEF
    return n;
}

static Symbol *insert_global_ident(Symbol_table *st, Type *type, const char *ident, StorageClass sclass)
{
    // Check for existing entry — dedup for pre-populated externs, upgrade on real definition.
    for (Symbol *s = st->symbols; s; s = s->next)
    {
        if (s->ns != NS_IDENT || strcmp(s->name, ident))
            continue;
        if (sclass != SC_EXTERN)
        {
            s->type = type;
            if (!istype_function(type))
            {
                s->offset  = st->size;
                st->size  += type->size;
            }
            if (sclass == SC_STATIC)
            {
                s->kind     = SYM_STATIC_GLOBAL;
                s->tu_index = current_global_tu;
            }
            else
            {
                s->kind = SYM_GLOBAL;
            }
        }
        return s;
    }
    // Fresh global symbol
    bool    is_extern = (sclass == SC_EXTERN);
    bool    is_fn     = istype_function(type);
    Symbol *n         = new_symbol(type, ident, (!is_extern && !is_fn) ? st->size : 0);
    if (!is_extern && !is_fn)
        st->size += type->size;
    if (is_extern)
        n->kind = SYM_EXTERN;
    else if (sclass == SC_STATIC)
    {
        n->kind     = SYM_STATIC_GLOBAL;
        n->tu_index = current_global_tu;
    }
    else
        n->kind = SYM_GLOBAL;
    append_symbol(st, n);
    return n;
}

static Symbol *insert_local_ident(Symbol_table *st, Type *type, const char *ident, bool is_param, StorageClass sclass)
{
    if (sclass == SC_STATIC)
    {
        // Local static: persistent storage in data section, not on the stack.
        Symbol *n   = new_symbol(type, ident, type_ctx.local_static_counter++);
        n->kind     = SYM_STATIC_LOCAL;
        n->tu_index = current_global_tu;
        append_symbol(st, n);
        return n;
    }
    Symbol *n;
    if (is_param)
    {
        n        = new_symbol(type, ident, st->param_offset);
        n->kind  = SYM_PARAM;
        st->param_offset += type->size;
    }
    else
    {
        st->local_offset += type->size;
        n        = new_symbol(type, ident, st->local_offset);
        n->kind  = SYM_LOCAL;
        st->size += type->size;
    }
    append_symbol(st, n);
    return n;
}

static Symbol *insert_ident(Node *node, Type *type, const char *ident, bool is_param, StorageClass sclass)
{
    DBG_PRINT("%s %s %s\n", __func__, fulltype_str(type), ident);
    Symbol_table *st = node->st;
    if (!st->depth)
        return insert_global_ident(st, type, ident, sclass);
    return insert_local_ident(st, type, ident, is_param, sclass);
}

// ---------------------------------------------------------------
// finalize_local_offsets — post-parse fixup pass
// ---------------------------------------------------------------
// Walk the flat scope list and make sym->offset bp-relative for all SYM_LOCAL
// symbols.  Must be called after program() returns, when all scope sizes are final.
void finalize_local_offsets(void)
{
    for (int i = 0; i < type_ctx.scope_count; i++)
    {
        Symbol_table *st = type_ctx.scope_list[i];
        if (st->depth == 0 || st->scope_type == ST_STRUCT)
            continue;
        // Compute go = sum of ancestor scope sizes (excluding global scope)
        int go = 0;
        for (Symbol_table *p = st->parent; p && p->depth > 0; p = p->parent)
            go += p->size;
        // Adjust local symbol offsets
        for (Symbol *s = st->symbols; s; s = s->next)
            if (s->ns == NS_IDENT && s->kind == SYM_LOCAL)
                s->offset += go;
    }
}

// ---------------------------------------------------------------
// fulltype_str
// ---------------------------------------------------------------
static char *fts2(Type *t, char *p)
{
    if (!t)
    {
        p += sprintf(p, "null");
        return p;
    }
    switch (t->base)
    {
    case TB_VOID:
        p += sprintf(p, "void");
        break;
    case TB_CHAR:
        p += sprintf(p, "char");
        break;
    case TB_UCHAR:
        p += sprintf(p, "uchar");
        break;
    case TB_SHORT:
        p += sprintf(p, "short");
        break;
    case TB_USHORT:
        p += sprintf(p, "ushort");
        break;
    case TB_INT:
        p += sprintf(p, "int");
        break;
    case TB_UINT:
        p += sprintf(p, "uint");
        break;
    case TB_LONG:
        p += sprintf(p, "long");
        break;
    case TB_ULONG:
        p += sprintf(p, "ulong");
        break;
    case TB_FLOAT:
        p += sprintf(p, "float");
        break;
    case TB_DOUBLE:
        p += sprintf(p, "double");
        break;
    case TB_POINTER:
        p += sprintf(p, "ptr(");
        p = fts2(t->u.ptr.pointee, p);
        p += sprintf(p, ")");
        break;
    case TB_ARRAY:
        p += sprintf(p, "arr[%d](", t->u.arr.count);
        p = fts2(t->u.arr.elem, p);
        p += sprintf(p, ")");
        break;
    case TB_FUNCTION:
        p += sprintf(p, "fn(ret=");
        p = fts2(t->u.fn.ret, p);
        p += sprintf(p, ")");
        break;
    case TB_STRUCT:
        p += sprintf(p, "struct{%s}", t->u.composite.tag ? t->u.composite.tag->name : "anon");
        break;
    case TB_ENUM:
        p += sprintf(p, "enum");
        break;
    default:
        p += sprintf(p, "??? (%d)", t->base);
        break;
    }
    return p;
}

const char *fulltype_str(Type *t)
{
    fts2(t, type_ctx.ft_buf);
    return type_ctx.ft_buf;
}

// ---------------------------------------------------------------
// Print tables
// ---------------------------------------------------------------
#ifdef DEBUG_ENABLED
void print_type_table()
{
    DBG_PRINT("------ Type table ------\n");
    for (Type *t = type_ctx.type_list; t; t = t->next)
        DBG_PRINT("  %016llx %s sz:%d al:%d\n", (unsigned long long)t, fulltype_str(t), t->size, t->align);
}

void print_symbol_table(Symbol_table *s, int depth)
{
    if (depth == 0)
    {
        DBG_PRINT("------ Symbol table ------\n");
        DBG_PRINT("Scope: id:%-4d depth:%-4d   0x%04x\n", s->scope_id, s->depth, s->size);
        for (Symbol *sm = s->symbols; sm; sm = sm->next)
        {
            if (sm->ns == NS_TAG)
                DBG_PRINT(
                    "%016llx %-10s tag %s\n", (unsigned long long)sm->type, sm->name, fulltype_str(sm->type));
            else
                DBG_PRINT("%016llx %-10s global 0x%02x %s\n",
                          (unsigned long long)sm->type,
                          sm->name,
                          sm->offset,
                          fulltype_str(sm->type));
        }
        // Print all scopes from flat list
        for (int i = 0; i < type_ctx.scope_count; i++)
            print_symbol_table(type_ctx.scope_list[i], type_ctx.scope_list[i]->depth);
        return;
    }
    for (int i = 0; i < depth; i++)
        DBG_PRINT("  ");
    DBG_PRINT("Scope: id:%-4d depth:%-4d   0x%04x\n", s->scope_id, s->depth, s->size);
    for (Symbol *sm = s->symbols; sm; sm = sm->next)
    {
        for (int i = 0; i < depth; i++)
            DBG_PRINT("  ");
        if (sm->ns == NS_TAG)
            DBG_PRINT(
                "%016llx %-10s tag %s\n", (unsigned long long)sm->type, sm->name, fulltype_str(sm->type));
        else
            DBG_PRINT("%016llx %-10s %s 0x%02x %s\n",
                      (unsigned long long)sm->type,
                      sm->name,
                      sm->kind == SYM_PARAM ? "param" : "local",
                      sm->offset,
                      fulltype_str(sm->type));
    }
}
#else
#define print_type_table()           ((void)0)
#define print_symbol_table(s, depth) ((void)0)
#endif

// ---------------------------------------------------------------
// Misc helpers for parser compatibility
// ---------------------------------------------------------------
const char *curr_scope_str()
{
    static char buf[64];
    sprintf(buf, "depth:%d", type_ctx.scope_depth);
    return buf;
}

Decl_spec to_typespec(Token_kind tk)
{
    switch (tk)
    {
    case TK_VOID:
        return DS_VOID;
    case TK_CHAR:
        return DS_CHAR;
    case TK_SHORT:
        return DS_SHORT;
    case TK_ENUM:
        return DS_ENUM;
    case TK_INT:
        return DS_INT;
    case TK_LONG:
        return DS_LONG;
    case TK_FLOAT:
        return DS_FLOAT;
    case TK_DOUBLE:
        return DS_DOUBLE;
    case TK_SIGNED:
        return DS_SIGNED;
    case TK_UNSIGNED:
        return DS_UNSIGNED;
    case TK_STRUCT:
        return DS_STRUCT;
    case TK_UNION:
        return DS_UNION;
    default:
        return 0;
    }
}
