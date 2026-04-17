/*
 * sim_c.c — CPU4 assembler + simulator
 *
 * Usage:  ./sim_c [-v] [-arch cpu4] [-maxsteps N] file.s
 * Output: putchar to stderr; state line to stdout
 *   r0:..r7:XXXXXXXX sp:XXXX bp:XXXX lr:XXXX pc:XXXX H:X cycles:N
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Memory                                                               */
/* ------------------------------------------------------------------ */

static uint8_t mem[65536];

#define MMIO_BASE 0xFF00u

static uint32_t g_cycles = 0;

/* ------------------------------------------------------------------ */
/* Profiling                                                            */
/* ------------------------------------------------------------------ */

static int       g_profile   = 0;
static int       g_dump      = 0;
static uint16_t  g_asm_end   = 0;   /* high-water mark of assembler cur */
static uint32_t  prof_count[65536];   /* execution count per PC address */
static int16_t   prof_lineno[65536];  /* addr -> source line index (-1 = none) */
static char    **prof_lines  = NULL;  /* original source lines (strdup'd) */
static int       prof_nlines = 0;

static void prof_split_lines(const char *src)
{
    /* Count lines */
    int n = 0;
    const char *p = src;
    while (*p) { if (*p++ == '\n') n++; }
    if (src[0] != '\0') n++;  /* last line even without trailing newline */

    prof_lines = malloc(((size_t)n + 1) * sizeof(char *));
    if (!prof_lines) { fprintf(stderr, "oom\n"); exit(1); }
    prof_nlines = 0;

    p = src;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char *line = malloc(len + 1);
        if (!line) { fprintf(stderr, "oom\n"); exit(1); }
        memcpy(line, p, len);
        line[len] = '\0';
        if (len > 0 && line[len-1] == '\r') line[--len] = '\0';
        prof_lines[prof_nlines++] = line;
        if (!eol) break;
        p = eol + 1;
    }
}

typedef struct { uint32_t count; uint16_t addr; } ProfEntry;

static int prof_entry_cmp(const void *a, const void *b)
{
    const ProfEntry *pa = (const ProfEntry *)a;
    const ProfEntry *pb = (const ProfEntry *)b;
    if (pb->count > pa->count) return  1;
    if (pb->count < pa->count) return -1;
    return 0;
}

static void print_profile(void)
{
    unsigned long long total = 0;
    for (int i = 0; i < 65536; i++) total += prof_count[i];
    fprintf(stderr, "\nProfile: %llu instructions executed\n\n", total);

    /* --- Section A: top 40 hottest instructions --- */
    ProfEntry *entries = malloc(65536 * sizeof(ProfEntry));
    if (!entries) { fprintf(stderr, "oom\n"); return; }
    int nentries = 0;
    for (int i = 0; i < 65536; i++) {
        if (prof_count[i] > 0) {
            entries[nentries].count = prof_count[i];
            entries[nentries].addr  = (uint16_t)i;
            nentries++;
        }
    }
    qsort(entries, (size_t)nentries, sizeof(ProfEntry), prof_entry_cmp);

    fprintf(stderr, "Top 40 hot instructions:\n");
    fprintf(stderr, "  %10s  %7s  %4s  %s\n", "count", "%total", "addr", "source");
    int top = nentries < 40 ? nentries : 40;
    for (int i = 0; i < top; i++) {
        uint16_t    a   = entries[i].addr;
        uint32_t    cnt = entries[i].count;
        double      pct = total ? (100.0 * cnt / total) : 0.0;
        int16_t     ln  = prof_lineno[a];
        const char *src = (ln >= 0 && ln < prof_nlines) ? prof_lines[ln] : "";
        while (*src == ' ' || *src == '\t') src++;
        fprintf(stderr, "  %10u  %6.2f%%  %04x  %s\n", cnt, pct, (unsigned)a, src);
    }
    free(entries);

    /* --- Section B: annotated listing --- */
    /* Build lineno2addr: first address seen for each source line */
    int *lineno2addr = malloc((size_t)prof_nlines * sizeof(int));
    if (!lineno2addr) { fprintf(stderr, "oom\n"); return; }
    for (int i = 0; i < prof_nlines; i++) lineno2addr[i] = -1;
    for (int a = 0; a < 65536; a++) {
        int16_t ln = prof_lineno[a];
        if (ln >= 0 && ln < prof_nlines && lineno2addr[ln] == -1)
            lineno2addr[ln] = a;
    }

    fprintf(stderr, "\nAnnotated listing:\n");
    for (int ln = 0; ln < prof_nlines; ln++) {
        int         a    = lineno2addr[ln];
        const char *text = prof_lines[ln];
        if (a >= 0 && prof_count[a] > 0) {
            uint32_t cnt = prof_count[a];
            double   pct = total ? (100.0 * cnt / total) : 0.0;
            fprintf(stderr, "%10u %6.2f%%  %04x  |%s\n", cnt, pct, (unsigned)a, text);
        } else {
            fprintf(stderr, "                    |%s\n", text);
        }
    }
    free(lineno2addr);
}

static uint8_t mmio_read8(uint16_t a) {
    uint32_t off = (uint16_t)(a - MMIO_BASE);
    if (off < 4) return (uint8_t)(g_cycles >> (off * 8));
    return 0;
}

static uint16_t g_watch_pc = 0; /* set to pc of instruction being executed */
static uint32_t g_watch_r0 = 0;
static uint16_t g_watch_sp = 0, g_watch_bp = 0;
/* Print writes below the code/data boundary after assembler init.
   Threshold defaults to 0x5000; updated to _globals_start after assembly. */
static int      g_assembler_done  = 0;
static uint16_t g_write_threshold = 0x5000;
static void write8_inner (uint16_t a, uint8_t  v) { if (a >= MMIO_BASE) return; if (g_assembler_done && a < g_write_threshold) fprintf(stderr, "  WRITE8 to %04x = %02x  at pc=%04x sp=%04x bp=%04x r0=%08x\n", a, v, g_watch_pc, g_watch_sp, g_watch_bp, g_watch_r0); mem[a] = v; }
static void write16_inner(uint16_t a, uint16_t v) { if (a >= MMIO_BASE) return; if (g_assembler_done && a < g_write_threshold) fprintf(stderr, "  WRITE16 to %04x = %04x  at pc=%04x sp=%04x bp=%04x r0=%08x\n", a, v, g_watch_pc, g_watch_sp, g_watch_bp, g_watch_r0); mem[a]=(uint8_t)v; mem[(uint16_t)(a+1)]=(uint8_t)(v>>8); }
static void write32_inner(uint16_t a, uint32_t v) { if (a >= MMIO_BASE) return; if (g_assembler_done && a < g_write_threshold) fprintf(stderr, "  WRITE32 to %04x = %08x  at pc=%04x sp=%04x bp=%04x r0=%08x\n", a, v, g_watch_pc, g_watch_sp, g_watch_bp, g_watch_r0); write16_inner(a,(uint16_t)v); write16_inner((uint16_t)(a+2),(uint16_t)(v>>16)); }
#define write8  write8_inner
#define write16 write16_inner
#define write32 write32_inner
static uint8_t  read8 (uint16_t a) { if (a >= MMIO_BASE) return mmio_read8(a); return mem[a]; }
static uint16_t read16(uint16_t a) { if (a >= MMIO_BASE) return (uint16_t)mmio_read8(a) | ((uint16_t)mmio_read8((uint16_t)(a+1))<<8); return (uint16_t)mem[a] | ((uint16_t)mem[(uint16_t)(a+1)] << 8); }
static uint32_t read32(uint16_t a) { if (a >= MMIO_BASE) return (uint32_t)mmio_read8(a)|((uint32_t)mmio_read8((uint16_t)(a+1))<<8)|((uint32_t)mmio_read8((uint16_t)(a+2))<<16)|((uint32_t)mmio_read8((uint16_t)(a+3))<<24); return (uint16_t)read16(a) | ((uint32_t)read16((uint16_t)(a+2)) << 16); }

/* CPU4 alignment checking */
static void check_align16(uint16_t addr, uint16_t pc) {
    if (addr & 1) {
        fprintf(stderr, "CPU4 alignment error: 16-bit access to unaligned address 0x%04x at pc=0x%04x\n", addr, pc);
        exit(1);
    }
}
static void check_align32(uint16_t addr, uint16_t pc) {
    if (addr & 3) {
        fprintf(stderr, "CPU4 alignment error: 32-bit access to unaligned address 0x%04x at pc=0x%04x\n", addr, pc);
        exit(1);
    }
}

/* ------------------------------------------------------------------ */
/* Symbol table                                                         */
/* ------------------------------------------------------------------ */

#define MAX_SYMS 2048
#define MAX_NAME 128

typedef struct { char name[MAX_NAME]; uint16_t addr; } Sym;
static Sym syms[MAX_SYMS];
static int nsyms = 0;

static int find_sym(const char *n)
{
    for (int i = 0; i < nsyms; i++)
        if (strcmp(syms[i].name, n) == 0) return i;
    return -1;
}

static void add_sym(const char *n, uint16_t addr)
{
    int i = find_sym(n);
    if (i >= 0) { syms[i].addr = addr; return; }
    if (nsyms >= MAX_SYMS) { fprintf(stderr, "too many symbols\n"); exit(1); }
    strncpy(syms[nsyms].name, n, MAX_NAME-1);
    syms[nsyms].name[MAX_NAME-1] = '\0';
    syms[nsyms].addr = addr;
    nsyms++;
}

static uint16_t lookup_sym(const char *n)
{
    int i = find_sym(n);
    if (i < 0) { fprintf(stderr, "undefined symbol: %s\n", n); exit(1); }
    return syms[i].addr;
}


/* ------------------------------------------------------------------ */
/* CPU4 instruction table                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    uint8_t     first_byte;
    int         extra;   /* extra bytes: 0, 1, or 2 */
    int         subfmt;  /* 0=F0a/F1a/F2/F3a, 1=F1b/F3c, 2=F3e, 3=adjw/lea,
                            4=F3d(beqz/bnez), 7=F0c(cbeq/cbne), 8=F0b(two-op+imm9) */
    int         subop;   /* F1b: subopcode in byte1[5:0] */
} Instr4;

static const Instr4 itab4[] = {
    /* F0a — 1 byte, no operands */
    {"halt",   0x00,0,0,0}, {"ret",    0x01,0,0,0},
    /* F0b — 3 bytes, rd rx imm9 (two-op+imm9); subfmt=8; subop=0/1 per first_byte */
    {"addli",  0x10,2,8,0}, {"subli",  0x10,2,8,1},
    {"mulli",  0x11,2,8,0}, {"divli",  0x11,2,8,1},
    {"modli",  0x12,2,8,0}, {"shlli",  0x12,2,8,1},
    {"shrli",  0x13,2,8,0}, {"leli",   0x13,2,8,1},
    {"gtli",   0x14,2,8,0}, {"eqli",   0x14,2,8,1},
    {"neli",   0x15,2,8,0}, {"andli",  0x15,2,8,1},
    {"orli",   0x16,2,8,0}, {"xorli",  0x16,2,8,1},
    {"lesli",  0x17,2,8,0}, {"gtsli",  0x17,2,8,1},
    {"divsli", 0x18,2,8,0}, {"modsli", 0x18,2,8,1},
    {"shrsli", 0x19,2,8,0}, {"bitex",  0x19,2,8,1},
    {"rsubli", 0x1a,2,8,0}, {"rdivli", 0x1a,2,8,1},
    {"rmodli", 0x1b,2,8,0}, {"rdivsli",0x1b,2,8,1},
    /* F0c — 3 bytes, rx + imm8 + disp9 (compare-and-branch); subfmt=7 */
    {"cbeq",   0x20,2,7,0}, {"cbne",   0x30,2,7,0},
    /* F1a — 2 bytes, rd rx ry */
    {"add",    0x40,1,0,0}, {"sub",    0x42,1,0,0},
    {"mul",    0x44,1,0,0}, {"div",    0x46,1,0,0},
    {"mod",    0x48,1,0,0}, {"shl",    0x4a,1,0,0},
    {"shr",    0x4c,1,0,0}, {"lt",     0x4e,1,0,0},
    {"le",     0x50,1,0,0}, {"eq",     0x52,1,0,0},
    {"ne",     0x54,1,0,0}, {"and",    0x56,1,0,0},
    {"or",     0x58,1,0,0}, {"xor",    0x5a,1,0,0},
    {"lts",    0x5c,1,0,0}, {"les",    0x5e,1,0,0},
    {"divs",   0x60,1,0,0}, {"mods",   0x62,1,0,0},
    {"shrs",   0x64,1,0,0}, {"fadd",   0x66,1,0,0},
    {"fsub",   0x68,1,0,0}, {"fmul",   0x6a,1,0,0},
    {"fdiv",   0x6c,1,0,0}, {"flt",    0x6e,1,0,0},
    {"fle",    0x70,1,0,0},
    /* F1b — 2 bytes, rd only; subop distinguishes the operation */
    {"sxb",    0x7e,1,1,0x00}, {"sxw",    0x7e,1,1,0x01},
    {"inc",    0x7e,1,1,0x02}, {"dec",    0x7e,1,1,0x03},
    {"pushr",  0x7e,1,1,0x04}, {"popr",   0x7e,1,1,0x05},
    {"zxb",    0x7e,1,1,0x06}, {"zxw",    0x7e,1,1,0x07},
    {"itof",   0x7e,1,1,0x08}, {"ftoi",   0x7e,1,1,0x09},
    {"jlr",    0x7e,1,1,0x0a}, {"jr",     0x7e,1,1,0x0b},
    {"ssp",    0x7e,1,1,0x0c}, {"putchar",0x7e,1,1,0x3f},
    /* F2 — 2 bytes, rx imm7 (bp-relative; imm scaled by access width) */
    {"lb",     0x80,1,0,0}, {"lw",     0x84,1,0,0},
    {"ll",     0x88,1,0,0}, {"sb",     0x8c,1,0,0},
    {"sw",     0x90,1,0,0}, {"sl",     0x94,1,0,0},
    {"lbx",    0x98,1,0,0}, {"lwx",    0x9c,1,0,0},
    {"addi",   0xa0,1,0,0},
    {"shli",   0xa4,1,0,0},
    {"andi",   0xa8,1,0,0},
    {"shrsi",  0xac,1,0,0},
    /* F3a — 3 bytes, imm16 only */
    {"j",      0xc0,2,0,0}, {"jl",     0xc1,2,0,0},
    {"enter",  0xc2,2,0,0},
    /* F3b — 3 bytes, rd + imm14 (byte offset / 4); subfmt=3 */
    {"adjw",   0xc4,2,3,0}, {"lea",    0xc6,2,3,0},
    /* F3c — 3 bytes, rx ry imm10 (0xd0-0xdd) */
    {"llb",    0xd0,2,1,0}, {"llw",    0xd1,2,1,0},
    {"lll",    0xd2,2,1,0}, {"slb",    0xd3,2,1,0},
    {"slw",    0xd4,2,1,0}, {"sll",    0xd5,2,1,0},
    {"llbx",   0xd6,2,1,0}, {"llwx",   0xd7,2,1,0},
    {"beq",    0xd8,2,1,0}, {"bne",    0xd9,2,1,0},
    {"blt",    0xda,2,1,0}, {"ble",    0xdb,2,1,0},
    {"blts",   0xdc,2,1,0}, {"bles",   0xdd,2,1,0},
    /* F3d — 3 bytes, rx + PC-relative imm10; subfmt=4 */
    {"beqz",   0xdf,2,4,0x00}, {"bnez",  0xdf,2,4,0x01},
    /* F3e — 3 bytes, rd imm16 */
    {"immw",   0xe0,2,2,0}, {"immwh",  0xe8,2,2,0},
    {"jz",     0xf0,2,2,0}, {"jnz",    0xf8,2,2,0},
    {NULL,0,0,0,0}
};

static const Instr4 *find_instr4(const char *n)
{
    for (int i = 0; itab4[i].name; i++)
        if (strcmp(itab4[i].name, n) == 0) return &itab4[i];
    return NULL;
}

static int is_branch4(const char *n)
{
    return !strcmp(n,"beq") || !strcmp(n,"bne") || !strcmp(n,"blt") ||
           !strcmp(n,"ble") || !strcmp(n,"blts") || !strcmp(n,"bles");
}

static int is_cbranch4(const char *n)
{
    return !strcmp(n,"cbeq") || !strcmp(n,"cbne");
}

static int parse_reg4(const char *s)
{
    if (s[0] == 'r' && s[1] >= '0' && s[1] <= '7' && s[2] == '\0')
        return s[1] - '0';
    return -1;
}

/* Split on commas and/or whitespace — for CPU4 operand lists. */
static int tokenize4(const char *s, char toks[][MAX_NAME], int max)
{
    int n = 0;
    const char *p = s;
    while (*p && n < max) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p) && *p != ',') p++;
        size_t len = (size_t)(p - start);
        if (len >= MAX_NAME) len = MAX_NAME - 1;
        strncpy(toks[n], start, len);
        toks[n][len] = '\0';
        n++;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Assembler                                                            */
/* ------------------------------------------------------------------ */

/* Trim trailing whitespace in-place. */
static void rtrim(char *s)
{
    char *p = s + strlen(s);
    while (p > s && isspace((unsigned char)p[-1])) *--p = '\0';
}

/* Strip comment (semicolon to end of line). */
static void strip_comment(char *s)
{
    char *p = s;
    while (*p && *p != ';') p++;
    *p = '\0';
    rtrim(s);
}


/* Parse a token value: hex/decimal integer or label name.
   On pass 1, labels resolve to 0. */
static uint32_t parse_tok(const char *s, int pass)
{
    char *end;
    long v = strtol(s, &end, 0);
    if (end != s && (*end == '\0' || isspace((unsigned char)*end))) {
        return (uint32_t)v;
    }
    if (pass == 2) return lookup_sym(s);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Float helpers                                                        */
/* ------------------------------------------------------------------ */

static float  bits2float(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }
static uint32_t float2bits(float f)  { uint32_t b; memcpy(&b, &f, 4); return b; }

/* ------------------------------------------------------------------ */
/* CPU4 assembler                                                       */
/* ------------------------------------------------------------------ */

static void assemble_cpu4(const char *src)
{
    size_t srclen = strlen(src);
    char *buf = malloc(srclen + 2);
    if (!buf) { fprintf(stderr, "oom\n"); exit(1); }
    memcpy(buf, src, srclen + 1);

    for (int pass = 1; pass <= 2; pass++) {
        int cur = 0;
        int lineno = 0;
        char *p = buf;
        while (*p) {
            char *eol = strchr(p, '\n');
            char line[512];
            size_t linelen;
            if (eol) {
                linelen = (size_t)(eol - p);
                if (linelen >= sizeof(line)) linelen = sizeof(line)-1;
                strncpy(line, p, linelen);
                line[linelen] = '\0';
                p = eol + 1;
            } else {
                strncpy(line, p, sizeof(line)-1);
                line[sizeof(line)-1] = '\0';
                p += strlen(p);
            }
            int cur_lineno = lineno++;
            strip_comment(line);
            if (line[0] == '\0') continue;

            char *lp = line;

            /* Section directive */
            if (lp[0] == '.') {
                char *eq = strchr(lp, '=');
                if (eq) cur = (int)strtol(eq+1, NULL, 0);
                continue;
            }

            /* Label */
            if (!isspace((unsigned char)lp[0])) {
                char *colon = strchr(lp, ':');
                if (colon) {
                    char lname[MAX_NAME];
                    size_t nlen = (size_t)(colon - lp);
                    if (nlen >= MAX_NAME) nlen = MAX_NAME-1;
                    strncpy(lname, lp, nlen);
                    lname[nlen] = '\0';
                    for (char *c = lname; *c; c++)
                        if (isspace((unsigned char)*c)) { *c = '\0'; break; }
                    if (pass == 1) add_sym(lname, (uint16_t)cur);
                    lp = colon + 1;
                    while (*lp && isspace((unsigned char)*lp)) lp++;
                    if (*lp == '\0') continue;
                }
            } else {
                while (*lp && isspace((unsigned char)*lp)) lp++;
                if (*lp == '\0') continue;
            }

            /* Split mnemonic from operand string */
            char mnem[MAX_NAME] = {0};
            const char *mp = lp;
            while (*mp && !isspace((unsigned char)*mp)) mp++;
            size_t mlen = (size_t)(mp - lp);
            if (mlen >= MAX_NAME) mlen = MAX_NAME-1;
            strncpy(mnem, lp, mlen);
            while (*mp && isspace((unsigned char)*mp)) mp++;

            /* Tokenize comma-separated operands */
            char ops[8][MAX_NAME];
            int nops = tokenize4(mp, ops, 8);

            /* Directives */
            if (strcmp(mnem, "align") == 0) {
                /* CPU4: align to 4-byte boundary (matches cpu4/assembler.py behaviour) */
                while (cur & 3) { if (pass == 2) write8((uint16_t)cur, 0); cur++; }
                continue;
            }
            if (strcmp(mnem, "allocb") == 0) {
                int n = (nops > 0) ? (int)strtol(ops[0], NULL, 0) : 0;
                cur += n; continue;
            }
            if (strcmp(mnem, "allocw") == 0) {
                int n = (nops > 0) ? (int)strtol(ops[0], NULL, 0) : 0;
                if (cur & 1) cur++;
                cur += n * 2; continue;
            }
            if (strcmp(mnem, "byte") == 0) {
                for (int i = 0; i < nops; i++) {
                    if (pass == 2) write8((uint16_t)cur, (uint8_t)parse_tok(ops[i], pass));
                    cur++;
                }
                continue;
            }
            if (strcmp(mnem, "word") == 0) {
                for (int i = 0; i < nops; i++) {
                    if (pass == 2) write16((uint16_t)cur, (uint16_t)parse_tok(ops[i], pass));
                    cur += 2;
                }
                continue;
            }
            if (strcmp(mnem, "long") == 0) {
                for (int i = 0; i < nops; i++) {
                    if (pass == 2) write32((uint16_t)cur, parse_tok(ops[i], pass));
                    cur += 4;
                }
                continue;
            }
            if (strcmp(mnem, "clearmem") == 0) continue;

            /* Pseudo-ops */
            char real_mnem[MAX_NAME];
            strncpy(real_mnem, mnem, MAX_NAME-1);
            real_mnem[MAX_NAME-1] = '\0';
            if (strcmp(mnem, "mov") == 0 && nops >= 2) {
                /* mov rd, rx  →  or rd, rx, rx */
                strcpy(real_mnem, "or");
                strncpy(ops[2], ops[1], MAX_NAME-1);
                nops = 3;
            } else if (nops >= 3 && (strcmp(mnem,"gt")==0 || strcmp(mnem,"ge")==0 ||
                                     strcmp(mnem,"gts")==0 || strcmp(mnem,"ges")==0 ||
                                     strcmp(mnem,"fgt")==0 || strcmp(mnem,"fge")==0)) {
                /* F1a: swap rx,ry (ops[1],ops[2]) and pick opposite comparison */
                char tmp[MAX_NAME];
                strncpy(tmp,    ops[1], MAX_NAME-1);
                strncpy(ops[1], ops[2], MAX_NAME-1);
                strncpy(ops[2], tmp,    MAX_NAME-1);
                if      (strcmp(mnem,"gt") ==0) strcpy(real_mnem,"lt");
                else if (strcmp(mnem,"ge") ==0) strcpy(real_mnem,"le");
                else if (strcmp(mnem,"gts")==0) strcpy(real_mnem,"lts");
                else if (strcmp(mnem,"ges")==0) strcpy(real_mnem,"les");
                else if (strcmp(mnem,"fgt")==0) strcpy(real_mnem,"flt");
                else                            strcpy(real_mnem,"fle");
            } else if (nops >= 3 && (strcmp(mnem,"bgt")==0 || strcmp(mnem,"bge")==0 ||
                                     strcmp(mnem,"bgts")==0 || strcmp(mnem,"bges")==0)) {
                /* F3b: swap rx,ry (ops[0],ops[1]) and pick opposite branch */
                char tmp[MAX_NAME];
                strncpy(tmp,    ops[0], MAX_NAME-1);
                strncpy(ops[0], ops[1], MAX_NAME-1);
                strncpy(ops[1], tmp,    MAX_NAME-1);
                if      (strcmp(mnem,"bgt") ==0) strcpy(real_mnem,"blt");
                else if (strcmp(mnem,"bge") ==0) strcpy(real_mnem,"ble");
                else if (strcmp(mnem,"bgts")==0) strcpy(real_mnem,"blts");
                else                              strcpy(real_mnem,"bles");
            }

            const Instr4 *instr = find_instr4(real_mnem);
            if (!instr) {
                fprintf(stderr, "cpu4 asm: unknown mnemonic: %s\n", real_mnem);
                exit(1);
            }

            if (pass == 2 && g_profile) prof_lineno[cur] = (int16_t)cur_lineno;
            int instr_addr = cur;
            int instr_len  = 1 + instr->extra;

            if (instr->extra == 0) {
                /* F0: opcode only */
                if (pass == 2) write8((uint16_t)cur, instr->first_byte);
                cur++;
            } else if (instr->extra == 1 && instr->subfmt == 1) {
                /* F1b: rd only */
                int rd2 = (nops > 0) ? parse_reg4(ops[0]) : 0;
                uint8_t b0 = instr->first_byte | (uint8_t)(rd2 >> 2);
                uint8_t b1 = (uint8_t)(((rd2 & 3) << 6) | (instr->subop & 0x3f));
                if (pass == 2) { write8((uint16_t)cur, b0); write8((uint16_t)(cur+1), b1); }
                cur += 2;
            } else if (instr->extra == 1 && (instr->first_byte & 0xc0) == 0x40) {
                /* F1a: rd, rx, ry */
                int rd2 = (nops > 0) ? parse_reg4(ops[0]) : 0;
                int rx2 = (nops > 1) ? parse_reg4(ops[1]) : 0;
                int ry2 = (nops > 2) ? parse_reg4(ops[2]) : 0;
                uint8_t b0 = instr->first_byte | (uint8_t)(rd2 >> 2);
                uint8_t b1 = (uint8_t)(((rd2 & 3) << 6) | (rx2 << 3) | ry2);
                if (pass == 2) { write8((uint16_t)cur, b0); write8((uint16_t)(cur+1), b1); }
                cur += 2;
            } else if (instr->extra == 1 && (instr->first_byte & 0xc0) == 0x80) {
                /* F2: rx, imm7 */
                int rx2 = (nops > 0) ? parse_reg4(ops[0]) : 0;
                int32_t imm7 = (nops > 1) ? (int32_t)strtol(ops[1], NULL, 0) : 0;
                uint8_t b0 = instr->first_byte | (uint8_t)(rx2 >> 1);
                uint8_t b1 = (uint8_t)(((rx2 & 1) << 7) | (imm7 & 0x7f));
                if (pass == 2) { write8((uint16_t)cur, b0); write8((uint16_t)(cur+1), b1); }
                cur += 2;
            } else if (instr->extra == 2 && instr->subfmt == 0) {
                /* F3a: imm16 only */
                uint16_t imm16 = (nops > 0) ? (uint16_t)parse_tok(ops[0], pass) : 0;
                if (pass == 2) {
                    write8((uint16_t)cur,   instr->first_byte);
                    write8((uint16_t)(cur+1), (uint8_t)(imm16 >> 8));
                    write8((uint16_t)(cur+2), (uint8_t)(imm16 & 0xff));
                }
                cur += 3;
            } else if (instr->extra == 2 && instr->subfmt == 1) {
                /* F3b: rx, ry, imm10 */
                int rx2 = (nops > 0) ? parse_reg4(ops[0]) : 0;
                int ry2 = (nops > 1) ? parse_reg4(ops[1]) : 0;
                int32_t imm10;
                if (is_branch4(real_mnem)) {
                    uint16_t tgt = (pass == 2 && nops > 2) ? (uint16_t)parse_tok(ops[2], pass) : 0;
                    imm10 = (int32_t)tgt - (instr_addr + instr_len);
                    if (pass == 2 && (imm10 < -512 || imm10 > 511)) {
                        fprintf(stderr, "error: %s displacement %d out of range at 0x%04x\n",
                                real_mnem, (int)imm10, (unsigned)instr_addr);
                        exit(1);
                    }
                } else {
                    imm10 = (nops > 2) ? (int32_t)strtol(ops[2], NULL, 0) : 0;
                }
                imm10 &= 0x3ff;
                uint8_t b0 = instr->first_byte;
                uint8_t b1 = (uint8_t)((rx2 << 5) | (ry2 << 2) | ((imm10 >> 8) & 3));
                uint8_t b2 = (uint8_t)(imm10 & 0xff);
                if (pass == 2) {
                    write8((uint16_t)cur,   b0);
                    write8((uint16_t)(cur+1), b1);
                    write8((uint16_t)(cur+2), b2);
                }
                cur += 3;
            } else if (instr->extra == 2 && instr->subfmt == 2) {
                /* F3e: rd, imm16 */
                int rd2 = (nops > 0) ? parse_reg4(ops[0]) : 0;
                uint16_t imm16 = (nops > 1) ? (uint16_t)parse_tok(ops[1], pass) : 0;
                uint8_t b0 = (instr->first_byte & 0xf8u) | (uint8_t)(rd2 & 7);
                if (pass == 2) {
                    write8((uint16_t)cur,   b0);
                    write8((uint16_t)(cur+1), (uint8_t)(imm16 >> 8));
                    write8((uint16_t)(cur+2), (uint8_t)(imm16 & 0xff));
                }
                cur += 3;
            } else if (instr->extra == 2 && instr->subfmt == 3) {
                /* new F3b: rd + imm14 (byte offset / 4); adjw has no reg (rd=0) */
                int rd2 = 0;
                int32_t byte_off = 0;
                if (strcmp(real_mnem, "adjw") == 0) {
                    byte_off = (nops > 0) ? (int32_t)strtol(ops[0], NULL, 0) : 0;
                } else { /* lea */
                    rd2 = (nops > 0) ? parse_reg4(ops[0]) : 0;
                    byte_off = (nops > 1) ? (int32_t)strtol(ops[1], NULL, 0) : 0;
                }
                int32_t imm14 = (byte_off >> 2) & 0x3fff;
                uint8_t b0 = (instr->first_byte & 0xfe) | (uint8_t)((rd2 >> 2) & 1);
                uint8_t b1 = (uint8_t)(((rd2 & 3) << 6) | ((imm14 >> 8) & 0x3f));
                uint8_t b2 = (uint8_t)(imm14 & 0xff);
                if (pass == 2) {
                    write8((uint16_t)cur,   b0);
                    write8((uint16_t)(cur+1), b1);
                    write8((uint16_t)(cur+2), b2);
                }
                cur += 3;
            } else if (instr->extra == 2 && instr->subfmt == 7) {
                /* F0c: cbeq/cbne — rx, imm7, label; encoding: 001odddiiiiiiiiiiiiiiiiii
                   imm17 = (imm7 << 10) | (disp10 & 0x3ff) */
                int rx2 = (nops > 0) ? parse_reg4(ops[0]) : 0;
                int32_t imm7 = (nops > 1) ? (int32_t)strtol(ops[1], NULL, 0) : 0;
                uint16_t tgt = (pass == 2 && nops > 2) ? (uint16_t)parse_tok(ops[2], pass) : 0;
                int32_t disp10 = (int32_t)tgt - (instr_addr + instr_len);
                if (pass == 2 && (disp10 < -512 || disp10 > 511)) {
                    fprintf(stderr, "error: %s displacement %d out of range at 0x%04x\n",
                            instr->name, (int)disp10, (unsigned)instr_addr);
                    exit(1);
                }
                int32_t imm17 = ((imm7 & 0x7f) << 10) | (disp10 & 0x3ff);
                /* Encoding: 001o ddd i iiiiiiiiiiiiiiii into 24 bits.
                   byte0[7:5] = 001, byte0[4] = opcode bit (0=cbeq, 1=cbne),
                   byte0[3:1] = rx[2:0], byte0[0] = imm17[16]
                   byte1[7:0] = imm17[15:8]
                   byte2[7:0] = imm17[7:0] */
                uint8_t b0 = (instr->first_byte & 0xf0) | (uint8_t)((rx2 & 7) << 1)
                             | (uint8_t)((imm17 >> 16) & 1);
                uint8_t b1 = (uint8_t)((imm17 >> 8) & 0xff);
                uint8_t b2 = (uint8_t)(imm17 & 0xff);
                if (pass == 2) {
                    write8((uint16_t)cur,   b0);
                    write8((uint16_t)(cur+1), b1);
                    write8((uint16_t)(cur+2), b2);
                }
                cur += 3;
            } else if (instr->extra == 2 && instr->subfmt == 8) {
                /* F0b: rd, rx, imm9 — encoding: 0001oooo odddxxxiiiiiiiii
                   b0 = first_byte (0x10-0x1b)
                   b1 = subop:1 | rd:3 | rx:3 | imm9[8]:1
                   b2 = imm9[7:0] */
                int rd2 = (nops > 0) ? parse_reg4(ops[0]) : 0;
                int rx2 = (nops > 1) ? parse_reg4(ops[1]) : 0;
                int32_t imm9 = (nops > 2) ? (int32_t)strtol(ops[2], NULL, 0) : 0;
                imm9 &= 0x1ff;
                uint8_t b0 = instr->first_byte;
                uint8_t b1 = (uint8_t)((instr->subop << 7) | (rd2 << 4) | (rx2 << 1) | ((imm9 >> 8) & 1));
                uint8_t b2 = (uint8_t)(imm9 & 0xff);
                if (pass == 2) {
                    write8((uint16_t)cur,   b0);
                    write8((uint16_t)(cur+1), b1);
                    write8((uint16_t)(cur+2), b2);
                }
                cur += 3;
            } else if (instr->extra == 2 && instr->subfmt == 4) {
                /* F3d: rx + PC-relative imm10 (beqz, bnez) */
                int rx2 = (nops > 0) ? parse_reg4(ops[0]) : 0;
                uint16_t tgt = (pass == 2 && nops > 1) ? (uint16_t)parse_tok(ops[1], pass) : 0;
                int32_t imm10 = (int32_t)tgt - (instr_addr + instr_len);
                if (pass == 2 && (imm10 < -512 || imm10 > 511)) {
                    fprintf(stderr, "error: %s displacement %d out of range at 0x%04x\n",
                            instr->name, (int)imm10, (unsigned)instr_addr);
                    exit(1);
                }
                imm10 &= 0x3ff;
                uint8_t b0 = 0xdf;
                uint8_t b1 = (uint8_t)((rx2 << 5) | (instr->subop << 2) | ((imm10 >> 8) & 3));
                uint8_t b2 = (uint8_t)(imm10 & 0xff);
                if (pass == 2) {
                    write8((uint16_t)cur,   b0);
                    write8((uint16_t)(cur+1), b1);
                    write8((uint16_t)(cur+2), b2);
                }
                cur += 3;
            } else {
                fprintf(stderr, "cpu4 asm: unhandled format for %s\n", real_mnem);
                exit(1);
            }
        }
        if (pass == 2 && (uint16_t)cur > g_asm_end)
            g_asm_end = (uint16_t)cur;
    }
    free(buf);
}

/* ------------------------------------------------------------------ */
/* Disassembler / dump                                                   */
/* ------------------------------------------------------------------ */

/* Reverse-lookup: (lookupop, subop) → mnemonic name */
static const char *lookup_mnemonic(uint8_t lookupop, int subop)
{
    for (int i = 0; itab4[i].name; i++) {
        if (itab4[i].first_byte == lookupop) {
            if (lookupop == 0x7e || lookupop == 0xdf
                || (lookupop >= 0x10 && lookupop <= 0x1b)) {
                if (itab4[i].subop == subop) return itab4[i].name;
            } else {
                return itab4[i].name;
            }
        }
    }
    return NULL;
}

/* Find a symbol name at a given address (first match) */
static const char *sym_at_addr(uint16_t addr)
{
    for (int i = 0; i < nsyms; i++)
        if (syms[i].addr == addr) return syms[i].name;
    return NULL;
}

/* Find a symbol name for a branch/jump target; returns "" if none */
static const char *sym_for_target(uint16_t addr)
{
    const char *s = sym_at_addr(addr);
    return s ? s : "";
}

static int sym_cmp(const void *a, const void *b)
{
    const Sym *sa = (const Sym *)a;
    const Sym *sb = (const Sym *)b;
    if (sa->addr != sb->addr) return (int)sa->addr - (int)sb->addr;
    return strcmp(sa->name, sb->name);
}

static int32_t sx9(int32_t v);  /* forward decl — defined with other sign-ext helpers below */
static int32_t sx10(int32_t v); /* forward decl — defined with other sign-ext helpers below */

static void print_dump(uint16_t code_end, uint16_t data_end)
{
    FILE *out = stderr;

    /* --- Symbol table --- */
    Sym *sorted = malloc((size_t)nsyms * sizeof(Sym));
    if (!sorted) { fprintf(stderr, "oom\n"); return; }
    memcpy(sorted, syms, (size_t)nsyms * sizeof(Sym));
    qsort(sorted, (size_t)nsyms, sizeof(Sym), sym_cmp);

    fprintf(out, "=== Symbol Table (%d symbols) ===\n", nsyms);
    for (int i = 0; i < nsyms; i++)
        fprintf(out, "  %04x  %s\n", sorted[i].addr, sorted[i].name);
    fprintf(out, "\n");
    free(sorted);

    /* --- Code disassembly --- */
    fprintf(out, "=== Code (0x0000 - 0x%04x) ===\n\n", code_end ? code_end - 1 : 0);

    uint16_t pc = 0;
    while (pc < code_end) {
        /* Print all labels at this address */
        for (int i = 0; i < nsyms; i++)
            if (syms[i].addr == pc) fprintf(out, "%s:\n", syms[i].name);

        uint8_t  b0 = mem[pc];
        uint8_t  b1 = 0, b2 = 0;
        uint8_t  lookupop;
        int      rd = 0, rx = 0, ry = 0, subop = 0;
        int32_t  imm = 0;
        int      len = 1;
        uint8_t  fmt2 = b0 >> 6;

        if (fmt2 == 0 && (b0 & 0xf0) == 0x10) {
            /* F0b: two-op + imm9 — 3 bytes */
            b1 = mem[(uint16_t)(pc+1)];
            b2 = mem[(uint16_t)(pc+2)];
            uint32_t ins24 = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | b2;
            lookupop = b0;  /* 0x10-0x1b */
            subop = (int)((ins24 >> 15) & 1);
            rd = rx = (int)((ins24 >> 12) & 0x7);
            ry = (int)((ins24 >> 9) & 0x7);
            imm = (int32_t)(ins24 & 0x1ff);
            len = 3;
        } else if (fmt2 == 0 && (b0 & 0xe0) == 0x20) {
            /* F0c: cbeq/cbne — 3 bytes */
            b1 = mem[(uint16_t)(pc+1)];
            b2 = mem[(uint16_t)(pc+2)];
            uint32_t ins24 = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | b2;
            lookupop = b0 & 0xf0;
            rd = rx = (int)((ins24 >> 17) & 0x7);
            imm = (int32_t)(ins24 & 0x1ffff);
            len = 3;
        } else if (fmt2 == 0) {
            /* F0a: 1 byte */
            lookupop = b0;
            len = 1;
        } else if (fmt2 == 1) {
            /* F1a or F1b: 2 bytes */
            b1 = mem[(uint16_t)(pc+1)];
            uint16_t ins16 = ((uint16_t)b0 << 8) | b1;
            len = 2;
            if ((b0 & 0xfe) == 0x7e) {
                lookupop = 0x7e;
                rd = (ins16 >> 6) & 0x7;
                subop = b1 & 0x3f;
            } else {
                lookupop = b0 & 0xfe;
                rd = (ins16 >> 6) & 0x7;
                rx = (ins16 >> 3) & 0x7;
                ry = ins16 & 0x7;
            }
        } else if (fmt2 == 2) {
            /* F2: 2 bytes */
            b1 = mem[(uint16_t)(pc+1)];
            uint16_t ins16 = ((uint16_t)b0 << 8) | b1;
            lookupop = b0 & 0xfc;
            rd = rx = (ins16 >> 7) & 0x7;
            imm = ins16 & 0x7f;
            len = 2;
        } else {
            /* F3*: 3 bytes */
            b1 = mem[(uint16_t)(pc+1)];
            b2 = mem[(uint16_t)(pc+2)];
            uint32_t ins24 = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | b2;
            len = 3;

            if ((b0 & 0xfc) == 0xc4) {
                /* F3b: adjw, lea (0xc4-0xc7) */
                lookupop = b0 & 0xfe;
                rd = rx = (int)((ins24 >> 14) & 0x7);
                imm = (int32_t)(ins24 & 0x3fff);
            } else if ((b0 & 0xfc) == 0xc0) {
                /* F3a: j, jl, enter (0xc0-0xc3) */
                lookupop = b0;
                imm = (int32_t)(uint32_t)(((uint16_t)b1 << 8) | b2);
            } else if (b0 == 0xdf) {
                /* F3d: beqz, bnez */
                lookupop = 0xdf;
                rd = rx = (int)((ins24 >> 13) & 0x7);
                subop = (int)((ins24 >> 10) & 0x7);
                imm = (int32_t)(ins24 & 0x3ff);
            } else if ((b0 & 0xf0) == 0xd0) {
                /* F3c (0xd0-0xdd) */
                lookupop = b0;
                rd = rx = (int)((ins24 >> 13) & 0x7);
                ry = (int)((ins24 >> 10) & 0x7);
                imm = (int32_t)(ins24 & 0x3ff);
            } else {
                /* F3e */
                lookupop = b0 & 0xf8;
                rd = b0 & 0x7;
                imm = (int32_t)(uint32_t)(((uint16_t)b1 << 8) | b2);
            }
        }

        const char *mnem = lookup_mnemonic(lookupop, subop);

        /* Format hex bytes */
        char hexbytes[12];
        if (len == 1) snprintf(hexbytes, sizeof(hexbytes), "%02x", b0);
        else if (len == 2) snprintf(hexbytes, sizeof(hexbytes), "%02x %02x", b0, b1);
        else snprintf(hexbytes, sizeof(hexbytes), "%02x %02x %02x", b0, b1, b2);

        /* Format operands */
        char operands[80] = "";
        if (!mnem) {
            snprintf(operands, sizeof(operands), "???");
            mnem = ".byte";
        } else if (fmt2 == 0 && (b0 & 0xf0) == 0x10) {
            /* F0b: two-op + imm9 — rd, rx, imm9 */
            int32_t simm9 = sx9(imm);
            snprintf(operands, sizeof(operands), "r%d, r%d, %d", rd, ry, (int)simm9);
        } else if (fmt2 == 0 && (b0 & 0xe0) == 0x20) {
            /* F0c: cbeq/cbne — rx, imm7, target */
            int32_t imm7_val = (imm >> 10) & 0x7f;
            int32_t disp10 = sx10(imm & 0x3ff);
            uint16_t target = (uint16_t)((int)(pc + len) + disp10);
            const char *tgt = sym_for_target(target);
            if (*tgt) snprintf(operands, sizeof(operands), "r%d, %d, %s", rx, (int)imm7_val, tgt);
            else snprintf(operands, sizeof(operands), "r%d, %d, 0x%04x", rx, (int)imm7_val, (unsigned)target);
        } else if (fmt2 == 0) {
            /* F0a: no operands */
        } else if (fmt2 == 1 && (b0 & 0xfe) == 0x7e) {
            /* F1b: single register */
            snprintf(operands, sizeof(operands), "r%d", rd);
        } else if (fmt2 == 1) {
            /* F1a: three register */
            snprintf(operands, sizeof(operands), "r%d, r%d, r%d", rd, rx, ry);
        } else if (fmt2 == 2) {
            /* F2: register + signed imm7 */
            int32_t simm7 = (imm >= 64) ? imm - 128 : imm;
            snprintf(operands, sizeof(operands), "r%d, %d", rx, (int)simm7);
        } else if ((b0 & 0xfc) == 0xc4) {
            /* F3b: adjw/lea */
            int32_t byte_off = (int32_t)(int16_t)((int16_t)(imm << 2));
            if (lookupop == 0xc4) {
                /* adjw: show byte offset */
                snprintf(operands, sizeof(operands), "%d", (int)byte_off);
            } else {
                /* lea: rd, byte_offset */
                snprintf(operands, sizeof(operands), "r%d, %d", rd, (int)byte_off);
            }
        } else if ((b0 & 0xfc) == 0xc0) {
            /* F3a: j/jl → target, enter → frame size */
            if (lookupop == 0xc2) {
                /* enter: show decimal frame size */
                snprintf(operands, sizeof(operands), "%d", (int)(imm & 0x3fff));
            } else {
                const char *tgt = sym_for_target((uint16_t)imm);
                if (*tgt) snprintf(operands, sizeof(operands), "%s", tgt);
                else snprintf(operands, sizeof(operands), "0x%04x", (unsigned)(imm & 0xffff));
            }
        } else if (b0 == 0xdf) {
            /* F3d: beqz/bnez — rx, target */
            int32_t simm10 = (imm >= 512) ? imm - 1024 : imm;
            uint16_t target = (uint16_t)((int)(pc + len) + simm10);
            const char *tgt = sym_for_target(target);
            if (*tgt) snprintf(operands, sizeof(operands), "r%d, %s", rx, tgt);
            else snprintf(operands, sizeof(operands), "r%d, 0x%04x", rx, (unsigned)target);
        } else if ((b0 & 0xf0) == 0xd0) {
            /* F3c: loads/stores/branches */
            int32_t simm10 = (imm >= 512) ? imm - 1024 : imm;
            if (lookupop >= 0xd8 && lookupop <= 0xdd) {
                /* branch: rx, ry, target */
                uint16_t target = (uint16_t)((int)(pc + len) + simm10);
                const char *tgt = sym_for_target(target);
                if (*tgt) snprintf(operands, sizeof(operands), "r%d, r%d, %s", rx, ry, tgt);
                else snprintf(operands, sizeof(operands), "r%d, r%d, 0x%04x", rx, ry, (unsigned)target);
            } else {
                /* load/store: rx, [ry+imm] */
                snprintf(operands, sizeof(operands), "r%d, [r%d%+d]", rx, ry, (int)simm10);
            }
        } else {
            /* F3e: rd, imm16 */
            if (lookupop == 0xf0 || lookupop == 0xf8) {
                /* jz/jnz: rd, target */
                const char *tgt = sym_for_target((uint16_t)imm);
                if (*tgt) snprintf(operands, sizeof(operands), "r%d, %s", rd, tgt);
                else snprintf(operands, sizeof(operands), "r%d, 0x%04x", rd, (unsigned)(imm & 0xffff));
            } else {
                /* immw/immwh: rd, hex_literal */
                snprintf(operands, sizeof(operands), "r%d, 0x%04x", rd, (unsigned)(imm & 0xffff));
            }
        }

        fprintf(out, "  %04x  %-9s %-7s %s\n", (unsigned)pc, hexbytes, mnem ? mnem : "???", operands);
        pc += (uint16_t)len;
    }

    /* --- Data section --- */
    if (data_end > code_end) {
        fprintf(out, "\n=== Data (0x%04x - 0x%04x) ===\n\n", code_end, data_end - 1);
        uint16_t addr = code_end;
        while (addr < data_end) {
            /* Print all labels at this address */
            for (int i = 0; i < nsyms; i++)
                if (syms[i].addr == addr) fprintf(out, "%s:\n", syms[i].name);

            /* Print up to 16 bytes per row */
            int row_len = data_end - addr;
            if (row_len > 16) row_len = 16;

            /* But break at the next symbol boundary */
            for (int i = 0; i < nsyms; i++) {
                if (syms[i].addr > addr && syms[i].addr < addr + row_len)
                    row_len = syms[i].addr - addr;
            }

            fprintf(out, "  %04x ", (unsigned)addr);
            for (int j = 0; j < row_len; j++)
                fprintf(out, " %02x", mem[(uint16_t)(addr + j)]);
            /* Pad to align ASCII column */
            for (int j = row_len; j < 16; j++)
                fprintf(out, "   ");
            fprintf(out, "  |");
            for (int j = 0; j < row_len; j++) {
                uint8_t c = mem[(uint16_t)(addr + j)];
                fprintf(out, "%c", (c >= 0x20 && c < 0x7f) ? c : '.');
            }
            fprintf(out, "|\n");
            addr += (uint16_t)row_len;
        }
    }

    fprintf(out, "\n");
}

/* ------------------------------------------------------------------ */
/* CPU4 sign-extension helpers                                          */
/* ------------------------------------------------------------------ */

#define MAX_STEPS 100000000
static int g_max_steps = MAX_STEPS;

static int32_t sx7 (int32_t v) { v &= 0x7f;   return (v >= 64)   ? v - 128   : v; }
static int32_t sx9 (int32_t v) { v &= 0x1ff;  return (v >= 256)  ? v - 512   : v; }
static int32_t sx10(int32_t v) { v &= 0x3ff;  return (v >= 512)  ? v - 1024  : v; }
static int32_t sx14(int32_t v) { v &= 0x3fff; return (v >= 8192) ? v - 16384 : v; }
static int32_t sx16(int32_t v) { return (int32_t)(int16_t)(v & 0xffff); }

/* ------------------------------------------------------------------ */
/* CPU4 executor                                                        */
/* ------------------------------------------------------------------ */

static void run_cpu4(int verbose)
{
    uint32_t r[8] = {0};
    uint16_t sp = 0, bp = 0, lr = 0, pc = 0;
    int H = 0;
    g_cycles = 0;

    enum { TRACE_N4 = 32 };
    struct { uint16_t pc; uint8_t op; uint32_t r0; uint16_t sp, bp; } trace[TRACE_N4];
    int trace_idx = 0;

    for (int step = 0; step < g_max_steps && !H; step++, g_cycles++) {
        uint8_t  b0 = read8(pc);
        uint16_t oldpc = pc;
        pc++;
        if (g_profile) prof_count[oldpc]++;

        uint8_t  b1 = 0, b2 = 0;
        int      rd = 0, rx = 0, ry = 0;
        int32_t  imm = 0;
        uint8_t  lookupop;
        int      subop = 0;
        uint8_t  fmt2 = b0 >> 6;

        if (fmt2 == 0 && (b0 & 0xf0) == 0x10) {
            /* F0b: two-op + imm9 — 3 bytes, 0001oooo odddxxxiiiiiiiii */
            b1 = read8(pc); pc++;
            b2 = read8(pc); pc++;
            uint32_t ins24 = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | b2;
            lookupop = b0;                  /* 0x10-0x1b */
            subop    = (int)((ins24 >> 15) & 1);
            rd = rx  = (int)((ins24 >> 12) & 0x7);
            ry       = (int)((ins24 >> 9) & 0x7);
            imm      = (int32_t)(ins24 & 0x1ff);
        } else if (fmt2 == 0 && (b0 & 0xe0) == 0x20) {
            /* F0c: cbeq/cbne — 3 bytes, 001odddiiiiiiiiiiiiiiiiii */
            b1 = read8(pc); pc++;
            b2 = read8(pc); pc++;
            uint32_t ins24 = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | b2;
            lookupop = b0 & 0xf0;           /* 0x20=cbeq, 0x30=cbne */
            rd = rx  = (int)((ins24 >> 17) & 0x7);
            imm      = (int32_t)(ins24 & 0x1ffff);  /* raw imm17 */
        } else if (fmt2 == 0) {
            /* F0a: 1 byte */
            lookupop = b0;
        } else if (fmt2 == 1) {
            /* F1a or F1b: 2 bytes */
            b1 = read8(pc); pc++;
            uint16_t ins16 = ((uint16_t)b0 << 8) | b1;
            if ((b0 & 0xfe) == 0x7e) {
                /* F1b */
                lookupop = 0x7e;
                rd       = (ins16 >> 6) & 0x7;
                subop    = b1 & 0x3f;
            } else {
                /* F1a */
                lookupop = b0 & 0xfe;
                rd = (ins16 >> 6) & 0x7;
                rx = (ins16 >> 3) & 0x7;
                ry =  ins16       & 0x7;
            }
        } else if (fmt2 == 2) {
            /* F2: 2 bytes, bp-relative */
            b1 = read8(pc); pc++;
            uint16_t ins16 = ((uint16_t)b0 << 8) | b1;
            lookupop = b0 & 0xfc;
            rd = rx  = (ins16 >> 7) & 0x7;
            imm      =  ins16       & 0x7f;  /* raw 7-bit; scaled per instruction */
        } else {
            /* F3a / F3b / F3c / F3d / F3e: 3 bytes */
            b1 = read8(pc); pc++;
            b2 = read8(pc); pc++;
            uint32_t ins24 = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | b2;
            if ((b0 & 0xfc) == 0xc4) {
                /* F3b: adjw, lea (0xc4–0xc7); rd + imm14 */
                lookupop = b0 & 0xfe;
                rd = rx  = (int)((ins24 >> 14) & 0x7);
                imm      = (int32_t)(ins24 & 0x3fff);  /* raw imm14 */
            } else if ((b0 & 0xfc) == 0xc0) {
                /* F3a: j, jl, enter (0xc0–0xc3) */
                lookupop = b0;
                imm      = (int32_t)(uint32_t)(((uint16_t)b1 << 8) | b2);
            } else if (b0 == 0xdf) {
                /* F3d: beqz, bnez */
                lookupop = 0xdf;
                rd = rx  = (int)((ins24 >> 13) & 0x7);
                subop    = (int)((ins24 >> 10) & 0x7);
                imm      = (int32_t)(ins24 & 0x3ff);
            } else if ((b0 & 0xf0) == 0xd0) {
                /* F3c: two-reg + imm10 (0xd0–0xdd) */
                lookupop = b0;
                rd = rx  = (int)((ins24 >> 13) & 0x7);
                ry       = (int)((ins24 >> 10) & 0x7);
                imm      = (int32_t)(ins24 & 0x3ff);
            } else {
                /* F3e: one-reg + imm16 (0xe0–0xff) */
                lookupop = b0 & 0xf8;
                rd       = b0 & 0x7;
                imm      = (int32_t)(uint32_t)(((uint16_t)b1 << 8) | b2);
            }
        }

        g_watch_pc = oldpc; g_watch_r0 = r[0]; g_watch_sp = sp; g_watch_bp = bp;
        trace[trace_idx].pc = oldpc; trace[trace_idx].op = b0;
        trace[trace_idx].r0 = r[0]; trace[trace_idx].sp = sp; trace[trace_idx].bp = bp;
        trace_idx = (trace_idx + 1) % TRACE_N4;

        if (verbose) {
            fprintf(stderr, "[%04x] op=%02x r0=%08x r1=%08x r2=%08x r3=%08x sp=%04x bp=%04x\n",
                    oldpc, b0, r[0], r[1], r[2], r[3], sp, bp);
        }

        switch (lookupop) {
        /* F0 */
        case 0x00: H = 1; break;  /* halt */
        case 0x01:                /* ret: unpack lr+bp from one 32-bit word */
            sp = bp;
            bp = (uint16_t)(read32(sp) & 0xffff);
            pc = (uint16_t)(read32(sp) >> 16);
            sp = (uint16_t)(sp + 4);
            break;
        /* F1a */
        case 0x40: r[rd]=r[rx]+r[ry]; break;  /* add */
        case 0x42: r[rd]=r[rx]-r[ry]; break;  /* sub */
        case 0x44: r[rd]=r[rx]*r[ry]; break;  /* mul */
        case 0x46: r[rd]=r[ry]?r[rx]/r[ry]:0; break;  /* div */
        case 0x48: r[rd]=r[ry]?r[rx]%r[ry]:0; break;  /* mod */
        case 0x4a: r[rd]=r[rx]<<(r[ry]&31); break;    /* shl */
        case 0x4c: r[rd]=r[rx]>>(r[ry]&31); break;    /* shr */
        case 0x4e: r[rd]=(r[rx]<r[ry])?1:0; break;    /* lt  */
        case 0x50: r[rd]=(r[rx]<=r[ry])?1:0; break;    /* le  */
        case 0x52: r[rd]=(r[rx]==r[ry])?1:0; break;   /* eq  */
        case 0x54: r[rd]=(r[rx]!=r[ry])?1:0; break;   /* ne  */
        case 0x56: r[rd]=r[rx]&r[ry]; break;  /* and */
        case 0x58: r[rd]=r[rx]|r[ry]; break;  /* or  */
        case 0x5a: r[rd]=r[rx]^r[ry]; break;  /* xor */
        case 0x5c: r[rd]=((int32_t)r[rx]<(int32_t)r[ry])?1:0; break; /* lts */
        case 0x5e: r[rd]=((int32_t)r[rx]<=(int32_t)r[ry])?1:0; break; /* les */
        case 0x60: r[rd]=(uint32_t)(r[ry]?(int32_t)r[rx]/(int32_t)r[ry]:0); break; /* divs */
        case 0x62: r[rd]=(uint32_t)(r[ry]?(int32_t)r[rx]%(int32_t)r[ry]:0); break; /* mods */
        case 0x64: r[rd]=(uint32_t)((int32_t)r[rx]>>(r[ry]&31)); break; /* shrs */
        case 0x66: r[rd]=float2bits(bits2float(r[rx])+bits2float(r[ry])); break; /* fadd */
        case 0x68: r[rd]=float2bits(bits2float(r[rx])-bits2float(r[ry])); break; /* fsub */
        case 0x6a: r[rd]=float2bits(bits2float(r[rx])*bits2float(r[ry])); break; /* fmul */
        case 0x6c: r[rd]=float2bits(bits2float(r[rx])/bits2float(r[ry])); break; /* fdiv */
        case 0x6e: r[rd]=(bits2float(r[rx])<bits2float(r[ry]))?1:0; break; /* flt */
        case 0x70: r[rd]=(bits2float(r[rx])<=bits2float(r[ry]))?1:0; break; /* fle */
        /* F1b */
        case 0x7e:
            if      (subop==0x00) r[rd]=(uint32_t)(int32_t)(int8_t) (r[rd]&0xff);   /* sxb    */
            else if (subop==0x01) r[rd]=(uint32_t)(int32_t)(int16_t)(r[rd]&0xffff); /* sxw    */
            else if (subop==0x02) r[rd]=(r[rd]+1)&0xffffffff;                        /* inc    */
            else if (subop==0x03) r[rd]=(r[rd]-1)&0xffffffff;                        /* dec    */
            else if (subop==0x04) { sp -= 4; write32(sp, r[rd]); }                   /* pushr  */
            else if (subop==0x05) { r[rd] = read32(sp); sp += 4; }                   /* popr   */
            else if (subop==0x06) r[rd] = r[rd] & 0xff;                              /* zxb    */
            else if (subop==0x07) r[rd] = r[rd] & 0xffff;                            /* zxw    */
            else if (subop==0x08) { int32_t iv=(r[rd]<0x80000000u)?(int32_t)r[rd]:(int32_t)(r[rd]-0x100000000ull); r[rd]=float2bits((float)iv); } /* itof */
            else if (subop==0x09) { float f=bits2float(r[rd]); r[rd]=(uint32_t)(int32_t)f; }  /* ftoi */
            else if (subop==0x0a) { uint16_t t=pc; pc=(uint16_t)(r[rd]&0xffff); lr=t; }       /* jlr  */
            else if (subop==0x0b) { pc=(uint16_t)(r[rd]&0xffff); }                             /* jr   */
            else if (subop==0x0c) { sp=(uint16_t)(r[rd]&0xffff); }                             /* ssp  */
            else if (subop==0x3f) { fputc((int)(r[rd]&0xff),stderr); fflush(stderr); }         /* putchar */
            break;
        /* F2 — bp-relative (imm is raw 7-bit; scaled by access size) */
        case 0x80: r[rd]=read8 ((uint16_t)((int32_t)bp+sx7(imm)));     break; /* lb  */
        case 0x84: { uint16_t a = (uint16_t)((int32_t)bp+sx7(imm)*2); check_align16(a, oldpc); r[rd]=read16(a); } break; /* lw  */
        case 0x88: { uint16_t a = (uint16_t)((int32_t)bp+sx7(imm)*4); check_align32(a, oldpc); r[rd]=read32(a); } break; /* ll  */
        case 0x8c: write8 ((uint16_t)((int32_t)bp+sx7(imm)),    (uint8_t) r[rx]); break; /* sb  */
        case 0x90: { uint16_t a = (uint16_t)((int32_t)bp+sx7(imm)*2); check_align16(a, oldpc); write16(a, (uint16_t)r[rx]); } break; /* sw  */
        case 0x94: { uint16_t a = (uint16_t)((int32_t)bp+sx7(imm)*4); check_align32(a, oldpc); write32(a, r[rx]); } break; /* sl  */
        case 0x98: r[rd]=(uint32_t)(int32_t)(int8_t) read8 ((uint16_t)((int32_t)bp+sx7(imm)));   break; /* lbx */
        case 0x9c: { uint16_t a = (uint16_t)((int32_t)bp+sx7(imm)*2); check_align16(a, oldpc); r[rd]=(uint32_t)(int32_t)(int16_t)read16(a); } break; /* lwx */
        case 0xa0: r[rd]=r[rx]+(uint32_t)sx7(imm); break;           /* addi */
        case 0xa4: r[rd]=r[rx]<<(imm&0x1f); break;                /* shli */
        case 0xa8: r[rd]=r[rx]&(uint32_t)imm; break;              /* andi (no sext) */
        case 0xac: r[rd]=(uint32_t)((int32_t)r[rx]>>(imm&0x1f)); break; /* shrsi */
        /* F3a */
        case 0xc0: pc=(uint16_t)imm; break;  /* j   */
        case 0xc1: lr=pc; pc=(uint16_t)imm; break; /* jl  */
        case 0xc2:            /* enter: pack lr+bp into one 32-bit word (4-byte overhead) */
            write32((uint16_t)(sp-4), ((uint32_t)lr << 16) | (uint32_t)bp);
            bp=(uint16_t)(sp-4); sp=(uint16_t)(sp-(uint16_t)imm-4);
            break;
        /* F3b: adjw=0xc4, lea=0xc6 */
        case 0xc4: sp=(uint16_t)(sp+(uint16_t)(sx14(imm)<<2)); break; /* adjw */
        case 0xc6: r[rd]=(uint32_t)((int32_t)bp+(sx14(imm)<<2)); break; /* lea */
        /* F0c: cbeq=0x20, cbne=0x30 — compare rx with imm7 and branch */
        case 0x20: if(r[rx]==(uint32_t)(imm>>10)) pc=(uint16_t)(pc+sx10(imm&0x3ff)); break; /* cbeq */
        case 0x30: if(r[rx]!=(uint32_t)(imm>>10)) pc=(uint16_t)(pc+sx10(imm&0x3ff)); break; /* cbne */
        /* F3c — register-relative */
        case 0xd0: r[rx]=read8 ((uint16_t)((int32_t)r[ry]+sx10(imm)));     break; /* llb  */
        case 0xd1: { uint16_t a = (uint16_t)((int32_t)r[ry]+sx10(imm)*2); check_align16(a, oldpc); r[rx]=read16(a); } break; /* llw  */
        case 0xd2: { uint16_t a = (uint16_t)((int32_t)r[ry]+sx10(imm)*4); check_align32(a, oldpc); r[rx]=read32(a); } break; /* lll  */
        case 0xd3: write8 ((uint16_t)((int32_t)r[ry]+sx10(imm)),    (uint8_t) r[rx]); break; /* slb  */
        case 0xd4: { uint16_t a = (uint16_t)((int32_t)r[ry]+sx10(imm)*2); check_align16(a, oldpc); write16(a, (uint16_t)r[rx]); } break; /* slw  */
        case 0xd5: { uint16_t a = (uint16_t)((int32_t)r[ry]+sx10(imm)*4); check_align32(a, oldpc); write32(a, r[rx]); } break; /* sll  */
        case 0xd6: r[rx]=(uint32_t)(int32_t)(int8_t) read8 ((uint16_t)((int32_t)r[ry]+sx10(imm)));   break; /* llbx */
        case 0xd7: { uint16_t a = (uint16_t)((int32_t)r[ry]+sx10(imm)*2); check_align16(a, oldpc); r[rx]=(uint32_t)(int32_t)(int16_t)read16(a); } break; /* llwx */
        case 0xd8: if(r[rx]==r[ry]) pc=(uint16_t)(pc+sx10(imm)); break; /* beq  */
        case 0xd9: if(r[rx]!=r[ry]) pc=(uint16_t)(pc+sx10(imm)); break; /* bne  */
        case 0xda: if(r[rx]< r[ry]) pc=(uint16_t)(pc+sx10(imm)); break; /* blt  */
        case 0xdb: if(r[rx]<=r[ry]) pc=(uint16_t)(pc+sx10(imm)); break; /* ble  */
        case 0xdc: if((int32_t)r[rx]<(int32_t)r[ry]) pc=(uint16_t)(pc+sx10(imm)); break; /* blts */
        case 0xdd: if((int32_t)r[rx]<=(int32_t)r[ry]) pc=(uint16_t)(pc+sx10(imm)); break; /* bles */
        /* F0b: two-op + imm9 (0x10-0x1b) */
        case 0x10: /* addli (sub=0) / subli (sub=1) */
            if (subop == 0) r[rd] = r[ry] + (uint32_t)sx9(imm);
            else            r[rd] = r[ry] - (uint32_t)sx9(imm);
            break;
        case 0x11: /* mulli (sub=0) / divli (sub=1) */
            if (subop == 0) r[rd] = r[ry] * (uint32_t)sx9(imm);
            else            r[rd] = (imm & 0x1ff) ? r[ry] / (uint32_t)sx9(imm) : 0;
            break;
        case 0x12: /* modli (sub=0) / shlli (sub=1) */
            if (subop == 0) r[rd] = (imm & 0x1ff) ? r[ry] % (uint32_t)sx9(imm) : 0;
            else            r[rd] = r[ry] << (imm & 31);
            break;
        case 0x13: /* shrli (sub=0) / leli (sub=1) */
            if (subop == 0) r[rd] = r[ry] >> (imm & 31);
            else            r[rd] = (r[ry] <= (uint32_t)sx9(imm)) ? 1 : 0;
            break;
        case 0x14: /* gtli (sub=0) / eqli (sub=1) */
            if (subop == 0) r[rd] = (r[ry] > (uint32_t)sx9(imm)) ? 1 : 0;
            else            r[rd] = (r[ry] == (uint32_t)sx9(imm)) ? 1 : 0;
            break;
        case 0x15: /* neli (sub=0) / andli (sub=1) */
            if (subop == 0) r[rd] = (r[ry] != (uint32_t)sx9(imm)) ? 1 : 0;
            else            r[rd] = r[ry] & (uint32_t)sx9(imm);
            break;
        case 0x16: /* orli (sub=0) / xorli (sub=1) */
            if (subop == 0) r[rd] = r[ry] | (uint32_t)sx9(imm);
            else            r[rd] = r[ry] ^ (uint32_t)sx9(imm);
            break;
        case 0x17: /* lesli (sub=0) / gtsli (sub=1) */
            if (subop == 0) r[rd] = ((int32_t)r[ry] <= sx9(imm)) ? 1 : 0;
            else            r[rd] = ((int32_t)r[ry] > sx9(imm)) ? 1 : 0;
            break;
        case 0x18: /* divsli (sub=0) / modsli (sub=1) */
            if (subop == 0) r[rd] = (uint32_t)((imm & 0x1ff) ? (int32_t)r[ry] / sx9(imm) : 0);
            else            r[rd] = (uint32_t)((imm & 0x1ff) ? (int32_t)r[ry] % sx9(imm) : 0);
            break;
        case 0x19: /* shrsli (sub=0) / bitex (sub=1) */
            if (subop == 0) r[rd] = (uint32_t)((int32_t)r[ry] >> (imm & 31));
            else {
                int shift = imm & 0x1f;
                int width = (imm >> 5) & 0x0f;
                uint32_t mask = (2u << width) - 1;
                r[rd] = (r[ry] >> shift) & mask;
            }
            break;
        case 0x1a: /* rsubli (sub=0) / rdivli (sub=1) */
            if (subop == 0) r[rd] = (uint32_t)sx9(imm) - r[ry];
            else            r[rd] = r[ry] ? (uint32_t)(sx9(imm) / (int32_t)r[ry]) : 0;
            break;
        case 0x1b: /* rmodli (sub=0) / rdivsli (sub=1) */
            if (subop == 0) r[rd] = r[ry] ? (uint32_t)(sx9(imm) % (int32_t)r[ry]) : 0;
            else            r[rd] = r[ry] ? (uint32_t)(sx9(imm) / (int32_t)r[ry]) : 0;
            break;
        /* F3d: beqz/bnez */
        case 0xdf:
            if      (subop==0x00) { if(!r[rd]) pc=(uint16_t)(pc+sx10(imm)); } /* beqz */
            else if (subop==0x01) { if( r[rd]) pc=(uint16_t)(pc+sx10(imm)); } /* bnez */
            break;
        /* F3e: one-reg + imm16 */
        case 0xe0: r[rd]=(uint32_t)(uint16_t)imm; break;                              /* immw  */
        case 0xe8: r[rd]=(r[rd]&0xffff)|((uint32_t)(uint16_t)imm<<16); break;        /* immwh */
        case 0xf0: if(!r[rd]) pc=(uint16_t)imm; break;                               /* jz   */
        case 0xf8: if( r[rd]) pc=(uint16_t)imm; break;                               /* jnz  */
        default:
            fprintf(stderr, "cpu4: unknown opcode 0x%02x at pc=%04x\n", b0, oldpc);
            fprintf(stderr, "Last %d instructions:\n", TRACE_N4);
            for (int _ti = 0; _ti < TRACE_N4; _ti++) {
                int _idx = (trace_idx + _ti) % TRACE_N4;
                fprintf(stderr, "  [%04x] op=%02x r0=%08x sp=%04x bp=%04x\n",
                        trace[_idx].pc, trace[_idx].op, trace[_idx].r0,
                        trace[_idx].sp, trace[_idx].bp);
            }
            H = 1;
            break;
        }

        for (int i = 0; i < 8; i++) r[i] &= 0xffffffff;
        sp &= 0xffff; bp &= 0xffff; lr &= 0xffff; pc &= 0xffff;

        /* SP/BP must be 4-byte aligned at all times */
        if (sp & 3) {
            fprintf(stderr, "CPU4 alignment error: SP misaligned 0x%04x at pc=0x%04x\n", sp, oldpc);
            H = 1;
        }
        if (bp & 3) {
            fprintf(stderr, "CPU4 alignment error: BP misaligned 0x%04x at pc=0x%04x\n", bp, oldpc);
            H = 1;
        }
    }

    printf("r0:%08x r1:%08x r2:%08x r3:%08x r4:%08x r5:%08x r6:%08x r7:%08x sp:%04x bp:%04x lr:%04x pc:%04x H:%x cycles:%u\n",
           r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], sp, bp, lr, pc, H, g_cycles);
}

/* ------------------------------------------------------------------ */
/* CPU                                                                  */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Main                                                                 */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 2);
    if (!buf) { fprintf(stderr, "oom\n"); exit(1); }
    size_t n = fread(buf, 1, (size_t)len, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    int verbose = 0;
    const char *filename = NULL;
    int maxsteps_override = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else if (strcmp(argv[i], "-profile") == 0) g_profile = 1;
        else if (strcmp(argv[i], "-dump") == 0) g_dump = 1;
        else if (strcmp(argv[i], "-arch") == 0 && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "cpu4") != 0) {
                fprintf(stderr, "sim_c: unsupported arch '%s' (only cpu4 is supported)\n", argv[i]);
                return 1;
            }
        }
        else if (strcmp(argv[i], "-maxsteps") == 0 && i+1 < argc) {
            maxsteps_override = atoi(argv[++i]);
        }
        else filename = argv[i];
    }
    if (!filename) {
        fprintf(stderr, "usage: sim_c [-v] [-dump] [-profile] [-arch cpu4] [-maxsteps N] file.s\n");
        return 1;
    }

    if (maxsteps_override > 0) g_max_steps = maxsteps_override;
    if (g_profile) memset(prof_lineno, 0xff, sizeof(prof_lineno));  /* -1 = no mapping */
    char *src = read_file(filename);
    g_assembler_done = 0;
    if (g_profile) prof_split_lines(src);
    assemble_cpu4(src);
    free(src);
    { int i = find_sym("_globals_start"); if (i >= 0) g_write_threshold = syms[i].addr; }
    g_assembler_done = 1;
    if (g_dump) {
        uint16_t code_end = g_asm_end;
        { int i = find_sym("_globals_start"); if (i >= 0) code_end = syms[i].addr; }
        print_dump(code_end, g_asm_end);
        return 0;
    }
    run_cpu4(verbose);
    if (g_profile) print_profile();
    return 0;
}
