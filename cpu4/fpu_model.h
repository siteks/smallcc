/* CPU4 floating-point semantics, bit-exact. This header IS the specification
 * for fadd/fsub/fmul/fdiv/flt/fle/itof/ftoi/frecip/frsqrt (docs/isa/cpu4.md,
 * "Floating-point semantics"); sim_c, irsim and the compiler's constant folder
 * include it, cpu4/cpu.py ports it line for line, and the RTL is checked
 * against it by tests/cases/floats/fpu_vectors.c.
 *
 *   - Operands with exponent field 0 are +-0 (denormals flush to zero).
 *   - Operands with exponent field 255 are treated as finite numbers of
 *     that magnitude; a result whose exponent reaches 255 saturates to
 *     +-inf; the machine never produces NaN.
 *   - fadd/fsub/fmul: correctly rounded, round-to-nearest-even, on the
 *     values above. A zero or underflowing result is +0. fmul with a zero
 *     operand gives a signed zero.
 *   - frecip: table seed; zero, denormal or huge (exponent >= 254) inputs give
 *     a zero with the input's sign.
 *   - fdiv(a,b) = fmul(a, frecip(b)): reciprocal seed times a, ~2^-16.
 *   - flt/fle compare sign and magnitude (so -0 < +0 is false); exponent
 *     255 patterns compare by magnitude.
 *   - itof: truncates toward zero for |i| > 2^24. ftoi: truncates toward
 *     zero; |x| < 1 gives 0; values that do not fit wrap (bits shifted out).
 */
#ifndef CPU4_FPU_MODEL_H
#define CPU4_FPU_MODEL_H
#include <stdint.h>
#include "fpu_roms.h"

static inline uint32_t cpu4_frecip(uint32_t x) {
    uint32_t sign = x & 0x80000000u, e = (x >> 23) & 0xff, i = (x >> 13) & 0x3ff;
    if (e == 0) return sign;                                    /* zero/denormal -> signed zero */
    int ee = (i == 0) ? (254 - (int)e) : (253 - (int)e);
    if (ee <= 0) return sign;                                   /* 1/huge underflows -> signed zero */
    return sign | ((uint32_t)ee << 23) | ((uint32_t)FRECIP_ROM[i] << 7);
}

static inline uint32_t cpu4_frsqrt(uint32_t x) {
    uint32_t e = (x >> 23) & 0xff, m = x & 0x7fffff;
    if (e == 0)   return 0x7f800000u;
    if (e == 255) return 0;
    uint32_t p = (~e) & 1, i = (p << 9) | (m >> 14);
    uint32_t ee = (e & 1) ? (379 - e) / 2 : (380 - e) / 2;
    return ((ee & 0xff) << 23) | ((uint32_t)FRSQRT_ROM[i] << 7);
}

/* Pack a rounded result: exp9 is the signed 9-bit exponent candidate,
 * mant the 24-bit mantissa (with the leading one) after rounding. */
static inline uint32_t cpu4_fpack(uint32_t sign, int exp9, uint32_t mant24) {
    if (exp9 <= 0)   return 0;                                  /* underflow -> +0 */
    if (exp9 >= 255) return sign | 0x7f800000u;                 /* overflow  -> +-inf */
    return sign | ((uint32_t)exp9 << 23) | (mant24 & 0x7fffff);
}

/* Round-to-nearest-even on a 24-bit mantissa given guard and sticky.
 * Returns the mantissa; *exp9 is bumped when rounding carries out. */
static inline uint32_t cpu4_rne(uint32_t mant24, uint32_t guard, uint32_t sticky, int *exp9) {
    uint32_t rnd = guard & (sticky | (mant24 & 1));
    uint32_t m = mant24 + rnd;
    if (m & 0x1000000u) { m >>= 1; (*exp9)++; }
    return m;
}

static inline uint32_t cpu4_fmul(uint32_t a, uint32_t b) {
    uint32_t sa = a & 0x80000000u, sb = b & 0x80000000u, sign = sa ^ sb;
    uint32_t ea = (a >> 23) & 0xff, eb = (b >> 23) & 0xff;
    if (ea == 0 || eb == 0) return sign;                        /* signed zero */
    uint64_t p = (uint64_t)(0x800000u | (a & 0x7fffff)) * (uint64_t)(0x800000u | (b & 0x7fffff)); /* 48 bits */
    int norm = (p >> 47) & 1;
    uint32_t mant, guard, sticky;
    if (norm) { mant = (uint32_t)(p >> 24) & 0xffffff; guard = (p >> 23) & 1; sticky = (p & 0x7fffff) != 0; }
    else      { mant = (uint32_t)(p >> 23) & 0xffffff; guard = (p >> 22) & 1; sticky = (p & 0x3fffff) != 0; }
    int exp9 = (int)ea + (int)eb - 127 + norm;
    mant = cpu4_rne(mant, guard, sticky, &exp9);
    return cpu4_fpack(sign, exp9, mant);
}

static inline uint32_t cpu4_fadd(uint32_t a, uint32_t b) {
    uint32_t sa = a >> 31, sb = b >> 31;
    uint32_t ea = (a >> 23) & 0xff, eb = (b >> 23) & 0xff;
    uint32_t ma = a & 0x7fffff, mb = b & 0x7fffff;
    uint32_t fa = ea ? (0x800000u | ma) : 0, fb = eb ? (0x800000u | mb) : 0;
    int a_bigger = (ea > eb) || (ea == eb && ma >= mb);
    uint32_t big = a_bigger ? fa : fb, small = a_bigger ? fb : fa;
    uint32_t bexp = a_bigger ? ea : eb;
    uint32_t d = a_bigger ? ea - eb : eb - ea;
    uint32_t sign = a_bigger ? sa : sb;
    int sub = (sa ^ sb) != 0;
    uint32_t ext_small = small << 3, aligned, sticky;             /* 27 bits: G R S */
    if (d >= 27) { aligned = 0; sticky = small != 0; }
    else { aligned = ext_small >> d; sticky = (ext_small & ((1u << d) - 1)) != 0; }
    aligned |= sticky;
    uint32_t bigx = big << 3;
    uint32_t sum = sub ? bigx - aligned : bigx + aligned;         /* 28 bits */
    if (sum == 0) return 0;                                       /* +0 */
    int exp9; uint32_t mant, guard, st;
    if (sum & 0x8000000u) {                                       /* carry: leading one at bit 27 */
        mant = (sum >> 4) & 0xffffff; guard = (sum >> 3) & 1; st = (sum & 7) != 0;
        exp9 = (int)bexp + 1;
    } else {
        int lzc = 0; uint32_t v = sum & 0x7ffffff;                /* 27 bits */
        while (!(v & 0x4000000u)) { v <<= 1; lzc++; }
        mant = (v >> 3) & 0xffffff; guard = (v >> 2) & 1; st = (v & 3) != 0;
        exp9 = (int)bexp - lzc;
    }
    mant = cpu4_rne(mant, guard, st, &exp9);
    return cpu4_fpack(sign << 31, exp9, mant);
}

static inline uint32_t cpu4_fsub(uint32_t a, uint32_t b) { return cpu4_fadd(a, b ^ 0x80000000u); }
static inline uint32_t cpu4_fdiv(uint32_t a, uint32_t b) { return cpu4_fmul(a, cpu4_frecip(b)); }

static inline uint32_t cpu4_flt(uint32_t a, uint32_t b) {
    uint32_t sa = a >> 31, sb = b >> 31, aa = a & 0x7fffffff, ab = b & 0x7fffffff;
    if (aa == 0 && ab == 0) return 0;
    if (sa && !sb) return 1;
    if (!sa && sb) return 0;
    return sa ? (aa > ab) : (aa < ab);
}
static inline uint32_t cpu4_fle(uint32_t a, uint32_t b) {
    uint32_t sa = a >> 31, sb = b >> 31, aa = a & 0x7fffffff, ab = b & 0x7fffffff;
    if (aa == 0 && ab == 0) return 1;
    if (sa && !sb) return 1;
    if (!sa && sb) return 0;
    return sa ? (aa >= ab) : (aa <= ab);
}

static inline uint32_t cpu4_itof(uint32_t i) {
    if (i == 0) return 0;
    uint32_t sign = i & 0x80000000u;
    uint32_t abs = sign ? (uint32_t)(0u - i) : i;               /* 0x80000000 stays */
    int pos = 31; while (!((abs >> pos) & 1)) pos--;
    uint32_t mant = (pos > 23) ? (abs >> (pos - 23)) : (abs << (23 - pos));   /* truncates */
    return sign | ((uint32_t)(127 + pos) << 23) | (mant & 0x7fffff);
}

static inline uint32_t cpu4_ftoi(uint32_t x) {
    uint32_t sign = x >> 31, e = (x >> 23) & 0xff;
    if (e == 0) return 0;
    uint32_t full = 0x800000u | (x & 0x7fffff), abs;
    if (e < 127) abs = 0;
    else if (e >= 150) { uint32_t sh = e - 150; abs = (sh >= 32) ? 0 : (full << sh); }
    else abs = full >> (150 - e);
    return sign ? (uint32_t)(0u - abs) : abs;
}
#endif
