// EXPECT_R0: 0
// Frame-slot promotion: struct locals whose address never escapes are
// carried as SSA values through loops and branches; the ones that escape
// (a call argument), union puns, partial accesses and struct copies must
// all still see memory's contents. loop_carried also covers legalize
// Pass H's liveness, which once missed the second definition of a value
// out-of-SSA had split, and deleted the stores an inlined struct return
// was read back from.
typedef struct { int x, y, z; } V;
typedef struct { float f; } F;
union U { float f; int i; };
static void vset(V *r, int x, int y, int z) { r->x = x; r->y = y; r->z = z; }
static V vmk(int x, int y, int z) { V v; v.x = x; v.y = y; v.z = z; return v; }
static V vadd(V a, V b) { return vmk(a.x + b.x, a.y + b.y, a.z + b.z); }
static int sink;
static void touch(V *p) { p->y += 7; sink = p->x; }

int loop_carried(int n) {
    V acc, step;
    int i;
    vset(&acc, 0, 0, 0);
    vset(&step, 1, 2, 3);
    for (i = 0; i < n; i++) {
        if (i & 1) acc = vadd(acc, step);
        else { acc.x = acc.x - 1; acc.z = acc.z + acc.y; }
        step.y = step.y + 1;
    }
    return acc.x * 10000 + acc.y * 100 + acc.z;
}

int escaped(int k) {
    V a, b;
    vset(&a, k, k + 1, k + 2);
    b = a;
    touch(&a);                  // a escapes: its fields live in memory
    b.y = b.y * 2;
    return a.y * 1000 + b.y * 10 + sink;
}

int pun(int n) {
    union U u;
    int s = 0, i;
    u.f = 1.5f;
    for (i = 0; i < n; i++) { s += u.i & 0xff; u.i = u.i + 0x100; }
    return s + ((u.i >> 8) & 0xff);
}

int partial(void) {
    struct { int w; char c[4]; } s;
    s.w = 0x01020304;
    *(int *)s.c = 0;
    s.c[1] = 9;
    return s.w + s.c[1] + s.c[0];
}

int copies(int n) {
    V a = vmk(1, 2, 3), b = vmk(4, 5, 6), t;
    int i;
    for (i = 0; i < n; i++) { t = a; a = b; b = t; a.x += i; }
    return a.x * 100 + b.x * 10 + a.z;
}

int main() {
    if (loop_carried(9) != -7538) return 1;
    if (escaped(5) != 13125) return 2;
    if (pun(3) != 3) return 3;
    if (partial() != 0x01020304 + 9) return 4;
    if (copies(4) != 563) return 5;
    return 0;
}
