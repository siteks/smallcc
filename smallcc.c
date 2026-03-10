
#include "smallcc.h"

// ---------------------------------------------------------------
// Arena allocator
// ---------------------------------------------------------------
#define ARENA_SIZE (16 * 1024 * 1024)
static char arena_storage[ARENA_SIZE];
Arena arena = { arena_storage, 0, sizeof(arena_storage) };

void *arena_alloc(size_t size)
{
    // Round up to 8-byte alignment
    size = (size + 7) & ~(size_t)7;
    if (arena.used + size > arena.cap)
        error("arena exhausted (used %zu, requested %zu)", arena.used, size);
    void *p = arena.base + arena.used;
    arena.used += size;
    return p;   // pre-zeroed: arena_storage is static (BSS)
}

char *arena_strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = arena_alloc(n);
    memcpy(p, s, n);
    return p;
}


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


static void harvest_globals(void)
{
    for (Symbol *s = type_ctx.symbol_table->symbols; s; s = s->next)
    {
        if (s->ns != NS_IDENT) continue;
        if (s->kind == SYM_STATIC_GLOBAL || s->kind == SYM_STATIC_LOCAL || s->kind == SYM_EXTERN || s->kind == SYM_BUILTIN) continue;
        insert_extern_sym(s->name, s->type);
    }
}

static void reset_tu(int tu)
{
    current_global_tu = tu;
    reset_codegen();
    reset_parser();
    reset_types_state();
    reset_preprocessor();
}


int main(int argc, char **argv)
{
    // Parse -o outfile
    FILE *out = stdout;
    int file_start = 1;
    if (argc >= 3 && strcmp(argv[1], "-o") == 0)
    {
        out = fopen(argv[2], "w");
        if (!out) { perror(argv[2]); return 1; }
        file_start = 3;
    }

    if (argc <= file_start)
    {
        fprintf(stderr, "Usage: smallcc [-o outfile] <source.c> [source2.c ...]\n");
        return 1;
    }

    set_asm_out(out);

    int tu_count = argc - file_start;

    // Preamble emitted exactly once
    fprintf(out, ".text=0\n");
    fprintf(out, "    ssp     0x1000\n");
    fprintf(out, "    jl      main\n");
    fprintf(out, "    halt\n");

    for (int tu = 0; tu < tu_count; tu++)
    {
        char *raw    = read_file(argv[file_start + tu]);
        char *source = preprocess(raw, argv[file_start + tu]);
        free(raw);

        reset_tu(tu);
        make_basic_types();

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
        resolve_symbols(node);
        derive_types(node);
        insert_coercions(node);
        finalize_local_offsets();
#ifdef DEBUG_ENABLED
        print_tree(node, 0);
        print_symbol_table(type_ctx.symbol_table, 0);
        print_type_table();
#endif

        gen_ir(node, tu);
        backend_emit_asm(codegen_ctx.ir_head);
        harvest_globals();
        free(source);
    }

    if (out != stdout) fclose(out);
    return 0;
}


