// EXPECT_R0: 0
// Array sizes, enum values and case labels are full integer constant
// expressions (docs/issues/0005).
enum { K = (1 << 3) | 1, L = K > 8 ? -2 : 2, M = sizeof(short) * 3 };
int main(void) {
    char buf[(K + 1) / 2 * 3];              /* 15 */
    int x = 9, r = 0;
    switch (x) {
    case K - 2 * 4 + 8: r = 1; break;       /* 9 */
    case 'a' ^ 'b': r = 2; break;
    }
    if (sizeof(buf) != 15 || L != -2 || M != 6 || r != 1) return 1;
    return 0;
}
