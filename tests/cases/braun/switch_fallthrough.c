// EXPECT_R0: 14
// Fall-through case: CFG edge from one case body to next (no break)
int main(void) {
    int x = 2;
    int r = 0;
    switch (x) {
        case 1: r = r + 1;
        case 2: r = r + 4;
        case 3: r = r + 10;
            break;
        case 4: r = r + 100;
    }
    return r;
}
