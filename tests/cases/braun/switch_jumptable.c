// EXPECT_R0: 650
// Dense 13-case switch: triggers jump table dispatch (threshold >= 12)
int compute(int x) {
    switch (x) {
        case 0:  return 0;
        case 1:  return 1;
        case 2:  return 4;
        case 3:  return 9;
        case 4:  return 16;
        case 5:  return 25;
        case 6:  return 36;
        case 7:  return 49;
        case 8:  return 64;
        case 9:  return 81;
        case 10: return 100;
        case 11: return 121;
        default: return 144;
    }
}

int main(void) {
    int sum = 0;
    int i;
    for (i = 0; i < 13; i++) {
        sum = sum + compute(i);
    }
    return sum;
}
