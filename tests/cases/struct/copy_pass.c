// EXPECT_R0: 30
/* Struct passed to function test */
struct P { int a; int b; };
int sum(struct P p) { return p.a + p.b; }
int main() {
    struct P s;
    s.a = 10;
    s.b = 20;
    return sum(s);  /* Should be 30 */
}
