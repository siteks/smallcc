// EXPECT_R0: 0
// A struct-returning call result passed directly as an argument to another
// call must survive intact. Regression (ray_tracer bug 2): the result lives
// in the callee's dead frame; without an immediate caller-side copy the next
// call's `enter` reused that stack area and clobbered the bytes (z member
// lost, or whole struct zeroed).
typedef struct { float x, y, z; } Vec;

static Vec mk0(void) { Vec v; v.x = 7.0f; v.y = 8.0f; v.z = 9.0f; return v; }
static Vec vec(float x, float y, float z) { Vec v; v.x=x; v.y=y; v.z=z; return v; }
static float sumv(Vec a) { return a.x + a.y + a.z; }
static Vec vscale(Vec a, float s) { return vec(a.x*s, a.y*s, a.z*s); }
static Vec vscale_r(float s, Vec a) { return vec(a.x*s, a.y*s, a.z*s); }
static float vdot(Vec a, Vec b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static float rhalf(float x) { return x * 0.5f; }

int main() {
    Vec r; Vec a;
    if (sumv(vec(1.0f, 2.0f, 3.0f)) != 6.0f) return 1;   /* A */
    if (sumv(mk0()) != 24.0f) return 2;                  /* B */
    r = vscale(mk0(), 2.0f);                             /* C */
    if (r.x != 14.0f || r.y != 16.0f || r.z != 18.0f) return 3;
    r = vscale(vec(1.0f,2.0f,3.0f), 2.0f);               /* D */
    if (r.x != 2.0f || r.y != 4.0f || r.z != 6.0f) return 4;
    r = vscale_r(2.0f, vec(1.0f,2.0f,3.0f));             /* E */
    if (r.x != 2.0f || r.y != 4.0f || r.z != 6.0f) return 5;
    /* float call result as 2nd arg, struct param as 1st (vnorm shape) */
    a.x = 1.0f; a.y = 2.0f; a.z = 3.0f;
    r = vscale(a, rhalf(vdot(a, a)));                    /* dot=14 half=7 */
    if (r.x != 7.0f || r.y != 14.0f || r.z != 21.0f) return 6;
    /* nested struct-returning calls */
    r = vscale(vscale(a, 2.0f), 0.5f);
    if (r.x != 1.0f || r.y != 2.0f || r.z != 3.0f) return 7;
    if (a.x != 1.0f || a.y != 2.0f || a.z != 3.0f) return 8;
    return 0;
}
