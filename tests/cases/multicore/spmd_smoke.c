// EXPECT_R0: 0
// SIM_ARGS: -cores 4
// SPMD smoke test: every core writes a token derived from CORE_ID into a
// shared SDRAM slot; core 0 busy-waits for all slots (the round-robin
// scheduler must keep workers running under a spinning core), then checks
// the sum. Also checks private-BRAM isolation: each core's global counter
// must reach its own 1000+id, unaffected by the other cores.
#include <stdint.h>

#define CORE_ID (*(volatile uint32_t *)0xff24)
#define NCORES  (*(volatile uint32_t *)0xff28)
#define SLOTS   ((volatile uint32_t *)0x00100000)

static uint32_t g_counter = 0;   /* private per core */

int main(void) {
    uint32_t id = CORE_ID;
    uint32_t n  = NCORES;
    uint32_t i, sum, expect;

    if (n != 4) return 2;

    for (i = 0; i < 1000u + id; i++) g_counter = g_counter + 1;

    SLOTS[id] = (id + 1u) + (g_counter << 16);

    if (id != 0) return 0;   /* workers halt; core 0 collects */

    for (i = 1; i < n; i++)
        while (SLOTS[i] == 0) { }

    sum = 0;
    expect = 0;
    for (i = 0; i < n; i++) {
        sum = sum + SLOTS[i];
        expect = expect + (i + 1u) + ((1000u + i) << 16);
    }
    return (sum == expect) ? 0 : 1;
}
