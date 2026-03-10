// EXPECT_R0: 99
int main() { enum{V=99}; int r=0; switch(V){case 99: r=V; break;} return r; }
