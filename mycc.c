
#include "mycc.h"


void error(char *fmt, ...)
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
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}


extern Token           *token;
extern char            *user_input;
extern Symbol_table    *symbol_table;
extern Symbol_table    *curr_scope_st;

typedef struct { char *name; Type2 *type; } ExternSym;
static ExternSym extern_syms[1024];
static int       extern_sym_count = 0;

static void harvest_globals(void)
{
    for (Symbol *s = symbol_table->idents; s; s = s->next)
    {
        if (s->is_static || s->is_extern_decl) continue;
        if (!strcmp(s->name, "putchar")) continue;
        bool already = false;
        for (int i = 0; i < extern_sym_count; i++)
            if (!strcmp(extern_syms[i].name, s->name)) { already = true; break; }
        if (!already)
        {
            extern_syms[extern_sym_count].name = s->name;
            extern_syms[extern_sym_count].type = s->type;
            extern_sym_count++;
        }
    }
}

static void reset_tu(int tu)
{
    current_global_tu = tu;
    reset_codegen();
    reset_parser();
    reset_types_state();
}

static void prepopulate_extern_syms(void)
{
    for (int i = 0; i < extern_sym_count; i++)
        insert_extern_sym(extern_syms[i].name, extern_syms[i].type);
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

        user_input = source;
        token = tokenise(source);
        print_tokens();
        Node *node = program();
        print_tree(node, 0);
        print_symbol_table(symbol_table, 0);
        print_type_table();
        propagate_types(0, node);
        print_tree(node, 0);
        print_symbol_table(symbol_table, 0);
        print_type_table();

        gen_code(node, tu);
        harvest_globals();

        if (file_mode) free(source);
    }

    return 0;
}


