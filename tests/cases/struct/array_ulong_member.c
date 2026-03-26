// EXPECT_R0: 1
/* Array of structs with unsigned long member assignment and check */
typedef struct S { int a1; int a2; int s3; void * m[4]; unsigned long b; } cr;



int main() {
    cr arr[1];
    char block[10000];
    arr[0].a1 = 10;
    arr[0].b = 0x12345678;

    /* Check that arr[1].b was set correctly */
    if (arr[0].b == 0x12345678) {
        return 1;  /* Success */
    }
    return 0;  /* Failure */
}
