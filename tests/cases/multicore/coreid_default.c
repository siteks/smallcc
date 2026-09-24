// EXPECT_R0: 0
// Without -cores, the multi-core MMIO registers must read as a
// single-core system: CORE_ID (0xFF24) = 0, NCORES (0xFF28) = 1.
#include <stdint.h>

int main(void) {
    volatile uint32_t *core_id = (volatile uint32_t *)0xff24;
    volatile uint32_t *ncores  = (volatile uint32_t *)0xff28;
    if (*core_id != 0) return 1;
    if (*ncores != 1) return 2;
    return 0;
}
