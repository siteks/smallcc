// EXPECT_R0: 3
int main() { int x=1; switch(x){case 1:x=x+1; case 2:x=x+1;} return x; }
