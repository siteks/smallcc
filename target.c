/* target.c — the Target descriptions and the encoding queries (target.h). */
#include <stdlib.h>
#include <string.h>
#include "target.h"
#include "isatool/isa_types.h"
#include "cpu4/isa_table_c.h"
#include "cpu5/isa_table_c.h"

const Target target_cpu4 = {
    .name         = "cpu4",
    .instrs       = cpu4_instrs,
    .pseudos      = cpu4_pseudos,
    .nregs        = 8,
    .caller_saved = 0x0f,            // r0-r3
    .callee_saved = 0xf0,            // r4-r7
    .ret_reg      = 0,
    .n_arg_regs   = 3,
    .arg_regs     = {1, 2, 3},
};

// CPU5 (proposal 0004): 16 registers. The minimal ABI change from CPU4:
// r0 returns, r1-r3 carry the first arguments, r0-r7 are caller-saved and
// r8-r15 callee-saved.
const Target target_cpu5 = {
    .name         = "cpu5",
    .instrs       = cpu5_instrs,
    .pseudos      = cpu5_pseudos,
    .nregs        = 16,
    .caller_saved = 0x00ff,          // r0-r7
    .callee_saved = 0xff00,          // r8-r15
    .ret_reg      = 0,
    .n_arg_regs   = 3,
    .arg_regs     = {1, 2, 3},
};

const Target *g_target = &target_cpu4;

static const Target *const all_targets[] = { &target_cpu4, &target_cpu5, NULL };

const Target *target_find(const char *name) {
    for (int i = 0; all_targets[i]; i++)
        if (!strcmp(all_targets[i]->name, name)) return all_targets[i];
    return NULL;
}

const char *target_names(void) {
    static char buf[128];
    buf[0] = 0;
    for (int i = 0; all_targets[i]; i++) {
        if (i) strncat(buf, ", ", sizeof buf - strlen(buf) - 1);
        strncat(buf, all_targets[i]->name, sizeof buf - strlen(buf) - 1);
    }
    return buf;
}

// ---- encoding queries -------------------------------------------------
// A small open-addressed cache from mnemonic to table entry per target.

#define OPC_CAP 512
static const Target   *opc_target;
static const char     *opc_key[OPC_CAP];
static const IsaInstr *opc_val[OPC_CAP];

static unsigned opc_hash(const char *s) {
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static const IsaInstr *find_real(const char *m) {
    for (const IsaInstr *i = g_target->instrs; i->name; i++)
        if (!strcmp(i->name, m)) return i;
    return NULL;
}

static const IsaInstr *isa_lookup(const char *m) {
    if (!m) return NULL;
    if (opc_target != g_target) {
        memset(opc_key, 0, sizeof opc_key);
        opc_target = g_target;
    }
    unsigned i = opc_hash(m) & (OPC_CAP - 1);
    for (int n = 0; n < OPC_CAP; n++, i = (i + 1) & (OPC_CAP - 1)) {
        if (!opc_key[i]) break;
        if (!strcmp(opc_key[i], m)) return opc_val[i];
    }
    const IsaInstr *r = find_real(m);
    if (!r)
        for (const IsaPseudo *p = g_target->pseudos; p->name; p++)
            if (!strcmp(p->name, m)) { r = find_real(p->real); break; }
    if (!opc_key[i]) { opc_key[i] = strdup(m); opc_val[i] = r; }   // the caller's buffer may be reused
    return r;
}

int isa_has(const char *m)   { return isa_lookup(m) != NULL; }
int isa_real(const char *m)  { const IsaInstr *i = isa_lookup(m); return i && !strcmp(i->name, m); }
int isa_bytes(const char *m) { const IsaInstr *i = isa_lookup(m); return i ? i->len : 0; }

int isa_imm_range(const char *m, int k, long *lo, long *hi) {
    const IsaInstr *in = isa_lookup(m);
    if (!in) return 0;
    for (int j = 0; j < in->nops; j++) {
        const IsaOp *o = &in->ops[j];
        if (o->kind == ISA_REG || k--) continue;
        long b = o->bits, l, h;
        switch (o->kind) {
        case ISA_UIMM:           l = 0;                h = (1L << b) - 1;       break;
        case ISA_RAW: case ISA_ABS: l = -(1L << (b-1)); h = (1L << b) - 1;       break;
        default:                 l = -(1L << (b-1));   h = (1L << (b-1)) - 1;   break;
        }
        int sc = o->kind == ISA_BYTES ? o->scale : 1;
        *lo = l * sc; *hi = h * sc;
        return 1;
    }
    return 0;
}

int isa_imm_fits(const char *m, int k, long v) {
    long lo, hi;
    if (!isa_imm_range(m, k, &lo, &hi)) return 0;
    const IsaInstr *in = isa_lookup(m);
    for (int j = 0, kk = k; j < in->nops; j++) {
        if (in->ops[j].kind == ISA_REG || kk--) continue;
        if (in->ops[j].kind == ISA_BYTES && in->ops[j].scale > 1 && v % in->ops[j].scale) return 0;
        break;
    }
    return v >= lo && v <= hi;
}
