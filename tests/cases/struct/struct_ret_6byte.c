// EXPECT_R0: 6
// Three-int struct has size=6 (max_align=2), which is not a multiple of 4.
// Before the fix, the retbuf was allocated with adjw -6, misaligning sp on CPU4.
typedef struct { int a; int b; int c; } Triple;
Triple make3(int a, int b, int c) {
    Triple t;
    t.a = a; t.b = b; t.c = c;
    return t;
}
int main() {
    Triple t = make3(1, 2, 3);
    return t.a + t.b + t.c;
}
