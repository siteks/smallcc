// EXPECT_R0: 0
// TIMEOUT: 4000000
// Sign extension written as a conditional OR, as instruction decoders do
// (mini-rv32ima): `x | ((x & 2^k) ? -2^(k+1) : 0)` and `if (x & 2^k)
// x |= -2^(k+1)`. When every bit of x above k is known zero this is
// sext(x, k+1), compiled as shifts (one shrsli when x is y >> (31-k)).
// The n_ functions are look-alikes that must not be rewritten: x wider
// than k+1 bits, the wrong mask, swapped arms, a bit other than the top.
// References compare against 2^(k) instead of testing the bit.
unsigned i_ternary(unsigned ir) { unsigned imm = ir >> 20; return imm | ((imm & 0x800) ? 0xfffff000 : 0); }
unsigned s_if(unsigned ir) {
    unsigned a = ((ir >> 7) & 0x1f) | ((ir & 0xfe000000) >> 20);
    if (a & 0x800) a |= 0xfffff000;
    return a;
}
int j_if_signed(unsigned ir) {
    int r = ((ir & 0x80000000) >> 11) | ((ir & 0x7fe00000) >> 20) | ((ir & 0x00100000) >> 9) | (ir & 0x000ff000);
    if (r & 0x00100000) r |= 0xffe00000;
    return r;
}
unsigned k0(unsigned x)  { unsigned b = x & 1; return b | ((b & 1) ? 0xfffffffe : 0); }
unsigned k30(unsigned x) { unsigned v = x >> 1; return v | ((v & 0x40000000) ? 0x80000000 : 0); }
unsigned n_wide(unsigned x)  { return x | ((x & 0x800) ? 0xfffff000 : 0); }
unsigned n_mask(unsigned ir) { unsigned imm = ir >> 20; return imm | ((imm & 0x800) ? 0xffffe000 : 0); }
unsigned n_swap(unsigned ir) { unsigned imm = ir >> 20; return imm | ((imm & 0x800) ? 0 : 0xfffff000); }
unsigned n_bit(unsigned ir)  { unsigned imm = ir >> 20; return imm | ((imm & 0x400) ? 0xfffff000 : 0); }

unsigned ref_sext(unsigned v, int n) { return v >= (1u << (n - 1)) ? v - (1u << n) : v; }

int main(void) {
    unsigned i, ir = 1, bad = 0;
    for (i = 0; i < 150; i++) {
        unsigned s, j, imm, w;
        ir = ir * 1103515245u + 12345u;
        if (i < 4) ir = i == 0 ? 0 : i == 1 ? 0xffffffffu : i == 2 ? 0x80000000u : 0x7fffffffu;
        imm = ir >> 20;
        if (i_ternary(ir) != ref_sext(imm, 12)) bad |= 1;
        s = ((ir >> 7) & 0x1f) | ((ir & 0xfe000000) >> 20);
        if (s_if(ir) != ref_sext(s, 12)) bad |= 2;
        j = ((ir & 0x80000000) >> 11) | ((ir & 0x7fe00000) >> 20) | ((ir & 0x00100000) >> 9) | (ir & 0x000ff000);
        if ((unsigned)j_if_signed(ir) != ref_sext(j, 21)) bad |= 4;
        if (k0(ir) != ((ir & 1) ? 0xffffffffu : 0)) bad |= 8;
        if (k30(ir) != ref_sext(ir >> 1, 31)) bad |= 16;
        w = ((ir >> 11) & 1) ? ir | 0xfffff000 : ir;
        if (n_wide(ir) != w) bad |= 32;
        if (n_mask(ir) != (((imm >> 11) & 1) ? imm | 0xffffe000 : imm)) bad |= 64;
        if (n_swap(ir) != (((imm >> 11) & 1) ? imm : imm | 0xfffff000)) bad |= 128;
        if (n_bit(ir) != (((imm >> 10) & 1) ? imm | 0xfffff000 : imm)) bad |= 256;
    }
    return (int)bad;
}
