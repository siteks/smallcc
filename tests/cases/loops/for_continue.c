// EXPECT_R0: 8
int main() { int s=0; int i; for(i=0;i<5;i=i+1){if(i==2)continue; s=s+i;} return s; }
