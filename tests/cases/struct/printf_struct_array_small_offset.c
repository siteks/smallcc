// EXPECT_R0: 1
// EXPECT_STDOUT: Current size 2000\n

#include <stdio.h>

int main()
{
    struct S {int size;} results[1];
    char stack_memblock[10];  /* Large stack allocation triggers the bug */
    results[0].size = 2000;
    printf("Current size %d\n", results[0].size);
    /* Return 1 if value is correct, 0 if bug manifests */
    return (results[0].size == 2000) ? 1 : 0;
}
