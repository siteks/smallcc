// SIM_ARGS: -load 0x20000 {dir}/sim_load.c -load 0x2030001 {dir}/sim_load.c
// EXPECT_R0: 0
// sim_c -load copies a file into SDRAM before the program starts. This
// test loads its own source twice: at 0x20000, and at an aliased,
// odd address whose chip byte is 0x30001.
#include <stdint.h>

int main(void) {
    static const char head[] = "// SIM_ARGS: -load";
    const uint8_t *a = (const uint8_t *)0x20000;
    const uint8_t *b = (const uint8_t *)0x30001;
    int i;
    for (i = 0; head[i]; i++) {
        if (a[i] != head[i]) return 1;
        if (b[i] != head[i]) return 2;
    }
    if (b[-1] != 0) return 3;                 /* nothing before the load address */
    if (*(const uint32_t *)0x20000 != 0x53202f2f) return 4;   /* "// S", little-endian */
    return 0;
}
