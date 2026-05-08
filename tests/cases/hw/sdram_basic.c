// EXPECT_R0: 0
// Round-trip writes through the transparent SDRAM pointer path at byte
// 0x10000 (the start of accessible SDRAM — the first 64 KB chip-side is
// shadowed by BRAM and unreachable except via the alias range).
#include <stdint.h>

int main(void) {
    volatile uint32_t *p = (volatile uint32_t *)0x10000;
    p[0]   = 0xCAFEBABE;
    p[1]   = 0xDEADBEEF;
    p[100] = 0x12345678;

    if (p[0]   != 0xCAFEBABE) return 1;
    if (p[1]   != 0xDEADBEEF) return 2;
    if (p[100] != 0x12345678) return 3;

    /* Far end of SDRAM (16 MB into the 32 MB region). */
    volatile uint32_t *far = (volatile uint32_t *)0x1000000;
    far[0] = 0xA1B2C3D4;
    if (far[0] != 0xA1B2C3D4) return 4;

    return 0;
}
