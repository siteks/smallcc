// EXPECT_R0: 14
/* Struct copy stress test - forces accumulator clobber between push and reload */
struct P { int a; int b; int c; int d; };  /* Larger struct = more copies = more chance to fail */

int use(int x) { return x + 1; }  /* Function call to clobber accum */

int main() {
    struct P s1;
    s1.a = 1;
    s1.b = 2;
    s1.c = 3;
    s1.d = 4;

    struct P s2;
    s2 = s1;  /* Copy with many field ops in between */

    /* Verify all fields */
    int sum = 0;
    sum = use(sum) + s2.a;  /* 0+1+1 = 2 */
    sum = use(sum) + s2.b;  /* 2+1+2 = 5 */
    sum = use(sum) + s2.c;  /* 5+1+3 = 9 */
    sum = use(sum) + s2.d;  /* 9+1+4 = 14 */
    return sum;  /* Should be 14 */
}
