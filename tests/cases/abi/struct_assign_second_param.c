// EXPECT_R0: 16
// Assigning the SECOND struct parameter to a local exercises IK_MEMCPY with
// its source pointer in r2. Regression (ray_tracer bug 3): the inline memcpy
// loop hardcoded r2 as the data scratch, clobbering the source pointer after
// the first halfword and then faulting on a garbage dereference.
typedef struct { float x, y, z; } Vec;

static float trace(Vec o0, Vec d0) {
    Vec o;
    Vec d;
    o = o0;
    d = d0;
    return o.x + d.x + d.y + d.z;
}

int main() {
    Vec a; Vec b;
    a.x = 1.0f; a.y = 2.0f; a.z = 3.0f;
    b.x = 4.0f; b.y = 5.0f; b.z = 6.0f;
    return (int)trace(a, b);   /* 1+4+5+6 = 16 */
}
