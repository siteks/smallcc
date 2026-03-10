// EXPECT_R0: 9
int main() { typedef struct{int x;int y;} S; typedef S *SP; S s; SP p=&s; s.y=9; return p->y; }
