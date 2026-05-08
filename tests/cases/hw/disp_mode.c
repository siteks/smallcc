// EXPECT_R0: 0
// DISP_MODE (0xFF1C) is a 4-bit RW register. Bits 0 (320x240) and 1
// (8bpp indexed) are independently meaningful; we just verify the
// write/read round-trip here.
#include <stdint.h>

int main(void) {
    volatile uint32_t *mmio = (volatile uint32_t *)0xff00;
    /* word index 7 == byte address 0xFF1C */
    mmio[7] = 0;          if (mmio[7] != 0)   return 1;
    mmio[7] = 1;          if (mmio[7] != 1)   return 2;
    mmio[7] = 2;          if (mmio[7] != 2)   return 3;
    mmio[7] = 3;          if (mmio[7] != 3)   return 4;
    mmio[7] = 0;          if (mmio[7] != 0)   return 5;
    return 0;
}
