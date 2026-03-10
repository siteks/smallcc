// EXPECT_R0: 6
int main() { struct I{int x;int y;}; struct O{struct I p;int z;} s={{1,2},3}; return s.p.x+s.p.y+s.z; }
