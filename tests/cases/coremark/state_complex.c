// EXPECT_R0: 1
// EXPECT_STDOUT: Done\n
/* Minimal single-file testcase for core_state_transition infinite loop.
 * Reduced from coremark core_state.c / core_portme.h - all types inlined.
 * Compile: smallcc -arch cpu4 -O2 -o test_state.s test_state.c
 * Run:     sim_c -arch cpu4 -maxsteps 50000 test_state.s
 */
#include <stdio.h>
typedef unsigned char  ee_u8;
typedef unsigned int   ee_u32;

typedef enum CORE_STATE
{
    CORE_START = 0,
    CORE_INVALID,
    CORE_S1,
    CORE_S2,
    CORE_INT,
    CORE_FLOAT,
    CORE_EXPONENT,
    CORE_SCIENTIFIC,
    NUM_CORE_STATES
} core_state_e;

static ee_u8
ee_isdigit(ee_u8 c)
{
    ee_u8 retval;
    retval = ((c >= '0') & (c <= '9')) ? 1 : 0;
    return retval;
}

enum CORE_STATE
core_state_transition(ee_u8 **instr, ee_u32 *transition_count)
{
    ee_u8 *         str = *instr;
    ee_u8           NEXT_SYMBOL;
    enum CORE_STATE state = CORE_START;
    for (; *str && state != CORE_INVALID; str++)
    {
        NEXT_SYMBOL = *str;
        if (NEXT_SYMBOL == ',') /* end of this input */
        {
            str++;
            break;
        }
        switch (state)
        {
            case CORE_START:
                if (ee_isdigit(NEXT_SYMBOL))
                {
                    state = CORE_INT;
                }
                else
                {
                    state = CORE_INVALID;
                    transition_count[CORE_INVALID]++;
                }
                transition_count[CORE_START]++;
                break;
            case CORE_INT:
                if (NEXT_SYMBOL == '.')
                {
                    state = CORE_FLOAT;
                    transition_count[CORE_INT]++;
                }
                else if (!ee_isdigit(NEXT_SYMBOL))
                {
                    state = CORE_INVALID;
                    transition_count[CORE_INT]++;
                }
                break;
            default:
                break;
        }
    }
    *instr = str;
    return state;
}

int main(void)
{
    static ee_u8 input[] =
        "5\0";

    ee_u32 transition_count[NUM_CORE_STATES];
    ee_u32 i;
    ee_u8 *p = input;

    for (i = 0; i < NUM_CORE_STATES; i++)
        transition_count[i] = 0;

    
    while (*p != 0)
    {
        enum CORE_STATE fstate = core_state_transition(&p, transition_count);
        (void)fstate;
    }
    printf("Done\n");
    return 1;
}
