// EXPECT_R0: 93
// Jump table with gaps in the case range: sparse entries filled with default
int classify(int x) {
    switch (x) {
        case 0:  return 1;
        case 1:  return 2;
        case 2:  return 3;
        case 4:  return 5;
        case 5:  return 6;
        case 6:  return 7;
        case 8:  return 9;
        case 9:  return 10;
        case 10: return 11;
        case 12: return 13;
        case 13: return 14;
        case 14: return 15;
        default: return -1;
    }
}

int main(void) {
    int sum = 0;
    int i;
    for (i = 0; i < 15; i++) {
        sum = sum + classify(i);
    }
    return sum;
}
