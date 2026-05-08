// EXPECT_R0: 36
//
// Regression: Braun's for(;;) (infinite-cond) path used to seal both
// cond_blk and body_blk BEFORE wiring cond_blk -> body_blk. With
// body_blk sealed at 0 preds, the first read_var on any loop-carried
// variable in the body went down read_var_recursive's "npreds == 0"
// branch (a fallback that writes a const 0 into the defs map) and
// every subsequent read in the body returned that cached zero. The
// outer loop's mbx/mby SSA collapsed to constants, the increments
// folded to literals at construction time, and the SSA back-edge phi
// never got a chance to form -- so the loop's local mutations never
// persisted across iterations.
//
// Concrete shape that demonstrated the bug: a doubly-nested
// for(;;) { for(...){...} ... if(++a>=N){a=0;if(++b>=M)break;} }
// where the inner loop ran fine (its own header was unsealed at
// construction time, sealed late) but the outer mbx/mby never
// advanced past 0. Surfaced in the NanoJPEG decoder bring-up.
//
// Each (mbx, mby) iteration adds mbx + mby*10. Outer for(;;) iterates
// (0,0), (0,1), (0,2), (1,0), (1,1), (1,2) -- six values summing to
// 0+1+2+10+11+12 = 36.

int main(void) {
    int mbx, mby;
    int total = 0;
    for (mbx = mby = 0;;) {
        int i;
        for (i = 0; i < 1; ++i) {
            total += mbx + mby * 10;
        }
        if (++mbx >= 3) {
            mbx = 0;
            if (++mby >= 2) break;
        }
    }
    return total;
}
