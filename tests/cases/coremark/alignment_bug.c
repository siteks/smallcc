// EXPECT_R0: 0

/* Single-file coremark test case for smallcc/cpu4 compiler bug.
   Reproduces: writes to low addresses and illegal non-aligned accesses
   when main() returns early after core_init_matrix (before iterate).

   Build: smallcc -I. -arch cpu4 -O2 -o test_single.s test_single.c
   Run:   sim_c -arch cpu4 test_single.s
*/

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef int16_t  ee_s16;
typedef uint16_t ee_u16;
typedef int32_t  ee_s32;
typedef uint8_t  ee_u8;
typedef uint32_t ee_u32;

#define ee_printf printf


typedef struct RESULTS_S {
    ee_s16              seed1;
    ee_u32              execs;
} core_results;

/* ---- Seeds (from core_portme.c) ---- */
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
volatile ee_s32 seed4_volatile = 1;
volatile ee_s32 seed5_volatile = 0;


ee_s32 get_seed_32(int i) {
    switch (i) {
        case 1: return seed1_volatile;
        default: return 0;
    }
}
#define get_seed(x) (ee_s16)get_seed_32(x)




int main(int argc, char *argv[])
{
    ee_printf("Started\n");
    ee_u16       i, j = 0, num_algorithms = 0;
    core_results results[1];
    ee_u8 stack_memblock[2000];

    if (sizeof(struct list_head_s) > 128) {
        ee_printf("list_head structure too big for comparable data!\n");
        return 0;
    }
    results[0].seed1      = get_seed(1);
    results[0].execs      = 1;


    return 0;  /* <-- early return: bug triggers here */
}
