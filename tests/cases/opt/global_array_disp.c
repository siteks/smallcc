// EXPECT_R0: 0
// Global array elements addressed with the array in the scaled load/store
// displacement (legalize Pass G3 on CPU5: lll rd, ri, g/4+k): words,
// signed and unsigned halfwords, an array inside a global struct, a
// negative constant offset, stores, and a byte array (not folded).
#include <stdint.h>
uint32_t w[16];
int16_t  hs[16];
uint16_t hu[16];
uint8_t  b[16];
struct { uint32_t pad; uint32_t regs[8]; uint16_t h[4]; } st;

uint32_t get_w(unsigned i)   { return w[i & 15]; }
void     put_w(unsigned i, uint32_t v) { w[i & 15] = v; }
int      get_hs(unsigned i)  { return hs[i & 15]; }
unsigned get_hu(unsigned i)  { return hu[i & 15]; }
uint32_t get_prev(unsigned i) { return w[(i & 15) - 1 + 1]; }
uint32_t reg(unsigned ir)    { return st.regs[(ir >> 15) & 7]; }
void     set_reg(unsigned ir, uint32_t v) { st.regs[(ir >> 7) & 7] = v; }
unsigned get_sh(unsigned i)  { return st.h[i & 3]; }

int main(void) {
    unsigned i, bad = 0;
    for (i = 0; i < 16; i++) {
        put_w(i, 0x01010101u * i + 0x80000000u);
        hs[i] = (int16_t)(-1000 * (int)i);
        hu[i] = (uint16_t)(0xf000 + i);
        b[i] = (uint8_t)(200 + i);
    }
    for (i = 0; i < 8; i++) set_reg(i << 7, 0x11111111u * i);
    for (i = 0; i < 4; i++) st.h[i] = (uint16_t)(0xa000 + i);
    st.pad = 0xdeadbeef;
    for (i = 0; i < 16; i++) {
        if (get_w(i) != 0x01010101u * i + 0x80000000u) bad |= 1;
        if (get_hs(i) != -1000 * (int)i) bad |= 2;
        if (get_hu(i) != 0xf000 + i) bad |= 4;
        if (b[i & 15] != 200 + i) bad |= 8;
        if (get_prev(i) != w[i]) bad |= 16;
    }
    for (i = 0; i < 8; i++) if (reg(i << 15) != 0x11111111u * i) bad |= 32;
    for (i = 0; i < 4; i++) if (get_sh(i) != 0xa000 + i) bad |= 64;
    if (st.pad != 0xdeadbeef) bad |= 128;
    return (int)bad;
}
