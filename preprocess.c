
#include "smallcc.h"

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

/* Null-terminate b, copy to arena, free the malloc'd backing store. */
static char *buf_finish(Buf *b)
{
    buf_appendc(b, '\0');
    char *r = arena_strdup(b->data);
    free(b->data);
    b->data = NULL; b->len = b->cap = 0;
    return r;
}

/* Emit a GCC-style linemarker: # <line> "<filename>"\n */
static void buf_emit_linemarker(Buf *b, int line, const char *filename)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "# %d \"%s\"\n", line, filename);
    buf_appends(b, tmp);
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
static int pp_cond_done [PP_MAX_COND_DEPTH]; /* 1 = some branch was taken */
static int pp_cond_depth;

/* ---- Include nesting guard ---- */
#define PP_MAX_INCLUDE_DEPTH 32
static int pp_include_depth;

/* ---- System include directory (for <> includes) ---- */
static const char *pp_sysinclude_dir = NULL;

void set_include_dir(const char *dir) { pp_sysinclude_dir = dir; }

/* ---- User include directories (for -I, searched for both "" and <> includes) ---- */
#define PP_MAX_IDIRS 64
static const char *pp_idirs[PP_MAX_IDIRS];
static int         pp_idir_count = 0;

void add_include_dir(const char *dir)
{
    if (pp_idir_count < PP_MAX_IDIRS)
        pp_idirs[pp_idir_count++] = dir;
    else
        error("too many include directories (max %d)", PP_MAX_IDIRS);
}

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
    pp_macro_count   = 0;
    pp_cond_depth    = 0;
    pp_include_depth = 0;
}

void pp_define(const char *name, const char *body)
{
    if (pp_macro_count >= PP_MAX_MACROS)
        error("preprocess: too many macros");
    PPMacro *m = &pp_macros[pp_macro_count++];
    strncpy(m->name, name, 63);
    m->name[63]  = '\0';
    m->nparams   = -1;   /* object-like */
    m->expanding = false;
    m->body      = arena_strdup(body);
}

/* ---- File I/O (local copy keeps read_file in smallcc.c static) ---- */
static char *pp_read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) error("Cannot open include file: %s", path);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = arena_alloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* Forward declaration */
static char *resolve_include_path(const char *current_file, const char *inc);

/* Returns arena-allocated path if accessible, NULL otherwise. */
static char *pp_try_path(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fclose(f);
    return arena_strdup(path);
}

/* Build "dir/name" in an arena buffer. */
static char *pp_join(const char *dir, const char *name)
{
    size_t dlen = strlen(dir), nlen = strlen(name);
    char *p = arena_alloc(dlen + 1 + nlen + 1);
    memcpy(p, dir, dlen);
    p[dlen] = '/';
    memcpy(p + dlen + 1, name, nlen + 1);
    return p;
}

/* Resolve #include "name": file-relative first, then -I dirs.
   Returns arena-allocated resolved path, or calls error. */
static char *pp_resolve_quoted(const char *filename, const char *name)
{
    /* 1. Directory of the including file */
    char *r = resolve_include_path(filename, name);
    char *found = pp_try_path(r);
    if (found) return found;

    /* 2. -I directories */
    for (int i = 0; i < pp_idir_count; i++) {
        r = pp_join(pp_idirs[i], name);
        found = pp_try_path(r);
        if (found) return found;
    }

    /* Not found — report the first candidate for a helpful message */
    char *first = resolve_include_path(filename, name);
    error("Cannot open include file: %s", first);
    return NULL; /* unreachable */
}

/* Resolve #include <name>: -I dirs first, then system include dir.
   Returns arena-allocated resolved path, or calls error. */
static char *pp_resolve_angled(const char *name)
{
    /* 1. -I directories */
    for (int i = 0; i < pp_idir_count; i++) {
        char *r = pp_join(pp_idirs[i], name);
        char *found = pp_try_path(r);
        if (found) return found;
    }

    /* 2. System include directory */
    if (!pp_sysinclude_dir)
        error("preprocess: no system include directory set for <%s>", name);
    char *r = pp_join(pp_sysinclude_dir, name);
    if (pp_try_path(r)) return r;
    error("Cannot open include file: <%s>", name);
    return NULL; /* unreachable */
}

/* ---- Path resolution: dir(current_file) + include_name ---- */
static char *resolve_include_path(const char *current_file, const char *inc)
{
    const char *slash = strrchr(current_file, '/');
    if (!slash) return arena_strdup(inc);
    size_t dir_len = (size_t)(slash - current_file) + 1;
    char  *result  = arena_alloc(dir_len + strlen(inc) + 1);
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

/* ---- Substitute function-like macro params; returns arena-allocated string ---- */
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
    return buf_finish(&out);
}

/* ---- Expand macros in text; returns arena-allocated null-terminated string ---- */
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
                                    args[nargs++] = arena_strdup(arg);
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
                        buf_appends(&out, exp);
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
    return buf_finish(&out);
}

/* ---- Integer constant expression evaluator for #if / #elif ---- */

static const char *eval_p;

static void eval_skip_ws(void)
{
    while (*eval_p == ' ' || *eval_p == '\t') eval_p++;
}

static long long pp_eval_expr(void);   /* forward */

static long long pp_eval_primary(void)
{
    eval_skip_ws();
    /* Parenthesised sub-expression */
    if (*eval_p == '(')
    {
        eval_p++;
        long long v = pp_eval_expr();
        eval_skip_ws();
        if (*eval_p == ')') eval_p++;
        return v;
    }
    /* Integer literal */
    if (isdigit((unsigned char)*eval_p))
    {
        char *end;
        long long v = strtoll(eval_p, &end, 0);
        eval_p = end;
        /* skip u/l/U/L suffixes */
        while (*eval_p == 'u' || *eval_p == 'U' ||
               *eval_p == 'l' || *eval_p == 'L') eval_p++;
        return v;
    }
    /* Identifier: defined(...) or defined NAME, or unknown→0 */
    if (pp_is_ident_start(*eval_p))
    {
        char name[64];
        eval_p = pp_read_ident(eval_p, name, sizeof(name));
        if (strcmp(name, "defined") == 0)
        {
            eval_skip_ws();
            int paren = (*eval_p == '(');
            if (paren) eval_p++;
            eval_skip_ws();
            char mname[64];
            eval_p = pp_read_ident(eval_p, mname, sizeof(mname));
            eval_skip_ws();
            if (paren && *eval_p == ')') eval_p++;
            return (pp_find(mname) != NULL) ? 1 : 0;
        }
        /* Any other identifier after macro expansion → treat as 0 */
        return 0;
    }
    /* Unexpected character — advance past it and return 0 */
    if (*eval_p) eval_p++;
    return 0;
}

static long long pp_eval_unary(void)
{
    eval_skip_ws();
    if (*eval_p == '!')  { eval_p++; return !pp_eval_unary(); }
    if (*eval_p == '~')  { eval_p++; return ~pp_eval_unary(); }
    if (*eval_p == '-')  { eval_p++; return -pp_eval_unary(); }
    if (*eval_p == '+')  { eval_p++; return +pp_eval_unary(); }
    return pp_eval_primary();
}

static long long pp_eval_mul(void)
{
    long long v = pp_eval_unary();
    for (;;)
    {
        eval_skip_ws();
        if (*eval_p == '*') { eval_p++; v *= pp_eval_unary(); }
        else if (*eval_p == '/' && *(eval_p+1) != '/') {
            eval_p++;
            long long r = pp_eval_unary();
            v = r ? v / r : 0;
        }
        else if (*eval_p == '%') {
            eval_p++;
            long long r = pp_eval_unary();
            v = r ? v % r : 0;
        }
        else break;
    }
    return v;
}

static long long pp_eval_add(void)
{
    long long v = pp_eval_mul();
    for (;;)
    {
        eval_skip_ws();
        if      (*eval_p == '+') { eval_p++; v += pp_eval_mul(); }
        else if (*eval_p == '-') { eval_p++; v -= pp_eval_mul(); }
        else break;
    }
    return v;
}

static long long pp_eval_shift(void)
{
    long long v = pp_eval_add();
    for (;;)
    {
        eval_skip_ws();
        if      (eval_p[0]=='<' && eval_p[1]=='<') { eval_p+=2; v <<= pp_eval_add(); }
        else if (eval_p[0]=='>' && eval_p[1]=='>') { eval_p+=2; v >>= pp_eval_add(); }
        else break;
    }
    return v;
}

static long long pp_eval_relat(void)
{
    long long v = pp_eval_shift();
    for (;;)
    {
        eval_skip_ws();
        if      (eval_p[0]=='<' && eval_p[1]=='=') { eval_p+=2; v = v <= pp_eval_shift(); }
        else if (eval_p[0]=='>' && eval_p[1]=='=') { eval_p+=2; v = v >= pp_eval_shift(); }
        else if (*eval_p=='<' && eval_p[1]!='<')   { eval_p++;  v = v <  pp_eval_shift(); }
        else if (*eval_p=='>' && eval_p[1]!='>')   { eval_p++;  v = v >  pp_eval_shift(); }
        else break;
    }
    return v;
}

static long long pp_eval_equal(void)
{
    long long v = pp_eval_relat();
    for (;;)
    {
        eval_skip_ws();
        if      (eval_p[0]=='=' && eval_p[1]=='=') { eval_p+=2; v = v == pp_eval_relat(); }
        else if (eval_p[0]=='!' && eval_p[1]=='=') { eval_p+=2; v = v != pp_eval_relat(); }
        else break;
    }
    return v;
}

static long long pp_eval_bitand(void)
{
    long long v = pp_eval_equal();
    eval_skip_ws();
    while (*eval_p == '&' && eval_p[1] != '&')
    {
        eval_p++;
        v &= pp_eval_equal();
        eval_skip_ws();
    }
    return v;
}

static long long pp_eval_bitxor(void)
{
    long long v = pp_eval_bitand();
    eval_skip_ws();
    while (*eval_p == '^')
    {
        eval_p++;
        v ^= pp_eval_bitand();
        eval_skip_ws();
    }
    return v;
}

static long long pp_eval_bitor(void)
{
    long long v = pp_eval_bitxor();
    eval_skip_ws();
    while (*eval_p == '|' && eval_p[1] != '|')
    {
        eval_p++;
        v |= pp_eval_bitxor();
        eval_skip_ws();
    }
    return v;
}

static long long pp_eval_logand(void)
{
    long long v = pp_eval_bitor();
    eval_skip_ws();
    while (eval_p[0] == '&' && eval_p[1] == '&')
    {
        eval_p += 2;
        long long r = pp_eval_bitor();
        v = v && r;
        eval_skip_ws();
    }
    return v;
}

static long long pp_eval_logor(void)
{
    long long v = pp_eval_logand();
    eval_skip_ws();
    while (eval_p[0] == '|' && eval_p[1] == '|')
    {
        eval_p += 2;
        long long r = pp_eval_logand();
        v = v || r;
        eval_skip_ws();
    }
    return v;
}

static long long pp_eval_expr(void)
{
    return pp_eval_logor();
}

/* Replace defined(NAME) / defined NAME with "1" or "0" before macro expansion.
   Returns an arena-allocated string with all defined(...) occurrences substituted. */
static char *pp_subst_defined(const char *text)
{
    Buf out = {0};
    const char *p = text;
    while (*p)
    {
        /* Check for 'defined' keyword */
        if (pp_is_ident_start(*p))
        {
            char tok[64];
            const char *after = pp_read_ident(p, tok, sizeof(tok));
            if (strcmp(tok, "defined") == 0)
            {
                const char *q = pp_skip_ws(after);
                int paren = (*q == '(');
                if (paren) q++;
                q = pp_skip_ws(q);
                char mname[64];
                q = pp_read_ident(q, mname, sizeof(mname));
                q = pp_skip_ws(q);
                if (paren && *q == ')') q++;
                buf_appendc(&out, (pp_find(mname) != NULL) ? '1' : '0');
                p = q;
                continue;
            }
            buf_appends(&out, tok);
            p = after;
            continue;
        }
        buf_appendc(&out, *p++);
    }
    return buf_finish(&out);
}

/* Strip C block comments from a string; returns arena-allocated result. */
static char *pp_strip_block_comments(const char *text)
{
    Buf out = {0};
    const char *p = text;
    while (*p)
    {
        if (p[0] == '/' && p[1] == '*')
        {
            p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/')) p++;
            if (*p) p += 2;
            buf_appendc(&out, ' ');  /* replace comment with a space */
        }
        else
            buf_appendc(&out, *p++);
    }
    return buf_finish(&out);
}

/* Macro-expand text then evaluate as a constant integer expression. */
static long long pp_eval_if_expr(const char *text)
{
    char *presubst = pp_subst_defined(text);  /* handle defined() first */
    char *expanded = pp_expand(presubst);
    char *stripped = pp_strip_block_comments(expanded);
    eval_p = stripped;
    return pp_eval_logor();
}

/* ------------------------------------------------------------------ */
char *preprocess(const char *src, const char *filename)
{
    if (++pp_include_depth > PP_MAX_INCLUDE_DEPTH)
        error("preprocess: #include nested too deeply");

    Buf        out  = {0};
    const char *p   = src;
    char        line[65536];
    int         pp_lineno = 0;

    /* Emit initial linemarker so the tokeniser knows which file this is */
    buf_emit_linemarker(&out, 1, filename);

    while (*p)
    {
        pp_lineno++;
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

            bool need_resync = false;
            bool is_include  = false;

            /* Conditional directives: always processed to maintain stack depth */
            if (strcmp(dir, "ifdef") == 0 || strcmp(dir, "ifndef") == 0)
            {
                char name[64];
                pp_read_ident(lp, name, sizeof(name));
                int outer   = currently_emitting();
                int defined = (pp_find(name) != NULL);
                int cond    = (strcmp(dir, "ifdef") == 0) ? defined : !defined;
                int taken   = outer && cond;
                if (pp_cond_depth >= PP_MAX_COND_DEPTH)
                    error("preprocess: #ifdef nested too deeply");
                pp_cond_outer[pp_cond_depth] = outer;
                pp_cond_emit [pp_cond_depth] = taken;
                pp_cond_done [pp_cond_depth] = taken;
                pp_cond_depth++;
                need_resync = currently_emitting();
            }
            else if (strcmp(dir, "if") == 0)
            {
                int outer = currently_emitting();
                int cond  = 0;
                if (outer)
                    cond = (pp_eval_if_expr(lp) != 0);
                int taken = outer && cond;
                if (pp_cond_depth >= PP_MAX_COND_DEPTH)
                    error("preprocess: #if nested too deeply");
                pp_cond_outer[pp_cond_depth] = outer;
                pp_cond_emit [pp_cond_depth] = taken;
                pp_cond_done [pp_cond_depth] = taken;
                pp_cond_depth++;
                need_resync = currently_emitting();
            }
            else if (strcmp(dir, "elif") == 0)
            {
                if (pp_cond_depth == 0)
                    error("preprocess: #elif without matching #if");
                int outer = pp_cond_outer[pp_cond_depth - 1];
                int done  = pp_cond_done [pp_cond_depth - 1];
                int cond  = 0;
                if (outer && !done)
                    cond = (pp_eval_if_expr(lp) != 0);
                int taken = outer && !done && cond;
                pp_cond_emit[pp_cond_depth - 1] = taken;
                if (taken) pp_cond_done[pp_cond_depth - 1] = 1;
                need_resync = currently_emitting();
            }
            else if (strcmp(dir, "else") == 0)
            {
                if (pp_cond_depth == 0)
                    error("preprocess: #else without matching #ifdef");
                int outer = pp_cond_outer[pp_cond_depth - 1];
                int done  = pp_cond_done [pp_cond_depth - 1];
                pp_cond_emit[pp_cond_depth - 1] = outer && !done;
                need_resync = currently_emitting();
            }
            else if (strcmp(dir, "endif") == 0)
            {
                if (pp_cond_depth == 0)
                    error("preprocess: #endif without matching #ifdef");
                pp_cond_depth--;
                need_resync = currently_emitting();
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
                                else
                                    error("macro '%s' has too many parameters (max %d)", m->name, PP_MAX_PARAMS);
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
                    m->body = arena_strdup(lp);
                }
                else if (strcmp(dir, "undef") == 0)
                {
                    char name[64];
                    pp_read_ident(lp, name, sizeof(name));
                    for (int i = 0; i < pp_macro_count; i++)
                    {
                        if (strcmp(pp_macros[i].name, name) == 0)
                        {
                            pp_macros[i] = pp_macros[--pp_macro_count];
                            break;
                        }
                    }
                }
                else if (strcmp(dir, "include") == 0)
                {
                    is_include = true;
                    char   inc_name[256];
                    size_t n = 0;
                    char  *resolved = NULL;

                    if (*lp == '"')
                    {
                        lp++; /* skip opening '"' */
                        while (*lp && *lp != '"' && n + 1 < sizeof(inc_name))
                            inc_name[n++] = *lp++;
                        inc_name[n] = '\0';
                        resolved = pp_resolve_quoted(filename, inc_name);
                    }
                    else if (*lp == '<')
                    {
                        lp++; /* skip '<' */
                        while (*lp && *lp != '>' && n + 1 < sizeof(inc_name))
                            inc_name[n++] = *lp++;
                        inc_name[n] = '\0';
                        resolved = pp_resolve_angled(inc_name);
                    }
                    else
                    {
                        error("preprocess: #include requires \"file\" or <file>");
                    }

                    char *raw      = pp_read_file(resolved);
                    char *expanded = preprocess(raw, resolved);
                    buf_appends(&out, expanded);
                    /* Ensure included content ends with a newline */
                    if (out.len > 0 && out.data[out.len - 1] != '\n')
                        buf_appendc(&out, '\n');
                    /* Resync to parent file after the included content */
                    buf_emit_linemarker(&out, pp_lineno + 1, filename);
                }
                else if (strcmp(dir, "error") == 0)
                {
                    error("%s:%d: #error %s", filename, pp_lineno, lp);
                }
                /* Unknown directives are silently ignored */

                /* For non-include directives, resync so the tokeniser stays in step */
                if (!is_include)
                    need_resync = true;
            }

            if (need_resync)
                buf_emit_linemarker(&out, pp_lineno + 1, filename);
        }
        else if (currently_emitting())
        {
            char *expanded = pp_expand(line);
            buf_appends(&out, expanded);
            buf_appendc(&out, '\n');
        }
    }

    pp_include_depth--;
    return buf_finish(&out);
}
