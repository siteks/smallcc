/*
 * irsim.c — IR interpreter for CPU4 SSA IR (post-OOS and post-IRC forms).
 *
 * Memory map (mirrors sim_c / CPU4 hardware):
 *   0x0000–0x2000  reserved / code area (not used by interpreter)
 *   0xF000         initial sp (stack grows downward)
 *   0x2000–...     data section: globals and string literals
 *   ...–0xFFFF     heap for stack frames
 *
 * Calling convention (mirrors emit.c):
 *   Non-variadic: first 3 args in args[0..2] (→ IK_PARAM param_idx 1–3),
 *                 args 3+ pushed onto mem stack at call_sp–4, –8, …;
 *                 callee reads them via IK_LOAD (NULL base, bp+4, bp+8, …).
 *   Variadic:     ALL args pushed onto mem stack right-to-left;
 *                 callee reads at bp+4, bp+8, … via IK_LOAD (NULL base).
 *
 * Frame layout after "enter N" (N = f->frame_size rounded to 4):
 *   bp+4  first stack param (or first variadic arg)
 *   bp+0  dummy saved lr|bp word (0)
 *   bp-M  spill slots (bp-relative negative offsets, from IRC)
 */

#include "irsim.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* =========================================================================
 * IrSim struct
 * ========================================================================= */

#define IRSIM_MEM_SIZE  65536
#define IRSIM_DATA_START 0x2000u
#define IRSIM_STACK_START 0xF000u
#define IRSIM_MMIO_BASE 0xFF00u

#define IRSIM_MAX_FNS    512
#define IRSIM_MAX_GADDRS 1024

struct IrSim {
    uint8_t  mem[IRSIM_MEM_SIZE];
    uint16_t data_ptr;               /* next free data byte */
    uint32_t cycles;                 /* instruction step counter (MMIO at 0xFF00) */

    /* Function table: name → Function* */
    char     *fn_names[IRSIM_MAX_FNS];
    Function *fn_ptrs [IRSIM_MAX_FNS];
    int       nfns;

    /* Global address table: label name → uint16_t address in mem[] */
    char     *ga_names[IRSIM_MAX_GADDRS];
    uint16_t  ga_addrs[IRSIM_MAX_GADDRS];
    int       ngaddrs;
};

/* =========================================================================
 * Memory helpers
 * ========================================================================= */

static uint32_t mem_read(IrSim *sim, uint16_t addr, int size)
{
    if (addr >= IRSIM_MMIO_BASE) {
        /* 32-bit cycle counter at 0xFF00, little-endian */
        uint32_t off = (uint32_t)(addr - IRSIM_MMIO_BASE);
        if (off < 4) {
            switch (size) {
            case 1: return (sim->cycles >> (off * 8)) & 0xff;
            case 4: return sim->cycles;
            default: return (sim->cycles >> (off * 8)) & 0xffff;
            }
        }
        return 0;
    }
    switch (size) {
    case 1: return sim->mem[addr];
    case 4: { uint32_t v; memcpy(&v, &sim->mem[addr], 4); return v; }
    default: { uint16_t v; memcpy(&v, &sim->mem[addr], 2); return v; }
    }
}

static void mem_write(IrSim *sim, uint16_t addr, int size, uint32_t val)
{
    if (addr >= IRSIM_MMIO_BASE) return;  /* MMIO writes silently ignored */
    switch (size) {
    case 1: sim->mem[addr] = val & 0xff; return;
    case 4: memcpy(&sim->mem[addr], &val, 4); return;
    default: { uint16_t v = (uint16_t)(val & 0xffff); memcpy(&sim->mem[addr], &v, 2); }
    }
}

/* =========================================================================
 * Value helpers
 * ========================================================================= */

static uint32_t get_val(uint32_t *vreg, Value *v)
{
    v = val_resolve(v);
    if (!v || v->kind == VAL_UNDEF) return 0;
    if (v->kind == VAL_CONST) return (uint32_t)(int32_t)v->iconst;
    return vreg[v->id];
}

static void set_val(uint32_t *vreg, Value *dst, uint32_t val)
{
    dst = val_resolve(dst);
    if (!dst || dst->kind != VAL_INST) return;
    vreg[dst->id] = val;
}

/* =========================================================================
 * ValType helpers (mirrors emit.c — local copies to avoid coupling)
 * ========================================================================= */

static int vt_size(ValType vt)
{
    switch (vt) {
    case VT_I8: case VT_U8:  return 1;
    case VT_I32: case VT_U32: case VT_F32: return 4;
    case VT_PTR: return 2;
    default: return 2;
    }
}

static int vt_signed(ValType vt)
{
    return vt == VT_I8 || vt == VT_I16 || vt == VT_I32;
}

/* =========================================================================
 * Symbol lookup
 * ========================================================================= */

static uint16_t lookup_gaddr(IrSim *sim, const char *name)
{
    for (int i = 0; i < sim->ngaddrs; i++)
        if (strcmp(sim->ga_names[i], name) == 0)
            return sim->ga_addrs[i];
    return 0;
}

static Function *lookup_fn(IrSim *sim, const char *name)
{
    for (int i = 0; i < sim->nfns; i++)
        if (strcmp(sim->fn_names[i], name) == 0)
            return sim->fn_ptrs[i];
    return NULL;
}

/* Look up a function by its address in the global address table */
static Function *lookup_fn_by_addr(IrSim *sim, uint16_t addr)
{
    /* For each function, its address is stored in ga_addrs under its name */
    for (int i = 0; i < sim->nfns; i++) {
        uint16_t fn_addr = lookup_gaddr(sim, sim->fn_names[i]);
        if (fn_addr == addr)
            return sim->fn_ptrs[i];
    }
    return NULL;
}

/* =========================================================================
 * External function stubs
 * (Called when IK_CALL names a function not in the fn_table.
 *  In practice this is rarely needed since the lib .c files are compiled.
 * ========================================================================= */

static uint32_t call_stub(IrSim *sim, const char *name, uint32_t *args, int nargs)
{
    if (strcmp(name, "putchar") == 0) {
        fputc(nargs > 0 ? (int)(args[0] & 0xff) : 0, stderr);
        return nargs > 0 ? args[0] & 0xff : 0;
    }
    if (strcmp(name, "strlen") == 0) {
        if (nargs < 1) return 0;
        uint16_t a = (uint16_t)args[0];
        int n = 0;
        while (sim->mem[a++]) n++;
        return (uint32_t)n;
    }
    if (strcmp(name, "strcmp") == 0) {
        if (nargs < 2) return 0;
        uint16_t a = (uint16_t)args[0], b = (uint16_t)args[1];
        while (sim->mem[a] && sim->mem[a] == sim->mem[b]) { a++; b++; }
        return (uint32_t)(int32_t)((int)sim->mem[a] - (int)sim->mem[b]);
    }
    if (strcmp(name, "strcpy") == 0) {
        if (nargs < 2) return 0;
        uint16_t d = (uint16_t)args[0], s = (uint16_t)args[1];
        do { sim->mem[d++] = sim->mem[s]; } while (sim->mem[s++]);
        return args[0];
    }
    if (strcmp(name, "strcat") == 0) {
        if (nargs < 2) return 0;
        uint16_t d = (uint16_t)args[0];
        while (sim->mem[d]) d++;
        uint16_t s = (uint16_t)args[1];
        do { sim->mem[d++] = sim->mem[s]; } while (sim->mem[s++]);
        return args[0];
    }
    if (strcmp(name, "abs") == 0) {
        if (nargs < 1) return 0;
        int32_t v = (int32_t)args[0];
        return v < 0 ? (uint32_t)(-v) : (uint32_t)v;
    }
    if (strcmp(name, "modf") == 0) {
        if (nargs < 2) return 0;
        float fv; memcpy(&fv, &args[0], 4);
        float ipart;
        float frac = modff(fv, &ipart);
        uint32_t ibits; memcpy(&ibits, &ipart, 4);
        mem_write(sim, (uint16_t)args[1], 4, ibits);
        uint32_t fbits; memcpy(&fbits, &frac, 4);
        return fbits;
    }
    fprintf(stderr, "irsim: unknown extern '%s'\n", name);
    return 0;
}

/* =========================================================================
 * Block execution
 * ========================================================================= */

typedef struct {
    uint32_t *vreg;    /* register file indexed by Value.id */
    uint32_t *args;    /* caller's argument array (for IK_PARAM) */
    int       nargs;
    uint16_t  bp;      /* frame base pointer */
    uint16_t  sp;      /* stack pointer (bottom of frame) */
    uint32_t  retval;
    int       done;
} SimFrame;

/* Forward declaration for recursive calls */
static uint32_t run_function(IrSim *sim, Function *f,
                              uint32_t *args, int nargs, uint16_t sp);

static Block *execute_block(IrSim *sim, Block *b, SimFrame *frame)
{
    uint32_t *vreg = frame->vreg;

    for (Inst *inst = b->head; inst; inst = inst->next) {
        if (inst->is_dead) continue;
        sim->cycles++;
        Value *dst = inst->dst;

        switch (inst->kind) {

        case IK_CONST:
            if (dst)
                set_val(vreg, dst, (uint32_t)(int32_t)inst->imm);
            break;

        case IK_COPY:
            if (dst && inst->nops >= 1)
                set_val(vreg, dst, get_val(vreg, inst->ops[0]));
            break;

        case IK_PHI:
            /* Should not appear post-OOS; ignore as safety net */
            break;

        case IK_PARAM:
            if (dst) {
                /* param_idx is 1-based (1→r1, 2→r2, 3→r3) mapping to args[0..2] */
                int pidx = inst->param_idx - 1;
                uint32_t pval = (pidx >= 0 && pidx < frame->nargs)
                                ? frame->args[pidx] : 0;
                set_val(vreg, dst, pval);
            }
            break;

        /* ---- Integer ALU ---- */
        case IK_ADD:
            if (dst && inst->nops >= 2)
                set_val(vreg, dst,
                    get_val(vreg, inst->ops[0]) + get_val(vreg, inst->ops[1]));
            break;
        case IK_SUB:
            if (dst && inst->nops >= 2)
                set_val(vreg, dst,
                    get_val(vreg, inst->ops[0]) - get_val(vreg, inst->ops[1]));
            break;
        case IK_MUL:
            if (dst && inst->nops >= 2)
                set_val(vreg, dst,
                    get_val(vreg, inst->ops[0]) * get_val(vreg, inst->ops[1]));
            break;
        case IK_DIV: /* signed division */
            if (dst && inst->nops >= 2) {
                int32_t a = (int32_t)get_val(vreg, inst->ops[0]);
                int32_t b = (int32_t)get_val(vreg, inst->ops[1]);
                set_val(vreg, dst, b ? (uint32_t)(a / b) : 0);
            }
            break;
        case IK_UDIV: /* unsigned division */
            if (dst && inst->nops >= 2) {
                uint32_t a = get_val(vreg, inst->ops[0]);
                uint32_t b = get_val(vreg, inst->ops[1]);
                set_val(vreg, dst, b ? a / b : 0);
            }
            break;
        case IK_MOD: /* signed remainder */
            if (dst && inst->nops >= 2) {
                int32_t a = (int32_t)get_val(vreg, inst->ops[0]);
                int32_t b = (int32_t)get_val(vreg, inst->ops[1]);
                set_val(vreg, dst, b ? (uint32_t)(a % b) : 0);
            }
            break;
        case IK_UMOD: /* unsigned remainder */
            if (dst && inst->nops >= 2) {
                uint32_t a = get_val(vreg, inst->ops[0]);
                uint32_t b = get_val(vreg, inst->ops[1]);
                set_val(vreg, dst, b ? a % b : 0);
            }
            break;
        case IK_SHL:
            if (dst && inst->nops >= 2)
                set_val(vreg, dst,
                    get_val(vreg, inst->ops[0]) << (get_val(vreg, inst->ops[1]) & 31u));
            break;
        case IK_SHR: /* arithmetic or logical based on dst vtype (mirrors emit.c) */
            if (dst && inst->nops >= 2) {
                uint32_t shift = get_val(vreg, inst->ops[1]) & 31u;
                uint32_t result;
                if (vt_signed(dst->vtype))
                    result = (uint32_t)((int32_t)get_val(vreg, inst->ops[0]) >> (int)shift);
                else
                    result = get_val(vreg, inst->ops[0]) >> shift;
                set_val(vreg, dst, result);
            }
            break;
        case IK_USHR: /* always logical */
            if (dst && inst->nops >= 2)
                set_val(vreg, dst,
                    get_val(vreg, inst->ops[0]) >> (get_val(vreg, inst->ops[1]) & 31u));
            break;
        case IK_AND:
            if (dst && inst->nops >= 2)
                set_val(vreg, dst,
                    get_val(vreg, inst->ops[0]) & get_val(vreg, inst->ops[1]));
            break;
        case IK_OR:
            if (dst && inst->nops >= 2)
                set_val(vreg, dst,
                    get_val(vreg, inst->ops[0]) | get_val(vreg, inst->ops[1]));
            break;
        case IK_XOR:
            if (dst && inst->nops >= 2)
                set_val(vreg, dst,
                    get_val(vreg, inst->ops[0]) ^ get_val(vreg, inst->ops[1]));
            break;
        case IK_NEG:
            if (dst && inst->nops >= 1)
                set_val(vreg, dst,
                    (uint32_t)(-(int32_t)get_val(vreg, inst->ops[0])));
            break;
        case IK_NOT:   /* logical NOT: (x == 0) ? 1 : 0 */
            if (dst && inst->nops >= 1)
                set_val(vreg, dst, get_val(vreg, inst->ops[0]) == 0 ? 1u : 0u);
            break;

        /* ---- Signed comparisons ---- */
        case IK_LT:
            if (dst && inst->nops >= 2)
                set_val(vreg, dst,
                    (int32_t)get_val(vreg, inst->ops[0]) < (int32_t)get_val(vreg, inst->ops[1]) ? 1u : 0u);
            break;
        case IK_LE:
            if (dst && inst->nops >= 2)
                set_val(vreg, dst,
                    (int32_t)get_val(vreg, inst->ops[0]) <= (int32_t)get_val(vreg, inst->ops[1]) ? 1u : 0u);
            break;
        case IK_EQ:
            if (dst && inst->nops >= 2)
                set_val(vreg, dst,
                    get_val(vreg, inst->ops[0]) == get_val(vreg, inst->ops[1]) ? 1u : 0u);
            break;
        case IK_NE:
            if (dst && inst->nops >= 2)
                set_val(vreg, dst,
                    get_val(vreg, inst->ops[0]) != get_val(vreg, inst->ops[1]) ? 1u : 0u);
            break;

        /* ---- Unsigned comparisons ---- */
        case IK_ULT:
            if (dst && inst->nops >= 2)
                set_val(vreg, dst,
                    get_val(vreg, inst->ops[0]) < get_val(vreg, inst->ops[1]) ? 1u : 0u);
            break;
        case IK_ULE:
            if (dst && inst->nops >= 2)
                set_val(vreg, dst,
                    get_val(vreg, inst->ops[0]) <= get_val(vreg, inst->ops[1]) ? 1u : 0u);
            break;

        /* ---- Float ALU ---- */
        case IK_FADD: case IK_FSUB: case IK_FMUL: case IK_FDIV: {
            if (!dst || inst->nops < 2) break;
            uint32_t av = get_val(vreg, inst->ops[0]);
            uint32_t bv = get_val(vreg, inst->ops[1]);
            float a, b;
            memcpy(&a, &av, 4); memcpy(&b, &bv, 4);
            float r;
            switch (inst->kind) {
            case IK_FADD: r = a + b; break;
            case IK_FSUB: r = a - b; break;
            case IK_FMUL: r = a * b; break;
            default:       r = b != 0.0f ? a / b : 0.0f; break;
            }
            uint32_t rv; memcpy(&rv, &r, 4);
            set_val(vreg, dst, rv);
            break;
        }

        /* ---- Float comparisons ---- */
        case IK_FLT: case IK_FLE: case IK_FEQ: case IK_FNE: {
            if (!dst || inst->nops < 2) break;
            uint32_t av = get_val(vreg, inst->ops[0]);
            uint32_t bv = get_val(vreg, inst->ops[1]);
            float a, b;
            memcpy(&a, &av, 4); memcpy(&b, &bv, 4);
            uint32_t r;
            switch (inst->kind) {
            case IK_FLT: r = (a < b) ? 1u : 0u; break;
            case IK_FLE: r = (a <= b) ? 1u : 0u; break;
            case IK_FEQ: r = (a == b) ? 1u : 0u; break;
            default:     r = (a != b) ? 1u : 0u; break;
            }
            set_val(vreg, dst, r);
            break;
        }

        /* ---- Conversions ---- */
        case IK_ITOF: {
            if (!dst || inst->nops < 1) break;
            /* Sign-extend before converting (target: int is 16-bit) */
            int32_t iv = (int32_t)get_val(vreg, inst->ops[0]);
            float fv = (float)iv;
            uint32_t rv; memcpy(&rv, &fv, 4);
            set_val(vreg, dst, rv);
            break;
        }
        case IK_FTOI: {
            if (!dst || inst->nops < 1) break;
            uint32_t bits = get_val(vreg, inst->ops[0]);
            float fv; memcpy(&fv, &bits, 4);
            set_val(vreg, dst, (uint32_t)(int32_t)fv);
            break;
        }
        case IK_SEXT8: {
            if (!dst || inst->nops < 1) break;
            int8_t v = (int8_t)(get_val(vreg, inst->ops[0]) & 0xffu);
            set_val(vreg, dst, (uint32_t)(int32_t)v);
            break;
        }
        case IK_SEXT16: {
            if (!dst || inst->nops < 1) break;
            int16_t v = (int16_t)(get_val(vreg, inst->ops[0]) & 0xffffu);
            set_val(vreg, dst, (uint32_t)(int32_t)v);
            break;
        }
        case IK_ZEXT: {
            if (!dst || inst->nops < 1) break;
            set_val(vreg, dst, get_val(vreg, inst->ops[0]));
            break;
        }
        case IK_TRUNC: {
            if (!dst || inst->nops < 1) break;
            int dsize = dst ? vt_size(dst->vtype) : 2;
            uint32_t v = get_val(vreg, inst->ops[0]);
            if (dsize == 1) v &= 0xffu;
            else if (dsize == 2) v &= 0xffffu;
            set_val(vreg, dst, v);
            break;
        }

        /* ---- Memory ---- */
        case IK_LOAD: {
            /*
             * ops[0] = base pointer (or NULL for bp-relative spill/param loads).
             * A NULL ops[0] means the address is bp + imm (spill slots have
             * negative imm; stack-param loads have positive imm like bp+4, bp+8).
             * A non-NULL ops[0] means the address is get_val(ops[0]) + imm.
             */
            if (!dst || inst->nops < 1) break;
            Value *base = val_resolve(inst->ops[0]);
            int off  = inst->imm;
            int size = inst->size ? inst->size : vt_size(dst ? dst->vtype : VT_I16);
            uint16_t addr;
            if (!base || base->kind == VAL_UNDEF)
                addr = (uint16_t)((int32_t)frame->bp + off);
            else
                addr = (uint16_t)((int32_t)get_val(vreg, base) + off);
            uint32_t val = mem_read(sim, addr, size);
            /* Apply sign extension for signed destination types */
            if (size == 1 && vt_signed(dst->vtype))
                val = (uint32_t)(int32_t)(int8_t)(uint8_t)val;
            else if (size == 2 && vt_signed(dst->vtype))
                val = (uint32_t)(int32_t)(int16_t)(uint16_t)val;
            set_val(vreg, dst, val);
            break;
        }
        case IK_STORE: {
            /*
             * nops == 1: spill store — ops[0] = value, address = bp + imm.
             * nops >= 2: regular store — ops[0] = base addr, ops[1] = value.
             */
            if (inst->nops < 1) break;
            Value *base, *val_v;
            if (inst->nops >= 2) {
                base  = val_resolve(inst->ops[0]);
                val_v = val_resolve(inst->ops[1]);
            } else {
                base  = NULL;
                val_v = val_resolve(inst->ops[0]);
            }
            int off  = inst->imm;
            int size = inst->size ? inst->size
                                  : (val_v ? vt_size(val_v->vtype) : 2);
            uint16_t addr;
            if (!base || base->kind == VAL_UNDEF)
                addr = (uint16_t)((int32_t)frame->bp + off);
            else
                addr = (uint16_t)((int32_t)get_val(vreg, base) + off);
            mem_write(sim, addr, size, val_v ? get_val(vreg, val_v) : 0);
            break;
        }
        case IK_ADDR: {
            /* dst = bp + inst->imm (frame-relative address of a local) */
            if (dst)
                set_val(vreg, dst,
                    (uint32_t)(uint16_t)((int32_t)frame->bp + inst->imm));
            break;
        }
        case IK_GADDR: {
            /* dst = address of a global symbol */
            if (dst)
                set_val(vreg, dst, lookup_gaddr(sim, inst->fname));
            break;
        }
        case IK_MEMCPY: {
            if (inst->nops < 2) break;
            uint16_t d = (uint16_t)get_val(vreg, inst->ops[0]);
            uint16_t s = (uint16_t)get_val(vreg, inst->ops[1]);
            memmove(&sim->mem[d], &sim->mem[s], (size_t)inst->imm);
            break;
        }

        /* ---- Calls ---- */
        case IK_CALL:
        case IK_ICALL: {
            /*
             * For IK_CALL:  fname = callee name; ops[0..nops-1] = args.
             * For IK_ICALL: ops[0] = function pointer; ops[1..nops-1] = args.
             */
            int nops_start = (inst->kind == IK_ICALL) ? 1 : 0;
            int call_nargs = inst->nops - nops_start;
            if (call_nargs < 0) call_nargs = 0;
            if (call_nargs > 32) call_nargs = 32;

            uint32_t call_args[32];
            for (int i = 0; i < call_nargs; i++)
                call_args[i] = get_val(vreg, inst->ops[nops_start + i]);

            int is_var = inst->calldesc && inst->calldesc->is_variadic;

            /*
             * Push extra args onto the simulated stack (right-to-left).
             * Non-variadic: args 0–2 passed in call_args[]; args 3+ on stack.
             * Variadic:     ALL args pushed on stack.
             */
            uint16_t call_sp = frame->sp;
            int first_stack = is_var ? 0 : 3;
            for (int i = call_nargs - 1; i >= first_stack; i--) {
                call_sp -= 4;
                mem_write(sim, call_sp, 4, call_args[i]);
            }

            uint32_t ret = 0;
            if (inst->kind == IK_CALL) {
                Function *callee = lookup_fn(sim, inst->fname);
                if (callee)
                    ret = run_function(sim, callee, call_args, call_nargs, call_sp);
                else
                    ret = call_stub(sim, inst->fname, call_args, call_nargs);
            } else {
                /* IK_ICALL: look up function by its registered address */
                uint16_t fp_addr = (uint16_t)get_val(vreg, inst->ops[0]);
                Function *callee = lookup_fn_by_addr(sim, fp_addr);
                if (callee) {
                    ret = run_function(sim, callee, call_args, call_nargs, call_sp);
                } else {
                    /* Try stub by scanning gaddr table for a matching address */
                    const char *fname = NULL;
                    for (int i = 0; i < sim->ngaddrs; i++)
                        if (sim->ga_addrs[i] == fp_addr) { fname = sim->ga_names[i]; break; }
                    if (fname)
                        ret = call_stub(sim, fname, call_args, call_nargs);
                    else
                        fprintf(stderr, "irsim: icall to unknown address 0x%04x\n", fp_addr);
                }
            }

            if (dst) set_val(vreg, dst, ret);
            break;
        }

        case IK_PUTCHAR: {
            /* Builtin putchar opcode — output to stderr (matching sim_c) */
            if (inst->nops >= 1)
                fputc((int)(get_val(vreg, inst->ops[0]) & 0xffu), stderr);
            break;
        }

        /* ---- Control flow ---- */
        case IK_BR:
            if (inst->nops >= 1 && get_val(vreg, inst->ops[0]))
                return inst->target;
            return inst->target2;

        case IK_JMP:
            return inst->target;

        case IK_RET:
            frame->retval = (inst->nops >= 1)
                            ? get_val(vreg, inst->ops[0]) : 0u;
            frame->done = 1;
            return NULL;

        default:
            break;
        }
    }
    return NULL; /* should not be reached in well-formed IR */
}

/* =========================================================================
 * Function execution
 * ========================================================================= */

static uint32_t run_function(IrSim *sim, Function *f,
                              uint32_t *args, int nargs, uint16_t sp)
{
    if (!f || f->nblocks == 0) return 0;

    /* Allocate the register file */
    uint32_t *vreg = calloc((size_t)f->nvalues, sizeof(uint32_t));
    if (!vreg) { fprintf(stderr, "irsim: oom\n"); return 0; }

    /*
     * Simulate "enter frame_size":
     *   bp = sp - 4  (make room for packed lr|bp word)
     *   sp = bp - frame_size_rounded
     *
     * This matches new CPU4 enter semantics (4-byte frame overhead).
     * We don't actually write dummy lr/bp into mem because we handle returns
     * via the done/retval mechanism.
     * However, the bp value establishes the frame for IK_LOAD/IK_STORE/IK_ADDR.
     */
    int frame = f->frame_size;
    if (frame & 3) frame += 4 - (frame & 3);   /* round up to 4-byte boundary */
    uint16_t bp     = sp - 4;
    uint16_t new_sp = (uint16_t)((int32_t)bp - frame);

    SimFrame sf = {
        .vreg   = vreg,
        .args   = args,
        .nargs  = nargs,
        .bp     = bp,
        .sp     = new_sp,
        .retval = 0,
        .done   = 0,
    };

    /* Walk the CFG following successor edges */
    Block *cur = f->blocks[0];
    while (cur && !sf.done) {
        Block *next = execute_block(sim, cur, &sf);
        cur = next;
    }

    free(vreg);
    return sf.retval;
}

/* =========================================================================
 * Global data population
 * ========================================================================= */

/*
 * Allocate `size` bytes in the data section and register the label.
 * Returns the allocated base address.
 */
static uint16_t alloc_data(IrSim *sim, const char *name, int size)
{
    /* Align to natural alignment (4 for longs/floats, 2 for words) */
    if (size >= 4 && (sim->data_ptr & 3u))
        sim->data_ptr += 4 - (sim->data_ptr & 3u);
    else if (size >= 2 && (sim->data_ptr & 1u))
        sim->data_ptr++;

    uint16_t addr = sim->data_ptr;
    sim->data_ptr += (uint16_t)size;

    if (sim->ngaddrs < IRSIM_MAX_GADDRS) {
        sim->ga_names[sim->ngaddrs] = strdup(name);
        sim->ga_addrs[sim->ngaddrs] = addr;
        sim->ngaddrs++;
    }
    return addr;
}

/*
 * Write the initial value of a gvar or strlit into mem[] at base_addr.
 * All labels must already be registered (pass-2 call after alloc_data pass).
 */
static void write_global_data(IrSim *sim, Sx *item, uint16_t base_addr)
{
    const char *tag = sx_car_sym(item);
    if (!tag) return;

    if (strcmp(tag, "strlit") == 0) {
        /* (strlit "label" byte0 byte1 ... byteN) — bytes start at element 2 */
        uint16_t addr = base_addr;
        Sx *cur = item->cdr;               /* skip "strlit" */
        if (cur) cur = cur->cdr;           /* skip label */
        while (cur && cur->kind == SX_PAIR) {
            Sx *b = cur->car;
            if (b && b->kind == SX_INT)
                sim->mem[addr++] = (uint8_t)(b->i & 0xff);
            cur = cur->cdr;
        }
        return;
    }

    if (strcmp(tag, "gvar") != 0) return;

    /* (gvar name size [init]) */
    Sx *size_sx = sx_nth(item, 2);
    Sx *init_sx = sx_nth(item, 3);   /* optional */
    int size = (size_sx && size_sx->kind == SX_INT) ? size_sx->i : 2;
    uint16_t addr = base_addr;

    if (!init_sx || (init_sx->kind == SX_INT && init_sx->i == 0)) {
        /* zero-fill — already done by calloc */
        return;
    }
    if (init_sx->kind == SX_INT) {
        /* Scalar integer initializer */
        mem_write(sim, addr, size, (uint32_t)init_sx->i);
        return;
    }
    if (init_sx->kind == SX_PAIR) {
        const char *itag = sx_car_sym(init_sx);
        if (!itag) return;

        if (strcmp(itag, "strref") == 0) {
            /* Pointer to a string literal: (strref "label") */
            Sx *lbl_sx = init_sx->cdr ? init_sx->cdr->car : NULL;
            const char *lbl = (lbl_sx && (lbl_sx->kind == SX_STR || lbl_sx->kind == SX_SYM))
                              ? lbl_sx->s : NULL;
            if (lbl) mem_write(sim, addr, 2, lookup_gaddr(sim, lbl));
            return;
        }
        if (strcmp(itag, "strbytes") == 0) {
            /* Inline byte array for char[] = "string" initializers */
            Sx *cur = init_sx->cdr;
            while (cur && cur->kind == SX_PAIR) {
                Sx *b = cur->car;
                if (b && b->kind == SX_INT)
                    sim->mem[addr++] = (uint8_t)(b->i & 0xff);
                cur = cur->cdr;
            }
            return;
        }
        if (strcmp(itag, "ginit") == 0) {
            /* Array initializer: (ginit elem_size v0 v1 ...) */
            Sx *es_sx = init_sx->cdr ? init_sx->cdr->car : NULL;
            int esize = (es_sx && es_sx->kind == SX_INT) ? es_sx->i : 2;
            Sx *vals  = init_sx->cdr ? init_sx->cdr->cdr : NULL;
            int written = 0;
            while (vals && vals->kind == SX_PAIR && written < size) {
                Sx *v = vals->car;
                if (v && v->kind == SX_INT) {
                    mem_write(sim, addr, esize, (uint32_t)v->i);
                    addr += esize; written += esize;
                } else if (v && v->kind == SX_PAIR) {
                    const char *vtag = sx_car_sym(v);
                    if (vtag && strcmp(vtag, "strref") == 0) {
                        Sx *lbl_sx = v->cdr ? v->cdr->car : NULL;
                        const char *lbl = (lbl_sx && (lbl_sx->kind == SX_STR || lbl_sx->kind == SX_SYM))
                                          ? lbl_sx->s : NULL;
                        if (lbl) mem_write(sim, addr, 2, lookup_gaddr(sim, lbl));
                        addr += 2; written += 2;
                    }
                }
                vals = vals->cdr;
            }
            return;
        }
    }
    /* Unknown init format — leave zero */
}

/* =========================================================================
 * Public API
 * ========================================================================= */

IrSim *irsim_new(void)
{
    IrSim *sim = calloc(1, sizeof(IrSim));
    if (!sim) return NULL;
    sim->data_ptr = IRSIM_DATA_START;
    return sim;
}

void irsim_add_function(IrSim *sim, Function *f)
{
    if (!f || !sim || sim->nfns >= IRSIM_MAX_FNS) return;

    /*
     * Assign a 2-byte slot in the data section as the function's address.
     * IK_GADDR "fname" will return this address; IK_ICALL resolves it back
     * to the Function* via lookup_fn_by_addr.
     */
    if (sim->data_ptr & 1u) sim->data_ptr++;   /* word-align */
    uint16_t fn_addr = sim->data_ptr;
    sim->data_ptr += 2;

    sim->fn_names[sim->nfns] = strdup(f->name);
    sim->fn_ptrs [sim->nfns] = f;
    sim->nfns++;

    /* Register in global address table so IK_GADDR can find it */
    if (sim->ngaddrs < IRSIM_MAX_GADDRS) {
        sim->ga_names[sim->ngaddrs] = strdup(f->name);
        sim->ga_addrs[sim->ngaddrs] = fn_addr;
        sim->ngaddrs++;
    }
}

void irsim_populate_globals(IrSim *sim, Sx *program)
{
    if (!program || !sim) return;

    /*
     * Two-pass approach:
     *   Pass 1 — walk the program list, allocate space and register addresses.
     *   Pass 2 — write values (strref pointers need all labels registered first).
     */
    enum { MAX_PENDING = 1024 };
    Sx      *pending_sx  [MAX_PENDING];
    uint16_t pending_addr[MAX_PENDING];
    int npending = 0;

    Sx *list = program->cdr;
    while (list && list->kind == SX_PAIR) {
        Sx *item = list->car;
        const char *tag = sx_car_sym(item);
        if (tag) {
            if (strcmp(tag, "strlit") == 0) {
                Sx *name_sx = sx_nth(item, 1);
                const char *name = (name_sx && (name_sx->kind == SX_STR || name_sx->kind == SX_SYM))
                                   ? name_sx->s : NULL;
                if (name) {
                    /* Count bytes (elements from index 2 onward) */
                    int nbytes = 0;
                    Sx *cur = item->cdr;
                    if (cur) cur = cur->cdr;   /* skip label */
                    while (cur && cur->kind == SX_PAIR) { nbytes++; cur = cur->cdr; }
                    uint16_t addr = alloc_data(sim, name, nbytes);
                    if (npending < MAX_PENDING) {
                        pending_sx  [npending] = item;
                        pending_addr[npending] = addr;
                        npending++;
                    }
                }
            } else if (strcmp(tag, "gvar") == 0) {
                Sx *name_sx = sx_nth(item, 1);
                Sx *size_sx = sx_nth(item, 2);
                const char *name = (name_sx && (name_sx->kind == SX_STR || name_sx->kind == SX_SYM))
                                   ? name_sx->s : NULL;
                int size = (size_sx && size_sx->kind == SX_INT) ? size_sx->i : 2;
                if (name) {
                    uint16_t addr = alloc_data(sim, name, size);
                    if (npending < MAX_PENDING) {
                        pending_sx  [npending] = item;
                        pending_addr[npending] = addr;
                        npending++;
                    }
                }
            }
        }
        list = list->cdr;
    }

    /* Pass 2: write values now that all labels are known */
    for (int i = 0; i < npending; i++)
        write_global_data(sim, pending_sx[i], pending_addr[i]);
}

uint32_t irsim_call_main(IrSim *sim)
{
    Function *main_fn = lookup_fn(sim, "main");
    if (!main_fn) {
        fprintf(stderr, "irsim: 'main' not found\n");
        return 0;
    }
    /* Call main with no arguments (argc=0, no argv) */
    uint32_t args[1] = {0};
    return run_function(sim, main_fn, args, 0, IRSIM_STACK_START);
}

void irsim_free(IrSim *sim)
{
    if (!sim) return;
    for (int i = 0; i < sim->nfns;    i++) free(sim->fn_names[i]);
    for (int i = 0; i < sim->ngaddrs; i++) free(sim->ga_names[i]);
    free(sim);
}
