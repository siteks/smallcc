// EXPECT_R0: 5
int main() { struct P{int a;int b;} s={3,5}; struct P *p=&s; return p->b; }
