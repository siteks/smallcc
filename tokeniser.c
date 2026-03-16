
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "smallcc.h"

// Tokenizer context instance
TokenContext token_ctx;

struct Keyword keywords[] =
{
    "auto",     TK_AUTO,
    "break",    TK_BREAK,
    "case",     TK_CASE,
    "char",     TK_CHAR,
    "const",    TK_CONST,
    "continue", TK_CONTINUE,
    "default",  TK_DEFAULT,
    "do",       TK_DO,
    "double",   TK_DOUBLE,
    "else",     TK_ELSE,
    "enum",     TK_ENUM,
    "extern",   TK_EXTERN,
    "float",    TK_FLOAT,
    "for",      TK_FOR,
    "goto",     TK_GOTO,
    "if",       TK_IF,
    "int",      TK_INT,
    "long",     TK_LONG,
    "register", TK_REGISTER,
    "return",   TK_RETURN,
    "short",    TK_SHORT,
    "signed",   TK_SIGNED,
    "sizeof",   TK_SIZEOF,
    "static",   TK_STATIC,
    "struct",   TK_STRUCT,
    "switch",   TK_SWITCH,
    "typedef",  TK_TYPEDEF,
    "union",    TK_UNION,
    "unsigned", TK_UNSIGNED,
    "va_arg",   TK_VA_ARG,
    "va_end",   TK_VA_END,
    "va_start", TK_VA_START,
    "void",     TK_VOID,
    "volatile", TK_VOLATILE,
    "while",    TK_WHILE,
    "",         TK_INVALID,
};

// All punctuator/operator tokens ordered longest-first for greedy matching.
// A single loop over this table replaces the strncmp cascade and the switch.
typedef struct { const char *str; int len; Token_kind kind; } PunctEntry;
#define PE(s, k) { s, sizeof(s)-1, k }
static const PunctEntry punct_table[] = {
    // 3-character
    PE("<<=", TK_SHIFTL_ASSIGN), PE(">>=", TK_SHIFTR_ASSIGN), PE("...", TK_ELLIPSIS),
    // 2-character
    PE("==", TK_EQ),   PE("!=", TK_NE),   PE(">=", TK_GE),   PE("<=", TK_LE),
    PE("++", TK_INC),  PE("--", TK_DEC),  PE("&&", TK_LOGAND), PE("||", TK_LOGOR),
    PE(">>", TK_SHIFTR), PE("<<", TK_SHIFTL), PE("->", TK_ARROW),
    PE("+=", TK_PLUS_ASSIGN),   PE("-=", TK_MINUS_ASSIGN), PE("*=", TK_STAR_ASSIGN),
    PE("/=", TK_SLASH_ASSIGN),  PE("&=", TK_AMP_ASSIGN),   PE("|=", TK_BITOR_ASSIGN),
    PE("^=", TK_BITXOR_ASSIGN), PE("%=", TK_PERCENT_ASSIGN),
    // 1-character
    PE("+", TK_PLUS),    PE("-", TK_MINUS),   PE("*", TK_STAR),    PE("/", TK_SLASH),
    PE("(", TK_LPAREN),  PE(")", TK_RPAREN),  PE("[", TK_LBRACKET), PE("]", TK_RBRACKET),
    PE(">", TK_GT),      PE("<", TK_LT),      PE("=", TK_ASSIGN),  PE(";", TK_SEMICOLON),
    PE(":", TK_COLON),   PE("{", TK_LBRACE),  PE("}", TK_RBRACE),  PE("&", TK_AMPERSAND),
    PE("~", TK_TILDE),   PE("!", TK_BANG),    PE(",", TK_COMMA),   PE(".", TK_DOT),
    PE("|", TK_BITOR),   PE("^", TK_BITXOR),  PE("%", TK_PERCENT), PE("?", TK_QUESTION),
    { NULL, 0, TK_INVALID },
};
#undef PE

// Direct-indexed names for every Token_kind value.
static const char *const token_names[] = {
    [TK_EMPTY]          = "EMPTY",
    [TK_IDENT]          = "IDENT",
    [TK_CONSTFLT]       = "CONSTFLT",
    [TK_CONSTINT]       = "CONSTINT",
    [TK_CHARACTER]      = "CHARACTER",
    [TK_STRING]         = "STRING",
    [TK_LPAREN]         = "LPAREN",
    [TK_RPAREN]         = "RPAREN",
    [TK_LBRACE]         = "LBRACE",
    [TK_RBRACE]         = "RBRACE",
    [TK_LBRACKET]       = "LBRACKET",
    [TK_RBRACKET]       = "RBRACKET",
    [TK_COMMA]          = "COMMA",
    [TK_SEMICOLON]      = "SEMICOLON",
    [TK_COLON]          = "COLON",
    [TK_EQ]             = "==",
    [TK_NE]             = "!=",
    [TK_GE]             = ">=",
    [TK_GT]             = ">",
    [TK_LE]             = "<=",
    [TK_LT]             = "<",
    [TK_SHIFTR]         = ">>",
    [TK_SHIFTL]         = "<<",
    [TK_ASSIGN]         = "=",
    [TK_PLUS]           = "+",
    [TK_MINUS]          = "-",
    [TK_STAR]           = "*",
    [TK_SLASH]          = "/",
    [TK_AMPERSAND]      = "&",
    [TK_TILDE]          = "~",
    [TK_BANG]           = "!",
    [TK_EOF]            = "EOF",
    [TK_INC]            = "++",
    [TK_DEC]            = "--",
    [TK_DOT]            = ".",
    [TK_ARROW]          = "->",
    [TK_LOGAND]         = "&&",
    [TK_LOGOR]          = "||",
    [TK_BITOR]          = "|",
    [TK_BITXOR]         = "^",
    [TK_PLUS_ASSIGN]    = "+=",
    [TK_MINUS_ASSIGN]   = "-=",
    [TK_STAR_ASSIGN]    = "*=",
    [TK_SLASH_ASSIGN]   = "/=",
    [TK_AMP_ASSIGN]     = "&=",
    [TK_BITOR_ASSIGN]   = "|=",
    [TK_BITXOR_ASSIGN]  = "^=",
    [TK_SHIFTL_ASSIGN]  = "<<=",
    [TK_SHIFTR_ASSIGN]  = ">>=",
    [TK_PERCENT]        = "%",
    [TK_QUESTION]       = "?",
    [TK_PERCENT_ASSIGN] = "%=",
    [TK_AUTO]           = "auto",
    [TK_BREAK]          = "break",
    [TK_CASE]           = "case",
    [TK_CONST]          = "const",
    [TK_CONTINUE]       = "continue",
    [TK_DEFAULT]        = "default",
    [TK_DO]             = "do",
    [TK_ELSE]           = "else",
    [TK_EXTERN]         = "extern",
    [TK_FOR]            = "for",
    [TK_GOTO]           = "goto",
    [TK_IF]             = "if",
    [TK_REGISTER]       = "register",
    [TK_RETURN]         = "return",
    [TK_SIZEOF]         = "sizeof",
    [TK_STATIC]         = "static",
    [TK_SWITCH]         = "switch",
    [TK_VOLATILE]       = "volatile",
    [TK_WHILE]          = "while",
    [TK_VOID]           = "void",
    [TK_CHAR]           = "char",
    [TK_SHORT]          = "short",
    [TK_INT]            = "int",
    [TK_LONG]           = "long",
    [TK_FLOAT]          = "float",
    [TK_DOUBLE]         = "double",
    [TK_SIGNED]         = "signed",
    [TK_UNSIGNED]       = "unsigned",
    [TK_STRUCT]         = "struct",
    [TK_UNION]          = "union",
    [TK_ENUM]           = "enum",
    [TK_TYPEDEF]        = "typedef",
    [TK_INVALID]        = "INVALID",
    [TK_ELLIPSIS]       = "...",
    [TK_VA_START]       = "va_start",
    [TK_VA_ARG]         = "va_arg",
    [TK_VA_END]         = "va_end",
    [TK_POST_INC]       = "post++",
    [TK_POST_DEC]       = "post--",
};

const char *token_str(Token_kind tk)
{
    if ((unsigned)tk < sizeof(token_names)/sizeof(token_names[0]) && token_names[tk])
        return token_names[tk];
    return "UNKNOWN";
}

// Bool table: true for tokens that name a type specifier or qualifier.
static const bool is_type_tok[TK_ENUM + 1] = {
    [TK_CONST]    = true, [TK_VOLATILE] = true,
    [TK_STRUCT]   = true, [TK_UNION]    = true, [TK_ENUM]   = true,
    [TK_VOID]     = true, [TK_CHAR]     = true, [TK_SHORT]  = true,
    [TK_INT]      = true, [TK_LONG]     = true, [TK_FLOAT]  = true,
    [TK_DOUBLE]   = true, [TK_UNSIGNED] = true, [TK_SIGNED] = true,
};

bool is_type_name(Token_kind tk)
{
    return (unsigned)tk < sizeof(is_type_tok)/sizeof(is_type_tok[0]) && is_type_tok[tk];
}

char *expect(Token_kind tk)
{
    DBG_PRINT("%s %s\n", __func__, token_ctx.current->val);
    if (token_ctx.current->kind == tk)
    {
        token_ctx.last_line = token_ctx.current->line;
        token_ctx.last_col  = token_ctx.current->col;
        char *val           = token_ctx.current->val;
        token_ctx.current   = token_ctx.current->next;
        return val;
    }
    else
    {
        src_error(token_ctx.current->line, token_ctx.current->col,
                  "Expected '%s' got '%s'", token_str(tk), token_str(token_ctx.current->kind));
    }
    return 0;
}


bool at_eof()
{
    return token_ctx.current->kind == TK_EOF;
}

int expect_number()
{
    if (token_ctx.current->kind == TK_CONSTINT)
    {
        token_ctx.last_line = token_ctx.current->line;
        token_ctx.last_col  = token_ctx.current->col;
        int val           = (int)token_ctx.current->ival;
        token_ctx.current = token_ctx.current->next;
        return val;
    }
    src_error(token_ctx.current->line, token_ctx.current->col,
              "Expected integer constant, got '%s'", token_ctx.current->val);
    return 0;
}

char *expect_ident()
{
    if (token_ctx.current->kind == TK_IDENT)
    {
        token_ctx.last_line = token_ctx.current->line;
        token_ctx.last_col  = token_ctx.current->col;
        char *val         = token_ctx.current->val;
        token_ctx.current = token_ctx.current->next;
        return val;
    }
    src_error(token_ctx.current->line, token_ctx.current->col,
              "Expected identifier, got '%s'", token_ctx.current->val);
    return 0;
}

Token *new_token(Token_kind kind, Token *cur, char *str, int len)
{
    Token *tok  = arena_alloc(sizeof(Token));
    tok->kind   = kind;
    tok->val    = arena_alloc(len + 1);
    tok->loc    = str - token_ctx.user_input;
    if (len)
        memcpy(tok->val, str, len);
    cur->next   = tok;
    return tok;
}

Token_kind find_token(char *str, int l)
{
    for (int i = 0; keywords[i].kind != TK_INVALID; i++)
    {
        if ((int)strlen(keywords[i].keyword) == l && !strncmp(str, keywords[i].keyword, l))
            return keywords[i].kind;
    }
    return TK_IDENT;
}

// Decode one character (possibly an escape sequence) at *src.
// Writes the decoded byte to *out and returns number of source chars consumed.
static int decode_string_char(const char *src, char *out)
{
    if (*src != '\\') { *out = *src; return 1; }
    src++;
    switch (*src)
    {
        case 'a':  *out = '\a'; return 2;
        case 'b':  *out = '\b'; return 2;
        case 'f':  *out = '\f'; return 2;
        case 'n':  *out = '\n'; return 2;
        case 'r':  *out = '\r'; return 2;
        case 't':  *out = '\t'; return 2;
        case 'v':  *out = '\v'; return 2;
        case '\\': *out = '\\'; return 2;
        case '?':  *out = '?';  return 2;
        case '\'': *out = '\''; return 2;
        case '"':  *out = '"';  return 2;
        case 'x':
        {
            char *q;
            *out = (char)strtol(src + 1, &q, 16);
            return 2 + (int)(q - (src + 1));
        }
        default:
            if (*src >= '0' && *src <= '7')
            {
                char *q;
                *out = (char)strtol(src, &q, 8);
                return 1 + (int)(q - src);
            }
            fprintf(stderr, "warning: unrecognized escape sequence '\\%c'\n", *src);
            *out = *src;
            return 2;
    }
}

Token *tokenise(char *p)
{
    Token head;
    head.next   = NULL;
    Token *cur  = &head;

    int cur_line = 1, cur_col = 1;
    const char *cur_file = token_ctx.filename ? token_ctx.filename : "?";
    token_ctx.logical_filename = cur_file;

    while (*p)
    {
        if (isspace(*p))
        {
            if (*p == '\n') { cur_line++; cur_col = 1; }
            else cur_col++;
            p++;
            continue;
        }

        // Linemarker: # <number> "filename"  (emitted by preprocessor, always at col 1)
        if (*p == '#' && cur_col == 1)
        {
            const char *q = p + 1;
            while (*q == ' ' || *q == '\t') q++;
            if (isdigit((unsigned char)*q))
            {
                char *end;
                int lm_line = (int)strtol(q, &end, 10);
                q = end;
                while (*q == ' ' || *q == '\t') q++;
                if (*q == '"')
                {
                    q++;
                    const char *fn_start = q;
                    while (*q && *q != '"' && *q != '\n') q++;
                    int fn_len = (int)(q - fn_start);
                    char *fn = arena_alloc(fn_len + 1);
                    memcpy(fn, fn_start, fn_len);
                    fn[fn_len] = '\0';
                    cur_file = fn;
                    token_ctx.logical_filename = fn;
                }
                cur_line = lm_line;
                // Skip to end of marker line (consuming the '\n' without incrementing cur_line)
                while (*p && *p != '\n') p++;
                if (*p == '\n') { p++; cur_col = 1; }
                continue;
            }
        }

        // Line comments: skip to end of line
        if (p[0] == '/' && p[1] == '/')
        {
            cur_col += 2; p += 2;
            while (*p && *p != '\n') { cur_col++; p++; }
            continue;
        }

        // Block comments: skip to closing */
        if (p[0] == '/' && p[1] == '*')
        {
            cur_col += 2; p += 2;
            while (*p && !(p[0] == '*' && p[1] == '/'))
            {
                if (*p == '\n') { cur_line++; cur_col = 1; }
                else cur_col++;
                p++;
            }
            if (*p) { cur_col += 2; p += 2; }  // skip closing */
            continue;
        }

        // Punctuators and operators: greedy longest-match via table
        {
            bool matched = false;
            for (const PunctEntry *pt = punct_table; pt->str; pt++)
            {
                /* Don't consume '.' as TK_DOT when followed by a digit —
                   that's a leading-dot float literal like .03 or .5e2. */
                if (pt->len == 1 && pt->str[0] == '.' && isdigit((unsigned char)p[1]))
                    continue;
                if (!strncmp(p, pt->str, pt->len))
                {
                    cur = new_token(pt->kind, cur, p, pt->len);
                    cur->line     = cur_line;
                    cur->col      = cur_col;
                    cur->filename = cur_file;
                    cur_col += pt->len;
                    p += pt->len;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }

        // Keywords and identifiers
        if (isalpha(*p) || *p == '_')
        {
            int tok_line = cur_line, tok_col = cur_col;
            char *q = p;
            while (*q && (isalnum(*q) || *q == '_')) q++;
            int l   = q - p;
            Token_kind tk = find_token(p, l);
            cur = new_token(tk, cur, p, l);
            cur->line     = tok_line;
            cur->col      = tok_col;
            cur->filename = cur_file;
            cur_col += l;
            p = q;
            continue;
        }

        if (isdigit(*p) || (*p == '.' && isdigit((unsigned char)p[1])))
        {
            // integers may be followed by u, l, or ul (any case)
            // floats may be followed by f, l
            //
            // Try both and use whichever decodes more characters
            int tok_line = cur_line, tok_col = cur_col;
            char *p_start = p;
            char *q;
            int ilen, flen;
            long long ival = strtol(p, &q, 0);
            ilen = q - p;
            double fval = strtod(p, &q);
            flen = q - p;
            if (flen > ilen)
            {
                if (*q == 'f' || *q == 'F' || *q == 'l' || *q == 'L')
                    q++;
                cur         = new_token(TK_CONSTFLT, cur, p, q - p);
                cur->fval   = fval;
            }
            else
            {
                if (*q == 'u' || *q == 'U')
                {
                    q++;
                    if (*q == 'l' || *q == 'L')
                        q++;
                }
                else if (*q == 'l' || *q == 'L')
                    q++;
                cur         = new_token(TK_CONSTINT, cur, p, q - p);
                cur->ival   = ival;
            }
            cur->line     = tok_line;
            cur->col      = tok_col;
            cur->filename = cur_file;
            cur_col += (int)(q - p_start);
            p = q;
            continue;
        }

        if (*p == '\'')
        {
            int tok_line = cur_line, tok_col = cur_col;
            char ch;
            int adv = decode_string_char(p + 1, &ch);
            if (p[1 + adv] != '\'')
                src_error(tok_line, tok_col, "Unterminated character constant");
            cur = new_token(TK_CHARACTER, cur, &ch, 1);
            cur->line     = tok_line;
            cur->col      = tok_col;
            cur->filename = cur_file;
            int advance = 1 + adv + 1;
            cur_col += advance;
            p += advance;  // opening quote + char/escape + closing quote
            continue;
        }

        if (*p == '"')
        {
            int tok_line = cur_line, tok_col = cur_col;
            char *start = p;
            cur_col++; p++;  // skip opening "
            int cap = 64, len = 0;
            char *buf = malloc(cap);
            // Handle adjacent string literals: "abc" "def" → "abcdef"
            do
            {
                while (*p && *p != '"')
                {
                    if (len + 2 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                    char c;
                    int adv = decode_string_char(p, &c);
                    buf[len++] = c;
                    // Track col; escape sequences don't contain newlines in practice
                    cur_col += adv;
                    p += adv;
                }
                if (*p == '"') { cur_col++; p++; }  // skip closing "
                // Skip whitespace and check for another string literal
                char *q = p;
                while (*q)
                {
                    if (*q == '\n') { cur_line++; cur_col = 1; q++; }
                    else if (isspace(*q)) { cur_col++; q++; }
                    else break;
                }
                if (*q == '"') { cur_col++; p = q + 1; } else { p = q; break; }
            } while (1);
            buf[len] = 0;
            Token *strtok = arena_alloc(sizeof(Token));
            strtok->kind = TK_STRING;
            strtok->val  = arena_strdup(buf);
            free(buf);
            strtok->ival = len;
            strtok->loc  = start - token_ctx.user_input;
            strtok->line     = tok_line;
            strtok->col      = tok_col;
            strtok->filename = cur_file;
            cur->next = strtok;
            cur = strtok;
            continue;
        }

        src_error(cur_line, cur_col, "Unexpected input '%c'", *p);
    }
    Token *eof_tok = new_token(TK_EOF, cur, p, 0);
    eof_tok->line     = cur_line;
    eof_tok->col      = cur_col;
    eof_tok->filename = cur_file;
    return head.next;
}

void print_tokens()
{
    for (Token *p = token_ctx.current; p; p = p->next)
    {
        fprintf(stderr, "Kind:%s val:%s\n", token_str(p->kind), p->val);
    }
}
