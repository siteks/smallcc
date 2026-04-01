// EXPECT_R0: 42
// Call through a function-pointer struct member with one int argument (param_size=2,
// not 4-byte aligned).  Before the fix, an unaligned adjw -2 was emitted before the
// call on CPU4, misaligning sp.
typedef struct { int (*fn)(int); } Handler;
int twice(int x) { return x * 2; }
int main() {
    Handler h;
    h.fn = twice;
    return h.fn(21);
}
