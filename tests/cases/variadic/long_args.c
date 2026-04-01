// EXPECT_R0: 42
// Variadic function receiving long (4-byte) arguments.
// All variadic args use 4-byte slots; va_arg(ap, long) advances ap by 4.
// Exercises the full path: caller pushes 4-byte slots, va_start points past
// the named param, va_arg reads and advances correctly.
#include <stdarg.h>
long sum_longs(int n, ...) {
    va_list ap;
    va_start(ap, n);
    long total = 0;
    int i;
    for (i = 0; i < n; i++)
        total += va_arg(ap, long);
    va_end(ap);
    return total;
}
int main() {
    return (int)sum_longs(3, 2L, 10L, 30L);
}
