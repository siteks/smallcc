// EXPECT_R0: 15
// va_start with a long as the last named parameter.
// Before the fix, va_start read the block-level param_offset (always 8),
// making ap point to the long parameter itself instead of the variadic args.
// The fix uses last_sym->offset + slot_size to correctly skip over the 4-byte
// named parameter.
#include <stdarg.h>
long add_after(long base, ...) {
    va_list ap;
    va_start(ap, base);
    long x = va_arg(ap, long);
    va_end(ap);
    return base + x;
}
int main() {
    return (int)add_after(5L, 10L);
}
