// EXPECT_R0: 5353
// A call to an inlined function whose argument calls the same function:
// the inner expansion binds the same parameter symbols, so the outer
// call's arguments must all be evaluated before any is bound.
static int f(int a, int b) { return a * 10 + b; }
typedef struct { int x, y; } P;
static P mk(int x, int y) { P p; p.x = x; p.y = y; return p; }
static P padd(P a, P b) { return mk(a.x + b.x, a.y + b.y); }
int main() {
    int r = f(1, f(2, 3));                       // 33
    P q = padd(mk(1, 2), padd(mk(30, 40), mk(500, 600)));
    return r * 100 + (q.x - 500) * 10 + (q.y - 642) + 1743;
}
