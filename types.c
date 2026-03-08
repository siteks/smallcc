

#include "mycc.h"

// Type system context instance
TypeContext type_ctx;

// Legacy pointer aliases for convenience (these still work because macros define t_*)

Symbol *insert_tag(Node *node, char *ident);
static Symbol *insert_typedef(Node *node, Type *type, const char *ident);
static Symbol *new_symbol(Type *type, const char *ident, int offset);
static Symbol *insert_ident(Node *node, Type *type, const char *ident, bool is_param);
static Type  *generate_struct_type2(Node *decl_node, int depth);
static Type  *type_from_declarator(Node *decl, Type *base);
static Type  *type_from_direct_decl(Node *dd, Type *base);
static Type  *typespec_to_base(Decl_spec typespec);

// ---------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------
static int do_align(int val, int size)
{
    if (size == 2) return (val & 1) ? (val & ~1) + 2 : val;
    if (size == 4) return (val & 3) ? (val & ~3) + 4 : val;
    return val;
}

static void append_symbol(Symbol **head, Symbol *sym)
{
    Symbol *s = *head, *ls = NULL;
    for (; s; s = s->next) ls = s;
    if (!ls) *head = sym;
    else     ls->next = sym;
}

static void append_type(Type *t)
{
    Type *last = 0;
    for (Type *p = type_ctx.type_list; p; p = p->next) last = p;
    if (last) last->next = t;
    else       type_ctx.type_list = t;
}

// ---------------------------------------------------------------
// Factory functions — intern types in global list
// ---------------------------------------------------------------
Type *get_basic_type(Type_base base)
{
    for (Type *p = type_ctx.type_list; p; p = p->next)
        if (p->base == base && !p->u.ptr.pointee)
            return p;

    Type *t = calloc(1, sizeof(Type));
    t->base = base;
    switch (base) {
        case TB_VOID:              t->size = 0; t->align = 1; break;
        case TB_CHAR:
        case TB_UCHAR:             t->size = 1; t->align = 1; break;
        case TB_SHORT:
        case TB_USHORT:            t->size = 2; t->align = 2; break;
        case TB_INT:
        case TB_UINT:              t->size = 2; t->align = 2; break;
        case TB_LONG:
        case TB_ULONG:             t->size = 4; t->align = 4; break;
        case TB_FLOAT:             t->size = 4; t->align = 4; break;
        case TB_DOUBLE:            t->size = 4; t->align = 4; break;
        default:                    t->size = 2; t->align = 2; break;
    }
    append_type(t);
    return t;
}

Type *get_pointer_type(Type *pointee)
{
    for (Type *p = type_ctx.type_list; p; p = p->next)
        if (p->base == TB_POINTER && p->u.ptr.pointee == pointee)
            return p;

    Type *t = calloc(1, sizeof(Type));
    t->base = TB_POINTER;
    t->u.ptr.pointee = pointee;
    t->size  = 2;
    t->align = 2;
    append_type(t);
    return t;
}

Type *get_array_type(Type *elem, int count)
{
    for (Type *p = type_ctx.type_list; p; p = p->next)
        if (p->base == TB_ARRAY && p->u.arr.elem == elem && p->u.arr.count == count)
            return p;

    Type *t = calloc(1, sizeof(Type));
    t->base = TB_ARRAY;
    t->u.arr.elem  = elem;
    t->u.arr.count = count;
    t->size  = elem->size * count;
    t->align = elem->align;
    append_type(t);
    return t;
}

Type *get_function_type(Type *ret, Param *params, bool is_variadic)
{
    for (Type *p = type_ctx.type_list; p; p = p->next)
        if (p->base == TB_FUNCTION && p->u.fn.ret == ret && p->u.fn.is_variadic == is_variadic)
            return p;

    Type *t = calloc(1, sizeof(Type));
    t->base = TB_FUNCTION;
    t->u.fn.ret         = ret;
    t->u.fn.params      = params;
    t->u.fn.is_variadic = is_variadic;
    t->size  = 2;
    t->align = 2;
    append_type(t);
    return t;
}

Type *get_struct_type(Symbol *tag, Field *members, bool is_union)
{
    for (Type *p = type_ctx.type_list; p; p = p->next)
        if (p->base == TB_STRUCT && p->u.composite.tag == tag)
            return p;

    Type *t = calloc(1, sizeof(Type));
    t->base = TB_STRUCT;
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
    Type *t     = calloc(1, sizeof(Type));
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
    for (int i = 0; i < decl->pointer_level; i++)
        t = get_pointer_type(t);
    return type_from_direct_decl(decl->children[0], t);
}

static Type *type_from_direct_decl(Node *dd, Type *base)
{
    // Skip the leading ident or grouped inner declarator
    int first = (dd->child_count
                 && (dd->children[0]->kind == ND_IDENT
                     || dd->children[0]->kind == ND_DECLARATOR)) ? 1 : 0;

    // Apply array/function suffixes right-to-left (rightmost = innermost)
    Type *t = base;
    for (int i = dd->child_count - 1; i >= first; i--)
    {
        Node *s = dd->children[i];
        if (s->kind == ND_ARRAY_DECL)
        {
            int count = (s->child_count && s->children[0]->kind == ND_LITERAL)
                        ? (int)s->children[0]->ival : 0;
            t = get_array_type(t, count);
        }
        else if (s->is_function)
        {
            bool variadic = s->child_count && s->children[0]->is_variadic;
            t = get_function_type(t, NULL, variadic);
        }
    }

    // Grouped inner declarator (e.g. (*fp)): recurse with accumulated type as new base
    if (first && dd->children[0]->kind == ND_DECLARATOR)
        return type_from_declarator(dd->children[0], t);
    return t;
}

// ---------------------------------------------------------------
// typespec_to_base
// ---------------------------------------------------------------
static Type *typespec_to_base(Decl_spec typespec)
{
    switch ((int)typespec) {
        case DS_VOID:                           return t_void;
        case DS_CHAR:
        case DS_SIGNED|DS_CHAR:                 return t_char;
        case DS_UNSIGNED|DS_CHAR:               return t_uchar;
        case DS_SHORT:
        case DS_SIGNED|DS_SHORT:                return t_short;
        case DS_UNSIGNED|DS_SHORT:              return t_ushort;
        case DS_INT:
        case DS_SIGNED|DS_INT:
        case DS_SIGNED:                         return t_int;
        case DS_UNSIGNED|DS_INT:
        case DS_UNSIGNED:                       return t_uint;
        case DS_LONG:
        case DS_SIGNED|DS_LONG:                 return t_long;
        case DS_UNSIGNED|DS_LONG:               return t_ulong;
        case DS_FLOAT:                          return t_float;
        case DS_DOUBLE:                         return t_double;
        case DS_ENUM:                           return t_int;
        default:                                return t_int;
    }
}

// Exported: build Type from a declaration-context node (ND_DECLARATION or similar).
// Determines the base type from typespec, then applies the first declarator child
// if present (used by type_name() for casts/sizeof).
Type *type2_from_decl_node(Node *node)
{
    Type *base;
    if (node->typespec & DS_TYPEDEF) {
        base = node->type;
    } else if (node->typespec & (DS_STRUCT | DS_UNION)) {
        base = (node->type && node->type != t_void) ? node->type : t_void;
    } else if (node->typespec & DS_ENUM) {
        base = (node->type && node->type->base == TB_ENUM) ? node->type : t_int;
    } else {
        base = typespec_to_base(node->typespec);
    }
    if (node->child_count && node->children[0]->kind == ND_DECLARATOR)
        return type_from_declarator(node->children[0], base);
    return base;
}

// ---------------------------------------------------------------
// Array helpers
// ---------------------------------------------------------------
int array_dimensions(Type *t)
{
    int d = 0;
    while (t && t->base == TB_ARRAY) { d++; t = t->u.arr.elem; }
    return d;
}

Type *array_elem_type(Type *t)
{
    while (t && t->base == TB_ARRAY) t = t->u.arr.elem;
    return t;
}

Type *elem_type(Type *t)
{
    if (!t) return 0;
    if (t->base == TB_ARRAY)   return t->u.arr.elem;
    if (t->base == TB_POINTER) return t->u.ptr.pointee;
    return t;
}

// ---------------------------------------------------------------
// Type checks
// ---------------------------------------------------------------
bool istype_float(Type *t)    { return t && t->base == TB_FLOAT; }
bool istype_double(Type *t)   { return t && t->base == TB_DOUBLE; }
bool istype_fp(Type *t)       { return istype_float(t) || istype_double(t); }
bool istype_char(Type *t)     { return t && t->base == TB_CHAR; }
bool istype_uchar(Type *t)    { return t && t->base == TB_UCHAR; }
bool istype_short(Type *t)    { return t && t->base == TB_SHORT; }
bool istype_ushort(Type *t)   { return t && t->base == TB_USHORT; }
bool istype_enum(Type *t)     { return t && t->base == TB_ENUM; }
bool istype_int(Type *t)      { return t && t->base == TB_INT; }
bool istype_uint(Type *t)     { return t && t->base == TB_UINT; }
bool istype_long(Type *t)     { return t && t->base == TB_LONG; }
bool istype_ulong(Type *t)    { return t && t->base == TB_ULONG; }
bool istype_ptr(Type *t)      { return t && t->base == TB_POINTER; }
bool istype_array(Type *t)    { return t && t->base == TB_ARRAY; }
bool istype_function(Type *t) { return t && t->base == TB_FUNCTION; }
bool istype_intlike(Type *t)
{
    if (!t) return false;
    return t->base == TB_CHAR  || t->base == TB_UCHAR
        || t->base == TB_SHORT || t->base == TB_USHORT
        || t->base == TB_INT   || t->base == TB_UINT
        || t->base == TB_LONG  || t->base == TB_ULONG
        || t->base == TB_ENUM;
}

// ---------------------------------------------------------------
// Per-TU reset and extern symbol injection
// ---------------------------------------------------------------
void reset_types_state(void)
{
    type_ctx.symbol_table      = calloc(1, sizeof(Symbol_table));
    type_ctx.curr_scope_st     = type_ctx.symbol_table;
    type_ctx.last_symbol_table = type_ctx.symbol_table;
    type_ctx.scope_depth       = 0;
    memset(type_ctx.scope_indices, 0, sizeof(type_ctx.scope_indices));
}

void insert_extern_sym(const char *name, Type *type)
{
    // Skip if already present (e.g. putchar from make_basic_types)
    for (Symbol *s = type_ctx.symbol_table->idents; s; s = s->next)
        if (!strcmp(s->name, name)) return;

    Symbol *sym         = calloc(1, sizeof(Symbol));
    sym->name           = name;
    sym->type           = type;
    sym->is_extern_decl = true;
    append_symbol(&type_ctx.symbol_table->idents, sym);
}

// ---------------------------------------------------------------
// make_basic_types
// ---------------------------------------------------------------
static void insert_builtin(char *name, Type *type)
{
    Symbol *sym = calloc(1, sizeof(Symbol));
    sym->name = name;
    sym->type = type;
    sym->offset = 0;
    append_symbol(&type_ctx.symbol_table->idents, sym);
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
    Param *p = calloc(1, sizeof(Param));
    p->type  = t_int;
    insert_builtin("putchar", get_function_type(t_int, p, false));
}

// ---------------------------------------------------------------
// find_offset
// ---------------------------------------------------------------
int find_offset(Type *t, char *field, Type **it)
{
    if (!t || t->base != TB_STRUCT) return -1;
    *it = 0;
    for (Field *f = t->u.composite.members; f; f = f->next) {
        if (f->name && !strcmp(field, f->name)) {
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
    int off = 0;
    int max_align = 1;
    for (Field *f = st->u.composite.members; f; f = f->next) {
        int al = f->type->align ? f->type->align : 1;
        if (st->u.composite.is_union) {
            f->offset = 0;
            if (f->type->size > off) off = f->type->size;
        } else {
            off = do_align(off, al);
            f->offset = off;
            off += f->type->size;
        }
        if (al > max_align) max_align = al;
    }
    st->align = max_align;
    st->size  = st->u.composite.is_union ? off : do_align(off, max_align);
}

// ---------------------------------------------------------------
// generate_struct_type2
// ---------------------------------------------------------------
static Type *generate_struct_type2(Node *decl_node, int depth)
{
    DBG_FUNC();
    if (!(decl_node->kind == ND_DECLARATION && decl_node->child_count >= 1
          && decl_node->children[0]->kind == ND_STRUCT))
        error("Should be struct declaration!\n");

    Node *struct_node = decl_node->children[0];
    char *tagname = struct_node->children[0]->u.ident;
    Symbol *tag = find_symbol(decl_node, tagname, NS_TAG);
    if (!tag) {
#ifdef DEBUG_ENABLED
        print_type_table();
        print_symbol_table(type_ctx.symbol_table, 0);
#endif
        error("Tag %s not found\n", tagname);
    }

    if (tag->type == t_void) {
        // Build Field list from struct member declarations
        DBG_PRINT("%s defining struct %s\n", __func__, tagname);
        Field *fields   = NULL;
        Field **last_ptr = &fields;

        for (int i = 1; i < struct_node->child_count; i++) {
            Node *d = struct_node->children[i];
            if (d->kind != ND_DECLARATION) continue;

            bool has_nested_struct = (d->child_count > 0
                                      && d->children[0]->kind == ND_STRUCT
                                      && d->children[0]->child_count > 1);

            for (int j = 0; j < d->child_count; j++) {
                Node *m = d->children[j];
                if (m->kind == ND_STRUCT) continue;  // handled via parent 'd'
                if (m->kind != ND_DECLARATOR) continue;

                Type *base;
                if (d->typespec & (DS_STRUCT | DS_UNION)) {
                    if (has_nested_struct) {
                        base = generate_struct_type2(d, depth + 1);
                    } else {
                        Node *sn   = d->children[0];
                        char *stag = sn->children[0]->u.ident;
                        Symbol *stag_sym = find_symbol(d, stag, NS_TAG);
                        base = (stag_sym && stag_sym->type) ? stag_sym->type : t_void;
                    }
                } else {
                    base = typespec_to_base(d->typespec);
                }

                Type *mty  = type_from_declarator(m, base);
                const char *fname = get_decl_ident(m);

                Field *f    = calloc(1, sizeof(Field));
                f->name     = fname ? strdup(fname) : strdup("");
                f->type     = mty;
                *last_ptr   = f;
                last_ptr    = &f->next;

                DBG_PRINT("%s  member '%s' type %s\n",
                    __func__, f->name, fulltype_str(mty));
            }
        }

        // Create struct Type entry
        Type *st   = calloc(1, sizeof(Type));
        st->base    = TB_STRUCT;
        st->u.composite.tag      = tag;
        st->u.composite.members  = fields;
        st->u.composite.is_union = !!(decl_node->typespec & DS_UNION);
        calc_struct_layout(st);
        append_type(st);
        tag->type = st;
        return st;

    } else if (struct_node->child_count > 1) {
        error("Redefinition of tag %s\n", tagname);
    }
    DBG_PRINT("%s referencing existing struct %s\n", __func__, tagname);
    return tag->type;
}

// ---------------------------------------------------------------
// add_types_and_symbols
// ---------------------------------------------------------------
void add_types_and_symbols(Node *node, bool is_param, int depth)
{
    DBG_FUNC();
    if (node->kind != ND_DECLARATION)
        error("Node should be ND_DECLARATION\n");

    // First pass: handle struct definitions and determine base type
    for (int i = 0; i < node->child_count; i++) {
        Node *n = node->children[i];
        if (n->kind == ND_STRUCT) {
            if (n->child_count == 1) {
                if (node->child_count == 1) {
                    // Standalone incomplete struct (e.g. "struct foo;")
                    DBG_PRINT("%s incomplete struct\n", __func__);
                    Type *t    = generate_struct_type2(node, 0);
                    if (n->symbol) n->symbol->type = t;
                    node->type  = t;
                } else {
                    // "struct foo" as type specifier with a declarator
                    Symbol *s   = find_symbol(node, n->children[0]->u.ident, NS_TAG);
                    node->type  = s->type;
                }
            } else if (n->child_count > 1) {
                // Full struct definition with body
                n->symbol = insert_tag(node, n->children[0]->u.ident);
                if (!depth) {
                    DBG_PRINT("%s creating struct type\n", __func__);
                    Type *t    = generate_struct_type2(node, 0);
                    n->symbol->type = t;
                    node->type  = t;
                }
            }
        }
    }

    // Determine base type for declarators
    Type *base;
    if (node->typespec & DS_TYPEDEF) {
        base = node->type;
    } else if (node->typespec & (DS_STRUCT | DS_UNION)) {
        base = (node->type && node->type->base == TB_STRUCT) ? node->type : t_void;
    } else if (node->typespec & DS_ENUM) {
        base = (node->type && node->type->base == TB_ENUM) ? node->type : t_int;
    } else {
        base = typespec_to_base(node->typespec);
    }

    // Second pass: process every ND_DECLARATOR child
    bool first_decl = true;
    for (int i = 0; i < node->child_count; i++) {
        Node *n = node->children[i];
        if (n->kind != ND_DECLARATOR || depth)
            continue;

        const char *ident = get_decl_ident(n);
        Type *ty   = type_from_declarator(n, base);
        DBG_PRINT("%s declarator ident:'%s' type:%s\n",
            __func__, ident ? ident : "", fulltype_str(ty));
        // char s[] = "hello" — fix up zero-size array from string literal init
        if (ty->base == TB_ARRAY && ty->u.arr.count == 0
            && n->child_count == 2 && n->children[1]->strval)
            ty = get_array_type(ty->u.arr.elem, n->children[1]->strval_len + 1);
        if (first_decl) {
            node->type = ty;
            first_decl = false;
        }
        DBG_PRINT("%s final type %s\n", __func__, fulltype_str(ty));

        if (ident) {
            if (node->sclass == TK_TYPEDEF)
                n->symbol = insert_typedef(node, ty, ident);
            else
                n->symbol = insert_ident(node, ty, ident, is_param);

            // For struct-typed variables, ensure symbol points to struct type
            if (node->child_count > 0
                && node->children[0]->kind == ND_STRUCT) {
                DBG_PRINT("%s setting sym type from tag\n", __func__);
                n->symbol->type = ty;  // ty already wraps struct base
            }
        }
    }
}

// ---------------------------------------------------------------
// Symbol table
// ---------------------------------------------------------------
Symbol_table *find_scope(Node *node)
{
    return node->st;
}

Symbol *find_symbol(Node *n, const char *name, Namespace nspace)
{
    Symbol_table *st = find_scope(n);
    Symbol *s;
    bool found = false;
    while (!found) {
        DBG_PRINT("Searching in scope:%s\n", scope_str(st->scope));
        s =     nspace == NS_IDENT   ? st->idents
            :   nspace == NS_TAG     ? st->tags
            :   nspace == NS_TYPEDEF ? st->typedefs
            :                          st->labels;
        for (; s; s = s->next) {
            DBG_PRINT("Name:%s\n", s->name);
            if (!strcmp(name, s->name)) { found = true; break; }
        }
        if (found) break;
        DBG_PRINT("Not found, going to enclosing scope\n");
        if (st->parent) st = st->parent;
        else break;
    }
    if (!found) {
#ifdef DEBUG_ENABLED
        print_type_table();
        print_symbol_table(type_ctx.symbol_table, 0);
#endif
        error("%s %s not found!\n",
              nspace == NS_IDENT ? "ident" : nspace == NS_TAG ? "tag" : "label", name);
    }
    return s;
}

static Symbol_table *new_st_scope()
{
    type_ctx.curr_scope_st->child_count++;
    type_ctx.curr_scope_st->children = (Symbol_table **)realloc(type_ctx.curr_scope_st->children,
                                type_ctx.curr_scope_st->child_count * sizeof(Symbol_table *));
    Symbol_table *nt = calloc(1, sizeof(Symbol_table));
    type_ctx.curr_scope_st->children[type_ctx.curr_scope_st->child_count - 1] = nt;
    Scope *sc   = calloc(1, sizeof(Scope));
    set_scope(sc);
    nt->scope   = *sc;
    nt->parent  = type_ctx.curr_scope_st;
    type_ctx.curr_scope_st = nt;
    return type_ctx.curr_scope_st;
}

Symbol_table *enter_new_scope(bool use_last_scope)
{
    if (use_last_scope) {
        type_ctx.scope_depth++;
        type_ctx.curr_scope_st = type_ctx.last_symbol_table;
        return type_ctx.last_symbol_table;
    }
    type_ctx.scope_indices[type_ctx.scope_depth++]++;
    type_ctx.scope_indices[type_ctx.scope_depth] = 0;
    type_ctx.last_symbol_table = new_st_scope();
    type_ctx.curr_scope_st     = type_ctx.last_symbol_table;
    return type_ctx.last_symbol_table;
}

void leave_scope()
{
    type_ctx.scope_depth--;
    type_ctx.curr_scope_st = type_ctx.curr_scope_st->parent;
}

void set_scope(Scope *sc)
{
    sc->depth   = type_ctx.scope_depth;
    sc->indices = calloc(type_ctx.scope_depth, sizeof(int));
    memcpy(sc->indices, type_ctx.scope_indices, type_ctx.scope_depth * sizeof(int));
}

const char *scope_str(Scope sc)
{
    static char buf[1024];
    int l = 0;
    int d = sc.depth;
    l = sprintf(buf, "%s:%d.",
        sc.scope_type == ST_COMPSTMT ? "compstmt" : "struct", d);
    for (int i = 0; i < d; i++)
        l += sprintf(buf + l, "%d%c", sc.indices[i], i < d - 1 ? '.' : ' ');
    return buf;
}

Symbol *insert_tag(Node *node, char *ident)
{
    DBG_PRINT("%s %s\n", __func__, ident);
    Symbol_table *st = find_scope(node);
    for (Symbol *s = st->tags; s; s = s->next)
        if (!strcmp(s->name, ident))
        {
            DBG_PRINT("%s tag already exists\n", __func__);
            return s;
        }
    Symbol *sym = new_symbol(t_void, ident, 0);
    append_symbol(&st->tags, sym);
    return sym;
}

Symbol *insert_enum_const(Node *node, Type *ety, char *ident, int value)
{
    DBG_PRINT("%s %s = %d\n", __func__, ident, value);
    Symbol_table *st = find_scope(node);
    Symbol *sym = new_symbol(ety, ident, value);
    sym->is_enum_const = true;
    append_symbol(&st->idents, sym);
    return sym;
}

static Symbol *insert_typedef(Node *node, Type *type, const char *ident)
{
    DBG_PRINT("%s %s\n", __func__, ident);
    Symbol_table *st = find_scope(node);
    for (Symbol *s = st->typedefs; s; s = s->next)
        if (!strcmp(s->name, ident)) return s;
    Symbol *sym = new_symbol(type, ident, 0);
    append_symbol(&st->typedefs, sym);
    return sym;
}

static Symbol *find_typedef_symbol(char *name)
{
    for (Symbol_table *st = type_ctx.curr_scope_st; st; st = st->parent)
        for (Symbol *s = st->typedefs; s; s = s->next)
            if (!strcmp(s->name, name)) return s;
    return NULL;
}

bool is_typedef_name(char *name)
{
    if (!strcmp(name, "va_list")) return true;
    return find_typedef_symbol(name) != NULL;
}

Type *find_typedef_type(char *name)
{
    if (!strcmp(name, "va_list")) return t_int;
    Symbol *s = find_typedef_symbol(name);
    if (s) return s->type;
    error("typedef '%s' not found\n", name);
    return NULL;
}

static Symbol *new_symbol(Type *type, const char *ident, int offset)
{
    Symbol *n   = calloc(1, sizeof(Symbol));
    n->name     = ident;
    n->type     = type;
    n->offset   = offset;
    return n;
}

static Symbol *insert_ident(Node *node, Type *type, const char *ident, bool is_param)
{
    DBG_PRINT("%s %s %s\n", __func__, fulltype_str(type), ident);
    Symbol_table *st = node->st;
    int go = 0;

    if (st->scope.depth) {
        if (node->sclass == TK_STATIC)
        {
            // Local static: persistent storage in data section, not on stack.
            Symbol *n          = new_symbol(type, ident, type_ctx.local_static_counter++);
            n->is_static       = true;
            n->is_local_static = true;
            // Do NOT increment st->size — not on the stack.
            append_symbol(&st->idents, n);
            return n;
        }
        // Compute go: sum of sizes of all non-global ancestor scopes
        Symbol_table *ancestor = st->parent;
        while (ancestor && ancestor->scope.depth > 0)
        {
            go += ancestor->size;
            ancestor = ancestor->parent;
        }
    } else {
        // Global scope — check for existing entry first (dedup for pre-populated externs)
        for (Symbol *s = st->idents; s; s = s->next)
        {
            if (!strcmp(s->name, ident))
            {
                if (node->sclass != TK_EXTERN)
                {
                    // Real definition: upgrade pre-populated extern entry
                    s->is_extern_decl = false;
                    s->type           = type;
                    if (!istype_function(type))
                    {
                        s->offset  = st->size;
                        st->size  += type->size;
                    }
                    if (node->sclass == TK_STATIC)
                    {
                        s->is_static = true;
                        s->tu_index  = current_global_tu;
                    }
                }
                return s;
            }
        }
        // Fresh symbol
        bool is_extern = (node->sclass == TK_EXTERN);
        bool is_fn     = istype_function(type);
        int  offset    = st->size;
        if (!is_extern && !is_fn) st->size += type->size;
        Symbol *n         = new_symbol(type, ident, is_extern || is_fn ? 0 : offset);
        n->is_extern_decl = is_extern;
        if (node->sclass == TK_STATIC)
        {
            n->is_static = true;
            n->tu_index  = current_global_tu;
        }
        append_symbol(&st->idents, n);
        return n;
    }
    st->global_offset = go;

    int offset       = 0;
    int param_offset = 8;
    int last_size    = 0;
    for (Symbol *s = st->idents; s; s = s->next) {
        if (s->is_param) {
            param_offset = s->offset;
            last_size    = s->type->size;
        } else {
            offset = s->offset;
        }
    }
    offset       += type->size;
    param_offset += last_size;
    if (!is_param) st->size += type->size;
    Symbol *n    = new_symbol(type, ident, is_param ? param_offset : offset);
    n->is_param  = is_param;
    append_symbol(&st->idents, n);
    return n;
}

// ---------------------------------------------------------------
// fulltype_str
// ---------------------------------------------------------------
static char *fts2(Type *t, char *p)
{
    if (!t) { p += sprintf(p, "null"); return p; }
    switch (t->base) {
        case TB_VOID:     p += sprintf(p, "void");   break;
        case TB_CHAR:     p += sprintf(p, "char");   break;
        case TB_UCHAR:    p += sprintf(p, "uchar");  break;
        case TB_SHORT:    p += sprintf(p, "short");  break;
        case TB_USHORT:   p += sprintf(p, "ushort"); break;
        case TB_INT:      p += sprintf(p, "int");    break;
        case TB_UINT:     p += sprintf(p, "uint");   break;
        case TB_LONG:     p += sprintf(p, "long");   break;
        case TB_ULONG:    p += sprintf(p, "ulong");  break;
        case TB_FLOAT:    p += sprintf(p, "float");  break;
        case TB_DOUBLE:   p += sprintf(p, "double"); break;
        case TB_POINTER:
            p += sprintf(p, "ptr(");
            p  = fts2(t->u.ptr.pointee, p);
            p += sprintf(p, ")");
            break;
        case TB_ARRAY:
            p += sprintf(p, "arr[%d](", t->u.arr.count);
            p  = fts2(t->u.arr.elem, p);
            p += sprintf(p, ")");
            break;
        case TB_FUNCTION:
            p += sprintf(p, "fn(ret=");
            p  = fts2(t->u.fn.ret, p);
            p += sprintf(p, ")");
            break;
        case TB_STRUCT:
            p += sprintf(p, "struct{%s}",
                t->u.composite.tag ? t->u.composite.tag->name : "anon");
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
        DBG_PRINT("  %016llx %s sz:%d al:%d\n",
            (unsigned long long)t, fulltype_str(t), t->size, t->align);
}

void print_symbol_table(Symbol_table *s, int depth)
{
    if (depth == 0) DBG_PRINT("------ Symbol table ------\n");
    for (int i = 0; i < depth; i++) DBG_PRINT("  ");
    DBG_PRINT("Scope:%-20s   0x%04x 0x%04x\n",
        scope_str(s->scope), s->size, s->global_offset);
    Symbol *sm;
    for (sm = s->idents; sm; sm = sm->next) {
        for (int i = 0; i < depth; i++) DBG_PRINT("  ");
        DBG_PRINT("%016llx %-10s %s 0x%02x %s\n",
            (unsigned long long)sm->type, sm->name,
            sm->is_param ? "param" : depth ? "local" : "global",
            sm->offset, fulltype_str(sm->type));
    }
    for (sm = s->tags; sm; sm = sm->next) {
        for (int i = 0; i < depth; i++) DBG_PRINT("  ");
        DBG_PRINT("%016llx %-10s tag %s\n",
            (unsigned long long)sm->type, sm->name, fulltype_str(sm->type));
    }
    for (int i = 0; i < s->child_count; i++)
        print_symbol_table(s->children[i], depth + 1);
}
#else
#define print_type_table() ((void)0)
#define print_symbol_table(s, depth) ((void)0)
#endif

// ---------------------------------------------------------------
// Misc helpers for parser compatibility
// ---------------------------------------------------------------
const char *curr_scope_str()
{
    static char buf[1024];
    sprintf(buf, "%d:", type_ctx.scope_depth);
    for (int i = 0; i < type_ctx.scope_depth; i++)
        sprintf(buf + strlen(buf), "%d.", type_ctx.scope_indices[i]);
    return buf;
}


Decl_spec to_typespec(Token_kind tk)
{
    switch (tk) {
        case TK_VOID:       return DS_VOID;
        case TK_CHAR:       return DS_CHAR;
        case TK_SHORT:      return DS_SHORT;
        case TK_ENUM:       return DS_ENUM;
        case TK_INT:        return DS_INT;
        case TK_LONG:       return DS_LONG;
        case TK_FLOAT:      return DS_FLOAT;
        case TK_DOUBLE:     return DS_DOUBLE;
        case TK_SIGNED:     return DS_SIGNED;
        case TK_UNSIGNED:   return DS_UNSIGNED;
        case TK_STRUCT:     return DS_STRUCT;
        case TK_UNION:      return DS_UNION;
        default:            return 0;
    }
}
