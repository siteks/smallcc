// EXPECT_R0: 0
// SIM_ARGS: -cores 2
// Single-writer/single-reader mailbox ping-pong over shared SDRAM (the
// communication primitive the ray-tracer tile farm uses). Core 0 sends
// values, core 1 doubles them and replies. No atomics: each word has
// exactly one writer; the non-zero value doubles as the full/empty flag.
#include <stdint.h>

#define CORE_ID (*(volatile uint32_t *)0xff24)
#define REQ     (*(volatile uint32_t *)0x00100000)   /* core0 -> core1 */
#define ACK     (*(volatile uint32_t *)0x00100004)   /* core1 -> core0 */

#define ROUNDS 50

int main(void) {
    uint32_t i, v;

    if (CORE_ID == 1) {
        for (i = 0; i < ROUNDS; i++) {
            while ((v = REQ) == 0) { }
            REQ = 0;
            ACK = v * 2u;
        }
        return 0;
    }

    for (i = 1; i <= ROUNDS; i++) {
        REQ = i;
        while ((v = ACK) == 0) { }
        ACK = 0;
        if (v != i * 2u) return (int)i;
    }
    return 0;
}
