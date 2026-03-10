// EXPECT_R0: 3
int main() { typedef struct{int a;int b;} P; P p; p.a=3; return p.a; }
