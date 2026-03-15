/*
 * sim_c.c — CPU3 assembler + simulator
 *
 * Usage:  ./sim_c [-v] file.s
 * Output: putchar to stderr; state line to stdout
 *         r0:XXXXXXXX sp:XXXX bp:XXXX lr:XXXX pc:XXXX H:X
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <math.h>

/* ------------------------------------------------------------------ */
/* Memory                                                               */
/* ------------------------------------------------------------------ */

static uint8_t mem[65536];

#define MMIO_BASE 0xFF00u

static uint32_t g_cycles = 0;

static uint8_t mmio_read8(uint16_t a) {
    uint32_t off = (uint16_t)(a - MMIO_BASE);
    if (off < 4) return (uint8_t)(g_cycles >> (off * 8));
    return 0;
}

static void   write8 (uint16_t a, uint8_t  v) { if (a >= MMIO_BASE) return; mem[a] = v; }
static void   write16(uint16_t a, uint16_t v) { if (a >= MMIO_BASE) return; mem[a]=(uint8_t)v; mem[(uint16_t)(a+1)]=(uint8_t)(v>>8); }
static void   write32(uint16_t a, uint32_t v) { if (a >= MMIO_BASE) return; write16(a,(uint16_t)v); write16((uint16_t)(a+2),(uint16_t)(v>>16)); }
static uint8_t  read8 (uint16_t a) { if (a >= MMIO_BASE) return mmio_read8(a); return mem[a]; }
static uint16_t read16(uint16_t a) { if (a >= MMIO_BASE) return (uint16_t)mmio_read8(a) | ((uint16_t)mmio_read8((uint16_t)(a+1))<<8); return (uint16_t)mem[a] | ((uint16_t)mem[(uint16_t)(a+1)] << 8); }
static uint32_t read32(uint16_t a) { if (a >= MMIO_BASE) return (uint32_t)mmio_read8(a)|((uint32_t)mmio_read8((uint16_t)(a+1))<<8)|((uint32_t)mmio_read8((uint16_t)(a+2))<<16)|((uint32_t)mmio_read8((uint16_t)(a+3))<<24); return (uint32_t)read16(a) | ((uint32_t)read16((uint16_t)(a+2)) << 16); }

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
    {"immb",0x40,1},{"adj",0x41,1},
    {"immw",0x80,2},{"immwh",0x81,2},{"j",0x82,2},{"jl",0x83,2},
    {"jz",0x84,2},{"jnz",0x85,2},{"enter",0x86,2},{"lea",0x87,2},
    {"ssp",0x88,2},
    {NULL,0,0}
};

static const Instr *find_instr(const char *n)
{
    for (int i = 0; itab[i].name; i++)
        if (strcmp(itab[i].name, n) == 0) return &itab[i];
    return NULL;
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
/* CPU                                                                  */
/* ------------------------------------------------------------------ */

#define MAX_STEPS 10000000

static void run_cpu(int verbose)
{
    uint32_t r0 = 0;
    uint16_t sp = 0, bp = 0, lr = 0, pc = 0;
    int H = 0;
    g_cycles = 0;

    for (int step = 0; step < MAX_STEPS && !H; step++, g_cycles++) {
        uint8_t op = read8(pc);
        uint16_t oldpc = pc;
        pc++;

        int8_t  imm8  = 0;
        uint16_t imm16 = 0;

        if ((op & 0xc0) == 0x40) {
            imm8 = (int8_t)read8(pc); pc++;
        } else if ((op & 0xc0) == 0x80) {
            imm16 = read16(pc); pc += 2;
        }

        if (verbose) {
            fprintf(stderr, "[%04x] op=%02x r0=%08x sp=%04x bp=%04x\n",
                    oldpc, op, r0, sp, bp);
        }

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
        default:
            fprintf(stderr, "unknown opcode 0x%02x at pc=%04x\n", op, oldpc);
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
    const char *filename = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else filename = argv[i];
    }
    if (!filename) {
        fprintf(stderr, "usage: sim_c [-v] file.s\n");
        return 1;
    }

    char *src = read_file(filename);
    assemble(src);
    free(src);
    run_cpu(verbose);
    return 0;
}
