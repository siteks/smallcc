// EXPECT_R0: 3
// State machine (switch dispatch): counts 1->0 transitions
int main() {
    int input[8];
    int state, count, i;
    input[0]=1; input[1]=0; input[2]=1; input[3]=1;
    input[4]=0; input[5]=1; input[6]=0; input[7]=1;
    state=0; count=0;
    for (i=0; i<8; i=i+1) {
        switch (state) {
        case 0: if (input[i]==1) state=1; break;
        case 1: if (input[i]==0) { state=2; count=count+1; } break;
        case 2: state = (input[i]==1) ? 1 : 0; break;
        }
    }
    return count;
}
