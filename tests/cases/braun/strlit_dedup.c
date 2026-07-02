// EXPECT_R0: 0
// String-literal dedup: the same literal used in a global initializer and
// in two different function bodies shares one _lN label and one copy of
// the data bytes. All references must still read the correct contents.
#include <string.h>

char *g = "dedup me";

char *f1(void) { return "dedup me"; }
char *f2(void) { char *other = "not this one"; (void)other; return "dedup me"; }

int main(void) {
    char *a = f1();
    char *b = f2();
    if (strcmp(a, b)) return 1;
    if (strcmp(a, g)) return 2;
    if (strcmp(a, "dedup me")) return 3;
    if (!strcmp(a, "not this one")) return 4;
    return 0;
}
