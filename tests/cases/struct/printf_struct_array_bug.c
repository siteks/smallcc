// EXPECT_R0: 1
// EXPECT_STDOUT: Current size 2000\n

// KNOWN_BUG: EXPECT_STDOUT fails - printf reads garbage due to stack alignment overlap.
/* Bug: printf with struct array element and large stack frame.
 * When an array of structs is followed by a large char array on the stack,
 * printf incorrectly reads the value due to F2 instruction alignment
 * causing overlap between format string pointer (bp-138) and value (bp-136).
 */
#include <stdio.h>

int main()
{
    struct S {int size;} results[1];
    char stack_memblock[130];  /* Large stack allocation triggers the bug */
    results[0].size = 2000;
    printf("Current size %d\n", results[0].size);
    /* Return 1 if value is correct, 0 if bug manifests */
    return (results[0].size == 2000) ? 1 : 0;
}
