// EXPECT_R0: 9
// XFAIL: issue 0005 (case labels accept only ident, char, -N or N; 1+2 is a parse error)
int main(void) {
    int x = 3;
    switch (x) {
    case 1 + 2: return 9;
    }
    return 0;
}
