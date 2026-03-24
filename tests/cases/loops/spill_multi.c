// EXPECT_R0: 88
// 9 simultaneously-live promoted vregs (a-h, i) with NPHYS=7 forces >=2 spills.
// Exercises split-interval spilling: spilled vregs must be reloaded at each
// loop back-edge (Phase 4b back-edge LOADs) and kept current by Phase 3b
// def-time STOREs after every redefinition.
int main() {
    int a, b, c, d, e, f, g, h, i;
    a=1; b=1; c=1; d=1; e=1; f=1; g=1; h=1;
    for (i=0; i<10; i=i+1) {
        a=a+1; b=b+1; c=c+1; d=d+1;
        e=e+1; f=f+1; g=g+1; h=h+1;
    }
    return a+b+c+d+e+f+g+h;
}
