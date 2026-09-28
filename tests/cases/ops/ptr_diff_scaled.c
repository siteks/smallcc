// EXPECT_R0: 3
int a[10];
int main(void) { int *p = &a[3]; int *q = &a[0]; return p - q; }
