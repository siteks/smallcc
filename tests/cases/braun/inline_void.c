// EXPECT_R0: 3143
// Void functions inline (no return expression); so do functions with
// four parameters (params bind by value in the inliner, the 3-register
// ABI limit does not apply).  Out-params through pointers must still work
// when the pointee is a local of the caller.
typedef struct { int x, y, z; } V;
static void vset(V *r, int x, int y, int z) { r->x = x; r->y = y; r->z = z; }
static void vsub(V *r, V *a, V *b) { r->x = a->x - b->x; r->y = a->y - b->y; r->z = a->z - b->z; }
static void bump(int *p) { *p = *p + 1; return; }
static int dot(V *a, V *b) { return a->x * b->x + a->y * b->y + a->z * b->z; }
int main(void) {
    V a, b, d;
    int n = 0;
    vset(&a, 10, 20, 30);
    vset(&b, 1, 2, 3);
    vsub(&d, &a, &b);          /* 9, 18, 27 */
    bump(&n); bump(&n);        /* 2 */
    return dot(&d, &d) + n * 1000 + (a.x - b.x);   /* 81+324+729 = 1134 + 2000 + 9 = 3143 */
}
