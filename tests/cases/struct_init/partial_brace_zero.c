// EXPECT_R0: 0
// A local brace initialiser zeroes every byte it does not store: members
// without an initialiser, padding, and zero elements of an inner array
// (docs/issues/0011).
struct In { short h; char t[3]; };
struct S { char c; int i; struct In in; char tail; };
int check(void) {
    struct S s = { 'x', 5, { 7, { 1, 0 } } };            /* t[2] and tail not given */
    unsigned char *b = (unsigned char *)&s;
    int k, pad = 0;
    for (k = 1; k < 4; k++) pad |= b[k];                /* padding after c */
    return s.c == 'x' && s.i == 5 && s.in.h == 7 && s.in.t[0] == 1 && s.in.t[1] == 0 &&
           s.in.t[2] == 0 && s.tail == 0 && pad == 0;
}
void dirty(void) {                                       /* leave garbage in the frame */
    volatile int junk[8];
    int k;
    for (k = 0; k < 8; k++) junk[k] = -1;
}
int main(void) {
    dirty();
    return check() ? 0 : 1;
}
