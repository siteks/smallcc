// EXPECT_R0: 0
// Struct params are caller-owned copies (ABI §4.3): mutating the parameter
// must not modify the caller's variable. Regression: struct args used to
// pass the caller's address with no copy (ray_tracer bug 1) — both through
// a real call and through the braun inliner.
typedef struct { float x, y, z; } Vec;

static float mutate(Vec a) { a.x = 99.0f; return a.x; }

static float mutate_big(Vec a, Vec b) {
    // Non-inlinable (control flow) variant
    if (a.x > 0.0f) { a.x = 50.0f; b.y = 60.0f; }
    return a.x + b.y;
}

int main() {
    Vec v; Vec w;
    v.x = 1.0f; v.y = 2.0f; v.z = 3.0f;
    w.x = 4.0f; w.y = 5.0f; w.z = 6.0f;
    if (mutate(v) != 99.0f) return 1;
    if (v.x != 1.0f) return 2;          /* caller variable must be intact */
    if (mutate_big(v, w) != 110.0f) return 3;
    if (v.x != 1.0f || w.y != 5.0f) return 4;
    return 0;
}
