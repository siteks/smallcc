// EXPECT_R0: 0
// Byte and halfword access into SDRAM through the transparent pointer
// path. Verifies llb/llbx/llw/llwx and slb/slw against a 32-bit baseline,
// and that the alignment check still fires (well, doesn't fire here —
// negative tests for misalignment would crash, which the harness doesn't
// support directly, so we just exercise valid alignments).
#include <stdint.h>

int main(void) {
    volatile uint8_t  *bp = (volatile uint8_t  *)0x10000;
    volatile uint16_t *wp = (volatile uint16_t *)0x10000;
    volatile uint32_t *lp = (volatile uint32_t *)0x10000;

    /* Lay down a 32-bit pattern, read it back through narrower views. */
    lp[0] = 0x44332211;
    if (bp[0] != 0x11) return 1;
    if (bp[1] != 0x22) return 2;
    if (bp[2] != 0x33) return 3;
    if (bp[3] != 0x44) return 4;
    if (wp[0] != 0x2211) return 5;
    if (wp[1] != 0x4433) return 6;

    /* Byte-store a value with the high bit set, read back zero-extended. */
    bp[100] = 0xC0;
    if (bp[100] != 0xC0) return 7;
    /* Loaded as int (sign-extending llbx for signed char) — ensure it
     * matches the written byte truncated to int after default promotion. */
    signed char sc = ((volatile signed char *)bp)[100];
    if ((int)sc != -64) return 8;     /* 0xC0 sign-extended */
    unsigned char uc = bp[100];
    if ((int)uc != 0xC0) return 9;    /* zero-extended via llb */

    /* Halfword store with the high bit set, both signed and unsigned views. */
    wp[10] = 0x8001;
    short ss = ((volatile short *)wp)[10];
    if ((int)ss != (int)(short)0x8001) return 10;
    unsigned short us = wp[10];
    if ((int)us != 0x8001) return 11;

    return 0;
}
