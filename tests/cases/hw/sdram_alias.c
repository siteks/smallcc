// EXPECT_R0: 0
// SDRAM addresses with addr[31:25] != 0 alias the SDRAM range (the chip
// only sees addr[24:0]). The first 64 KB of SDRAM (chip byte 0..0xFFFF)
// is shadowed by BRAM in the un-aliased CPU range, so the only way to
// reach it is via the alias range. Verify the write goes to the *same*
// chip byte regardless of alias value.
#include <stdint.h>

int main(void) {
    /* Two CPU addresses that should map to the same chip byte
     * (addr[24:0] == 0x10000). */
    volatile uint32_t *direct = (volatile uint32_t *)0x10000;     /* normal range */
    volatile uint32_t *aliased = (volatile uint32_t *)0x2010000;  /* alias range */

    direct[0] = 0xAABBCCDD;
    if (aliased[0] != 0xAABBCCDD) return 1;

    aliased[1] = 0x11223344;
    if (direct[1] != 0x11223344) return 2;

    /* The alias range can also reach the un-shadowed first 64 KB of SDRAM
     * (chip bytes 0..0xFFFF), which the un-aliased range can't. */
    volatile uint32_t *low_sdram = (volatile uint32_t *)0x2000100;
    low_sdram[0] = 0xFEEDFACE;
    if (low_sdram[0] != 0xFEEDFACE) return 3;

    return 0;
}
