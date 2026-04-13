
#include "smallcc.h"
#include "sx.h"
#include "lower.h"
#include "braun.h"
#include "dom.h"
#include "oos.h"
#include "opt.h"
#include "legalize.h"
#include "alloc.h"
#include "emit.h"
#include "irsim.h"
#include <dirent.h>
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
    // Parse flags: -o outfile, -stats, -DNAME[=VALUE]
    FILE *out = stdout;
    int file_start = 1;
    bool show_stats = false;
    bool preprocess_only = false;
    FILE *ssa_out = NULL;  // -ssa file: write Braun SSA IR to this file after braun_function
    FILE *oos_out = NULL;  // -oos file: write post-OOS IR to this file
    FILE *irc_out = NULL;  // -irc file: write post-IRC IR to this file
    int run_oos = 0;       // -runoos: interpret post-OOS IR
    int run_irc = 0;       // -runirc: interpret post-IRC IR
    IrSim *irsim = NULL;
    const char *cmdline_defines[256];
    int num_defines = 0;

    while (file_start < argc && argv[file_start][0] == '-')
    {
        if (strcmp(argv[file_start], "-h") == 0 || strcmp(argv[file_start], "--help") == 0)
        {
            printf(
                "Usage: smallcc [options] <source.c> [source2.c ...]\n"
                "\n"
                "Output:\n"
                "  -o <file>          Write assembly to <file> (default: stdout)\n"
                "  -E                 Preprocess only; write preprocessed source to stdout\n"
                "\n"
                "Preprocessor:\n"
                "  -DNAME[=VALUE]     Define preprocessor macro\n"
                "  -I<dir>            Add directory to #include <...> search path\n"
                "\n"
                "IR dumps:\n"
                "  -ssa <file>        Write Braun SSA IR to <file> after braun_function\n"
                "  -oos <file>        Write post-OOS IR to <file> after out_of_ssa\n"
                "  -irc <file>        Write post-IRC IR to <file> after irc_allocate\n"
                "\n"
                "IR interpretation (prints r0:XXXXXXXX to stdout):\n"
                "  -runoos            Interpret post-OOS IR (skips register allocation)\n"
                "  -runirc            Interpret post-IRC IR (after register allocation)\n"
                "\n"
                "Debug:\n"
                "  -ann               Annotate assembly output with source comments\n"
                "  -O0                Disable all optional optimization passes\n"
                "  -O1                Safe passes only (fold_br + dead_blocks + copy_prop)\n"
                "  -O / -O2           All passes (default)\n"
                "  -Opass=NAME        Enable single pass (additive)\n"
                "  -Ono-pass=NAME     Disable single pass (subtractive)\n"
                "  -Omask=0xNNNN      Set exact pass bitmask (hex)\n"
                "\n"
                "Diagnostics:\n"
                "  -stats             Print per-TU arena usage to stderr\n"
                "  -h, --help         Show this help\n"
            );
            return 0;
        }
        else if (strcmp(argv[file_start], "-o") == 0 && file_start + 1 < argc)
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
        else if (strcmp(argv[file_start], "-E") == 0)
        {
            preprocess_only = true;
            file_start++;
        }
        else if (strncmp(argv[file_start], "-D", 2) == 0)
        {
            if (num_defines < 256)
                cmdline_defines[num_defines++] = argv[file_start] + 2;
            else {
                fprintf(stderr, "error: too many -D defines (max 256)\n");
                return 1;
            }
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
        else if (strcmp(argv[file_start], "-arch") == 0 && file_start + 1 < argc)
        {
            const char *arch = argv[file_start + 1];
            if (strcmp(arch, "cpu4") == 0) { /* cpu4 is the only target */ }
            else { fprintf(stderr, "smallcc: unknown arch: %s\n", arch); return 1; }
            file_start += 2;
        }
        else if (strcmp(argv[file_start], "-ssa") == 0 && file_start + 1 < argc)
        {
            ssa_out = fopen(argv[file_start + 1], "w");
            if (!ssa_out) { perror(argv[file_start + 1]); return 1; }
            file_start += 2;
        }
        else if (strcmp(argv[file_start], "-oos") == 0 && file_start + 1 < argc)
        {
            oos_out = fopen(argv[file_start + 1], "w");
            if (!oos_out) { perror(argv[file_start + 1]); return 1; }
            file_start += 2;
        }
        else if (strcmp(argv[file_start], "-irc") == 0 && file_start + 1 < argc)
        {
            irc_out = fopen(argv[file_start + 1], "w");
            if (!irc_out) { perror(argv[file_start + 1]); return 1; }
            file_start += 2;
        }
        else if (strcmp(argv[file_start], "-runoos") == 0)
        {
            run_oos = 1;
            file_start++;
        }
        else if (strcmp(argv[file_start], "-runirc") == 0)
        {
            run_irc = 1;
            file_start++;
        }
        else if (strcmp(argv[file_start], "-ann") == 0)
        {
            flag_annotate = 1;
            file_start++;
        }
        else if (strncmp(argv[file_start], "-O", 2) == 0)
        {
            const char *arg = argv[file_start] + 2;
            if (strcmp(arg, "0") == 0)
                opt_flags = OPT_NONE;
            else if (strcmp(arg, "1") == 0)
                opt_flags = OPT_SAFE;
            else if (strcmp(arg, "2") == 0 || strcmp(arg, "") == 0)
                opt_flags = OPT_ALL;
            else if (strncmp(arg, "mask=", 5) == 0)
                opt_flags = (unsigned)strtoul(arg + 5, NULL, 0);
            else if (strncmp(arg, "pass=", 5) == 0 || strncmp(arg, "no-pass=", 8) == 0) {
                int negate = (arg[0] == 'n');
                const char *name = negate ? arg + 8 : arg + 5;
                static const struct { const char *name; unsigned bit; } pass_tbl[] = {
                    {"fold_br",        OPT_FOLD_BR},
                    {"dead_blocks",    OPT_DEAD_BLOCKS},
                    {"copy_prop",      OPT_COPY_PROP},
                    {"cse",            OPT_CSE},
                    {"redundant_bool", OPT_REDUNDANT_BOOL},
                    {"narrow_loads",   OPT_NARROW_LOADS},
                    {"licm_const",     OPT_LICM_CONST},
                    {"jump_thread",    OPT_JUMP_THREAD},
                    {"unroll",         OPT_UNROLL},
                    {"leg_e",          OPT_LEG_E},
                    {"leg_f",          OPT_LEG_F},
                    {NULL, 0}
                };
                int found = 0;
                for (int j = 0; pass_tbl[j].name; j++) {
                    if (strcmp(name, pass_tbl[j].name) == 0) {
                        if (negate) opt_flags &= ~pass_tbl[j].bit;
                        else        opt_flags |= pass_tbl[j].bit;
                        found = 1;
                        break;
                    }
                }
                if (!found)
                    fprintf(stderr, "smallcc: unknown pass name: %s\n", name);
            }
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
        fprintf(stderr, "Usage: smallcc [options] <source.c> [source2.c ...]\n"
                        "Run 'smallcc -h' for full option list.\n");
        return 1;
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

    if (run_oos || run_irc) {
        irsim = irsim_new();
        // In sim mode, suppress assembly output
        out = fopen("/dev/null", "w");
        if (!out) { perror("/dev/null"); return 1; }
    }

    if (!preprocess_only) {
        // Preamble: emit crt0_cpu4.s if present, else built-in default
        char crt0_path[4096];
        snprintf(crt0_path, sizeof(crt0_path), "%s/lib/crt0_cpu4.s", compiler_dir);
        FILE *crt0 = fopen(crt0_path, "r");
        if (crt0) {
            char buf[256];
            while (fgets(buf, sizeof(buf), crt0))
                fputs(buf, out);
            fclose(crt0);
        } else {
            fprintf(out, ".text=0\n");
            fprintf(out, "    immw     r0, 0xF000\n");
            fprintf(out, "    ssp     r0\n");
            fprintf(out, "    jl      main\n");
            fprintf(out, "    halt\n");
        }

        if (show_stats)
            fprintf(stderr, "%-4s  %-10s  %8s  %8s\n", "TU", "file", "arena_before", "arena_used");
    }

    // For CPU4: defer all globals/strlits to after all functions, so that
    // _globals_start: marks the clean code/data boundary for clearmem + watchpoint.
    FILE *globals_buf = tmpfile();
    if (!globals_buf) { perror("tmpfile"); return 1; }

    int cpu4_strlit_id = 0;  // monotonically increasing string literal ID across TUs
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
        finalize_local_offsets();
#ifdef DEBUG_ENABLED
        print_tree(node, 0);
        print_symbol_table(type_ctx.symbol_table, 0);
        print_type_table();
#endif

        if (flag_annotate)
            set_ann_source(source);

        {
            // Nanopass pipeline: Node* → SSA → IRC → CPU4
            // Phase 1: emit global variables and top-level string literals
            Sx *sx_prog = lower_globals(node, tu, &cpu4_strlit_id);
            emit_globals(sx_prog, globals_buf);
            if (irsim) irsim_populate_globals(irsim, sx_prog);
            // Phase 2: compile each function directly from Node* to SSA
            Node *decls = (node && node->kind == ND_PROGRAM) ? node->ch[0] : node;
            for (Node *d = decls; d; d = d->next) {
                if (d->kind != ND_DECLARATION || !d->u.declaration.is_func_defn) continue;
                Function *f = braun_function(d, tu, &cpu4_strlit_id);
                braun_register_inline_candidate(d, tu);
                if (irsim) {
                    // Register function-body string literals with the irsim
                    // before braun_emit_strlits clears them.
                    int ns = braun_nstrlits();
                    for (int _si = 0; _si < ns; _si++) {
                        char _lbl[32]; const char *_dat; int _len;
                        braun_get_strlit(_si, _lbl, &_dat, &_len);
                        irsim_add_strlit(irsim, _lbl, _dat, _len);
                    }
                }
                braun_emit_strlits(globals_buf);
                if (f) {
                    if (ssa_out) { fprintf(ssa_out, "=== SSA: %s ===\n", f->name); print_function(f, ssa_out); }
                    split_critical_edges(f);
                    compute_dominators(f);
                    out_of_ssa(f);
                    if (opt_flags & OPT_FOLD_BR)        opt_fold_branches(f);
                    if (opt_flags & OPT_DEAD_BLOCKS)    opt_remove_dead_blocks(f);
                    if (opt_flags & OPT_COPY_PROP)      opt_copy_prop(f);
                    if (opt_flags & OPT_CSE)            opt_cse(f);
                    if (opt_flags & OPT_REDUNDANT_BOOL) opt_redundant_bool(f);
                    if (opt_flags & OPT_NARROW_LOADS)   opt_narrow_loads(f);
                    if (opt_flags & OPT_LICM_CONST)     opt_licm_const(f);
                    if (opt_flags & OPT_JUMP_THREAD)    opt_jump_thread(f);
                    if (opt_flags & OPT_UNROLL)         opt_unroll_loops(f);
                    if (opt_flags & OPT_COPY_PROP)      opt_copy_prop(f);
                    compute_dominators(f);
                    if (oos_out) { fprintf(oos_out, "=== OOS: %s ===\n", f->name); print_function(f, oos_out); }
                    if (getenv("DUMP_IR")) { fprintf(stderr, "=== after oos ===\n"); print_function(f, stderr); }
                    if (run_oos && irsim) { irsim_add_function(irsim, f); continue; }
                    legalize_function(f);
                    irc_allocate(f);
                    if (irc_out) { fprintf(irc_out, "=== IRC: %s ===\n", f->name); print_function(f, irc_out); }
                    if (getenv("DUMP_IR")) { fprintf(stderr, "=== after irc ===\n"); print_function(f, stderr); }
                    if (run_irc && irsim) { irsim_add_function(irsim, f); continue; }
                    emit_function(f, out);
                }
            }
        }
        harvest_globals();

        if (show_stats)
        {
            size_t tu_used = arena.used - arena_before;
            fprintf(stderr, "%-4d  %-10s  %8zu  %8zu\n",
                    tu, all_files[tu], arena_before, tu_used);
        }
    }

    // Flush deferred globals after all functions — _globals_start: marks
    // the code/data boundary used by clearmem and the sim_c write watchpoint.
    if (!run_oos && !run_irc) {
        fprintf(out, "    align\n");
        fprintf(out, "_globals_start:\n");
        rewind(globals_buf);
        char xbuf[4096];
        size_t n;
        while ((n = fread(xbuf, 1, sizeof(xbuf), globals_buf)) > 0)
            fwrite(xbuf, 1, n, out);
    }
    fclose(globals_buf);

    if (show_stats)
        fprintf(stderr, "arena total: %zu / %zu bytes (%.1f%%)\n",
                arena.used, arena.cap,
                100.0 * (double)arena.used / (double)arena.cap);

    if (irsim) {
        uint32_t result = irsim_call_main(irsim);
        printf("r0:%08x\n", result);
        irsim_free(irsim);
    }

    if (out != stdout) fclose(out);
    if (ssa_out) fclose(ssa_out);
    if (oos_out) fclose(oos_out);
    if (irc_out) fclose(irc_out);
    return 0;
}


