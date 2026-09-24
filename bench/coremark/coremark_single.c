/*
 * CoreMark v1.0 — single-file build for smallcc (CPU4, ILP32 model)
 *
 * Faithful concatenation of the canonical CoreMark sources from ../coremark
 * (core_main.c, core_list_join.c, core_matrix.c, core_state.c, core_util.c,
 * cpu3/core_portme.c, cpu3/core_portme.h, coremark.h). Only the per-platform
 * porting layer has been adapted:
 *
 *   - HAS_PRINTF=1: ee_printf is just printf (smallcc supplies one).
 *   - MAIN_HAS_NOARGC=1: our crt0 calls main() without argc/argv, so the
 *     dispatch picks the no-arg main signature.
 *   - SEED_METHOD=SEED_VOLATILE, MEM_METHOD=MEM_STACK, MULTITHREAD=1.
 *   - ee_ptr_int is uint32_t to match ILP32 pointer width.
 *   - barebones_clock reads the CPU4 µs counter at 0xFF00.
 *
 * Configured for the 2K performance run (TOTAL_DATA_SIZE=2000, seeds 0/0/0x66,
 * iterations=1). Expected output for that configuration:
 *
 *   seedcrc   0xe9f5
 *   crclist   0xe714
 *   crcmatrix 0x1fd7
 *   crcstate  0x8e3a
 *   crcfinal  0xe714  (same as crclist with iterations=1)
 *   "Correct operation validated."
 *
 * Compile and run:
 *   ./smallcc -arch cpu4 -o bench/coremark/coremark.s bench/coremark/coremark_single.c
 *   ./sim_c -arch cpu4 -maxsteps 4000000 bench/coremark/coremark.s   (from the smallcc root)
 *
 * Original copyright (matrix/state/list/main/util/portme bodies):
 *   Copyright 2018 Embedded Microprocessor Benchmark Consortium (EEMBC),
 *   licensed under the Apache License 2.0. Original author: Shay Gal-on.
 */

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================
 * Porting layer (was cpu3/core_portme.h)
 * ============================================================ */

#define HAS_FLOAT          1
#define HAS_TIME_H         1
#define USE_CLOCK          1
#define HAS_STDIO          1
#define HAS_PRINTF         1   /* use smallcc's printf */
#define MAIN_HAS_NOARGC    1   /* crt0 calls main() with no args */
#define MAIN_HAS_NORETURN  0

#define COMPILER_VERSION   "smallcc"
#define COMPILER_FLAGS     "-arch cpu4"
#define MEM_LOCATION       "STACK"

#define SEED_ARG           0
#define SEED_FUNC          1
#define SEED_VOLATILE      2
#define SEED_METHOD        SEED_VOLATILE

#define MEM_STATIC         0
#define MEM_MALLOC         1
#define MEM_STACK          2
#define MEM_METHOD         MEM_STACK

#define MULTITHREAD        1

#ifndef ITERATIONS
#define ITERATIONS         1
#endif

#ifndef TOTAL_DATA_SIZE
#define TOTAL_DATA_SIZE    (2 * 1000)
#endif

/* TOTAL_DATA_SIZE-keyed run-mode auto-pick (matches cpu3/core_portme.h) */
#if !defined(PROFILE_RUN) && !defined(PERFORMANCE_RUN) && !defined(VALIDATION_RUN)
#  if (TOTAL_DATA_SIZE == 1200)
#    define PROFILE_RUN     1
#  elif (TOTAL_DATA_SIZE == 2000)
#    define PERFORMANCE_RUN 1
#  else
#    define VALIDATION_RUN  1
#  endif
#endif

/* Sized-integer typedefs. Critically, ee_ptr_int must match pointer width;
 * under our ILP32 model that is 32 bits, not 16 (the canonical cpu3 portme
 * predates the ptr-widening transition). */
typedef int8_t          ee_s8;
typedef uint8_t         ee_u8;
typedef int16_t         ee_s16;
typedef uint16_t        ee_u16;
typedef int32_t         ee_s32;
typedef uint32_t        ee_u32;
typedef float           ee_f32;
typedef uint32_t        ee_ptr_int;
typedef size_t          ee_size_t;

#ifndef NULL
#define NULL ((void *)0)
#endif

#define align_mem(x) (void *)(4 + (((ee_ptr_int)(x) - 1) & ~3))

#define CORETIMETYPE   ee_u32
typedef ee_u32         CORE_TICKS;

typedef struct CORE_PORTABLE_S {
    ee_u8 portable_id;
} core_portable;

void portable_init(core_portable *p, int *argc, char *argv[]);
void portable_fini(core_portable *p);
int  ee_printf_ignored(const char *fmt, ...);   /* prototype kept; printf via macro below */

#if HAS_PRINTF
#define ee_printf printf
#endif

/* ============================================================
 * coremark.h (verbatim, less the include of core_portme.h since
 * we inlined the porting layer above)
 * ============================================================ */

#define FLAGS_STR COMPILER_FLAGS

#if HAS_FLOAT
typedef double secs_ret;
#else
typedef ee_u32 secs_ret;
#endif

#if MAIN_HAS_NORETURN
#define MAIN_RETURN_VAL
#define MAIN_RETURN_TYPE void
#else
#define MAIN_RETURN_VAL  0
#define MAIN_RETURN_TYPE int
#endif

void       start_time(void);
void       stop_time(void);
CORE_TICKS get_time(void);
secs_ret   time_in_secs(CORE_TICKS ticks);

ee_u16 crcu8(ee_u8 data, ee_u16 crc);
ee_u16 crc16(ee_s16 newval, ee_u16 crc);
ee_u16 crcu16(ee_u16 newval, ee_u16 crc);
ee_u16 crcu32(ee_u32 newval, ee_u16 crc);
ee_u8  check_data_types(void);

#define ID_LIST             (1 << 0)
#define ID_MATRIX           (1 << 1)
#define ID_STATE            (1 << 2)
#define ALL_ALGORITHMS_MASK (ID_LIST | ID_MATRIX | ID_STATE)
#define NUM_ALGORITHMS      3

typedef struct list_data_s {
    ee_s16 data16;
    ee_s16 idx;
} list_data;

typedef struct list_head_s {
    struct list_head_s *next;
    struct list_data_s *info;
} list_head;

#define MATDAT_INT 1
#if MATDAT_INT
typedef ee_s16 MATDAT;
typedef ee_s32 MATRES;
#else
typedef ee_f16 MATDAT;
typedef ee_f32 MATRES;
#endif

typedef struct MAT_PARAMS_S {
    int     N;
    MATDAT *A;
    MATDAT *B;
    MATRES *C;
} mat_params;

typedef enum CORE_STATE {
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

typedef struct RESULTS_S {
    /* inputs */
    ee_s16              seed1;
    ee_s16              seed2;
    ee_s16              seed3;
    void               *memblock[4];
    ee_u32              size;
    ee_u32              iterations;
    ee_u32              execs;
    struct list_head_s *list;
    mat_params          mat;
    /* outputs */
    ee_u16 crc;
    ee_u16 crclist;
    ee_u16 crcmatrix;
    ee_u16 crcstate;
    ee_s16 err;
    /* multithread specific */
    core_portable port;
} core_results;

void *iterate(void *pres);

/* List benchmark functions */
list_head *core_list_init(ee_u32 blksize, list_head *memblock, ee_s16 seed);
ee_u16     core_bench_list(core_results *res, ee_s16 finder_idx);

/* State benchmark functions */
void   core_init_state(ee_u32 size, ee_s16 seed, ee_u8 *p);
ee_u16 core_bench_state(ee_u32 blksize, ee_u8 *memblock,
                        ee_s16 seed1, ee_s16 seed2, ee_s16 step, ee_u16 crc);

/* Matrix benchmark functions */
ee_u32 core_init_matrix(ee_u32 blksize, void *memblk, ee_s32 seed, mat_params *p);
ee_u16 core_bench_matrix(mat_params *p, ee_s16 seed, ee_u16 crc);

/* ============================================================
 * Porting layer (was cpu3/core_portme.c)
 * ============================================================ */

#if VALIDATION_RUN
volatile ee_s32 seed1_volatile = 0x3415;
volatile ee_s32 seed2_volatile = 0x3415;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PERFORMANCE_RUN
volatile ee_s32 seed1_volatile = 0x0;
volatile ee_s32 seed2_volatile = 0x0;
volatile ee_s32 seed3_volatile = 0x66;
#endif
#if PROFILE_RUN
volatile ee_s32 seed1_volatile = 0x8;
volatile ee_s32 seed2_volatile = 0x8;
volatile ee_s32 seed3_volatile = 0x8;
#endif
volatile ee_s32 seed4_volatile = ITERATIONS;
volatile ee_s32 seed5_volatile = 0;

/* CPU4 µs counter at 0xFF00 (32-bit, wraps every ~71 minutes). */
static CORETIMETYPE barebones_clock(void) {
    return *(volatile unsigned long *)0xFF00;
}
#define CLOCKS_PER_SEC              1000000
#define GETMYTIME(_t)               (*_t = barebones_clock())
#define MYTIMEDIFF(fin, ini)        ((fin) - (ini))
#define TIMER_RES_DIVIDER           1
#define EE_TICKS_PER_SEC            (CLOCKS_PER_SEC / TIMER_RES_DIVIDER)

static CORETIMETYPE start_time_val, stop_time_val;

void start_time(void) { GETMYTIME(&start_time_val); }
void stop_time(void)  { GETMYTIME(&stop_time_val); }

CORE_TICKS get_time(void) {
    return (CORE_TICKS)(MYTIMEDIFF(stop_time_val, start_time_val));
}

secs_ret time_in_secs(CORE_TICKS ticks) {
    return ((secs_ret)ticks) / (secs_ret)EE_TICKS_PER_SEC;
}

ee_u32 default_num_contexts = 1;

void portable_init(core_portable *p, int *argc, char *argv[]) {
    (void)argc;
    (void)argv;
    if (sizeof(ee_ptr_int) != sizeof(ee_u8 *))
        ee_printf("ERROR! Please define ee_ptr_int to a type that holds a pointer!\n");
    if (sizeof(ee_u32) != 4)
        ee_printf("ERROR! Please define ee_u32 to a 32b unsigned type!\n");
    p->portable_id = 1;
}

void portable_fini(core_portable *p) {
    p->portable_id = 0;
}

/* ============================================================
 * core_util.c — get_seed_32, CRC, check_data_types
 * ============================================================ */

#if (SEED_METHOD == SEED_VOLATILE)
ee_s32 get_seed_32(int i) {
    ee_s32 retval;
    switch (i) {
        case 1: retval = seed1_volatile; break;
        case 2: retval = seed2_volatile; break;
        case 3: retval = seed3_volatile; break;
        case 4: retval = seed4_volatile; break;
        case 5: retval = seed5_volatile; break;
        default: retval = 0; break;
    }
    return retval;
}
#endif

ee_u16 crcu8(ee_u8 data, ee_u16 crc) {
    ee_u8 i = 0, x16 = 0, carry = 0;
    for (i = 0; i < 8; i++) {
        x16 = (ee_u8)((data & 1) ^ ((ee_u8)crc & 1));
        data >>= 1;
        if (x16 == 1) { crc ^= 0x4002; carry = 1; }
        else            carry = 0;
        crc >>= 1;
        if (carry) crc |= 0x8000; else crc &= 0x7fff;
    }
    return crc;
}

ee_u16 crcu16(ee_u16 newval, ee_u16 crc) {
    crc = crcu8((ee_u8)(newval), crc);
    crc = crcu8((ee_u8)((newval) >> 8), crc);
    return crc;
}

ee_u16 crcu32(ee_u32 newval, ee_u16 crc) {
    crc = crc16((ee_s16)newval, crc);
    crc = crc16((ee_s16)(newval >> 16), crc);
    return crc;
}

ee_u16 crc16(ee_s16 newval, ee_u16 crc) {
    return crcu16((ee_u16)newval, crc);
}

ee_u8 check_data_types(void) {
    ee_u8 retval = 0;
    if (sizeof(ee_u8) != 1)              { ee_printf("ERROR: ee_u8 is not an 8b datatype!\n"); retval++; }
    if (sizeof(ee_u16) != 2)             { ee_printf("ERROR: ee_u16 is not a 16b datatype!\n"); retval++; }
    if (sizeof(ee_s16) != 2)             { ee_printf("ERROR: ee_s16 is not a 16b datatype!\n"); retval++; }
    if (sizeof(ee_s32) != 4)             { ee_printf("ERROR: ee_s32 is not a 32b datatype!\n"); retval++; }
    if (sizeof(ee_u32) != 4)             { ee_printf("ERROR: ee_u32 is not a 32b datatype!\n"); retval++; }
    if (sizeof(ee_ptr_int) != sizeof(int *)) {
        ee_printf("ERROR: ee_ptr_int is not a datatype that holds an int pointer!\n");
        retval++;
    }
    if (retval > 0)
        ee_printf("ERROR: Please modify the datatypes in core_portme.h!\n");
    return retval;
}

/* ============================================================
 * core_state.c — state machine benchmark
 * ============================================================ */

ee_u16 core_bench_state(ee_u32 blksize, ee_u8 *memblock,
                        ee_s16 seed1, ee_s16 seed2, ee_s16 step, ee_u16 crc) {
    ee_u32 final_counts[NUM_CORE_STATES];
    ee_u32 track_counts[NUM_CORE_STATES];
    ee_u8 *p = memblock;
    ee_u32 i;
    enum CORE_STATE core_state_transition(ee_u8 **instr, ee_u32 *transition_count);

    for (i = 0; i < NUM_CORE_STATES; i++)
        final_counts[i] = track_counts[i] = 0;

    while (*p != 0) {
        enum CORE_STATE fstate = core_state_transition(&p, track_counts);
        final_counts[fstate]++;
    }
    p = memblock;
    while (p < (memblock + blksize)) {
        if (*p != ',') *p ^= (ee_u8)seed1;
        p += step;
    }
    p = memblock;
    while (*p != 0) {
        enum CORE_STATE fstate = core_state_transition(&p, track_counts);
        final_counts[fstate]++;
    }
    p = memblock;
    while (p < (memblock + blksize)) {
        if (*p != ',') *p ^= (ee_u8)seed2;
        p += step;
    }
    for (i = 0; i < NUM_CORE_STATES; i++) {
        crc = crcu32(final_counts[i], crc);
        crc = crcu32(track_counts[i], crc);
    }
    return crc;
}

static ee_u8 *intpat[4]   = { (ee_u8 *)"5012", (ee_u8 *)"1234", (ee_u8 *)"-874", (ee_u8 *)"+122" };
static ee_u8 *floatpat[4] = { (ee_u8 *)"35.54400", (ee_u8 *)".1234500", (ee_u8 *)"-110.700", (ee_u8 *)"+0.64400" };
static ee_u8 *scipat[4]   = { (ee_u8 *)"5.500e+3", (ee_u8 *)"-.123e-2", (ee_u8 *)"-87e+832", (ee_u8 *)"+0.6e-12" };
static ee_u8 *errpat[4]   = { (ee_u8 *)"T0.3e-1F", (ee_u8 *)"-T.T++Tq", (ee_u8 *)"1T3.4e4z", (ee_u8 *)"34.0e-T^" };

void core_init_state(ee_u32 size, ee_s16 seed, ee_u8 *p) {
    ee_u32 total = 0, next = 0, i;
    ee_u8 *buf = 0;
    size--;
    next = 0;
    while ((total + next + 1) < size) {
        if (next > 0) {
            for (i = 0; i < next; i++)
                *(p + total + i) = buf[i];
            *(p + total + i) = ',';
            total += next + 1;
        }
        seed++;
        switch (seed & 0x7) {
            case 0: case 1: case 2:
                buf = intpat[(seed >> 3) & 0x3]; next = 4; break;
            case 3: case 4:
                buf = floatpat[(seed >> 3) & 0x3]; next = 8; break;
            case 5: case 6:
                buf = scipat[(seed >> 3) & 0x3]; next = 8; break;
            case 7:
                buf = errpat[(seed >> 3) & 0x3]; next = 8; break;
            default: break;
        }
    }
    size++;
    while (total < size) { *(p + total) = 0; total++; }
}

static ee_u8 ee_isdigit(ee_u8 c) {
    return ((c >= '0') & (c <= '9')) ? 1 : 0;
}

enum CORE_STATE core_state_transition(ee_u8 **instr, ee_u32 *transition_count) {
    ee_u8          *str = *instr;
    ee_u8           NEXT_SYMBOL;
    enum CORE_STATE state = CORE_START;
    for (; *str && state != CORE_INVALID; str++) {
        NEXT_SYMBOL = *str;
        if (NEXT_SYMBOL == ',') { str++; break; }
        switch (state) {
            case CORE_START:
                if      (ee_isdigit(NEXT_SYMBOL))                       state = CORE_INT;
                else if (NEXT_SYMBOL == '+' || NEXT_SYMBOL == '-')       state = CORE_S1;
                else if (NEXT_SYMBOL == '.')                              state = CORE_FLOAT;
                else { state = CORE_INVALID; transition_count[CORE_INVALID]++; }
                transition_count[CORE_START]++;
                break;
            case CORE_S1:
                if      (ee_isdigit(NEXT_SYMBOL)) { state = CORE_INT;     transition_count[CORE_S1]++; }
                else if (NEXT_SYMBOL == '.')      { state = CORE_FLOAT;   transition_count[CORE_S1]++; }
                else                              { state = CORE_INVALID; transition_count[CORE_S1]++; }
                break;
            case CORE_INT:
                if      (NEXT_SYMBOL == '.')         { state = CORE_FLOAT;   transition_count[CORE_INT]++; }
                else if (!ee_isdigit(NEXT_SYMBOL))   { state = CORE_INVALID; transition_count[CORE_INT]++; }
                break;
            case CORE_FLOAT:
                if      (NEXT_SYMBOL == 'E' || NEXT_SYMBOL == 'e') { state = CORE_S2;      transition_count[CORE_FLOAT]++; }
                else if (!ee_isdigit(NEXT_SYMBOL))                  { state = CORE_INVALID; transition_count[CORE_FLOAT]++; }
                break;
            case CORE_S2:
                if (NEXT_SYMBOL == '+' || NEXT_SYMBOL == '-') { state = CORE_EXPONENT; transition_count[CORE_S2]++; }
                else                                          { state = CORE_INVALID;  transition_count[CORE_S2]++; }
                break;
            case CORE_EXPONENT:
                if (ee_isdigit(NEXT_SYMBOL)) { state = CORE_SCIENTIFIC; transition_count[CORE_EXPONENT]++; }
                else                          { state = CORE_INVALID;    transition_count[CORE_EXPONENT]++; }
                break;
            case CORE_SCIENTIFIC:
                if (!ee_isdigit(NEXT_SYMBOL)) { state = CORE_INVALID; transition_count[CORE_INVALID]++; }
                break;
            default: break;
        }
    }
    *instr = str;
    return state;
}

/* ============================================================
 * core_matrix.c — matrix benchmark
 * ============================================================ */

ee_s16 matrix_test(ee_u32 N, MATRES *C, MATDAT *A, MATDAT *B, MATDAT val);
ee_s16 matrix_sum(ee_u32 N, MATRES *C, MATDAT clipval);
void   matrix_mul_const(ee_u32 N, MATRES *C, MATDAT *A, MATDAT val);
void   matrix_mul_vect(ee_u32 N, MATRES *C, MATDAT *A, MATDAT *B);
void   matrix_mul_matrix(ee_u32 N, MATRES *C, MATDAT *A, MATDAT *B);
void   matrix_mul_matrix_bitextract(ee_u32 N, MATRES *C, MATDAT *A, MATDAT *B);
void   matrix_add_const(ee_u32 N, MATDAT *A, MATDAT val);

#define matrix_test_next(x)      (x + 1)
#define matrix_clip(x, y)        ((y) ? (x) & 0x0ff : (x) & 0x0ffff)
#define matrix_big(x)            (0xf000 | (x))
#define bit_extract(x, from, to) (((x) >> (from)) & (~(0xffffffff << (to))))

ee_u16 core_bench_matrix(mat_params *p, ee_s16 seed, ee_u16 crc) {
    ee_u32  N   = p->N;
    MATRES *C   = p->C;
    MATDAT *A   = p->A;
    MATDAT *B   = p->B;
    MATDAT  val = (MATDAT)seed;
    crc = crc16(matrix_test(N, C, A, B, val), crc);
    return crc;
}

ee_s16 matrix_test(ee_u32 N, MATRES *C, MATDAT *A, MATDAT *B, MATDAT val) {
    ee_u16 crc     = 0;
    MATDAT clipval = matrix_big(val);
    matrix_add_const(N, A, val);
    matrix_mul_const(N, C, A, val);
    crc = crc16(matrix_sum(N, C, clipval), crc);
    matrix_mul_vect(N, C, A, B);
    crc = crc16(matrix_sum(N, C, clipval), crc);
    matrix_mul_matrix(N, C, A, B);
    crc = crc16(matrix_sum(N, C, clipval), crc);
    matrix_mul_matrix_bitextract(N, C, A, B);
    crc = crc16(matrix_sum(N, C, clipval), crc);
    matrix_add_const(N, A, -val);
    return crc;
}

ee_u32 core_init_matrix(ee_u32 blksize, void *memblk, ee_s32 seed, mat_params *p) {
    ee_u32  N = 0;
    MATDAT *A;
    MATDAT *B;
    ee_s32  order = 1;
    MATDAT  val;
    ee_u32  i = 0, j = 0;
    if (seed == 0) seed = 1;
    while (j < blksize) { i++; j = i * i * 2 * 4; }
    N = i - 1;
    A = (MATDAT *)align_mem(memblk);
    B = A + N * N;
    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            seed         = ((order * seed) % 65536);
            val          = (seed + order);
            val          = matrix_clip(val, 0);
            B[i * N + j] = val;
            val          = (val + order);
            val          = matrix_clip(val, 1);
            A[i * N + j] = val;
            order++;
        }
    }
    p->A = A;
    p->B = B;
    p->C = (MATRES *)align_mem(B + N * N);
    p->N = N;
    return N;
}

ee_s16 matrix_sum(ee_u32 N, MATRES *C, MATDAT clipval) {
    MATRES tmp = 0, prev = 0, cur = 0;
    ee_s16 ret = 0;
    ee_u32 i, j;
    for (i = 0; i < N; i++) {
        for (j = 0; j < N; j++) {
            cur = C[i * N + j];
            tmp += cur;
            if (tmp > clipval) { ret += 10; tmp = 0; }
            else               { ret += (cur > prev) ? 1 : 0; }
            prev = cur;
        }
    }
    return ret;
}

void matrix_mul_const(ee_u32 N, MATRES *C, MATDAT *A, MATDAT val) {
    ee_u32 i, j;
    for (i = 0; i < N; i++)
        for (j = 0; j < N; j++)
            C[i * N + j] = (MATRES)A[i * N + j] * (MATRES)val;
}

void matrix_add_const(ee_u32 N, MATDAT *A, MATDAT val) {
    ee_u32 i, j;
    for (i = 0; i < N; i++)
        for (j = 0; j < N; j++)
            A[i * N + j] += val;
}

void matrix_mul_vect(ee_u32 N, MATRES *C, MATDAT *A, MATDAT *B) {
    ee_u32 i, j;
    for (i = 0; i < N; i++) {
        C[i] = 0;
        for (j = 0; j < N; j++)
            C[i] += (MATRES)A[i * N + j] * (MATRES)B[j];
    }
}

void matrix_mul_matrix(ee_u32 N, MATRES *C, MATDAT *A, MATDAT *B) {
    ee_u32 i, j, k;
    for (i = 0; i < N; i++)
        for (j = 0; j < N; j++) {
            C[i * N + j] = 0;
            for (k = 0; k < N; k++)
                C[i * N + j] += (MATRES)A[i * N + k] * (MATRES)B[k * N + j];
        }
}

void matrix_mul_matrix_bitextract(ee_u32 N, MATRES *C, MATDAT *A, MATDAT *B) {
    ee_u32 i, j, k;
    for (i = 0; i < N; i++)
        for (j = 0; j < N; j++) {
            C[i * N + j] = 0;
            for (k = 0; k < N; k++) {
                MATRES tmp = (MATRES)A[i * N + k] * (MATRES)B[k * N + j];
                C[i * N + j] += bit_extract(tmp, 2, 4) * bit_extract(tmp, 5, 7);
            }
        }
}

/* ============================================================
 * core_list_join.c — linked-list benchmark
 * ============================================================ */

list_head *core_list_find(list_head *list, list_data *info);
list_head *core_list_reverse(list_head *list);
list_head *core_list_remove(list_head *item);
list_head *core_list_undo_remove(list_head *item_removed, list_head *item_modified);
list_head *core_list_insert_new(list_head *insert_point, list_data *info,
                                list_head **memblock, list_data **datablock,
                                list_head *memblock_end, list_data *datablock_end);
typedef ee_s32 (*list_cmp)(list_data *a, list_data *b, core_results *res);
list_head *core_list_mergesort(list_head *list, list_cmp cmp, core_results *res);

ee_s16 calc_func(ee_s16 *pdata, core_results *res) {
    ee_s16 data = *pdata;
    ee_s16 retval;
    ee_u8  optype = (data >> 7) & 1;
    if (optype) return (data & 0x007f);
    {
        ee_s16 flag  = data & 0x7;
        ee_s16 dtype = ((data >> 3) & 0xf);
        dtype |= dtype << 4;
        switch (flag) {
            case 0:
                if (dtype < 0x22) dtype = 0x22;
                retval = core_bench_state(res->size, res->memblock[3],
                                          res->seed1, res->seed2, dtype, res->crc);
                if (res->crcstate == 0) res->crcstate = retval;
                break;
            case 1:
                retval = core_bench_matrix(&(res->mat), dtype, res->crc);
                if (res->crcmatrix == 0) res->crcmatrix = retval;
                break;
            default:
                retval = data;
                break;
        }
        res->crc = crcu16(retval, res->crc);
        retval &= 0x007f;
        *pdata = (data & 0xff00) | 0x0080 | retval;
        return retval;
    }
}

ee_s32 cmp_complex(list_data *a, list_data *b, core_results *res) {
    ee_s16 val1 = calc_func(&(a->data16), res);
    ee_s16 val2 = calc_func(&(b->data16), res);
    return val1 - val2;
}

ee_s32 cmp_idx(list_data *a, list_data *b, core_results *res) {
    if (res == NULL) {
        a->data16 = (a->data16 & 0xff00) | (0x00ff & (a->data16 >> 8));
        b->data16 = (b->data16 & 0xff00) | (0x00ff & (b->data16 >> 8));
    }
    return a->idx - b->idx;
}

void copy_info(list_data *to, list_data *from) {
    to->data16 = from->data16;
    to->idx    = from->idx;
}

ee_u16 core_bench_list(core_results *res, ee_s16 finder_idx) {
    ee_u16     retval = 0;
    ee_u16     found = 0, missed = 0;
    list_head *list     = res->list;
    ee_s16     find_num = res->seed3;
    list_head *this_find;
    list_head *finder, *remover;
    list_data  info = {0};
    ee_s16     i;

    info.idx = finder_idx;
    for (i = 0; i < find_num; i++) {
        info.data16 = (i & 0xff);
        this_find   = core_list_find(list, &info);
        list        = core_list_reverse(list);
        if (this_find == NULL) {
            missed++;
            retval += (list->next->info->data16 >> 8) & 1;
        } else {
            found++;
            if (this_find->info->data16 & 0x1)
                retval += (this_find->info->data16 >> 9) & 1;
            if (this_find->next != NULL) {
                finder          = this_find->next;
                this_find->next = finder->next;
                finder->next    = list->next;
                list->next      = finder;
            }
        }
        if (info.idx >= 0) info.idx++;
    }
    retval += found * 4 - missed;
    if (finder_idx > 0)
        list = core_list_mergesort(list, cmp_complex, res);
    remover = core_list_remove(list->next);
    finder  = core_list_find(list, &info);
    if (!finder) finder = list->next;
    while (finder) {
        retval = crc16(list->info->data16, retval);
        finder = finder->next;
    }
    remover = core_list_undo_remove(remover, list->next);
    list    = core_list_mergesort(list, cmp_idx, NULL);
    finder  = list->next;
    while (finder) {
        retval = crc16(list->info->data16, retval);
        finder = finder->next;
    }
    return retval;
}

list_head *core_list_init(ee_u32 blksize, list_head *memblock, ee_s16 seed) {
    ee_u32 per_item = 16 + sizeof(struct list_data_s);
    ee_u32 size     = (blksize / per_item) - 2;
    list_head *memblock_end  = memblock + size;
    list_data *datablock     = (list_data *)(memblock_end);
    list_data *datablock_end = datablock + size;
    ee_u32 i;
    list_head *finder, *list = memblock;
    list_data  info;

    list->next         = NULL;
    list->info         = datablock;
    list->info->idx    = 0x0000;
    list->info->data16 = (ee_s16)0x8080;
    memblock++;
    datablock++;
    info.idx    = 0x7fff;
    info.data16 = (ee_s16)0xffff;
    core_list_insert_new(list, &info, &memblock, &datablock, memblock_end, datablock_end);

    for (i = 0; i < size; i++) {
        ee_u16 datpat = ((ee_u16)(seed ^ i) & 0xf);
        ee_u16 dat    = (datpat << 3) | (i & 0x7);
        info.data16 = (dat << 8) | dat;
        core_list_insert_new(list, &info, &memblock, &datablock, memblock_end, datablock_end);
    }
    finder = list->next;
    i      = 1;
    while (finder->next != NULL) {
        if (i < size / 5)
            finder->info->idx = i++;
        else {
            ee_u16 pat = (ee_u16)(i++ ^ seed);
            finder->info->idx = 0x3fff & (((i & 0x07) << 8) | pat);
        }
        finder = finder->next;
    }
    list = core_list_mergesort(list, cmp_idx, NULL);
    return list;
}

list_head *core_list_insert_new(list_head *insert_point, list_data *info,
                                list_head **memblock, list_data **datablock,
                                list_head *memblock_end, list_data *datablock_end) {
    list_head *newitem;
    if ((*memblock + 1) >= memblock_end) return NULL;
    if ((*datablock + 1) >= datablock_end) return NULL;
    newitem = *memblock;
    (*memblock)++;
    newitem->next       = insert_point->next;
    insert_point->next  = newitem;
    newitem->info       = *datablock;
    (*datablock)++;
    copy_info(newitem->info, info);
    return newitem;
}

list_head *core_list_remove(list_head *item) {
    list_data *tmp;
    list_head *ret = item->next;
    tmp        = item->info;
    item->info = ret->info;
    ret->info  = tmp;
    item->next = item->next->next;
    ret->next  = NULL;
    return ret;
}

list_head *core_list_undo_remove(list_head *item_removed, list_head *item_modified) {
    list_data *tmp;
    tmp                 = item_removed->info;
    item_removed->info  = item_modified->info;
    item_modified->info = tmp;
    item_removed->next  = item_modified->next;
    item_modified->next = item_removed;
    return item_removed;
}

list_head *core_list_find(list_head *list, list_data *info) {
    if (info->idx >= 0) {
        while (list && (list->info->idx != info->idx))
            list = list->next;
        return list;
    } else {
        while (list && ((list->info->data16 & 0xff) != info->data16))
            list = list->next;
        return list;
    }
}

list_head *core_list_reverse(list_head *list) {
    list_head *next = NULL, *tmp;
    while (list) {
        tmp        = list->next;
        list->next = next;
        next       = list;
        list       = tmp;
    }
    return next;
}

list_head *core_list_mergesort(list_head *list, list_cmp cmp, core_results *res) {
    list_head *p, *q, *e, *tail;
    ee_s32     insize, nmerges, psize, qsize, i;
    insize = 1;
    while (1) {
        p    = list;
        list = NULL;
        tail = NULL;
        nmerges = 0;
        while (p) {
            nmerges++;
            q = p; psize = 0;
            for (i = 0; i < insize; i++) {
                psize++;
                q = q->next;
                if (!q) break;
            }
            qsize = insize;
            while (psize > 0 || (qsize > 0 && q)) {
                if      (psize == 0)               { e = q; q = q->next; qsize--; }
                else if (qsize == 0 || !q)         { e = p; p = p->next; psize--; }
                else if (cmp(p->info, q->info, res) <= 0)
                                                   { e = p; p = p->next; psize--; }
                else                               { e = q; q = q->next; qsize--; }
                if (tail) tail->next = e;
                else      list       = e;
                tail = e;
            }
            p = q;
        }
        tail->next = NULL;
        if (nmerges <= 1) return list;
        insize *= 2;
    }
}

/* ============================================================
 * core_main.c — iterate + main
 * ============================================================ */

static ee_u16 list_known_crc[]   = { (ee_u16)0xd4b0, (ee_u16)0x3340, (ee_u16)0x6a79, (ee_u16)0xe714, (ee_u16)0xe3c1 };
static ee_u16 matrix_known_crc[] = { (ee_u16)0xbe52, (ee_u16)0x1199, (ee_u16)0x5608, (ee_u16)0x1fd7, (ee_u16)0x0747 };
static ee_u16 state_known_crc[]  = { (ee_u16)0x5e47, (ee_u16)0x39bf, (ee_u16)0xe5a4, (ee_u16)0x8e3a, (ee_u16)0x8d84 };

void *iterate(void *pres) {
    ee_u32        i;
    ee_u16        crc;
    core_results *res        = (core_results *)pres;
    ee_u32        iterations = res->iterations;
    res->crc                 = 0;
    res->crclist             = 0;
    res->crcmatrix           = 0;
    res->crcstate            = 0;
    for (i = 0; i < iterations; i++) {
        crc      = core_bench_list(res, 1);
        res->crc = crcu16(crc, res->crc);
        crc      = core_bench_list(res, -1);
        res->crc = crcu16(crc, res->crc);
        if (i == 0) res->crclist = res->crc;
    }
    return NULL;
}

#define get_seed(x) (ee_s16) get_seed_32(x)

#if (MEM_METHOD == MEM_STATIC)
ee_u8 static_memblk[TOTAL_DATA_SIZE];
#endif
char *mem_name[3] = { "Static", "Heap", "Stack" };

#if MAIN_HAS_NOARGC
MAIN_RETURN_TYPE main(void) {
    int   argc = 0;
    char *argv[1];
#else
MAIN_RETURN_TYPE main(int argc, char *argv[]) {
#endif
    ee_printf("Started\n");
    ee_u16       i, j = 0, num_algorithms = 0;
    ee_s16       known_id = -1, total_errors = 0;
    ee_u16       seedcrc = 0;
    CORE_TICKS   total_time;
    core_results results[MULTITHREAD];
#if (MEM_METHOD == MEM_STACK)
    ee_u8 stack_memblock[TOTAL_DATA_SIZE * MULTITHREAD];
#endif
    portable_init(&(results[0].port), &argc, argv);
    if (sizeof(struct list_head_s) > 128) {
        ee_printf("list_head structure too big for comparable data!\n");
        return MAIN_RETURN_VAL;
    }
    results[0].seed1      = get_seed(1);
    results[0].seed2      = get_seed(2);
    results[0].seed3      = get_seed(3);
    results[0].iterations = get_seed_32(4);
    results[0].execs      = get_seed_32(5);
    if (results[0].execs == 0)
        results[0].execs = ALL_ALGORITHMS_MASK;
    if ((results[0].seed1 == 0) && (results[0].seed2 == 0) && (results[0].seed3 == 0)) {
        results[0].seed1 = 0;
        results[0].seed2 = 0;
        results[0].seed3 = 0x66;
    }
    if ((results[0].seed1 == 1) && (results[0].seed2 == 0) && (results[0].seed3 == 0)) {
        results[0].seed1 = 0x3415;
        results[0].seed2 = 0x3415;
        results[0].seed3 = 0x66;
    }

#if (MEM_METHOD == MEM_STATIC)
    results[0].memblock[0] = (void *)static_memblk;
    results[0].size        = TOTAL_DATA_SIZE;
    results[0].err         = 0;
#elif (MEM_METHOD == MEM_STACK)
    for (i = 0; i < MULTITHREAD; i++) {
        results[i].memblock[0] = stack_memblock + i * TOTAL_DATA_SIZE;
        results[i].size        = TOTAL_DATA_SIZE;
        results[i].seed1       = results[0].seed1;
        results[i].seed2       = results[0].seed2;
        results[i].seed3       = results[0].seed3;
        results[i].err         = 0;
        results[i].execs       = results[0].execs;
    }
#endif

    /* Find out how many algorithms we have */
    for (i = 0; i < NUM_ALGORITHMS; i++)
        if ((1 << (ee_u32)i) & results[0].execs)
            num_algorithms++;
    for (i = 0; i < MULTITHREAD; i++)
        results[i].size = results[i].size / num_algorithms;

    /* Assign per-algorithm pointers into the shared block */
    for (i = 0; i < NUM_ALGORITHMS; i++) {
        ee_u32 ctx;
        if ((1 << (ee_u32)i) & results[0].execs) {
            for (ctx = 0; ctx < MULTITHREAD; ctx++)
                results[ctx].memblock[i + 1]
                    = (char *)(results[ctx].memblock[0]) + results[0].size * j;
            j++;
        }
    }

    /* Init blocks */
    for (i = 0; i < MULTITHREAD; i++) {
        if (results[i].execs & ID_LIST)
            results[i].list = core_list_init(results[0].size, results[i].memblock[1], results[i].seed1);
        if (results[i].execs & ID_MATRIX)
            core_init_matrix(results[0].size, results[i].memblock[2],
                             (ee_s32)results[i].seed1 | (((ee_s32)results[i].seed2) << 16),
                             &(results[i].mat));
        if (results[i].execs & ID_STATE)
            core_init_state(results[0].size, results[i].seed1, results[i].memblock[3]);
    }

    /* Auto-pick iterations if requested */
    if (results[0].iterations == 0) {
        secs_ret secs_passed = 0;
        ee_u32   divisor;
        results[0].iterations = 1;
        while (secs_passed < (secs_ret)1) {
            results[0].iterations *= 10;
            start_time();
            iterate(&results[0]);
            stop_time();
            secs_passed = time_in_secs(get_time());
        }
        divisor = (ee_u32)secs_passed;
        if (divisor == 0) divisor = 1;
        results[0].iterations *= 1 + 10 / divisor;
    }

    /* Run benchmark */
    start_time();
    iterate(&results[0]);
    stop_time();
    total_time = get_time();

    /* Seed CRC for known-config detection */
    seedcrc = crc16(results[0].seed1, seedcrc);
    seedcrc = crc16(results[0].seed2, seedcrc);
    seedcrc = crc16(results[0].seed3, seedcrc);
    seedcrc = crc16(results[0].size,  seedcrc);
    switch (seedcrc) {
        case 0x8a02: known_id = 0; ee_printf("6k performance run parameters for coremark.\n"); break;
        case 0x7b05: known_id = 1; ee_printf("6k validation run parameters for coremark.\n"); break;
        case 0x4eaf: known_id = 2; ee_printf("Profile generation run parameters for coremark.\n"); break;
        case 0xe9f5: known_id = 3; ee_printf("2K performance run parameters for coremark.\n"); break;
        case 0x18f2: known_id = 4; ee_printf("2K validation run parameters for coremark.\n"); break;
        default:     total_errors = -1; break;
    }
    if (known_id >= 0) {
        for (i = 0; i < default_num_contexts; i++) {
            results[i].err = 0;
            if ((results[i].execs & ID_LIST) && (results[i].crclist != list_known_crc[known_id])) {
                ee_printf("[%u]ERROR! list crc 0x%04x - should be 0x%04x\n",
                          i, results[i].crclist, list_known_crc[known_id]);
                results[i].err++;
            }
            if ((results[i].execs & ID_MATRIX) && (results[i].crcmatrix != matrix_known_crc[known_id])) {
                ee_printf("[%u]ERROR! matrix crc 0x%04x - should be 0x%04x\n",
                          i, results[i].crcmatrix, matrix_known_crc[known_id]);
                results[i].err++;
            }
            if ((results[i].execs & ID_STATE) && (results[i].crcstate != state_known_crc[known_id])) {
                ee_printf("[%u]ERROR! state crc 0x%04x - should be 0x%04x\n",
                          i, results[i].crcstate, state_known_crc[known_id]);
                results[i].err++;
            }
            total_errors += results[i].err;
        }
    }
    total_errors += check_data_types();

    ee_printf("CoreMark Size    : %lu\n", (long unsigned)results[0].size);
    ee_printf("Total ticks      : %lu\n", (long unsigned)total_time);
#if HAS_FLOAT
    ee_printf("Total time (secs): %f\n", time_in_secs(total_time));
    if (time_in_secs(total_time) > 0)
        ee_printf("Iterations/Sec   : %f\n",
                  default_num_contexts * results[0].iterations
                      / time_in_secs(total_time));
#else
    ee_printf("Total time (secs): %d\n", time_in_secs(total_time));
    if (time_in_secs(total_time) > 0)
        ee_printf("Iterations/Sec   : %d\n",
                  default_num_contexts * results[0].iterations
                      / time_in_secs(total_time));
#endif
    if (time_in_secs(total_time) < 10) {
        ee_printf("ERROR! Must execute for at least 10 secs for a valid result!\n");
        total_errors++;
    }

    ee_printf("Iterations       : %lu\n",
              (long unsigned)default_num_contexts * results[0].iterations);
    ee_printf("Compiler version : %s\n", COMPILER_VERSION);
    ee_printf("Compiler flags   : %s\n", COMPILER_FLAGS);
    ee_printf("Memory location  : %s\n", MEM_LOCATION);
    ee_printf("seedcrc          : 0x%04x\n", seedcrc);
    if (results[0].execs & ID_LIST)
        for (i = 0; i < default_num_contexts; i++)
            ee_printf("[%d]crclist       : 0x%04x\n", i, results[i].crclist);
    if (results[0].execs & ID_MATRIX)
        for (i = 0; i < default_num_contexts; i++)
            ee_printf("[%d]crcmatrix     : 0x%04x\n", i, results[i].crcmatrix);
    if (results[0].execs & ID_STATE)
        for (i = 0; i < default_num_contexts; i++)
            ee_printf("[%d]crcstate      : 0x%04x\n", i, results[i].crcstate);
    for (i = 0; i < default_num_contexts; i++)
        ee_printf("[%d]crcfinal      : 0x%04x\n", i, results[i].crc);

    if (total_errors == 0)
        ee_printf("Correct operation validated. See README.md for run and reporting rules.\n");
    if (total_errors > 0)
        ee_printf("Errors detected\n");
    if (total_errors < 0)
        ee_printf("Cannot validate operation for these seed values, please compare with results on a known platform.\n");

    portable_fini(&(results[0].port));
    return MAIN_RETURN_VAL;
}
