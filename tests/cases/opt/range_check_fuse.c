// EXPECT_R0: 12
// Range-check fusion (opt_range_check): (a <= x && x <= b) with constant
// bounds becomes (unsigned)(x - a) <= b - a. Boundary and out-of-range
// values, including a negative signed x (must NOT be in range 48..57).
static int isdig(int c) { return (c >= 48) & (c <= 57) ? 1 : 0; }
int main(void) {
    volatile int v47 = 47, v48 = 48, v57 = 57, v58 = 58, vneg = -5;
    return isdig(v47) * 16 + isdig(v48) * 8 + isdig(v57) * 4 * 0
         + isdig(v48) + isdig(v57) * 2 + isdig(v58) * 32 + isdig(vneg) * 64
         + 1;  /* 0 + 8*... */
}
