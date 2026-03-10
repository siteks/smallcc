// EXPECT_R0: 1
int main() { struct P{int x;int y;} s; struct P *p=&s; p->x=1; return p->x; }
