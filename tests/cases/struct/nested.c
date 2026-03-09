// EXPECT_R0: 42
struct Inner { int x; };
struct Outer { struct Inner a; int b; };
int main() {
    struct Outer o;
    o.a.x = 42;
    o.b = 10;
    return o.a.x;
}
