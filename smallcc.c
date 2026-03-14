
#include "smallcc.h"
#include <dirent.h>

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

void src_error(int line, int col, const char *fmt, ...)
{
    fprintf(stderr, "%s:%d:%d: error: ",
            token_ctx.filename ? token_ctx.filename : "?", line, col);
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
    char *buf = arena_alloc(sz + 1);
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


static void get_compiler_dir(const char *argv0, char *dir, size_t cap)
{
    strncpy(dir, argv0, cap - 1);
    dir[cap - 1] = '\0';
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = '\0';
    else
        strncpy(dir, ".", cap - 1);
}

static int compare_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

// Quick scan of a source file's raw text for #include <header> directives.
// For each "<header>" found, if lib_dir/header.c exists it is added to files[].
// Does not run the full preprocessor — just looks for the literal pattern.
// Conservative: a header inside a dead #ifdef branch will still trigger
// lib compilation, which is fine.
static int collect_needed_libs(char **user_files, int user_count,
                                const char *lib_dir,
                                char **files, int max_files)
{
    // Collect unique header stems that have a matching lib/*.c
    char seen[64][64];
    int  seen_count = 0;
    int  lib_count  = 0;

    for (int u = 0; u < user_count; u++)
    {
        FILE *f = fopen(user_files[u], "r");
        if (!f) continue;
        char line[1024];
        while (fgets(line, sizeof(line), f))
        {
            // Look for: #include <name>
            const char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '#') continue;
            p++;
            while (*p == ' ' || *p == '\t') p++;
            if (strncmp(p, "include", 7) != 0) continue;
            p += 7;
            while (*p == ' ' || *p == '\t') p++;
            if (*p != '<') continue;
            p++;
            char hdr[64];
            size_t n = 0;
            while (*p && *p != '>' && n + 1 < sizeof(hdr))
                hdr[n++] = *p++;
            hdr[n] = '\0';
            if (!n) continue;

            // Strip extension to get stem (stdio.h -> stdio)
            char stem[64];
            strncpy(stem, hdr, sizeof(stem) - 1);
            stem[sizeof(stem) - 1] = '\0';
            char *dot = strrchr(stem, '.');
            if (dot) *dot = '\0';

            // Check if already queued
            int already = 0;
            for (int i = 0; i < seen_count; i++)
                if (strcmp(seen[i], stem) == 0) { already = 1; break; }
            if (already) continue;

            // Check if lib/stem.c exists
            char path[4096];
            snprintf(path, sizeof(path), "%s/%s.c", lib_dir, stem);
            FILE *lf = fopen(path, "r");
            if (!lf) continue;
            fclose(lf);

            // Record it
            strncpy(seen[seen_count++], stem, 63);
            if (lib_count < max_files)
            {
                char *p2 = malloc(strlen(path) + 1);
                if (!p2) error("out of memory");
                strcpy(p2, path);
                files[lib_count++] = p2;
            }
        }
        fclose(f);
    }

    // Sort so compilation order is deterministic (alphabetical by stem)
    qsort(files, lib_count, sizeof(char *), compare_str);
    return lib_count;
}

int main(int argc, char **argv)
{
    // Parse flags: -o outfile, -stats, -O[N]
    FILE *out = stdout;
    int file_start = 1;
    bool show_stats = false;
    int opt_level = 0;

    while (file_start < argc && argv[file_start][0] == '-')
    {
        if (strcmp(argv[file_start], "-o") == 0 && file_start + 1 < argc)
        {
            out = fopen(argv[file_start + 1], "w");
            if (!out) { perror(argv[file_start + 1]); return 1; }
            file_start += 2;
        }
        else if (strcmp(argv[file_start], "-stats") == 0)
        {
            show_stats = true;
            file_start++;
        }
        else if (strncmp(argv[file_start], "-O", 2) == 0)
        {
            const char *n = argv[file_start] + 2;
            opt_level = (*n == '\0') ? 1 : atoi(n);
            file_start++;
        }
        else if (strcmp(argv[file_start], "-ann") == 0)
        {
            flag_annotate = 1;
            file_start++;
        }
        else
        {
            fprintf(stderr, "smallcc: unknown option: %s\n", argv[file_start]);
            return 1;
        }
    }

    if (argc <= file_start)
    {
        fprintf(stderr, "Usage: smallcc [-o outfile] [-stats] [-ann] <source.c> [source2.c ...]\n");
        return 1;
    }

    set_asm_out(out);

    // Determine compiler binary directory for include/ and lib/ resolution
    char compiler_dir[4096];
    get_compiler_dir(argv[0], compiler_dir, sizeof(compiler_dir));

    // Set system include directory
    char include_dir[4096];
    snprintf(include_dir, sizeof(include_dir), "%s/include", compiler_dir);
    set_include_dir(include_dir);

    // Collect lib/*.c files for headers actually #include'd by user files
    char lib_dir[4096];
    snprintf(lib_dir, sizeof(lib_dir), "%s/lib", compiler_dir);
    char *lib_files[64];
    int lib_count = collect_needed_libs(argv + file_start, argc - file_start,
                                        lib_dir, lib_files, 64);

    int user_count = argc - file_start;
    int tu_count = lib_count + user_count;

    // Build unified file list: lib files first, then user files
    char **all_files = malloc(tu_count * sizeof(char *));
    if (!all_files) error("out of memory");
    for (int i = 0; i < lib_count; i++)  all_files[i] = lib_files[i];
    for (int i = 0; i < user_count; i++) all_files[lib_count + i] = argv[file_start + i];

    // Preamble emitted exactly once
    fprintf(out, ".text=0\n");
    fprintf(out, "    ssp     0x1000\n");
    fprintf(out, "    jl      main\n");
    fprintf(out, "    halt\n");

    if (show_stats)
        fprintf(stderr, "%-4s  %-10s  %8s  %8s\n", "TU", "file", "arena_before", "arena_used");

    for (int tu = 0; tu < tu_count; tu++)
    {
        size_t arena_before = arena.used;

        token_ctx.filename = all_files[tu];
        char *raw    = read_file(all_files[tu]);
        char *source = preprocess(raw, all_files[tu]);

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
        label_su(node);          // Sethi-Ullman: label + reorder commutative children
        finalize_local_offsets();
#ifdef DEBUG_ENABLED
        print_tree(node, 0);
        print_symbol_table(type_ctx.symbol_table, 0);
        print_type_table();
#endif

        if (flag_annotate)
            set_ann_source(source);
        gen_ir(node, tu);
        mark_basic_blocks();
        peephole(opt_level);
        backend_emit_asm(codegen_ctx.ir_head);
        harvest_globals();
        free(source);

        if (show_stats)
        {
            size_t tu_used = arena.used - arena_before;
            fprintf(stderr, "%-4d  %-10s  %8zu  %8zu\n",
                    tu, all_files[tu], arena_before, tu_used);
        }
    }

    if (show_stats)
        fprintf(stderr, "arena total: %zu / %zu bytes (%.1f%%)\n",
                arena.used, arena.cap,
                100.0 * (double)arena.used / (double)arena.cap);

    if (out != stdout) fclose(out);
    return 0;
}


