// EXPECT_R0: 0
// TIMEOUT: 4000000
// Jump-table bounds: a small positive minimum (the table is extended down
// to 0), a minimum of 32 or more (subtracted, or folded into the load's
// displacement up to 32767), negative case values (rebased in a register),
// a minimum too large for an immediate, and selectors below, inside and
// above each range, including negatives that an unsigned range check must
// send to default. The selector stays live in the case bodies.
int lowmin(int s) {
    switch (s) {
    case 3:  return s * 2;
    case 4:  return s + 40;
    case 5:  return 7;
    case 6:  return s - 1;
    case 7:  return 11;
    case 8:  return s * s;
    case 9:  return 13;
    case 11: return s ^ 5;
    default: return -s;
    }
}

int highmin(int s) {
    switch (s) {
    case 100: return 1;
    case 101: return s;
    case 102: return 3;
    case 103: return 4;
    case 105: return 5;
    case 106: return s + 6;
    case 107: return 7;
    case 108: return 8;
    default:  return 99;
    }
}

int negs(int s) {
    switch (s) {
    case -4: return 1;
    case -3: return 2;
    case -2: return s * 3;
    case -1: return 4;
    case 0:  return 5;
    case 1:  return 6;
    case 2:  return s + 7;
    case 3:  return 8;
    default: return 0;
    }
}

int far(int s, int base) {
    switch (s - base) {
    case -30000: return 1;
    case -29999: return 2;
    case -29998: return 3;
    case -29996: return 4;
    case -29995: return 5;
    case -29994: return 6;
    case -29993: return 7;
    case -29992: return 8;
    default:     return 0;
    }
}

int big(int s) {
    switch (s) {
    case 40000: return 1;
    case 40001: return 2;
    case 40002: return 3;
    case 40003: return 4;
    case 40005: return 5;
    case 40006: return 6;
    case 40007: return 7;
    case 40008: return 8;
    default:    return 0;
    }
}

int edge(int s) {
    switch (s) {
    case 32767: return 1;
    case 32768: return 2;
    case 32769: return 3;
    case 32770: return 4;
    case 32771: return 5;
    case 32772: return 6;
    case 32773: return 7;
    case 32774: return 8;
    default:    return 0;
    }
}

int ref_lowmin(int s) {
    if (s == 3) return s * 2;  if (s == 4) return s + 40; if (s == 5) return 7;
    if (s == 6) return s - 1;  if (s == 7) return 11;     if (s == 8) return s * s;
    if (s == 9) return 13;     if (s == 11) return s ^ 5;
    return -s;
}

int ref_highmin(int s) {
    if (s == 100) return 1; if (s == 101) return s; if (s == 102) return 3;
    if (s == 103) return 4; if (s == 105) return 5; if (s == 106) return s + 6;
    if (s == 107) return 7; if (s == 108) return 8;
    return 99;
}

int ref_negs(int s) {
    if (s == -4) return 1; if (s == -3) return 2; if (s == -2) return s * 3;
    if (s == -1) return 4; if (s == 0) return 5;  if (s == 1) return 6;
    if (s == 2) return s + 7; if (s == 3) return 8;
    return 0;
}

int main(void) {
    int s;
    for (s = -40; s < 40; s++) {
        if (lowmin(s) != ref_lowmin(s)) return 1;
        if (negs(s) != ref_negs(s)) return 2;
    }
    for (s = 60; s < 140; s++)
        if (highmin(s) != ref_highmin(s)) return 3;
    if (lowmin(-2147483647 - 1) != ref_lowmin(-2147483647 - 1)) return 4;
    if (highmin(-100) != 99 || negs(2147483647) != 0) return 5;
    for (s = -30010; s < -29980; s++) {
        int k = s + 30000, want = 0;
        if (k >= 0 && k <= 8 && k != 3) want = k < 3 ? k + 1 : k;
        if (far(s, 0) != want) return 6;
    }
    for (s = 39990; s < 40020; s++) {
        int k = s - 40000, want = 0;
        if (k >= 0 && k <= 8 && k != 4) want = k < 4 ? k + 1 : k;
        if (big(s) != want) return 7;
    }
    for (s = 32760; s < 32790; s++) {
        int k = s - 32767, want = 0;
        if (k >= 0 && k <= 7) want = k + 1;
        if (edge(s) != want) return 8;
    }
    return 0;
}
