#ifndef STDINT_H
#define STDINT_H

/* Exact-width integer types */
typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef long               int32_t;
typedef unsigned long      uint32_t;

/* Minimum-width integer types */
typedef int8_t             int_least8_t;
typedef uint8_t            uint_least8_t;
typedef int16_t            int_least16_t;
typedef uint16_t           uint_least16_t;
typedef int32_t            int_least32_t;
typedef uint32_t           uint_least32_t;

/* Fastest minimum-width integer types */
typedef int8_t             int_fast8_t;
typedef uint8_t            uint_fast8_t;
typedef int16_t            int_fast16_t;
typedef uint16_t           uint_fast16_t;
typedef int32_t            int_fast32_t;
typedef uint32_t           uint_fast32_t;

/* Integer types wide enough to hold a pointer */
typedef unsigned int       uintptr_t;
typedef int                intptr_t;

/* Greatest-width integer types */
typedef long               intmax_t;
typedef unsigned long      uintmax_t;

/* Limits of exact-width types */
#define INT8_MIN           (-128)
#define INT8_MAX           127
#define UINT8_MAX          255

#define INT16_MIN          (-32768)
#define INT16_MAX          32767
#define UINT16_MAX         65535

#define INT32_MIN          (-2147483648L)
#define INT32_MAX          2147483647L
#define UINT32_MAX         4294967295UL

/* Limits of other types */
#define INTPTR_MIN         INT16_MIN
#define INTPTR_MAX         INT16_MAX
#define UINTPTR_MAX        UINT16_MAX

#define INTMAX_MIN         INT32_MIN
#define INTMAX_MAX         INT32_MAX
#define UINTMAX_MAX        UINT32_MAX

/* Macros for integer constants */
#define INT8_C(v)          (v)
#define UINT8_C(v)         (v)
#define INT16_C(v)         (v)
#define UINT16_C(v)        (v)
#define INT32_C(v)         (v ## L)
#define UINT32_C(v)        (v ## UL)
#define INTMAX_C(v)        (v ## L)
#define UINTMAX_C(v)       (v ## UL)

#endif
