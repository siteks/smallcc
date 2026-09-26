// EXPECT_R0: 3
// XFAIL: issue 0006 (pointer difference is typed as the pointer and never divided by the element size)
int a[10];
int main(void) { int *p = &a[3]; int *q = &a[0]; return p - q; }
