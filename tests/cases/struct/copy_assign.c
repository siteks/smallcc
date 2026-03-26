// EXPECT_R0: 141
/* Struct copy assignment test - copies a struct and verifies fields */
struct P { int a; int b; };
int main() {
    struct P s1;
    s1.a = 42;
    s1.b = 99;
    struct P s2;
    s2 = s1;
    return s2.a + s2.b;  /* 42 + 99 = 141 */
}
