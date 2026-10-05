// EXPECT_R0: 12880
// Twenty dense switches in one function, each a jump table on CPU5:
// more tables than the emitter used to track (16).
int f(int s) {
    int t = 0;
    switch ((s + 0) & 7) {
    case 0: t += 1; break;
    case 1: t += 2; break;
    case 2: t += 3; break;
    case 3: t += 4; break;
    case 4: t += 5; break;
    case 5: t += 6; break;
    case 6: t += 7; break;
    case 7: t += 8; break;
    }
    switch ((s + 1) & 7) {
    case 0: t += 9; break;
    case 1: t += 10; break;
    case 2: t += 11; break;
    case 3: t += 12; break;
    case 4: t += 13; break;
    case 5: t += 14; break;
    case 6: t += 15; break;
    case 7: t += 16; break;
    }
    switch ((s + 2) & 7) {
    case 0: t += 17; break;
    case 1: t += 18; break;
    case 2: t += 19; break;
    case 3: t += 20; break;
    case 4: t += 21; break;
    case 5: t += 22; break;
    case 6: t += 23; break;
    case 7: t += 24; break;
    }
    switch ((s + 3) & 7) {
    case 0: t += 25; break;
    case 1: t += 26; break;
    case 2: t += 27; break;
    case 3: t += 28; break;
    case 4: t += 29; break;
    case 5: t += 30; break;
    case 6: t += 31; break;
    case 7: t += 32; break;
    }
    switch ((s + 4) & 7) {
    case 0: t += 33; break;
    case 1: t += 34; break;
    case 2: t += 35; break;
    case 3: t += 36; break;
    case 4: t += 37; break;
    case 5: t += 38; break;
    case 6: t += 39; break;
    case 7: t += 40; break;
    }
    switch ((s + 5) & 7) {
    case 0: t += 41; break;
    case 1: t += 42; break;
    case 2: t += 43; break;
    case 3: t += 44; break;
    case 4: t += 45; break;
    case 5: t += 46; break;
    case 6: t += 47; break;
    case 7: t += 48; break;
    }
    switch ((s + 6) & 7) {
    case 0: t += 49; break;
    case 1: t += 50; break;
    case 2: t += 51; break;
    case 3: t += 52; break;
    case 4: t += 53; break;
    case 5: t += 54; break;
    case 6: t += 55; break;
    case 7: t += 56; break;
    }
    switch ((s + 7) & 7) {
    case 0: t += 57; break;
    case 1: t += 58; break;
    case 2: t += 59; break;
    case 3: t += 60; break;
    case 4: t += 61; break;
    case 5: t += 62; break;
    case 6: t += 63; break;
    case 7: t += 64; break;
    }
    switch ((s + 8) & 7) {
    case 0: t += 65; break;
    case 1: t += 66; break;
    case 2: t += 67; break;
    case 3: t += 68; break;
    case 4: t += 69; break;
    case 5: t += 70; break;
    case 6: t += 71; break;
    case 7: t += 72; break;
    }
    switch ((s + 9) & 7) {
    case 0: t += 73; break;
    case 1: t += 74; break;
    case 2: t += 75; break;
    case 3: t += 76; break;
    case 4: t += 77; break;
    case 5: t += 78; break;
    case 6: t += 79; break;
    case 7: t += 80; break;
    }
    switch ((s + 10) & 7) {
    case 0: t += 81; break;
    case 1: t += 82; break;
    case 2: t += 83; break;
    case 3: t += 84; break;
    case 4: t += 85; break;
    case 5: t += 86; break;
    case 6: t += 87; break;
    case 7: t += 88; break;
    }
    switch ((s + 11) & 7) {
    case 0: t += 89; break;
    case 1: t += 90; break;
    case 2: t += 91; break;
    case 3: t += 92; break;
    case 4: t += 93; break;
    case 5: t += 94; break;
    case 6: t += 95; break;
    case 7: t += 96; break;
    }
    switch ((s + 12) & 7) {
    case 0: t += 97; break;
    case 1: t += 98; break;
    case 2: t += 99; break;
    case 3: t += 100; break;
    case 4: t += 101; break;
    case 5: t += 102; break;
    case 6: t += 103; break;
    case 7: t += 104; break;
    }
    switch ((s + 13) & 7) {
    case 0: t += 105; break;
    case 1: t += 106; break;
    case 2: t += 107; break;
    case 3: t += 108; break;
    case 4: t += 109; break;
    case 5: t += 110; break;
    case 6: t += 111; break;
    case 7: t += 112; break;
    }
    switch ((s + 14) & 7) {
    case 0: t += 113; break;
    case 1: t += 114; break;
    case 2: t += 115; break;
    case 3: t += 116; break;
    case 4: t += 117; break;
    case 5: t += 118; break;
    case 6: t += 119; break;
    case 7: t += 120; break;
    }
    switch ((s + 15) & 7) {
    case 0: t += 121; break;
    case 1: t += 122; break;
    case 2: t += 123; break;
    case 3: t += 124; break;
    case 4: t += 125; break;
    case 5: t += 126; break;
    case 6: t += 127; break;
    case 7: t += 128; break;
    }
    switch ((s + 16) & 7) {
    case 0: t += 129; break;
    case 1: t += 130; break;
    case 2: t += 131; break;
    case 3: t += 132; break;
    case 4: t += 133; break;
    case 5: t += 134; break;
    case 6: t += 135; break;
    case 7: t += 136; break;
    }
    switch ((s + 17) & 7) {
    case 0: t += 137; break;
    case 1: t += 138; break;
    case 2: t += 139; break;
    case 3: t += 140; break;
    case 4: t += 141; break;
    case 5: t += 142; break;
    case 6: t += 143; break;
    case 7: t += 144; break;
    }
    switch ((s + 18) & 7) {
    case 0: t += 145; break;
    case 1: t += 146; break;
    case 2: t += 147; break;
    case 3: t += 148; break;
    case 4: t += 149; break;
    case 5: t += 150; break;
    case 6: t += 151; break;
    case 7: t += 152; break;
    }
    switch ((s + 19) & 7) {
    case 0: t += 153; break;
    case 1: t += 154; break;
    case 2: t += 155; break;
    case 3: t += 156; break;
    case 4: t += 157; break;
    case 5: t += 158; break;
    case 6: t += 159; break;
    case 7: t += 160; break;
    }
    return t;
}

int main(void) {
    int s, t = 0;
    for (s = 0; s < 8; s++) t += f(s);
    return t;
}
