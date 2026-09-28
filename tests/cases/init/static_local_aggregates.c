// EXPECT_R0: 0
// Static locals use the same layout as globals: struct and string-pointer
// initialisers used to be silently zero (docs/issues/0004).
struct S { char c; int v; };
int f(void) {
    static struct S s = { 'k', 5 * 7 };
    static char *msg = "ok";
    static int arr[3][2] = { {1, 2}, {3} };
    static float g = 0.25f + 0.5f;
    s.v++;
    return s.c == 'k' && msg[1] == 'k' && arr[1][0] == 3 && arr[1][1] == 0 && g == 0.75f ? s.v : -1;
}
int main(void) {
    f();
    return f() == 37 ? 0 : 1;
}
