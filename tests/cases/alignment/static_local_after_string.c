// EXPECT_R0: 7
// EXPECT_STDOUT: bars
// docs/issues/0001: function-static data (`_ls<n>`) was emitted without a
// preceding `align`, so a `static const uint32_t[]` that followed an
// odd-length string literal in the image landed at a 2- or 3-mod-4
// address. The hardware word load ignores the low address bits and
// returned the aligned neighbour; sim_c trapped with an alignment error.
// The string literal here is 5 bytes ("bars" + NUL), pushing everything
// after it off a 4-byte boundary. The 2-byte static after it then leaves
// the following 4-byte static 2-mod-4 in BSS as well.
#include <stdio.h>
int main(void) {
    static const unsigned int col[8] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00,
                                        0xFF00FF, 0xFF0000, 0x0000FF, 0};
    static short sh = 0x1234;
    static int zi;
    int r = 0;
    putchar('b'); putchar('a'); putchar('r'); putchar('s');
    zi = 7;
    if (col[0] == 0xFFFFFF && col[1] == 0xFFFF00 && col[6] == 0x0000FF && col[7] == 0) r |= 1;
    if (sh == 0x1234) r |= 2;
    if (zi == 7) r |= 4;
    return r;
}
