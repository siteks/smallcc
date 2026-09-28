// EXPECT_R0: 0
// CFLAGS: -Oparam=inline_cf_nodes=200
// Functions with control flow inlined into their callers: early returns
// from loops, struct locals whose address is taken, nested inlining, void
// functions, and the same function inlined several times in one caller.
typedef struct { int x, y, z; } V3;
int none[1];

static void vadd(V3 *r, V3 *a, V3 *b) { r->x = a->x + b->x; r->y = a->y + b->y; r->z = a->z + b->z; }

static int find(int *a, int n, int key) {
    int i;
    for (i = 0; i < n; i++)
        if (a[i] == key) return i;       /* early return from a loop */
    return -1;
}

static int dotsum(V3 *a, V3 *b) {
    V3 s;                                /* struct local, address taken */
    vadd(&s, a, b);
    if (s.x < 0) return 0;
    return s.x + s.y + s.z;
}

static void fill(int *a, int n, int v) {
    while (n-- > 0) *a++ = v;
    if (v) return;                       /* early return in a void function */
    a[-1] = 99;
}

static int classify(int k) {
    switch (k & 3) {
    case 0: return find(none, 0, 0) + 10;
    case 1: return 11;
    default: break;
    }
    return k > 100 ? 13 : 12;
}

int main(void) {
    int a[6] = {5, 7, 9, 7, 3, 1};
    int b[4];
    V3 p = {1, 2, 3}, q = {10, 20, 30};
    if (find(a, 6, 9) != 2 || find(a, 6, 7) != 1 || find(a, 6, 4) != -1) return 1;
    if (dotsum(&p, &q) != 66 || dotsum(&q, &p) != 66) return 2;
    fill(b, 4, 0);
    if (b[0] != 0 || b[3] != 99) return 3;
    fill(b, 3, 4);
    if (b[2] != 4 || b[3] != 99) return 4;
    if (classify(4) != 9 || classify(5) != 11 || classify(6) != 12 || classify(203) != 13) return 5;
    return 0;
}
