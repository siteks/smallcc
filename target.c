/* target.c — the Target descriptions and the encoding queries (target.h). */
#include <stddef.h>
#include <stdio.h>
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
    // Tuned by tools/tune.py (2026-09-28): corpus -0.70%, CoreMark -0.61%,
    // ray tracer -0.83%, JPEG -0.52% cycles against the hand-set values
    // (2 5 16 2 4 1 4 4 10 6 4).
    .tune         = { .lc_reserve = 6, .lc_reserve_large = 6, .lc_large_body = 16,
                      .lc_cap_reserve = 5, .lc_max_hoist = 4, .lc_min_uses = 1,
                      .licm_reserve = 5, .licm_max = 4, .licm_dense_hi = 30,
                      .licm_dense_lo = 6, .lsr_reserve = 7, .ipra = 1, .ipra_reserve = 2, .inline_cf_nodes = 0, .frame_promote = 1, .spill_cost = 1, .spill_slots = 1,
                      .jt_min_cases = 12, .jt_density = 50 },
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
    // Tuned by tools/tune.py (2026-09-28): corpus -0.75%, CoreMark -2.65%,
    // ray tracer -2.09%, JPEG +0.27% against CPU4's hand-set values. With
    // CPU5's 16-bit immediates a hoisted constant mostly replaces a free
    // immediate operand, so constant hoisting is sparing and LSR is off.
    .tune         = { .lc_reserve = 14, .lc_reserve_large = 14, .lc_large_body = 64,
                      .lc_cap_reserve = 10, .lc_max_hoist = 2, .lc_min_uses = 3,
                      .licm_reserve = 4, .licm_max = 5, .licm_dense_hi = 1000,
                      .licm_dense_lo = 14, .lsr_reserve = 15, .ipra = 1, .ipra_reserve = 0, .inline_cf_nodes = 160, .frame_promote = 1, .spill_cost = 1, .spill_slots = 1,
                      // Jump tables (2026-10-05): rv32emu -7.3%; 7 cases stay a
                      // chain (CoreMark's state machine is 2.8% slower as a table).
                      .jt_min_cases = 8, .jt_density = 10 },
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

// ---- optimiser budgets ------------------------------------------------

Tune g_tune;

static const struct { const char *name; size_t off; } tune_fields[] = {
#define TF(n) { #n, offsetof(Tune, n) }
    TF(lc_reserve), TF(lc_reserve_large), TF(lc_large_body), TF(lc_cap_reserve),
    TF(lc_max_hoist), TF(lc_min_uses), TF(licm_reserve), TF(licm_max),
    TF(licm_dense_hi), TF(licm_dense_lo), TF(lsr_reserve), TF(ipra), TF(ipra_reserve), TF(inline_cf_nodes), TF(frame_promote), TF(spill_cost), TF(spill_slots),
    TF(jt_min_cases), TF(jt_density),
#undef TF
};

int tune_set(const char *nv) {
    const char *eq = strchr(nv, '=');
    if (!eq) return 0;
    for (size_t i = 0; i < sizeof tune_fields / sizeof tune_fields[0]; i++)
        if (strlen(tune_fields[i].name) == (size_t)(eq - nv) &&
            !strncmp(tune_fields[i].name, nv, eq - nv)) {
            *(int *)((char *)&g_tune + tune_fields[i].off) = atoi(eq + 1);
            return 1;
        }
    return 0;
}

void tune_list(void) {
    for (size_t i = 0; i < sizeof tune_fields / sizeof tune_fields[0]; i++)
        fprintf(stderr, "%s=%d\n", tune_fields[i].name,
                *(int *)((char *)&g_tune + tune_fields[i].off));
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

int isa_pcrel_range(const char *m, long *lo, long *hi) {
    const IsaInstr *in = isa_lookup(m);
    if (!in) return 0;
    for (int j = 0; j < in->nops; j++)
        if (in->ops[j].kind == ISA_PCREL) {
            *lo = -(1L << (in->ops[j].bits - 1)); *hi = (1L << (in->ops[j].bits - 1)) - 1;
            return 1;
        }
    return 0;
}

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
