
#include "mycc.h"


void error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}


static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { error("Cannot open %s\n", path); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}


// Cross-TU context instance
ExternContext extern_ctx;

static void harvest_globals(void)
{
    for (Symbol *s = type_ctx.symbol_table->idents; s; s = s->next)
    {
        if (s->is_static || s->is_extern_decl) continue;
        if (!strcmp(s->name, "putchar")) continue;
        bool already = false;
        for (int i = 0; i < extern_ctx.count; i++)
            if (!strcmp(extern_ctx.syms[i].name, s->name)) { already = true; break; }
        if (!already)
        {
            extern_ctx.syms[extern_ctx.count].name = s->name;
            extern_ctx.syms[extern_ctx.count].type = s->type;
            extern_ctx.count++;
        }
    }
}

static void reset_tu(int tu)
{
    // Zero-initialize all context structs on first call
    static bool initialized = false;
    if (!initialized) {
        memset(&type_ctx, 0, sizeof(type_ctx));
        memset(&token_ctx, 0, sizeof(token_ctx));
        memset(&parser_ctx, 0, sizeof(parser_ctx));
        memset(&codegen_ctx, 0, sizeof(codegen_ctx));
        memset(&extern_ctx, 0, sizeof(extern_ctx));
        initialized = true;
    }
    current_global_tu = tu;
    reset_codegen();
    reset_parser();
    reset_types_state();
}

static void prepopulate_extern_syms(void)
{
    for (int i = 0; i < extern_ctx.count; i++)
        insert_extern_sym(extern_ctx.syms[i].name, extern_ctx.syms[i].type);
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: mycc <source.c> [source2.c ...] | mycc \"C code\"\n");
        return 1;
    }

    FILE *probe = fopen(argv[1], "r");
    bool file_mode = (probe != NULL);
    if (probe) fclose(probe);

    int tu_count = file_mode ? (argc - 1) : 1;

    // Preamble emitted exactly once
    printf(".text=0\n");
    printf("    ssp     0x1000\n");
    printf("    jl      main\n");
    printf("    halt\n");

    for (int tu = 0; tu < tu_count; tu++)
    {
        char *source = file_mode ? read_file(argv[tu + 1]) : argv[1];

        reset_tu(tu);
        make_basic_types();
        prepopulate_extern_syms();

        token_ctx.user_input = source;
        token_ctx.current = tokenise(source);
#ifdef DEBUG_ENABLED
        print_tokens();
#endif
        Node *node = program();
#ifdef DEBUG_ENABLED
        print_tree(node, 0);
        print_symbol_table(type_ctx.symbol_table, 0);
        print_type_table();
#endif
        propagate_types(0, node);
#ifdef DEBUG_ENABLED
        print_tree(node, 0);
        print_symbol_table(type_ctx.symbol_table, 0);
        print_type_table();
#endif

        gen_code(node, tu);
        harvest_globals();

        if (file_mode) free(source);
    }

    return 0;
}


