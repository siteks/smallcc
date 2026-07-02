// EXPECT_R0: 30
// Redundant-load CSE (opt_load_cse) semantic guards: the second load of
// g[0] may be reused (no clobber between), but the loads around the store
// must NOT be — reusing across the store would return 10, not 20 (sum 30).
unsigned g[2];
int main(void) {
    unsigned a, b, c;
    g[0] = 10;
    a = g[0];          /* 10 */
    b = g[0];          /* CSE-able: same value, no clobber between */
    g[0] = 20;         /* clobber */
    c = g[0];          /* must reload: 20 */
    return (int)(a + (b - a) + c);   /* 10 + 0 + 20 */
}
