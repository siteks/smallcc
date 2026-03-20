// EXPECT_R0: 100
// char, short, long locals with a function call in between.
// The call must not clobber the long that lives near the bottom of the frame.
int add(int a, int b) { return a + b; }
int main() {
    char c;
    short s;
    long n;
    c = 10;
    s = 30;
    n = 60L;
    int t = add((int)c, (int)s);   /* call exercises the frame */
    return (int)(t + n);
}
