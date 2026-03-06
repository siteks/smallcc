

#include "mycc.h"

// Type table — linked list of all Type2 entries (interned)
Type2   *types;

// Basic type globals
Type2   *t_void;
Type2   *t_char;
Type2   *t_uchar;
Type2   *t_short;
Type2   *t_ushort;
Type2   *t_int;
Type2   *t_uint;
Type2   *t_long;
Type2   *t_ulong;
Type2   *t_float;
Type2   *t_double;


static int scope_depth;
static int scope_indices[100];

Symbol_table    *symbol_table;
Symbol_table    *last_symbol_table;
Symbol_table    *curr_scope_st;

static Symbol *insert_tag(Node *node, char *ident);
static Symbol *new_symbol(Type2 *type, char *ident, int offset);
static Symbol *insert_ident(Node *node, Type2 *type, char *ident, bool is_param);
static Type2  *generate_struct_type2(Node *decl_node, int depth);
static Type2  *apply_derivation(Type2 *base, const char *ts);
static Type2  *typespec_to_base(Type_base typespec);

// ---------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------
static int do_align(int val, int size)
{
    if (size == 2) return (val & 1) ? (val & ~1) + 2 : val;
    if (size == 4) return (val & 3) ? (val & ~3) + 4 : val;
    return val;
}

static void append_type(Type2 *t)
{
    Type2 *last = 0;
    for (Type2 *p = types; p; p = p->next) last = p;
    if (last) last->next = t;
    else       types = t;
}

// ---------------------------------------------------------------
// Factory functions — intern types in global list
// ---------------------------------------------------------------
Type2 *get_basic_type(Type2_base base)
{
    for (Type2 *p = types; p; p = p->next)
        if (p->base == base && !p->u.ptr.pointee)
            return p;

    Type2 *t = calloc(1, sizeof(Type2));
    t->base = base;
    switch (base) {
        case TB2_VOID:              t->size = 0; t->align = 1; break;
        case TB2_CHAR:
        case TB2_UCHAR:             t->size = 1; t->align = 1; break;
        case TB2_SHORT:
        case TB2_USHORT:            t->size = 2; t->align = 2; break;
        case TB2_INT:
        case TB2_UINT:              t->size = 2; t->align = 2; break;
        case TB2_LONG:
        case TB2_ULONG:             t->size = 4; t->align = 4; break;
        case TB2_FLOAT:             t->size = 4; t->align = 4; break;
        case TB2_DOUBLE:            t->size = 4; t->align = 4; break;
        default:                    t->size = 2; t->align = 2; break;
    }
    append_type(t);
    return t;
}

Type2 *get_pointer_type(Type2 *pointee)
{
    for (Type2 *p = types; p; p = p->next)
        if (p->base == TB2_POINTER && p->u.ptr.pointee == pointee)
            return p;

    Type2 *t = calloc(1, sizeof(Type2));
    t->base = TB2_POINTER;
    t->u.ptr.pointee = pointee;
    t->size  = 2;
    t->align = 2;
    append_type(t);
    return t;
}

Type2 *get_array_type(Type2 *elem, int count)
{
    for (Type2 *p = types; p; p = p->next)
        if (p->base == TB2_ARRAY && p->u.arr.elem == elem && p->u.arr.count == count)
            return p;

    Type2 *t = calloc(1, sizeof(Type2));
    t->base = TB2_ARRAY;
    t->u.arr.elem  = elem;
    t->u.arr.count = count;
    t->size  = elem->size * count;
    t->align = elem->align;
    append_type(t);
    return t;
}

Type2 *get_function_type(Type2 *ret, Param *params, bool is_variadic)
{
    for (Type2 *p = types; p; p = p->next)
        if (p->base == TB2_FUNCTION && p->u.fn.ret == ret)
            return p;

    Type2 *t = calloc(1, sizeof(Type2));
    t->base = TB2_FUNCTION;
    t->u.fn.ret         = ret;
    t->u.fn.params      = params;
    t->u.fn.is_variadic = is_variadic;
    t->size  = 2;
    t->align = 2;
    append_type(t);
    return t;
}

Type2 *get_struct_type(Symbol *tag, Field *members, bool is_union)
{
    for (Type2 *p = types; p; p = p->next)
        if (p->base == TB2_STRUCT && p->u.composite.tag == tag)
            return p;

    Type2 *t = calloc(1, sizeof(Type2));
    t->base = TB2_STRUCT;
    t->u.composite.tag      = tag;
    t->u.composite.members  = members;
    t->u.composite.is_union = is_union;
    append_type(t);
    return t;
}

// ---------------------------------------------------------------
// Apply derivation string to base type, building Type2 chain.
// Processed left-to-right, built inside-out:
//   "[3][4]" -> array(3, array(4, base))
//   "*"      -> pointer(base)
//   "()"     -> function(ret=base)
// ---------------------------------------------------------------
static Type2 *apply_derivation(Type2 *base, const char *ts)
{
    if (!ts || !ts[0]) return base;

    if (ts[0] == '[') {
        char *end;
        int count = (int)strtol(ts + 1, &end, 0);
        if (*end == ']') end++;
        Type2 *inner = apply_derivation(base, end);
        return get_array_type(inner, count);
    } else if (ts[0] == '*') {
        Type2 *inner = apply_derivation(base, ts + 1);
        return get_pointer_type(inner);
    } else if (ts[0] == '(') {
        Type2 *inner = apply_derivation(base, ts + 2);
        return get_function_type(inner, NULL, false);
    }
    return base;
}

// ---------------------------------------------------------------
// typespec_to_base
// ---------------------------------------------------------------
static Type2 *typespec_to_base(Type_base typespec)
{
    switch ((int)typespec) {
        case TB_VOID:                           return t_void;
        case TB_CHAR:
        case TB_SIGNED|TB_CHAR:                 return t_char;
        case TB_UNSIGNED|TB_CHAR:               return t_uchar;
        case TB_SHORT:
        case TB_SIGNED|TB_SHORT:                return t_short;
        case TB_UNSIGNED|TB_SHORT:              return t_ushort;
        case TB_INT:
        case TB_SIGNED|TB_INT:
        case TB_SIGNED:                         return t_int;
        case TB_UNSIGNED|TB_INT:
        case TB_UNSIGNED:                       return t_uint;
        case TB_LONG:
        case TB_SIGNED|TB_LONG:                 return t_long;
        case TB_UNSIGNED|TB_LONG:               return t_ulong;
        case TB_FLOAT:                          return t_float;
        case TB_DOUBLE:                         return t_double;
        case TB_ENUM:                           return t_int;
        default:                                return t_int;
    }
}

// Exported: build Type2 from node's typespec + derivation string.
// For struct/union, node->type must already be set to the struct type.
Type2 *type2_from_ts(Node *node, char *ts)
{
    Type2 *base;
    if (node->typespec & (TB_STRUCT | TB_UNION)) {
        base = (node->type && node->type != t_void) ? node->type : t_void;
    } else {
        base = typespec_to_base(node->typespec);
    }
    return apply_derivation(base, ts);
}

// ---------------------------------------------------------------
// Array helpers
// ---------------------------------------------------------------
int array_dimensions(Type2 *t)
{
    int d = 0;
    while (t && t->base == TB2_ARRAY) { d++; t = t->u.arr.elem; }
    return d;
}

Type2 *array_elem_type(Type2 *t)
{
    while (t && t->base == TB2_ARRAY) t = t->u.arr.elem;
    return t;
}

Type2 *elem_type(Type2 *t)
{
    if (!t) return 0;
    if (t->base == TB2_ARRAY)   return t->u.arr.elem;
    if (t->base == TB2_POINTER) return t->u.ptr.pointee;
    return t;
}

// ---------------------------------------------------------------
// Type checks
// ---------------------------------------------------------------
bool istype_float(Type2 *t)    { return t && t->base == TB2_FLOAT; }
bool istype_double(Type2 *t)   { return t && t->base == TB2_DOUBLE; }
bool istype_char(Type2 *t)     { return t && t->base == TB2_CHAR; }
bool istype_uchar(Type2 *t)    { return t && t->base == TB2_UCHAR; }
bool istype_short(Type2 *t)    { return t && t->base == TB2_SHORT; }
bool istype_ushort(Type2 *t)   { return t && t->base == TB2_USHORT; }
bool istype_enum(Type2 *t)     { return t && t->base == TB2_ENUM; }
bool istype_int(Type2 *t)      { return t && t->base == TB2_INT; }
bool istype_uint(Type2 *t)     { return t && t->base == TB2_UINT; }
bool istype_long(Type2 *t)     { return t && t->base == TB2_LONG; }
bool istype_ulong(Type2 *t)    { return t && t->base == TB2_ULONG; }
bool istype_ptr(Type2 *t)      { return t && t->base == TB2_POINTER; }
bool istype_array(Type2 *t)    { return t && t->base == TB2_ARRAY; }
bool istype_function(Type2 *t) { return t && t->base == TB2_FUNCTION; }
bool istype_intlike(Type2 *t)
{
    if (!t) return false;
    return t->base == TB2_CHAR  || t->base == TB2_UCHAR
        || t->base == TB2_SHORT || t->base == TB2_USHORT
        || t->base == TB2_INT   || t->base == TB2_UINT
        || t->base == TB2_LONG  || t->base == TB2_ULONG
        || t->base == TB2_ENUM;
}

// ---------------------------------------------------------------
// make_basic_types
// ---------------------------------------------------------------
void make_basic_types()
{
    fprintf(stderr, "%s\n", __func__);
    t_void   = get_basic_type(TB2_VOID);
    t_char   = get_basic_type(TB2_CHAR);
    t_uchar  = get_basic_type(TB2_UCHAR);
    t_short  = get_basic_type(TB2_SHORT);
    t_ushort = get_basic_type(TB2_USHORT);
    t_int    = get_basic_type(TB2_INT);
    t_uint   = get_basic_type(TB2_UINT);
    t_long   = get_basic_type(TB2_LONG);
    t_ulong  = get_basic_type(TB2_ULONG);
    t_float  = get_basic_type(TB2_FLOAT);
    t_double = get_basic_type(TB2_DOUBLE);
}

// ---------------------------------------------------------------
// find_offset
// ---------------------------------------------------------------
int find_offset(Type2 *t, char *field, Type2 **it)
{
    if (!t || t->base != TB2_STRUCT) return -1;
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
static void calc_struct_layout(Type2 *st)
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
static Type2 *generate_struct_type2(Node *decl_node, int depth)
{
    fprintf(stderr, "%s\n", __func__);
    if (!(decl_node->kind == ND_DECLARATION && decl_node->child_count >= 1
          && decl_node->children[0]->kind == ND_STRUCT))
        error("Should be struct declaration!\n");

    Node *struct_node = decl_node->children[0];
    char *tagname = struct_node->children[0]->val;
    Symbol *tag = find_symbol(decl_node, tagname, NS_TAG);
    if (!tag) {
        print_type_table();
        print_symbol_table(symbol_table, 0);
        error("Tag %s not found\n", tagname);
    }

    if (tag->type == t_void) {
        // Build Field list from struct member declarations
        fprintf(stderr, "%s defining struct %s\n", __func__, tagname);
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

                Type2 *base;
                if (d->typespec & (TB_STRUCT | TB_UNION)) {
                    if (has_nested_struct) {
                        base = generate_struct_type2(d, depth + 1);
                    } else {
                        Node *sn   = d->children[0];
                        char *stag = sn->children[0]->val;
                        Symbol *stag_sym = find_symbol(d, stag, NS_TAG);
                        base = (stag_sym && stag_sym->type) ? stag_sym->type : t_void;
                    }
                } else {
                    base = typespec_to_base(d->typespec);
                }

                char *ts   = tstr_compact(m);
                Type2 *mty = apply_derivation(base, ts);
                char *fname = get_decl_ident(m);

                Field *f    = calloc(1, sizeof(Field));
                f->name     = fname ? strdup(fname) : strdup("");
                f->type     = mty;
                *last_ptr   = f;
                last_ptr    = &f->next;

                fprintf(stderr, "%s  member '%s' type %s\n",
                    __func__, f->name, fulltype_str(mty));
            }
        }

        // Create struct Type2 entry
        Type2 *st   = calloc(1, sizeof(Type2));
        st->base    = TB2_STRUCT;
        st->u.composite.tag      = tag;
        st->u.composite.members  = fields;
        st->u.composite.is_union = !!(decl_node->typespec & TB_UNION);
        calc_struct_layout(st);
        append_type(st);
        tag->type = st;
        return st;

    } else if (struct_node->child_count > 1) {
        error("Redefinition of tag %s\n", tagname);
    }
    fprintf(stderr, "%s referencing existing struct %s\n", __func__, tagname);
    return tag->type;
}

// ---------------------------------------------------------------
// add_types_and_symbols
// ---------------------------------------------------------------
void add_types_and_symbols(Node *node, bool is_param, int depth)
{
    fprintf(stderr, "%s\n", __func__);
    if (node->kind != ND_DECLARATION)
        error("Node should be ND_DECLARATION\n");

    // First pass: handle struct definitions and determine base type
    for (int i = 0; i < node->child_count; i++) {
        Node *n = node->children[i];
        if (n->kind == ND_STRUCT) {
            if (n->child_count == 1) {
                if (node->child_count == 1) {
                    // Standalone incomplete struct (e.g. "struct foo;")
                    fprintf(stderr, "%s incomplete struct\n", __func__);
                    Type2 *t    = generate_struct_type2(node, 0);
                    if (n->symbol) n->symbol->type = t;
                    node->type  = t;
                } else {
                    // "struct foo" as type specifier with a declarator
                    Symbol *s   = find_symbol(node, n->children[0]->val, NS_TAG);
                    node->type  = s->type;
                    node->typetag = n->children[0]->val;
                }
            } else if (n->child_count > 1) {
                // Full struct definition with body
                n->symbol = insert_tag(node, n->children[0]->val);
                if (!n->struct_depth) {
                    fprintf(stderr, "%s creating struct type\n", __func__);
                    Type2 *t    = generate_struct_type2(node, 0);
                    n->symbol->type = t;
                    node->type  = t;
                    node->typetag = n->children[0]->val;
                }
            }
        }
    }

    // Determine base type for declarators
    Type2 *base;
    if (node->typespec & (TB_STRUCT | TB_UNION)) {
        base = (node->type && node->type->base == TB2_STRUCT) ? node->type : t_void;
    } else {
        base = typespec_to_base(node->typespec);
    }

    // Second pass: process every ND_DECLARATOR child
    bool first_decl = true;
    for (int i = 0; i < node->child_count; i++) {
        Node *n = node->children[i];
        if (n->kind != ND_DECLARATOR || depth)
            continue;

        char *ts    = tstr_compact(n);
        char *ident = get_decl_ident(n);
        fprintf(stderr, "%s declarator ts:'%s' ident:'%s'\n",
            __func__, ts, ident ? ident : "");

        Type2 *ty = apply_derivation(base, ts);
        if (first_decl) {
            node->type = ty;
            first_decl = false;
        }
        fprintf(stderr, "%s final type %s\n", __func__, fulltype_str(ty));

        if (ident) {
            n->symbol = insert_ident(node, ty, ident, is_param);

            // For struct-typed variables, ensure symbol points to struct type
            if (!n->is_struct && node->child_count > 0
                && node->children[0]->kind == ND_STRUCT) {
                fprintf(stderr, "%s setting sym type from tag\n", __func__);
                n->symbol->type = ty;  // ty already wraps struct base
            }
        }
    }
}

// ---------------------------------------------------------------
// Symbol table
// ---------------------------------------------------------------
static Symbol_table *find_scope(Node *node)
{
    Scope sc = node->scope;
    fprintf(stderr, "%s scope is %s\n", __func__, scope_str(sc));
    Symbol_table *st = symbol_table;
    if (sc.depth) {
        for (int d = 1; d <= sc.depth; d++) {
            int index = sc.indices[d - 1] - 1;
            st = st->children[index];
        }
    }
    return st;
}

Symbol *find_symbol(Node *n, char *name, Namespace nspace)
{
    Symbol_table *st = find_scope(n);
    Symbol *s;
    bool found = false;
    while (!found) {
        fprintf(stderr, "Searching in scope:%s\n", scope_str(st->scope));
        s =     nspace == NS_IDENT  ? st->idents
            :   nspace == NS_TAG    ? st->tags
            :   nspace == NS_MEMBER ? st->members
            :                         st->labels;
        for (; s; s = s->next) {
            fprintf(stderr, "Name:%s\n", s->name);
            if (!strcmp(name, s->name)) { found = true; break; }
        }
        if (found) break;
        fprintf(stderr, "Not found, going to enclosing scope\n");
        if (st->parent) st = st->parent;
        else break;
    }
    if (!found) {
        print_type_table();
        print_symbol_table(symbol_table, 0);
        error("%s %s not found!\n",
              nspace == NS_IDENT ? "ident" : nspace == NS_TAG ? "tag" :
              nspace == NS_MEMBER ? "member" : "label", name);
    }
    return s;
}

static Symbol_table *new_st_scope()
{
    curr_scope_st->child_count++;
    curr_scope_st->children = (Symbol_table **)realloc(curr_scope_st->children,
                                curr_scope_st->child_count * sizeof(Symbol_table *));
    Symbol_table *nt = calloc(1, sizeof(Symbol_table));
    curr_scope_st->children[curr_scope_st->child_count - 1] = nt;
    Scope *sc   = calloc(1, sizeof(Scope));
    set_scope(sc);
    nt->scope   = *sc;
    nt->parent  = curr_scope_st;
    curr_scope_st = nt;
    return curr_scope_st;
}

Symbol_table *enter_new_scope(bool use_last_scope)
{
    if (use_last_scope) {
        scope_depth++;
        curr_scope_st = last_symbol_table;
        return last_symbol_table;
    }
    scope_indices[scope_depth++]++;
    scope_indices[scope_depth] = 0;
    last_symbol_table = new_st_scope();
    curr_scope_st     = last_symbol_table;
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
    l = sprintf(buf, "%s:%d.",
        sc.scope_type == ST_COMPSTMT ? "compstmt" : "struct", d);
    for (int i = 0; i < d; i++)
        l += sprintf(buf + l, "%d%c", sc.indices[i], i < d - 1 ? '.' : ' ');
    return buf;
}

static Symbol *insert_tag(Node *node, char *ident)
{
    fprintf(stderr, "%s %s\n", __func__, ident);
    Scope sc = node->scope;
    Symbol_table *st = symbol_table;
    if (sc.depth) {
        for (int d = 1; d <= sc.depth; d++) {
            int index = sc.indices[d - 1] - 1;
            st = st->children[index];
        }
    }
    Symbol *s = 0, *ls = 0;
    for (s = st->tags; s; s = s->next) {
        ls = s;
        if (!strcmp(s->name, ident)) {
            fprintf(stderr, "%s tag already exists\n", __func__);
            return s;
        }
    }
    Symbol *sym = new_symbol(t_void, ident, 0);
    if (!ls) st->tags  = sym;
    else     ls->next  = sym;
    return sym;
}

static Symbol *new_symbol(Type2 *type, char *ident, int offset)
{
    Symbol *n   = calloc(1, sizeof(Symbol));
    n->name     = ident;
    n->type     = type;
    n->offset   = offset;
    return n;
}

static Symbol *insert_ident(Node *node, Type2 *type, char *ident, bool is_param)
{
    fprintf(stderr, "%s %s %s\n", __func__, fulltype_str(type), ident);
    Scope sc = node->scope;
    Symbol_table *st = symbol_table;
    int go = 0;

    if (sc.depth) {
        for (int d = 1; d <= sc.depth; d++) {
            if (d > 1) go += st->size;
            int index = sc.indices[d - 1] - 1;
            st = st->children[index];
        }
    } else {
        // Global scope
        int offset = st->size;
        st->size  += type->size;
        Symbol *n  = new_symbol(type, ident, offset);
        Symbol *s = 0, *ls = 0;
        for (s = st->idents; s; s = s->next) ls = s;
        if (!ls) st->idents = n;
        else     ls->next   = n;
        return n;
    }
    st->global_offset = go;

    Symbol *s = 0, *ls = 0;
    int offset       = 0;
    int param_offset = 8;
    int last_size    = 0;
    for (s = st->idents; s; s = s->next) {
        if (s->is_param) {
            param_offset = s->offset;
            last_size    = s->type->size;
        } else {
            offset = s->offset;
        }
        ls = s;
    }
    offset       += type->size;
    param_offset += last_size;
    if (!is_param) st->size += type->size;
    Symbol *n    = new_symbol(type, ident, is_param ? param_offset : offset);
    n->is_param  = is_param;
    if (!ls) st->idents = n;
    else     ls->next   = n;
    return n;
}

// ---------------------------------------------------------------
// fulltype_str
// ---------------------------------------------------------------
static char ft_buf[512];
static char *fts2(Type2 *t, char *p)
{
    if (!t) { p += sprintf(p, "null"); return p; }
    switch (t->base) {
        case TB2_VOID:     p += sprintf(p, "void");   break;
        case TB2_CHAR:     p += sprintf(p, "char");   break;
        case TB2_UCHAR:    p += sprintf(p, "uchar");  break;
        case TB2_SHORT:    p += sprintf(p, "short");  break;
        case TB2_USHORT:   p += sprintf(p, "ushort"); break;
        case TB2_INT:      p += sprintf(p, "int");    break;
        case TB2_UINT:     p += sprintf(p, "uint");   break;
        case TB2_LONG:     p += sprintf(p, "long");   break;
        case TB2_ULONG:    p += sprintf(p, "ulong");  break;
        case TB2_FLOAT:    p += sprintf(p, "float");  break;
        case TB2_DOUBLE:   p += sprintf(p, "double"); break;
        case TB2_POINTER:
            p += sprintf(p, "ptr(");
            p  = fts2(t->u.ptr.pointee, p);
            p += sprintf(p, ")");
            break;
        case TB2_ARRAY:
            p += sprintf(p, "arr[%d](", t->u.arr.count);
            p  = fts2(t->u.arr.elem, p);
            p += sprintf(p, ")");
            break;
        case TB2_FUNCTION:
            p += sprintf(p, "fn(ret=");
            p  = fts2(t->u.fn.ret, p);
            p += sprintf(p, ")");
            break;
        case TB2_STRUCT:
            p += sprintf(p, "struct{%s}",
                t->u.composite.tag ? t->u.composite.tag->name : "anon");
            break;
        case TB2_ENUM:
            p += sprintf(p, "enum");
            break;
        default:
            p += sprintf(p, "???(%d)", t->base);
            break;
    }
    return p;
}

char *fulltype_str(Type2 *t)
{
    fts2(t, ft_buf);
    return ft_buf;
}

// ---------------------------------------------------------------
// Print tables
// ---------------------------------------------------------------
void print_type_table()
{
    fprintf(stderr, "------ Type table ------\n");
    for (Type2 *t = types; t; t = t->next)
        fprintf(stderr, "  %016llx %s sz:%d al:%d\n",
            (unsigned long long)t, fulltype_str(t), t->size, t->align);
}

void print_symbol_table(Symbol_table *s, int depth)
{
    if (depth == 0) fprintf(stderr, "------ Symbol table ------\n");
    for (int i = 0; i < depth; i++) fprintf(stderr, "  ");
    fprintf(stderr, "Scope:%-20s   0x%04x 0x%04x\n",
        scope_str(s->scope), s->size, s->global_offset);
    Symbol *sm;
    for (sm = s->idents; sm; sm = sm->next) {
        for (int i = 0; i < depth; i++) fprintf(stderr, "  ");
        fprintf(stderr, "%016llx %-10s %s 0x%02x %s\n",
            (unsigned long long)sm->type, sm->name,
            sm->is_param ? "param" : depth ? "local" : "global",
            sm->offset, fulltype_str(sm->type));
    }
    for (sm = s->tags; sm; sm = sm->next) {
        for (int i = 0; i < depth; i++) fprintf(stderr, "  ");
        fprintf(stderr, "%016llx %-10s tag %s\n",
            (unsigned long long)sm->type, sm->name, fulltype_str(sm->type));
    }
    for (sm = s->members; sm; sm = sm->next) {
        for (int i = 0; i < depth; i++) fprintf(stderr, "  ");
        fprintf(stderr, "%016llx %-10s 0x%02x %s\n",
            (unsigned long long)sm->type, sm->name,
            sm->offset, fulltype_str(sm->type));
    }
    for (int i = 0; i < s->child_count; i++)
        print_symbol_table(s->children[i], depth + 1);
}

// ---------------------------------------------------------------
// Misc helpers for parser compatibility
// ---------------------------------------------------------------
char *curr_scope_str()
{
    static char buf[1024];
    sprintf(buf, "%d:", scope_depth);
    for (int i = 0; i < scope_depth; i++)
        sprintf(buf + strlen(buf), "%d.", scope_indices[i]);
    return buf;
}

static char sbuf[1024];
static char *typebase_str(Type_base tb)
{
    return
        tb == TB_VOID     ? "void "     :
        tb == TB_CHAR     ? "char "     :
        tb == TB_SHORT    ? "short "    :
        tb == TB_ENUM     ? "enum "     :
        tb == TB_INT      ? "int "      :
        tb == TB_LONG     ? "long "     :
        tb == TB_FLOAT    ? "float "    :
        tb == TB_DOUBLE   ? "double "   :
        tb == TB_SIGNED   ? "signed "   :
        tb == TB_UNSIGNED ? "unsigned " :
        tb == TB_STRUCT   ? "struct "   :
        tb == TB_UNION    ? "union "    :
                            "";
}

char *typespec_str(Type_base tb)
{
    char *p = sbuf;
    sbuf[0] = 0;
    for (int i = TB_UNION; i > 0; i >>= 1) {
        if (tb & i) p = stpcpy(p, typebase_str(tb & i));
    }
    return sbuf;
}

Type_base to_typespec(Token_kind tk)
{
    switch (tk) {
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
