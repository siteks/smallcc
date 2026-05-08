// EXPECT_R0: 0
// SDRAM MMIO keyhole — software-paced single-word access. The hardware
// implements this as a separate pbus master from the transparent path;
// both reach the same chip. Tests the documented sequence:
//   1. write SDRAM_ADDR (word address)
//   2. write SDRAM_WDATA (for writes)
//   3. write SDRAM_CTRL (1=read, 2=write)
//   4. poll SDRAM_STATUS until busy clears
//   5. read SDRAM_RDATA (for reads)
#include <stdint.h>

int main(void) {
    volatile uint32_t *mmio = (volatile uint32_t *)0xff00;
    /*  word index → byte address
     *   [40] = 0xFFA0  SDRAM_ADDR (word address into chip)
     *   [41] = 0xFFA4  SDRAM_WDATA
     *   [42] = 0xFFA8  SDRAM_CTRL
     *   [43] = 0xFFAC  SDRAM_RDATA
     *   [44] = 0xFFB0  SDRAM_STATUS
     */

    /* Write 0xCAFEBABE to chip word 0x4000 (= byte 0x10000). */
    mmio[40] = 0x4000;
    mmio[41] = 0xCAFEBABE;
    mmio[42] = 2;
    while (mmio[44] & 1u) ;

    /* Read it back via the transparent path. */
    volatile uint32_t *p = (volatile uint32_t *)0x10000;
    if (p[0] != 0xCAFEBABE) return 1;

    /* Now read the same word via the keyhole. */
    mmio[40] = 0x4000;
    mmio[42] = 1;
    while (mmio[44] & 1u) ;
    if (mmio[43] != 0xCAFEBABE) return 2;

    /* Cross-path round-trip: transparent write, keyhole read. */
    p[1] = 0xDEADBEEF;        /* byte 0x10004 = chip word 0x4001 */
    mmio[40] = 0x4001;
    mmio[42] = 1;
    while (mmio[44] & 1u) ;
    if (mmio[43] != 0xDEADBEEF) return 3;

    return 0;
}
