
#include "mycc.h"

/* ======================================================
   Minimum Viable Preprocessor
   ====================================================== */

/* ---- Growing output buffer ---- */
typedef struct
{
    char  *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_grow(Buf *b, size_t need)
{
    if (b->len + need <= b->cap) return;
    size_t new_cap = b->cap ? b->cap * 2 : 256;
    if (new_cap < b->len + need) new_cap = b->len + need + 256;
    b->data = realloc(b->data, new_cap);
    if (!b->data) { fprintf(stderr, "preprocess: out of memory\n"); exit(1); }
    b->cap = new_cap;
}

static void buf_appendc(Buf *b, char c)
{
    buf_grow(b, 1);
    b->data[b->len++] = c;
}

static void buf_append(Buf *b, const char *s, size_t n)
{
    if (!n) return;
    buf_grow(b, n);
    memcpy(b->data + b->len, s, n);
    b->len += n;
}

static void buf_appends(Buf *b, const char *s)
{
    buf_append(b, s, strlen(s));
}

/* ---- Macro table ---- */
#define PP_MAX_MACROS  512
#define PP_MAX_PARAMS   16

typedef struct
{
    char  name[64];
    char *body;
    int   nparams;                   /* -1 = object-like, >=0 = function-like */
    char  params[PP_MAX_PARAMS][64];
    bool  expanding;                 /* anti-infinite-recursion guard */
} PPMacro;

static PPMacro pp_macros[PP_MAX_MACROS];
static int     pp_macro_count;

/* ---- Conditional compilation stack ---- */
#define PP_MAX_COND_DEPTH 64
static int pp_cond_emit [PP_MAX_COND_DEPTH]; /* 1 = this block is emitting */
static int pp_cond_outer[PP_MAX_COND_DEPTH]; /* was the outer scope emitting */
static int pp_cond_depth;

/* ---- Include nesting guard ---- */
#define PP_MAX_INCLUDE_DEPTH 32
static int pp_include_depth;

/* ------------------------------------------------------------------ */

static int currently_emitting(void)
{
    return pp_cond_depth == 0 || pp_cond_emit[pp_cond_depth - 1];
}

static PPMacro *pp_find(const char *name)
{
    for (int i = 0; i < pp_macro_count; i++)
        if (strcmp(pp_macros[i].name, name) == 0)
            return &pp_macros[i];
    return NULL;
}

void reset_preprocessor(void)
{
    for (int i = 0; i < pp_macro_count; i++)
        free(pp_macros[i].body);
    pp_macro_count   = 0;
    pp_cond_depth    = 0;
    pp_include_depth = 0;
}

/* ---- File I/O (local copy keeps read_file in mycc.c static) ---- */
static char *pp_read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) error("Cannot open include file: %s", path);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    if (!buf) error("preprocess: malloc failed");
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* ---- Path resolution: dir(current_file) + include_name ---- */
static char *resolve_include_path(const char *current_file, const char *inc)
{
    const char *slash = strrchr(current_file, '/');
    if (!slash) return strdup(inc);
    size_t dir_len = (size_t)(slash - current_file) + 1;
    char  *result  = malloc(dir_len + strlen(inc) + 1);
    if (!result) error("preprocess: malloc failed");
    memcpy(result, current_file, dir_len);
    strcpy(result + dir_len, inc);
    return result;
}

/* ---- Read one logical line, joining backslash continuations ---- */
static const char *read_logical_line(const char *p, char *line, size_t cap)
{
    size_t n = 0;
    while (*p && *p != '\n')
    {
        if (*p == '\\' && *(p + 1) == '\n') { p += 2; continue; }
        if (n + 1 < cap) line[n++] = *p;
        p++;
    }
    if (*p == '\n') p++;
    line[n] = '\0';
    return p;
}

/* ---- Character helpers ---- */
static bool pp_is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static bool pp_is_ident_cont (char c) { return isalnum((unsigned char)c) || c == '_'; }

static const char *pp_skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Read identifier from p, write into name[cap], return pointer past it. */
static const char *pp_read_ident(const char *p, char *name, size_t cap)
{
    size_t n = 0;
    while (pp_is_ident_cont(*p) && n + 1 < cap) name[n++] = *p++;
    name[n] = '\0';
    return p;
}

/* Forward declaration */
static char *pp_expand(const char *text);

/* ---- Substitute function-like macro params; returns malloc'd string ---- */
static char *substitute_args(PPMacro *m, char **args, int nargs)
{
    Buf out = {0};
    const char *p = m->body;
    while (*p)
    {
        if (pp_is_ident_start(*p))
        {
            char tok[64];
            const char *next = pp_read_ident(p, tok, sizeof(tok));
            int found = -1;
            for (int i = 0; i < m->nparams && i < nargs; i++)
                if (strcmp(m->params[i], tok) == 0) { found = i; break; }
            if (found >= 0)
                buf_appends(&out, args[found]);
            else
                buf_appends(&out, tok);
            p = next;
        }
        else
        {
            buf_appendc(&out, *p++);
        }
    }
    buf_appendc(&out, '\0');
    return out.data;
}

/* ---- Expand macros in text; returns malloc'd null-terminated string ---- */
static char *pp_expand(const char *text)
{
    Buf out = {0};
    const char *p = text;
    while (*p)
    {
        /* String literals: copy verbatim, no macro expansion inside */
        if (*p == '"')
        {
            buf_appendc(&out, *p++);
            while (*p && *p != '"')
            {
                if (*p == '\\' && *(p + 1)) buf_appendc(&out, *p++);
                buf_appendc(&out, *p++);
            }
            if (*p == '"') buf_appendc(&out, *p++);
            continue;
        }
        /* Character literals: copy verbatim */
        if (*p == '\'')
        {
            buf_appendc(&out, *p++);
            while (*p && *p != '\'')
            {
                if (*p == '\\' && *(p + 1)) buf_appendc(&out, *p++);
                buf_appendc(&out, *p++);
            }
            if (*p == '\'') buf_appendc(&out, *p++);
            continue;
        }
        /* Line comments: copy rest verbatim (tokeniser will strip later) */
        if (*p == '/' && *(p + 1) == '/')
        {
            while (*p) buf_appendc(&out, *p++);
            continue;
        }
        /* Potential macro identifier */
        if (pp_is_ident_start(*p))
        {
            char tok[64];
            const char *after = pp_read_ident(p, tok, sizeof(tok));
            PPMacro *m = pp_find(tok);
            if (m && !m->expanding)
            {
                if (m->nparams < 0)
                {
                    /* Object-like macro */
                    m->expanding = true;
                    char *exp = pp_expand(m->body);
                    m->expanding = false;
                    buf_appends(&out, exp);
                    free(exp);
                    p = after;
                }
                else
                {
                    /* Function-like macro: only expand if followed by '(' */
                    const char *q = pp_skip_ws(after);
                    if (*q == '(')
                    {
                        q++; /* skip '(' */
                        char *args[PP_MAX_PARAMS] = {0};
                        int   nargs = 0;
                        q = pp_skip_ws(q);
                        if (*q != ')')
                        {
                            while (1)
                            {
                                Buf arg_buf = {0};
                                int depth = 0;
                                while (*q
                                    && !(*q == ',' && depth == 0)
                                    && !(*q == ')' && depth == 0))
                                {
                                    if      (*q == '(') depth++;
                                    else if (*q == ')') depth--;
                                    buf_appendc(&arg_buf, *q++);
                                }
                                buf_appendc(&arg_buf, '\0');
                                /* Trim surrounding whitespace */
                                char *arg = arg_buf.data;
                                size_t alen = strlen(arg);
                                while (alen > 0 &&
                                       (arg[alen-1] == ' ' || arg[alen-1] == '\t'))
                                    arg[--alen] = '\0';
                                while (*arg == ' ' || *arg == '\t') arg++;
                                if (nargs < PP_MAX_PARAMS)
                                    args[nargs++] = strdup(arg);
                                free(arg_buf.data);
                                if (*q == ')') { q++; break; }
                                if (*q == ',') q++;
                            }
                        }
                        else
                        {
                            q++; /* skip ')' */
                        }
                        char *subst = substitute_args(m, args, nargs);
                        m->expanding = true;
                        char *exp = pp_expand(subst);
                        m->expanding = false;
                        free(subst);
                        buf_appends(&out, exp);
                        free(exp);
                        for (int i = 0; i < nargs; i++) free(args[i]);
                        p = q;
                    }
                    else
                    {
                        /* No '(' — not a call, copy name as-is */
                        buf_appends(&out, tok);
                        p = after;
                    }
                }
            }
            else
            {
                buf_appends(&out, tok);
                p = after;
            }
            continue;
        }
        buf_appendc(&out, *p++);
    }
    buf_appendc(&out, '\0');
    return out.data;
}

/* ------------------------------------------------------------------ */
char *preprocess(const char *src, const char *filename)
{
    if (++pp_include_depth > PP_MAX_INCLUDE_DEPTH)
        error("preprocess: #include nested too deeply");

    Buf        out  = {0};
    const char *p   = src;
    char        line[65536];

    while (*p)
    {
        p = read_logical_line(p, line, sizeof(line));

        const char *lp = pp_skip_ws(line);

        if (*lp == '#')
        {
            lp++; /* skip '#' */
            lp = pp_skip_ws(lp);

            /* Read directive keyword */
            char   dir[32];
            size_t dn = 0;
            while (isalpha((unsigned char)*lp) && dn + 1 < sizeof(dir))
                dir[dn++] = *lp++;
            dir[dn] = '\0';
            lp = pp_skip_ws(lp);

            /* Conditional directives: always processed to maintain stack depth */
            if (strcmp(dir, "ifdef") == 0 || strcmp(dir, "ifndef") == 0)
            {
                char name[64];
                pp_read_ident(lp, name, sizeof(name));
                int outer   = currently_emitting();
                int defined = (pp_find(name) != NULL);
                int cond    = (strcmp(dir, "ifdef") == 0) ? defined : !defined;
                if (pp_cond_depth >= PP_MAX_COND_DEPTH)
                    error("preprocess: #ifdef nested too deeply");
                pp_cond_outer[pp_cond_depth] = outer;
                pp_cond_emit [pp_cond_depth] = outer && cond;
                pp_cond_depth++;
            }
            else if (strcmp(dir, "else") == 0)
            {
                if (pp_cond_depth == 0)
                    error("preprocess: #else without matching #ifdef");
                int outer = pp_cond_outer[pp_cond_depth - 1];
                int was   = pp_cond_emit [pp_cond_depth - 1];
                pp_cond_emit[pp_cond_depth - 1] = outer && !was;
            }
            else if (strcmp(dir, "endif") == 0)
            {
                if (pp_cond_depth == 0)
                    error("preprocess: #endif without matching #ifdef");
                pp_cond_depth--;
            }
            /* Other directives: only processed when emitting */
            else if (currently_emitting())
            {
                if (strcmp(dir, "define") == 0)
                {
                    if (pp_macro_count >= PP_MAX_MACROS)
                        error("preprocess: too many macros");
                    char name[64];
                    const char *after_name = pp_read_ident(lp, name, sizeof(name));
                    PPMacro *m = &pp_macros[pp_macro_count++];
                    strncpy(m->name, name, 63);
                    m->name[63]  = '\0';
                    m->expanding = false;

                    if (*after_name == '(')
                    {
                        /* Function-like: NAME( immediately, no space */
                        const char *q = after_name + 1;
                        m->nparams = 0;
                        q = pp_skip_ws(q);
                        if (*q != ')')
                        {
                            while (1)
                            {
                                q = pp_skip_ws(q);
                                char pname[64];
                                q = pp_read_ident(q, pname, sizeof(pname));
                                if (m->nparams < PP_MAX_PARAMS)
                                {
                                    strncpy(m->params[m->nparams], pname, 63);
                                    m->params[m->nparams][63] = '\0';
                                    m->nparams++;
                                }
                                q = pp_skip_ws(q);
                                if (*q == ')') { q++; break; }
                                if (*q == ',') q++;
                            }
                        }
                        else
                        {
                            q++; /* skip ')' */
                        }
                        lp = pp_skip_ws(q);
                    }
                    else
                    {
                        /* Object-like */
                        m->nparams = -1;
                        lp = pp_skip_ws(after_name);
                    }
                    m->body = strdup(lp);
                }
                else if (strcmp(dir, "undef") == 0)
                {
                    char name[64];
                    pp_read_ident(lp, name, sizeof(name));
                    for (int i = 0; i < pp_macro_count; i++)
                    {
                        if (strcmp(pp_macros[i].name, name) == 0)
                        {
                            free(pp_macros[i].body);
                            pp_macros[i] = pp_macros[--pp_macro_count];
                            break;
                        }
                    }
                }
                else if (strcmp(dir, "include") == 0)
                {
                    if (*lp != '"')
                        error("preprocess: only #include \"file\" is supported");
                    lp++; /* skip opening '"' */
                    char   inc_name[256];
                    size_t n = 0;
                    while (*lp && *lp != '"' && n + 1 < sizeof(inc_name))
                        inc_name[n++] = *lp++;
                    inc_name[n] = '\0';

                    char *resolved = resolve_include_path(filename, inc_name);
                    char *raw      = pp_read_file(resolved);
                    char *expanded = preprocess(raw, resolved);
                    buf_appends(&out, expanded);
                    /* Ensure included content ends with a newline */
                    if (out.len > 0 && out.data[out.len - 1] != '\n')
                        buf_appendc(&out, '\n');
                    free(expanded);
                    free(raw);
                    free(resolved);
                }
                /* Unknown directives are silently ignored */
            }
        }
        else if (currently_emitting())
        {
            char *expanded = pp_expand(line);
            buf_appends(&out, expanded);
            free(expanded);
            buf_appendc(&out, '\n');
        }
    }

    pp_include_depth--;
    buf_appendc(&out, '\0');
    return out.data;
}
