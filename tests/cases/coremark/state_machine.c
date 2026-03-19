// EXPECT_R0: 5
// State machine with function-pointer dispatch (coremark core_state.c style)
typedef int (*state_fn)(int);
int count_a, count_b;
int state_A(int in) {
    if (in==0) return 1;
    count_a=count_a+1; return 0;
}
int state_B(int in) {
    if (in==1) return 0;
    count_b=count_b+1; return 1;
}
int main() {
    state_fn states[2];
    int input[12];
    int state,i;
    states[0]=state_A; states[1]=state_B;
    input[0]=1; input[1]=1; input[2]=0; input[3]=0;
    input[4]=1; input[5]=0; input[6]=1; input[7]=1;
    input[8]=0; input[9]=0; input[10]=1; input[11]=0;
    count_a=0; count_b=0; state=0;
    for (i=0;i<12;i=i+1) state=states[state](input[i]);
    return count_a+count_b;
}
