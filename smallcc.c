
#include "smallcc.h"
#include "ir3.h"
#include <dirent.h>

/* Target architecture: 3 = cpu3 (default), 4 = cpu4 */
int g_target_arch = 3;
#ifdef __APPLE__
#  include <mach-o/dyld.h>
#endif

// ---------------------------------------------------------------
// Arena allocators
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

// Scratch arena — backend temporaries (IR3, SSA, CFG, IncPhi).
// Reset per-TU before the RISC backend pipeline.
#define SCRATCH_SIZE (4 * 1024 * 1024)
static char scratch_storage[SCRATCH_SIZE];
Arena scratch = { scratch_storage, 0, sizeof(scratch_storage) };

void *scratch_alloc(size_t size)
{
    size = (size + 7) & ~(size_t)7;
    if (scratch.used + size > scratch.cap)
        error("scratch arena exhausted (used %zu, requested %zu)", scratch.used, size);
    void *p = scratch.base + scratch.used;
    scratch.used += size;
    memset(p, 0, size);   // zero so stale data from a previous TU can't leak
    return p;
}

void scratch_reset(void) { scratch.used = 0; }


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
    /* Determine the best filename to report: prefer the current token's logical
       filename, then the most recent linemarker filename, then the TU filename. */
    const char *fname;
    if (token_ctx.current && token_ctx.current->filename)
        fname = token_ctx.current->filename;
    else if (token_ctx.logical_filename)
        fname = token_ctx.logical_filename;
    else if (token_ctx.filename)
        fname = token_ctx.filename;
    else
        fname = "?";

    fprintf(stderr, "%s:%d:%d: error: ", fname, line, col);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");

    /* Re-read the original source file to show the actual line + caret.
       (token_ctx.user_input holds preprocessed text; line numbers there
        do not match the original source after #include expansion.) */
    FILE *f = fopen(fname, "r");
    if (f) {
        char buf[1024];
        buf[0] = '\0';
        for (int l = 1; l <= line; l++) {
            if (!fgets(buf, sizeof(buf), f)) { buf[0] = '\0'; break; }
        }
        fclose(f);
        buf[strcspn(buf, "\n")] = '\0';
        fprintf(stderr, "    %s\n    %*s^\n", buf, col - 1, "");
    }

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
        if (s->kind == SYM_ENUM_CONST) continue;  // enum consts have no cross-TU linkage; re-declared from headers each TU
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
    char buf[4096];
    bool resolved = false;

#ifdef __APPLE__
    uint32_t sz = sizeof(buf);
    if (_NSGetExecutablePath(buf, &sz) == 0) resolved = true;
#endif

#ifdef __linux__
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = '\0'; resolved = true; }
#endif

    if (!resolved) {
        /* Fallback: realpath on argv[0] — works for explicit paths, not bare names on PATH */
        if (realpath(argv0, buf)) resolved = true;
    }

    if (resolved) {
        char *slash = strrchr(buf, '/');
        if (slash) *slash = '\0';
        strncpy(dir, buf, cap - 1);
        dir[cap - 1] = '\0';
    } else {
        strncpy(dir, ".", cap - 1);
        dir[cap - 1] = '\0';
    }
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
    // Parse flags: -o outfile, -stats, -O[N], -DNAME[=VALUE]
    FILE *out = stdout;
    int file_start = 1;
    bool show_stats = false;
    bool preprocess_only = false;
    int opt_level = 0;
    const char *cmdline_defines[256];
    int num_defines = 0;
    const char *ssa_dump_path = NULL;

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
        else if (strcmp(argv[file_start], "-E") == 0)
        {
            preprocess_only = true;
            file_start++;
        }
        else if (strcmp(argv[file_start], "-ann") == 0)
        {
            flag_annotate = 1;
            file_start++;
        }
        else if (strcmp(argv[file_start], "-no-newinsns") == 0)
        {
            flag_no_newinsns = 1;
            file_start++;
        }
        else if (strncmp(argv[file_start], "-D", 2) == 0)
        {
            if (num_defines < 256)
                cmdline_defines[num_defines++] = argv[file_start] + 2;
            file_start++;
        }
        else if (strncmp(argv[file_start], "-I", 2) == 0)
        {
            const char *dir = argv[file_start] + 2;
            if (*dir == '\0' && file_start + 1 < argc)
            {
                /* -I dir (space-separated) */
                dir = argv[file_start + 1];
                file_start++;
            }
            add_include_dir(dir);
            file_start++;
        }
        else if (strcmp(argv[file_start], "-ssa") == 0 && file_start + 1 < argc)
        {
            ssa_dump_path = argv[file_start + 1];
            file_start += 2;
        }
        else if (strcmp(argv[file_start], "-arch") == 0 && file_start + 1 < argc)
        {
            const char *a = argv[file_start + 1];
            if (strcmp(a, "cpu4") == 0)      g_target_arch = 4;
            else if (strcmp(a, "cpu3") == 0) g_target_arch = 3;
            else {
                fprintf(stderr, "smallcc: unknown arch: %s (use cpu3 or cpu4)\n", a);
                return 1;
            }
            file_start += 2;
        }
        else
        {
            fprintf(stderr, "smallcc: unknown option: %s\n", argv[file_start]);
            return 1;
        }
    }

    if (argc <= file_start)
    {
        fprintf(stderr, "Usage: smallcc [-o outfile] [-E] [-stats] [-ann] [-arch cpu3|cpu4] [-ssa file] [-DNAME[=VAL]] [-Idir] <source.c> [source2.c ...]\n");
        return 1;
    }

    set_asm_out(out);

    FILE *ssa_out = NULL;
    if (ssa_dump_path) {
        ssa_out = fopen(ssa_dump_path, "w");
        if (!ssa_out) { perror(ssa_dump_path); return 1; }
    }

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

    if (!preprocess_only) {
        // Preamble: emit arch-specific crt0 if present, then plain crt0.s, else built-in default
        char crt0_path[4096];
        snprintf(crt0_path, sizeof(crt0_path), "%s/lib/crt0_cpu%d.s", compiler_dir, g_target_arch);
        FILE *crt0 = fopen(crt0_path, "r");
        if (!crt0) {
            snprintf(crt0_path, sizeof(crt0_path), "%s/lib/crt0.s", compiler_dir);
            crt0 = fopen(crt0_path, "r");
        }
        if (crt0) {
            char buf[256];
            while (fgets(buf, sizeof(buf), crt0))
                fputs(buf, out);
            fclose(crt0);
        } else {
            fprintf(out, ".text=0\n");
            fprintf(out, "    ssp     0x8000\n");
            fprintf(out, "    jl      main\n");
            fprintf(out, "    halt\n");
        }

        if (show_stats)
            fprintf(stderr, "%-4s  %-10s  %8s  %8s\n", "TU", "file", "arena_before", "arena_used");
    }

    for (int tu = 0; tu < tu_count; tu++)
    {
        size_t arena_before = arena.used;

        token_ctx.filename = all_files[tu];
        char *raw = read_file(all_files[tu]);

        /* Apply -D command-line defines before preprocessing each TU.
           reset_tu() -> reset_preprocessor() clears them, so re-add here. */
        for (int d = 0; d < num_defines; d++)
        {
            const char *def = cmdline_defines[d];
            const char *eq  = strchr(def, '=');
            char name[64];
            if (eq)
            {
                size_t nlen = (size_t)(eq - def);
                if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
                memcpy(name, def, nlen);
                name[nlen] = '\0';
                pp_define(name, eq + 1);
            }
            else
            {
                strncpy(name, def, sizeof(name) - 1);
                name[sizeof(name) - 1] = '\0';
                pp_define(name, "1");
            }
        }

        char *source = preprocess(raw, all_files[tu]);

        if (preprocess_only) {
            /* Only emit preprocessed output for user files, not auto-included lib TUs */
            if (tu >= lib_count) {
                if (tu > lib_count)
                    fprintf(out, "\n");
                if (user_count > 1)
                    fprintf(out, "# %s\n", all_files[tu]);
                fputs(source, out);
            }
            reset_preprocessor();
            continue;
        }

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
        if (g_target_arch == 4) {
            /* Per-function pipeline: scratch_reset() before each function so
             * the 4 MB arena is reused rather than accumulated across functions.
             * Preamble and data-section nodes are passed through immediately. */
            ir3_reset();
            IRInst *p = codegen_ctx.ir_head;

            /* Preamble (ssp / jl / halt) and any leading data nodes */
            IR3Inst *data = braun_emit_data(&p);
            if (data) risc_backend_emit(data);

            while (p) {
                /* p is at a function SYMLABEL */
                scratch_reset();
                IR3Inst *fn = braun_emit_function(&p);
                if (fn) {
                    ir3_optimize(fn, opt_level);
                    if (ssa_out) ir3_dump(fn, ssa_out);  /* dump true SSA before deconstruction */
                    braun_decon_phis();
                    irc_regalloc(fn);
                    risc_backend_emit(fn);
                }
                /* Inter-function or trailing data/global-var/string-literal nodes */
                data = braun_emit_data(&p);
                if (data) risc_backend_emit(data);
            }
        } else {
            backend_emit_asm(codegen_ctx.ir_head);
        }
        harvest_globals();

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
    if (ssa_out) fclose(ssa_out);
    return 0;
}


