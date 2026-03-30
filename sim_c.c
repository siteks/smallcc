/*
 * sim_c.c — CPU3/CPU4 assembler + simulator
 *
 * Usage:  ./sim_c [-v] [-arch cpu3|cpu4] file.s
 * Output: putchar to stderr; state line to stdout
 *   cpu3:  r0:XXXXXXXX sp:XXXX bp:XXXX lr:XXXX pc:XXXX H:X cycles:N
 *   cpu4:  r0:..r7:XXXXXXXX sp:XXXX bp:XXXX lr:XXXX pc:XXXX H:X cycles:N
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
/* Instruction table                                                    */
/* ------------------------------------------------------------------ */

typedef struct { const char *name; uint8_t op; int fmt; } Instr;
static const Instr itab[] = {
    {"halt",0x00,0},{"ret",0x01,0},{"push",0x02,0},{"pushw",0x03,0},
    {"pop",0x04,0},{"popw",0x05,0},{"lb",0x06,0},{"lw",0x07,0},
    {"ll",0x08,0},{"sb",0x09,0},{"sw",0x0a,0},{"sl",0x0b,0},
    {"add",0x0c,0},{"sub",0x0d,0},{"mul",0x0e,0},{"div",0x0f,0},
    {"mod",0x10,0},{"shl",0x11,0},{"shr",0x12,0},{"lt",0x13,0},
    {"le",0x14,0},{"gt",0x15,0},{"ge",0x16,0},{"eq",0x17,0},
    {"ne",0x18,0},{"and",0x19,0},{"or",0x1a,0},{"xor",0x1b,0},
    {"sxb",0x1c,0},{"sxw",0x1d,0},{"putchar",0x1e,0},{"jli",0x1f,0},
    {"fadd",0x20,0},{"fsub",0x21,0},{"fmul",0x22,0},{"fdiv",0x23,0},
    {"flt",0x24,0},{"fle",0x25,0},{"fgt",0x26,0},{"fge",0x27,0},
    {"itof",0x28,0},{"ftoi",0x29,0},
    {"lts",0x2a,0},{"les",0x2b,0},{"gts",0x2c,0},{"ges",0x2d,0},
    {"divs",0x2e,0},{"mods",0x2f,0},{"shrs",0x30,0},
    {"immb",0x40,1},{"adj",0x41,1},
    {"immw",0x80,2},{"immwh",0x81,2},{"j",0x82,2},{"jl",0x83,2},
    {"jz",0x84,2},{"jnz",0x85,2},{"enter",0x86,2},{"lea",0x87,2},
    {"ssp",0x88,2},{"adjw",0x89,2},
    {NULL,0,0}
};

static const Instr *find_instr(const char *n)
{
    for (int i = 0; itab[i].name; i++)
        if (strcmp(itab[i].name, n) == 0) return &itab[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* CPU4 instruction table                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    uint8_t     first_byte;
    int         extra;   /* extra bytes: 0, 1, or 2 */
    int         subfmt;  /* 0=F0/F1a/F2/F3a, 1=F1b/F3b, 2=F3c */
    int         subop;   /* F1b: subopcode in byte1[5:0] */
} Instr4;

static const Instr4 itab4[] = {
    /* F0 — 1 byte, no operands */
    {"halt",   0x00,0,0,0}, {"ret",    0x01,0,0,0},
    {"itof",   0x02,0,0,0}, {"ftoi",   0x03,0,0,0},
    {"jlr",    0x04,0,0,0}, {"push",   0x05,0,0,0}, {"pop",    0x06,0,0,0},
    {"putchar",0x1e,0,0,0},
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
    {"sxb",    0x7e,1,1,0x00}, {"sxw",   0x7e,1,1,0x01},
    {"inc",    0x7e,1,1,0x02}, {"dec",   0x7e,1,1,0x03},
    {"pushr",  0x7e,1,1,0x04}, {"popr",  0x7e,1,1,0x05},
    /* F2 — 2 bytes, rx imm7 (bp-relative; imm scaled by access width) */
    {"lb",     0x80,1,0,0}, {"lw",     0x84,1,0,0},
    {"ll",     0x88,1,0,0}, {"sb",     0x8c,1,0,0},
    {"sw",     0x90,1,0,0}, {"sl",     0x94,1,0,0},
    {"lbx",    0x98,1,0,0}, {"lwx",    0x9c,1,0,0},
    {"addi",   0xa0,1,0,0},
    /* F3a — 3 bytes, imm16 only */
    {"j",      0xc0,2,0,0}, {"jl",     0xc1,2,0,0},
    {"jz",     0xc2,2,0,0}, {"jnz",    0xc3,2,0,0},
    {"enter",  0xc4,2,0,0}, {"ssp",    0xc5,2,0,0},
    {"adjw",   0xc6,2,0,0},
    /* F3b — 3 bytes, rx ry imm10 */
    {"llb",    0xd0,2,1,0}, {"llw",    0xd1,2,1,0},
    {"lll",    0xd2,2,1,0}, {"slb",    0xd3,2,1,0},
    {"slw",    0xd4,2,1,0}, {"sll",    0xd5,2,1,0},
    {"llbx",   0xd6,2,1,0}, {"llwx",   0xd7,2,1,0},
    {"beq",    0xd8,2,1,0}, {"bne",    0xd9,2,1,0},
    {"blt",    0xda,2,1,0}, {"ble",    0xdb,2,1,0},
    {"blts",   0xdc,2,1,0}, {"bles",   0xdd,2,1,0},
    {"addli",  0xde,2,1,0},
    /* F3c — 3 bytes, rd imm16 */
    {"immw",   0xe8,2,2,0}, {"immwh",  0xf0,2,2,0},
    {"lea",    0xf8,2,2,0},
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
   On pass 1, labels resolve to 0.
   Returns the numeric value. */
static uint32_t parse_tok(const char *s, int pass)
{
    char *end;
    long v = strtol(s, &end, 0);
    if (end != s && (*end == '\0' || isspace((unsigned char)*end))) {
        return (uint32_t)v;
    }
    /* label reference */
    if (pass == 2) return lookup_sym(s);
    return 0;
}

/* Read up to max tokens from s into toks[]. Returns count.
   Tokens are copies of whitespace-separated words. */
static int tokenize(const char *s, char toks[][MAX_NAME], int max)
{
    int n = 0;
    const char *p = s;
    while (*p && n < max) {
        while (*p && isspace((unsigned char)*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        size_t len = (size_t)(p - start);
        if (len >= MAX_NAME) len = MAX_NAME-1;
        strncpy(toks[n], start, len);
        toks[n][len] = '\0';
        n++;
    }
    return n;
}

static void assemble(const char *src)
{
    /* Make a mutable copy for parsing */
    size_t srclen = strlen(src);
    char *buf = malloc(srclen + 2);
    if (!buf) { fprintf(stderr, "oom\n"); exit(1); }
    memcpy(buf, src, srclen + 1);

    for (int pass = 1; pass <= 2; pass++) {
        int cur = 0;  /* current byte address */
        int lineno = 0;

        char *p = buf;
        while (*p) {
            /* Extract one line */
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

            /* Check for section directive: .text=N or .data=N */
            if (lp[0] == '.') {
                /* Find '=' if present */
                char *eq = strchr(lp, '=');
                if (eq) {
                    cur = (int)strtol(eq+1, NULL, 0);
                }
                /* else: no explicit address; continue from cur */
                continue;
            }

            /* Check for label: starts at column 0 (no leading space) */
            if (!isspace((unsigned char)lp[0])) {
                char *colon = strchr(lp, ':');
                if (colon) {
                    /* Extract label name */
                    char lname[MAX_NAME];
                    size_t nlen = (size_t)(colon - lp);
                    if (nlen >= MAX_NAME) nlen = MAX_NAME-1;
                    strncpy(lname, lp, nlen);
                    lname[nlen] = '\0';
                    /* Trim whitespace from label name */
                    for (char *c = lname; *c; c++)
                        if (isspace((unsigned char)*c)) { *c = '\0'; break; }
                    if (pass == 1) add_sym(lname, (uint16_t)cur);
                    lp = colon + 1;
                    while (*lp && isspace((unsigned char)*lp)) lp++;
                    /* If nothing after label, continue */
                    if (*lp == '\0') continue;
                }
            } else {
                /* Skip leading whitespace */
                while (*lp && isspace((unsigned char)*lp)) lp++;
                if (*lp == '\0') continue;
            }

            /* Tokenize remainder: mnemonic + operands */
            char toks[16][MAX_NAME];
            int ntok = tokenize(lp, toks, 16);
            if (ntok == 0) continue;

            const char *mnem = toks[0];

            /* Handle directives */
            if (strcmp(mnem, "align") == 0) {
                if (cur & 1) {
                    if (pass == 2) write8((uint16_t)cur, 0);
                    cur++;
                }
                continue;
            }
            if (strcmp(mnem, "allocb") == 0) {
                int n = (ntok > 1) ? (int)strtol(toks[1], NULL, 0) : 0;
                cur += n;
                continue;
            }
            if (strcmp(mnem, "allocw") == 0) {
                int n = (ntok > 1) ? (int)strtol(toks[1], NULL, 0) : 0;
                if (cur & 1) cur++;  /* word align */
                cur += n * 2;
                continue;
            }
            if (strcmp(mnem, "byte") == 0) {
                for (int i = 1; i < ntok; i++) {
                    if (pass == 2)
                        write8((uint16_t)cur, (uint8_t)parse_tok(toks[i], pass));
                    cur++;
                }
                continue;
            }
            if (strcmp(mnem, "word") == 0) {
                for (int i = 1; i < ntok; i++) {
                    if (pass == 2)
                        write16((uint16_t)cur, (uint16_t)parse_tok(toks[i], pass));
                    cur += 2;
                }
                continue;
            }
            if (strcmp(mnem, "long") == 0) {
                for (int i = 1; i < ntok; i++) {
                    if (pass == 2)
                        write32((uint16_t)cur, parse_tok(toks[i], pass));
                    cur += 4;
                }
                continue;
            }

            /* clearmem: pseudo-instruction — no-op in simulator (memory is zero-initialised).
               On physical hardware, replace with a real clear-memory loop in crt0.s. */
            if (strcmp(mnem, "clearmem") == 0) {
                continue;
            }

            /* Regular instruction */
            const Instr *instr = find_instr(mnem);
            if (!instr) {
                fprintf(stderr, "unknown mnemonic: %s\n", mnem);
                exit(1);
            }

            if (pass == 2 && g_profile) prof_lineno[cur] = (int16_t)cur_lineno;

            if (instr->fmt == 0) {
                if (pass == 2) write8((uint16_t)cur, instr->op);
                cur++;
            } else if (instr->fmt == 1) {
                /* signed 8-bit immediate */
                int8_t imm8 = (ntok > 1) ? (int8_t)(int)strtol(toks[1], NULL, 0) : 0;
                if (pass == 2) {
                    write8((uint16_t)cur, instr->op);
                    write8((uint16_t)(cur+1), (uint8_t)imm8);
                }
                cur += 2;
            } else {
                /* fmt == 2: 16-bit immediate (possibly a label) */
                uint16_t imm16 = 0;
                if (ntok > 1) {
                    if (pass == 2)
                        imm16 = (uint16_t)parse_tok(toks[1], pass);
                }
                if (pass == 2) {
                    write8((uint16_t)cur, instr->op);
                    write16((uint16_t)(cur+1), imm16);
                }
                cur += 3;
            }
        }
    }
    free(buf);
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
                if (cur & 1) { if (pass == 2) write8((uint16_t)cur, 0); cur++; }
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
                /* F3c: rd, imm16 */
                int rd2 = (nops > 0) ? parse_reg4(ops[0]) : 0;
                uint16_t imm16 = (nops > 1) ? (uint16_t)parse_tok(ops[1], pass) : 0;
                uint8_t b0 = (instr->first_byte & 0xf8u) | (uint8_t)(rd2 & 7);
                if (pass == 2) {
                    write8((uint16_t)cur,   b0);
                    write8((uint16_t)(cur+1), (uint8_t)(imm16 >> 8));
                    write8((uint16_t)(cur+2), (uint8_t)(imm16 & 0xff));
                }
                cur += 3;
            } else {
                fprintf(stderr, "cpu4 asm: unhandled format for %s\n", real_mnem);
                exit(1);
            }
        }
    }
    free(buf);
}

/* ------------------------------------------------------------------ */
/* CPU4 sign-extension helpers                                          */
/* ------------------------------------------------------------------ */

#define MAX_STEPS 100000000
static int g_max_steps = MAX_STEPS;

static int32_t sx7 (int32_t v) { v &= 0x7f;  return (v >= 64)  ? v - 128  : v; }
static int32_t sx10(int32_t v) { v &= 0x3ff; return (v >= 512) ? v - 1024 : v; }
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

        if (fmt2 == 0) {
            /* F0: 1 byte */
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
            /* F3a / F3b / F3c: 3 bytes */
            b1 = read8(pc); pc++;
            b2 = read8(pc); pc++;
            uint32_t ins24 = ((uint32_t)b0 << 16) | ((uint32_t)b1 << 8) | b2;
            if ((b0 & 0xf0) == 0xc0) {
                /* F3a */
                lookupop = b0;
                imm      = (int32_t)(uint32_t)(((uint16_t)b1 << 8) | b2);
            } else if ((b0 & 0xf0) == 0xd0) {
                /* F3b */
                lookupop = b0;
                rd = rx  = (ins24 >> 13) & 0x7;
                ry       = (ins24 >> 10) & 0x7;
                imm      = (int32_t)(ins24 & 0x3ff);
            } else {
                /* F3c (0xe0-0xff) */
                lookupop = b0 & 0xf8;
                rd       =  b0 & 0x7;
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
        case 0x01:                /* ret */
            sp = bp;
            bp = (uint16_t)read32(sp);
            pc = (uint16_t)read32((uint16_t)(sp+4));
            sp = (uint16_t)(sp + 8);
            break;
        case 0x02: { int32_t iv=(r[0]<0x80000000u)?(int32_t)r[0]:(int32_t)(r[0]-0x100000000ull); r[0]=float2bits((float)iv); } break; /* itof */
        case 0x03: { float f=bits2float(r[0]); r[0]=(uint32_t)(int32_t)f; } break; /* ftoi */
        case 0x04: { uint16_t t=pc; pc=(uint16_t)(r[0]&0xffff); lr=t; } break; /* jlr */
        case 0x05:                /* push */
            sp = (uint16_t)(sp - 4);
            write32(sp, r[0]);
            break;
        case 0x06:                /* pop */
            r[0] = read32(sp);
            sp = (uint16_t)(sp + 4);
            break;
        case 0x1e: fputc((int)(r[0]&0xff),stderr); fflush(stderr); break; /* putchar */
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
            if      (subop==0x00) r[rd]=(uint32_t)(int32_t)(int8_t) (r[rd]&0xff);   /* sxb */
            else if (subop==0x01) r[rd]=(uint32_t)(int32_t)(int16_t)(r[rd]&0xffff); /* sxw */
            else if (subop==0x02) r[rd]=(r[rd]+1)&0xffffffff;                        /* inc */
            else if (subop==0x03) r[rd]=(r[rd]-1)&0xffffffff;                        /* dec */
            else if (subop==0x04) { sp -= 4; write32(sp, r[rd]); }                   /* pushr */
            else if (subop==0x05) { r[rd] = read32(sp); sp += 4; }                   /* popr */
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
        case 0xa0: r[rd]=r[rx]+(uint32_t)sx7(imm); break; /* addi */
        /* F3a */
        case 0xc0: pc=(uint16_t)imm; break;  /* j   */
        case 0xc1: lr=pc; pc=(uint16_t)imm; break; /* jl  */
        case 0xc2: if(!r[0]) pc=(uint16_t)imm; break; /* jz  */
        case 0xc3: if( r[0]) pc=(uint16_t)imm; break; /* jnz */
        case 0xc4:            /* enter */
            write32((uint16_t)(sp-4),lr); write32((uint16_t)(sp-8),bp);
            bp=(uint16_t)(sp-8); sp=(uint16_t)(sp-(uint16_t)imm-8);
            break;
        case 0xc5: sp=(uint16_t)imm; break;  /* ssp  */
        case 0xc6: sp=(uint16_t)(sp+sx16(imm)); break; /* adjw */
        /* F3b — register-relative */
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
        case 0xde: r[rx]=r[ry]+(uint32_t)sx10(imm); break; /* addli */
        /* F3c */
        case 0xe8: r[rd]=(uint32_t)(uint16_t)imm; break; /* immw  */
        case 0xf0: r[rd]=(r[rd]&0xffff)|((uint32_t)(uint16_t)imm<<16); break; /* immwh */
        case 0xf8: r[rd]=(uint32_t)((int32_t)bp+sx16(imm)); break; /* lea   */
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

static void run_cpu(int verbose)
{
    uint32_t r0 = 0;
    uint16_t sp = 0, bp = 0, lr = 0, pc = 0;
    int H = 0;
    g_cycles = 0;

    for (int step = 0; step < g_max_steps && !H; step++, g_cycles++) {
        uint8_t op = read8(pc);
        uint16_t oldpc = pc;
        pc++;
        if (g_profile) prof_count[oldpc]++;

        int8_t  imm8  = 0;
        uint16_t imm16 = 0;

        if ((op & 0xc0) == 0x40) {
            imm8 = (int8_t)read8(pc); pc++;
        } else if ((op & 0xc0) == 0x80) {
            imm16 = read16(pc); pc += 2;
        }

        g_watch_pc = oldpc;
        g_watch_r0 = r0;
        g_watch_sp = sp;
        g_watch_bp = bp;

        if (verbose) {
            fprintf(stderr, "[%04x] op=%02x r0=%08x sp=%04x bp=%04x\n",
                    oldpc, op, r0, sp, bp);
        }

        /* Ring buffer: record last 32 instructions for crash dumps */
        enum { TRACE_N = 32 };
        static struct { uint16_t pc; uint8_t op; uint32_t r0; uint16_t sp, bp; } trace[TRACE_N];
        static int trace_idx = 0;
        trace[trace_idx].pc = oldpc; trace[trace_idx].op = op;
        trace[trace_idx].r0 = r0;   trace[trace_idx].sp = sp; trace[trace_idx].bp = bp;
        trace_idx = (trace_idx + 1) % TRACE_N;

        switch (op) {
        case 0x00: /* halt */    H = 1; break;
        case 0x01: /* ret  */    sp = bp; bp = (uint16_t)read32(sp); pc = (uint16_t)read32((uint16_t)(sp+4)); sp = (uint16_t)(sp+8); break;
        case 0x02: /* push */    sp = (uint16_t)(sp-4); write32(sp, r0); break;
        case 0x03: /* pushw */   sp = (uint16_t)(sp-2); write16(sp, (uint16_t)r0); break;
        case 0x04: /* pop  */    r0 = read32(sp); sp = (uint16_t)(sp+4); break;
        case 0x05: /* popw */    r0 = read16(sp); sp = (uint16_t)(sp+2); break;
        case 0x06: /* lb   */    r0 = read8((uint16_t)r0); break;
        case 0x07: /* lw   */    r0 = read16((uint16_t)r0); break;
        case 0x08: /* ll   */    r0 = read32((uint16_t)r0); break;
        case 0x09: /* sb   */    { uint16_t a=(uint16_t)read32(sp); sp=(uint16_t)(sp+4); write8(a,(uint8_t)r0); } break;
        case 0x0a: /* sw   */    { uint16_t a=(uint16_t)read32(sp); sp=(uint16_t)(sp+4); write16(a,(uint16_t)r0); } break;
        case 0x0b: /* sl   */    { uint16_t a=(uint16_t)read32(sp); sp=(uint16_t)(sp+4); write32(a,r0); } break;
        case 0x0c: /* add  */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=l+r0; } break;
        case 0x0d: /* sub  */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=l-r0; } break;
        case 0x0e: /* mul  */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=l*r0; } break;
        case 0x0f: /* div  */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=(r0?l/r0:0); } break;
        case 0x10: /* mod  */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=(r0?l%r0:0); } break;
        case 0x11: /* shl  */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=l<<(r0&31); } break;
        case 0x12: /* shr  */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=l>>(r0&31); } break;
        case 0x13: /* lt   */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=(l<r0)?1:0; } break;
        case 0x14: /* le   */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=(l<=r0)?1:0; } break;
        case 0x15: /* gt   */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=(l>r0)?1:0; } break;
        case 0x16: /* ge   */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=(l>=r0)?1:0; } break;
        case 0x17: /* eq   */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=(l==r0)?1:0; } break;
        case 0x18: /* ne   */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=(l!=r0)?1:0; } break;
        case 0x19: /* and  */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=l&r0; } break;
        case 0x1a: /* or   */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=l|r0; } break;
        case 0x1b: /* xor  */    { uint32_t l=read32(sp); sp=(uint16_t)(sp+4); r0=l^r0; } break;
        case 0x1c: /* sxb  */    r0 = (uint32_t)(int32_t)(int8_t)(r0 & 0xff); break;
        case 0x1d: /* sxw  */    r0 = (uint32_t)(int32_t)(int16_t)(r0 & 0xffff); break;
        case 0x1e: /* putchar */ fputc((int)(r0 & 0xff), stderr); fflush(stderr); break;
        case 0x1f: /* jli  */    lr = pc; pc = (uint16_t)(r0 & 0xffff); break;
        case 0x20: /* fadd */    { float l=bits2float(read32(sp)); sp=(uint16_t)(sp+4); r0=float2bits(l+bits2float(r0)); } break;
        case 0x21: /* fsub */    { float l=bits2float(read32(sp)); sp=(uint16_t)(sp+4); r0=float2bits(l-bits2float(r0)); } break;
        case 0x22: /* fmul */    { float l=bits2float(read32(sp)); sp=(uint16_t)(sp+4); r0=float2bits(l*bits2float(r0)); } break;
        case 0x23: /* fdiv */    { float l=bits2float(read32(sp)); sp=(uint16_t)(sp+4); r0=float2bits(l/bits2float(r0)); } break;
        case 0x24: /* flt  */    { float l=bits2float(read32(sp)); sp=(uint16_t)(sp+4); r0=(l<bits2float(r0))?1:0; } break;
        case 0x25: /* fle  */    { float l=bits2float(read32(sp)); sp=(uint16_t)(sp+4); r0=(l<=bits2float(r0))?1:0; } break;
        case 0x26: /* fgt  */    { float l=bits2float(read32(sp)); sp=(uint16_t)(sp+4); r0=(l>bits2float(r0))?1:0; } break;
        case 0x27: /* fge  */    { float l=bits2float(read32(sp)); sp=(uint16_t)(sp+4); r0=(l>=bits2float(r0))?1:0; } break;
        case 0x28: /* itof */    { int32_t iv=(int32_t)r0; r0=float2bits((float)iv); } break;
        case 0x29: /* ftoi */    { float f=bits2float(r0); r0=(uint32_t)(int32_t)f; } break;
        case 0x2a: /* lts  */    { int32_t l=(int32_t)read32(sp); sp=(uint16_t)(sp+4); r0=(l<(int32_t)r0)?1:0; } break;
        case 0x2b: /* les  */    { int32_t l=(int32_t)read32(sp); sp=(uint16_t)(sp+4); r0=(l<=(int32_t)r0)?1:0; } break;
        case 0x2c: /* gts  */    { int32_t l=(int32_t)read32(sp); sp=(uint16_t)(sp+4); r0=(l>(int32_t)r0)?1:0; } break;
        case 0x2d: /* ges  */    { int32_t l=(int32_t)read32(sp); sp=(uint16_t)(sp+4); r0=(l>=(int32_t)r0)?1:0; } break;
        case 0x2e: /* divs */    { int32_t l=(int32_t)read32(sp); sp=(uint16_t)(sp+4); r0=(uint32_t)(r0?(int32_t)l/(int32_t)r0:0); } break;
        case 0x2f: /* mods */    { int32_t l=(int32_t)read32(sp); sp=(uint16_t)(sp+4); r0=(uint32_t)(r0?(int32_t)l%(int32_t)r0:0); } break;
        case 0x30: /* shrs */    { int32_t l=(int32_t)read32(sp); sp=(uint16_t)(sp+4); r0=(uint32_t)(l>>(r0&31)); } break;
        case 0x40: /* immb */    r0 = (uint32_t)(int32_t)imm8; break;
        case 0x41: /* adj  */    sp = (uint16_t)(sp + imm8); break;
        case 0x80: /* immw */    r0 = imm16; break;
        case 0x81: /* immwh */   r0 = (r0 & 0xffff) | ((uint32_t)imm16 << 16); break;
        case 0x82: /* j    */    pc = imm16; break;
        case 0x83: /* jl   */    lr = pc; pc = imm16; break;
        case 0x84: /* jz   */    if (!r0) pc = imm16; break;
        case 0x85: /* jnz  */    if ( r0) pc = imm16; break;
        case 0x86: /* enter */   write32((uint16_t)(sp-4),lr); write32((uint16_t)(sp-8),bp); bp=(uint16_t)(sp-8); sp=(uint16_t)(sp-imm16-8); break;
        case 0x87: /* lea  */    r0 = (uint32_t)((int32_t)bp + (int32_t)(int16_t)imm16); break;
        case 0x88: /* ssp  */    sp = imm16; break;
        case 0x89: /* adjw */    sp = (uint16_t)(sp + (int16_t)imm16); break;
        default:
            fprintf(stderr, "unknown opcode 0x%02x at pc=%04x\n", op, oldpc);
            fprintf(stderr, "Last %d instructions:\n", TRACE_N);
            for (int _ti = 0; _ti < TRACE_N; _ti++) {
                int _idx = (trace_idx + _ti) % TRACE_N;
                fprintf(stderr, "  [%04x] op=%02x r0=%08x sp=%04x bp=%04x\n",
                        trace[_idx].pc, trace[_idx].op, trace[_idx].r0, trace[_idx].sp, trace[_idx].bp);
            }
            H = 1;
            break;
        }

        /* Mask registers to their widths */
        r0 &= 0xffffffff;
        sp &= 0xffff;
        bp &= 0xffff;
        lr &= 0xffff;
        pc &= 0xffff;
    }

    printf("r0:%08x sp:%04x bp:%04x lr:%04x pc:%04x H:%x cycles:%u\n",
           r0, sp, bp, lr, pc, H, g_cycles);
}

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
    int arch = 3;
    const char *filename = NULL;
    int maxsteps_override = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else if (strcmp(argv[i], "-profile") == 0) g_profile = 1;
        else if (strcmp(argv[i], "-arch") == 0 && i+1 < argc) {
            i++;
            if (strcmp(argv[i], "cpu4") == 0) arch = 4;
            else arch = 3;
        }
        else if (strcmp(argv[i], "-maxsteps") == 0 && i+1 < argc) {
            maxsteps_override = atoi(argv[++i]);
        }
        else filename = argv[i];
    }
    if (!filename) {
        fprintf(stderr, "usage: sim_c [-v] [-profile] [-arch cpu3|cpu4] [-maxsteps N] file.s\n");
        return 1;
    }

    if (maxsteps_override > 0) g_max_steps = maxsteps_override;
    if (g_profile) memset(prof_lineno, 0xff, sizeof(prof_lineno));  /* -1 = no mapping */
    char *src = read_file(filename);
    g_assembler_done = 0;
    if (arch == 4) {
        if (g_profile) prof_split_lines(src);
        assemble_cpu4(src);
        free(src);
        { int i = find_sym("_globals_start"); if (i >= 0) g_write_threshold = syms[i].addr; }
        g_assembler_done = 1;
        run_cpu4(verbose);
        if (g_profile) print_profile();
    } else {
        if (g_profile) prof_split_lines(src);
        assemble(src);
        free(src);
        { int i = find_sym("_globals_start"); if (i >= 0) g_write_threshold = syms[i].addr; }
        g_assembler_done = 1;
        run_cpu(verbose);
        if (g_profile) print_profile();
    }
    return 0;
}
