// EXPECT_R0: 42
int main() { enum{X=42}; int r=0; switch(1){case 1: r=X; break;} return r; }
