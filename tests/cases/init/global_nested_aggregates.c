// EXPECT_R0: 0
// Static initialisers laid out by type: nested arrays, brace elision,
// structs in arrays, char arrays inside structs, padded strings, unions,
// constant expressions of every kind (docs/issues/0004).
struct P { char tag; int v; short s; };
struct Q { char name[6]; struct P p[2]; };
union U { int i; char c[4]; };

int m[2][2][2] = { { {1, 2}, {3, 4} }, { {5, 6}, {7, 8} } };
int flat[2][3] = { 1, 2, 3, 4, 5 };                 /* brace elision, last zero */
struct P ps[2] = { { 'a', 10, -3 }, { 'b', 20 } };
struct Q q = { "hi", { { 'x', 1 << 10, 7 }, 'y', ~0, 2 } };
char pad[8] = "abc";                                 /* 4 zero bytes follow */
int after = 0x5a5a;                                  /* must not overlap pad */
union U u = { 0x01020304 };
float f = 1.5f * 2 - 0.5f;
int neg = -(3 * 4) % 5;
unsigned big = 0xffffffffu >> 4;
int shifted = -16 >> 2;                              /* arithmetic: -4 */
enum { A = 3, B = A * 4 };
char ch = B + 1;
int *pm = &m[0][0][0];
char *str = "xyz";
int sel = sizeof(int) == 4 ? 7 : 9;

int main(void) {
    int i, j, k, n = 1;
    for (i = 0; i < 2; i++) for (j = 0; j < 2; j++) for (k = 0; k < 2; k++)
        if (m[i][j][k] != n++) return 1;
    if (flat[0][2] != 3 || flat[1][1] != 5 || flat[1][2] != 0) return 2;
    if (ps[0].tag != 'a' || ps[0].v != 10 || ps[0].s != -3) return 3;
    if (ps[1].tag != 'b' || ps[1].v != 20 || ps[1].s != 0) return 4;
    if (q.name[0] != 'h' || q.name[2] != 0 || q.name[5] != 0) return 5;
    if (q.p[0].v != 1024 || q.p[0].s != 7 || q.p[1].tag != 'y' || q.p[1].v != -1 || q.p[1].s != 2) return 6;
    if (pad[2] != 'c' || pad[3] != 0 || pad[7] != 0 || after != 0x5a5a) return 7;
    if (u.c[0] != 4 || u.c[3] != 1) return 8;
    if (f != 2.5f) return 9;
    if (neg != -2 || big != 0x0fffffff || shifted != -4) return 10;
    if (ch != 13) return 11;
    if (*pm != 1 || str[1] != 'y' || sel != 7) return 12;
    return 0;
}
